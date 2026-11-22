#!/usr/bin/env python3
"""Drive a PS Vita running the VitaSync plugin from Linux: see its screen and
send it buttons, sticks and touches over USB.

  vitactl.py watch start|stop|status   keep the screen in latest.png (3 fps)
             start --size WxH          video size: 960x544 (default, ~47 frames/s),
                                       896x504 (54), 864x488 (57), 480x272 (60);
                                       shots are always 960x544
             start --audio             also the Vita's sound (plugin build 101+),
                                       kept with the video for `record`; the Vita
                                       is silent until `watch stop`
  vitactl.py press BTN... [ms]         tap buttons (default 150 ms)
  vitactl.py hold BTN... ms            hold buttons
  vitactl.py stick L|R x y ms          move a stick (0-255, 128 = centre)
  vitactl.py touch [T] x y [ms]        one finger (default 150 ms)
  vitactl.py touch2 [T] x1 y1 x2 y2 [ms]   two fingers
  vitactl.py swipe [T] x1 y1 x2 y2 [ms]    one finger moving (default 300 ms)
  vitactl.py pinch [T] x y d1 d2 [ms]      two fingers spreading from d1 to d2 apart
  vitactl.py shot NAME                 save the next screen frame as NAME
  vitactl.py record SECONDS FILE.mkv [--before S]
                                       save every frame of the next SECONDS (and
                                       of the S seconds before, up to 15) to FILE
  vitactl.py analyze FILE.mkv          frame rate, freezes, sudden changes and
                                       input-to-screen delays of a recording; with
                                       sound: loudness, silences, sudden sounds and
                                       input-to-sound delays
  vitactl.py run FILE                  run commands from FILE (plus "wait ms")
  vitactl.py kill                      quit the game (only games that read touch)
  vitactl.py status                    devices, watch, and what the Vita reports

Buttons: SELECT START UP RIGHT DOWN LEFT L R TRIANGLE CIRCLE CROSS SQUARE,
PS VOLUP VOLDOWN (plugin build 90+/91+), L3 R3 (PS TV and controllers only).

Touch positions are screenshot pixels (960x544). T is optional:
  --rear      the rear touchpad, at the point behind that screen pixel (build 91+)
  --force N   pressure 1-128 (default 128)

Input works with or without `watch`; `shot` and `record` need it. While it
runs, `watch` also keeps every frame of the last ~20 s in memory, so `record
--before` can save something that already happened. Input commands log their
time, so a recording shows when each input was sent. The plugin releases
all input when the video stream stops, so don't stop `watch` in the middle
of an input command. Standard library only.
"""

import array
import glob
import json
import math
import os
import select
import shlex
import shutil
import signal
import subprocess
import sys
import time

# SceCtrl bit values. The first 16 bits go in report bytes 1-2.
BUTTONS = {
    "SELECT": 0x0001, "L3": 0x0002, "R3": 0x0004, "START": 0x0008,
    "UP": 0x0010, "RIGHT": 0x0020, "DOWN": 0x0040, "LEFT": 0x0080,
    "L": 0x0100, "R": 0x0200,
    "TRIANGLE": 0x1000, "CIRCLE": 0x2000, "CROSS": 0x4000, "SQUARE": 0x8000,
    # Kernel buttons: not in the 16-bit mask, sent as bits of report byte 11.
    "PS": 0x10000, "VOLUP": 0x100000, "VOLDOWN": 0x200000,
}
PS = BUTTONS["PS"]
KERNEL_BUTTON_BITS = {BUTTONS["PS"]: 0x01, BUTTONS["VOLUP"]: 0x02, BUTTONS["VOLDOWN"]: 0x04}

FRONT, REAR = 0, 1
FRONT_MAX_X, FRONT_MAX_Y = 1919, 1087   # front panel units (1920x1088)
FORCE_DEFAULT = 128
REAR_BOUNDS_FALLBACK = (0, 1919, 108, 889)   # min X, max X, min Y, max Y

REPORT_ID_OUTPUT = 2      # PC -> Vita, 64 bytes on the wire
REPORT_ID_HEARTBEAT = 1   # Vita -> PC, about every 500 ms
HEARTBEAT_MAGIC = 0xC7
RESEND_S = 0.1            # the plugin drops input after 1 s without reports
SCREEN_W, SCREEN_H = 960, 544
AUDIO_DEVICE = "hw:CARD=PSVita,DEV=0"   # the plugin's USB sound card (build 101+)
# Sizes the plugin offers (scaled by the Vita's hardware), with the frames/s
# measured over USB 2: 960x544 47, 896x504 54, 864x488 57, 480x272 60.
# (Plugin builds before 106 also offered 1280x720: upscaled, 30 frames/s at
# most, and the size being started when an experimental build hung.)
VIDEO_SIZES = ("960x544", "896x504", "864x488", "480x272")
TOUCH_SCALE = 2           # touch grid is 1920x1088
FRAME_MAX_AGE_S = 3.0     # older than this, the stream is considered stopped
RING_SEGMENTS = 20        # watch keeps this many 1 s pieces of full-rate video
RING_FILES = RING_SEGMENTS + 2   # never list the piece being written
RECORD_BEFORE_MAX_S = 15
INPUT_COMMANDS = ("press", "hold", "stick", "touch", "touch2", "swipe", "pinch", "kill")
UDEV_HINT = ("install the udev rule: sudo cp linux/70-vitasync.rules /etc/udev/rules.d/ "
             "&& sudo udevadm control --reload && sudo udevadm trigger")


class VitaError(Exception):
    pass


# ---- state and devices ----

def state_dir():
    base = os.environ.get("XDG_RUNTIME_DIR") or f"/tmp/vitactl-{os.getuid()}"
    path = os.path.join(base, "vitactl")
    os.makedirs(path, exist_ok=True)
    return path


def state_file(name):
    return os.path.join(state_dir(), name)


def read_sysfs(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def find_hidraw():
    for node in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        if "HID_ID=0003:0000054C:00001337" in read_sysfs(node + "/device/uevent"):
            return "/dev/" + os.path.basename(node)
    return None


def find_video():
    for node in sorted(glob.glob("/sys/class/video4linux/video*")):
        if read_sysfs(node + "/name") == "PSVita" and read_sysfs(node + "/index") == "0":
            return "/dev/" + os.path.basename(node)
    return None


def need_hidraw():
    dev = find_hidraw()
    if not dev:
        raise VitaError("Vita not found (no 054c:1337 hidraw). Is the plugin running? Check lsusb.")
    return dev


def open_hidraw(dev, flags):
    try:
        return os.open(dev, flags)
    except PermissionError:
        raise VitaError(f"no permission on {dev}; {UDEV_HINT}")


# ---- video (watch) ----

def watch_pid():
    """PID of our background ffmpeg, or None."""
    try:
        with open(state_file("ffmpeg.pid")) as f:
            pid = int(f.read().strip())
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            if b"ffmpeg" not in f.read():
                return None
        return pid
    except (OSError, ValueError):
        return None


def frame_age():
    try:
        return time.time() - os.stat(state_file("latest.png")).st_mtime
    except OSError:
        return None


def stop_ffmpeg(pid):
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    for _ in range(30):
        if not os.path.exists(f"/proc/{pid}"):
            break
        time.sleep(0.1)
    else:
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def recent_uvc_kernel_lines():
    """The last uvcvideo kernel messages from the past minute, if readable."""
    try:
        out = subprocess.run(["journalctl", "-k", "--since=-60s", "--no-pager", "-q"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [l for l in out.splitlines() if "uvcvideo" in l][-3:]


def watch_size():
    try:
        with open(state_file("size")) as f:
            return f.read().strip() or "960x544"
    except OSError:
        return "960x544"


def watch_audio():
    try:
        with open(state_file("audio")) as f:
            return f.read().strip() == "1"
    except OSError:
        return False


def audio_card_present():
    try:
        with open("/proc/asound/cards") as f:
            return "PSVita" in f.read()
    except OSError:
        return False


def watch_start(size="960x544", audio=False):
    if size not in VIDEO_SIZES:
        raise VitaError(f"--size must be one of {', '.join(VIDEO_SIZES)}")
    if watch_pid():
        if watch_size() != size or watch_audio() != audio:
            raise VitaError(f"watch is running at {watch_size()}"
                            f"{' with sound' if watch_audio() else ''}; "
                            "`watch stop` first to change size or sound")
        print(f"watch already running; screen in {state_file('latest.png')}")
        return
    video = find_video()
    if not video:
        raise VitaError("Vita video not found (no /dev/video* named PSVita). Is the plugin running?")
    if audio and not audio_card_present():
        raise VitaError("no PSVita sound card (arecord -l); the plugin needs build 101 or later")
    latest = state_file("latest.png")
    log_path = state_file("ffmpeg.log")
    ring = ring_dir()
    shutil.rmtree(ring, ignore_errors=True)
    os.makedirs(ring)
    # One process, one stream: latest.png at 3 fps for `shot`, and every frame
    # in 1 s pieces stamped with wall-clock time (-ts mono2abs, -copyts) for
    # `record`. Each piece starts at time 0 (concat needs real lengths); the
    # list keeps its wall-clock start. The pieces wrap around, so memory use
    # stays bounded.
    # With --audio, the Vita's sound card is a second input: ALSA stamps its
    # packets with the wall clock too, so the pieces carry both in sync.
    sound_in = ["-f", "alsa", "-thread_queue_size", "1024", "-sample_rate", "48000",
                "-channels", "2", "-i", AUDIO_DEVICE] if audio else []
    sound_out = ["-map", "1:a", "-c:a", "pcm_s16le"] if audio else []
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin", "-y",
           "-f", "v4l2", "-ts", "mono2abs", "-input_format", "nv12",
           "-thread_queue_size", "32", "-video_size", size, "-i", video] + sound_in + [
           "-copyts",
           "-map", "0:v", "-vf", f"fps=3,scale={SCREEN_W}:{SCREEN_H}",
           "-f", "image2", "-update", "1", "-atomic_writing", "1", latest,
           "-map", "0:v", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "12",
           "-g", "10", "-fps_mode", "passthrough"] + sound_out + [
           "-f", "segment", "-segment_time", "1", "-segment_wrap", str(RING_FILES),
           "-segment_format", "matroska", "-reset_timestamps", "1",
           "-segment_list", os.path.join(ring, "list.csv"), "-segment_list_type", "csv",
           "-segment_list_size", str(RING_SEGMENTS), os.path.join(ring, "%03d.mkv")]
    # A single attempt: on 2026-11-21, three quick retries after the Vita
    # stopped answering video setup requests were followed by a crash.
    started = time.time()
    with open(log_path, "w") as log:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
    with open(state_file("size"), "w") as f:
        f.write(size)
    with open(state_file("audio"), "w") as f:
        f.write("1" if audio else "0")
    with open(state_file("ffmpeg.pid"), "w") as f:
        f.write(str(proc.pid))
    while time.time() - started < 8 and proc.poll() is None:
        age = frame_age()
        if age is not None and age < time.time() - started:
            print(f"watch started ({video}); screen in {latest}")
            return
        time.sleep(0.1)
    stop_ffmpeg(proc.pid)
    os.remove(state_file("ffmpeg.pid"))
    tail = read_sysfs(log_path).splitlines()[-3:]
    kernel = recent_uvc_kernel_lines()
    raise VitaError(
        "video did not start. ffmpeg said:\n  " + "\n  ".join(tail) +
        ("\nkernel said:\n  " + "\n  ".join(kernel) if kernel else "") +
        "\nDon't retry in a loop (repeated attempts once crashed the Vita). "
        "Unplug and replug the cable, then try once more; if it still fails, "
        "power the Vita off and on and run henlo.")


def watch_stop():
    pid = watch_pid()
    if not pid:
        print("watch not running")
        return
    stop_ffmpeg(pid)
    try:
        os.remove(state_file("ffmpeg.pid"))
    except OSError:
        pass
    print("watch stopped")


def watch_state():
    pid = watch_pid()
    age = frame_age()
    if not pid:
        return "not running"
    if age is None or age > FRAME_MAX_AGE_S:
        return f"running (pid {pid}) but no recent frame" + ("" if age is None else f" (last {age:.0f} s ago)")
    rate = ring_rate()
    rate = f", {rate:.1f} frames/s over the last {RATE_PIECES} s" if rate else ""
    sound = " with sound" if watch_audio() else ""
    return (f"running (pid {pid}) at {watch_size()}{sound}{rate}, last frame {age:.1f} s ago, "
            f"{state_file('latest.png')}")


RATE_PIECES = 5


def ring_rate():
    """Frames per second the Vita delivered over the last complete pieces."""
    pieces = ring_pieces()[-RATE_PIECES:]
    if len(pieces) < 2:
        return None
    frames = 0
    for path, _, _ in pieces:
        r = subprocess.run(["ffprobe", "-v", "error", "-count_packets", "-select_streams", "v:0",
                            "-show_entries", "stream=nb_read_packets", "-of", "csv=p=0", path],
                           capture_output=True, text=True)
        try:
            frames += int(r.stdout.split(",")[0])
        except ValueError:
            return None
    # Right after a start the first pieces can be empty: no figure yet.
    return frames / (pieces[-1][2] - pieces[0][1]) if frames else None


def need_watch():
    age = frame_age()
    if not watch_pid() or age is None or age > FRAME_MAX_AGE_S:
        raise VitaError("video is not streaming; run `vitactl.py watch start` first")


def shot(name):
    need_watch()
    # A frame written at least 0.2 s after now, so it shows the effect of the
    # input sent just before, not a frame already in the pipeline.
    after = time.time() + 0.2
    deadline = time.time() + 3
    latest = state_file("latest.png")
    while time.time() < deadline:
        try:
            if os.stat(latest).st_mtime >= after:
                shutil.copyfile(latest, name)
                print(name)
                return
        except OSError:
            pass
        time.sleep(0.05)
    raise VitaError("no new frame within 3 s; is watch still running?")


# ---- recording ----

def ring_dir():
    return state_file("ring")


def log_input(argv):
    """Note when an input command starts, for `record`. Kept short."""
    path = state_file("inputs.log")
    try:
        if os.path.getsize(path) > 200_000:
            with open(path) as f:
                keep = f.readlines()[-500:]
            with open(path, "w") as f:
                f.writelines(keep)
    except OSError:
        pass
    with open(path, "a") as f:
        f.write(f"{time.time():.3f}\t{' '.join(argv)}\n")


def inputs_between(t0, t1):
    out = []
    try:
        with open(state_file("inputs.log")) as f:
            for line in f:
                stamp, _, what = line.rstrip("\n").partition("\t")
                t = float(stamp)
                if t0 <= t <= t1:
                    out.append({"t": round(t - t0, 3), "input": what})
    except (OSError, ValueError):
        pass
    return out


def ring_pieces():
    """[(file, start, end)] in wall-clock seconds, oldest first."""
    out = []
    try:
        with open(os.path.join(ring_dir(), "list.csv")) as f:
            for line in f:
                parts = line.strip().split(",")
                if len(parts) == 3:
                    out.append((os.path.join(ring_dir(), parts[0]),
                                float(parts[1]), float(parts[2])))
    except (OSError, ValueError):
        return []
    return out


def run_quiet(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        tail = "\n  ".join(r.stderr.strip().splitlines()[-4:])
        raise VitaError(f"{what} failed:\n  {tail}")
    return r


def cmd_record(args):
    before = 0.0
    if "--before" in args:
        i = args.index("--before")
        if i + 1 >= len(args):
            raise VitaError("--before needs seconds")
        try:
            before = float(args[i + 1])
        except ValueError:
            raise VitaError(f"bad --before {args[i + 1]!r}")
        args = args[:i] + args[i + 2:]
    if len(args) != 2:
        raise VitaError("usage: record SECONDS FILE.mkv [--before S]")
    try:
        secs = float(args[0])
    except ValueError:
        raise VitaError(f"bad SECONDS {args[0]!r}")
    if not 0 <= secs <= 300 or not 0 <= before <= RECORD_BEFORE_MAX_S or secs + before <= 0:
        raise VitaError(f"SECONDS must be 0-300 and --before 0-{RECORD_BEFORE_MAX_S}, not both 0")
    record(secs, args[1], before)


def record(secs, name, before):
    need_watch()
    now = time.time()
    t0, t1 = now - before, now + secs
    tmp = os.path.join(state_dir(), f"rec-{os.getpid()}")
    os.makedirs(tmp, exist_ok=True)
    try:
        # Copy each piece as soon as it is complete: the ring wraps after
        # about RING_FILES seconds, so long recordings can't wait until the end.
        saved = {}   # start -> (copy, start, end)
        deadline = t1 + 5
        while True:
            for path, start, end in ring_pieces():
                if end > t0 and start < t1 and start not in saved:
                    copy = os.path.join(tmp, f"{len(saved):04d}.mkv")
                    shutil.copyfile(path, copy)
                    saved[start] = (copy, start, end)
            if saved and max(e for _, _, e in saved.values()) >= t1:
                break
            if time.time() > deadline:
                raise VitaError("the video pieces stopped coming; is watch still running?")
            time.sleep(0.2)
        pieces = sorted(saved.values(), key=lambda p: p[1])
        first = pieces[0][1]
        if first > t0 + 0.2:
            print(f"note: frames only from {first - t0:.1f} s after the requested start "
                  "(watch started recently)")
        listing = os.path.join(tmp, "list.txt")
        with open(listing, "w") as f:
            f.writelines(f"file '{p[0]}'\n" for p in pieces)
        start = max(t0, first) - first
        end = start + (t1 - max(t0, first))
        sound = has_audio(pieces[0][0])
        sound_args = ["-af", f"atrim=start={start:.3f}:end={end:.3f},asetpts=PTS-STARTPTS",
                      "-c:a", "pcm_s16le"] if sound else []
        run_quiet(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                   "-f", "concat", "-safe", "0", "-i", listing,
                   "-vf", f"trim=start={start:.3f}:end={end:.3f},setpts=PTS-STARTPTS",
                   "-c:v", "libx264", "-preset", "veryfast", "-crf", "12",
                   "-fps_mode", "passthrough"] + sound_args + [name], "saving the recording")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    t0 = max(t0, first)
    info = {"start": round(t0, 3), "seconds": round(t1 - t0, 3), "audio": sound,
            "inputs": inputs_between(t0, t1)}
    with open(name + ".json", "w") as f:
        json.dump(info, f, indent=1)
    frames = frame_times(name)
    print(f"{name}: {len(frames)} frames over {t1 - t0:.1f} s{' with sound' if sound else ''}, "
          f"{len(info['inputs'])} inputs (in {name}.json)")


# ---- analysis ----

def frame_times(name):
    r = run_quiet(["ffprobe", "-v", "error", "-select_streams", "v:0",
                   "-show_entries", "frame=best_effort_timestamp_time",
                   "-of", "csv=p=0", name], "reading frame times")
    out = []
    for line in r.stdout.split():
        field = line.split(",")[0]
        if field not in ("", "N/A"):
            out.append(float(field))
    return out


def filter_times(name, vf, key):
    """Run a filter chain ending in a logging filter; return what it logged
    for each frame, as {time: text after key}."""
    r = subprocess.run(["ffmpeg", "-hide_banner", "-nostdin", "-i", name, "-vf", vf,
                        "-f", "null", "-"], capture_output=True, text=True)
    out = []
    for line in r.stderr.splitlines():
        if key in line:
            out.append(line)
    return out


def has_audio(name):
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a", "-show_entries",
                        "stream=index", "-of", "csv=p=0", name], capture_output=True, text=True)
    return bool(r.stdout.strip())


SOUND_STEP = 0.01        # loudness windows: 10 ms
SILENT_DB = -60.0
ONSET_DB = 12.0          # a sudden sound: this much above the 200 ms before it...
ONSET_FLOOR_DB = -50.0   # ...and louder than this


def loudness(name):
    """RMS level in dBFS of each 10 ms of the (mono-mixed) sound."""
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-i", name,
                        "-map", "0:a", "-ac", "1", "-ar", "48000", "-f", "s16le", "-"],
                       capture_output=True)
    pcm = array.array("h")
    pcm.frombytes(r.stdout[:len(r.stdout) // 2 * 2])
    n = int(48000 * SOUND_STEP)
    out = []
    for i in range(0, len(pcm) - n + 1, n):
        power = sum(x * x for x in pcm[i:i + n]) / n
        out.append(10 * math.log10(power / 32768 ** 2) if power > 0 else -120.0)
    return out


def sound_onsets(db):
    """Indexes of windows where a sound starts suddenly."""
    onsets, last = [], -100
    for k in range(20, len(db)):
        before = sorted(db[k - 20:k])[10]
        if db[k] >= before + ONSET_DB and db[k] > ONSET_FLOOR_DB and k - last >= 10:
            onsets.append(k)
            last = k
    return onsets


def sound_section(name, info, lines, events, duration):
    """Add the sound part of the report; returns the onset times."""
    db = loudness(name)
    if not db:
        lines.append("sound: none decoded")
        return []
    k_sound = next((k for k, v in enumerate(db) if v > SILENT_DB), None)
    power = sum(10 ** (v / 10) for v in db) / len(db)
    lines.append(f"sound: {len(db) * SOUND_STEP:.2f} s, peak {max(db):.1f} dB, average "
                 f"{10 * math.log10(power) if power > 0 else -120:.1f} dB, "
                 f"{100 * sum(1 for v in db if v <= SILENT_DB) / len(db):.0f}% silent "
                 f"(below {SILENT_DB:.0f} dB)")
    if k_sound is None:
        lines.append("  silent all along")
        return []
    run = 0
    silences = []
    for k in range(k_sound, len(db) + 1):
        if k < len(db) and db[k] <= SILENT_DB:
            run += 1
            continue
        if run * SOUND_STEP >= 0.1:
            silences.append(((k - run) * SOUND_STEP, run * SOUND_STEP))
        run = 0
    for t, length in silences[:10]:
        lines.append(f"  silence for {length * 1000:.0f} ms from {t:.3f} s")
    onsets = [k * SOUND_STEP for k in sound_onsets(db)]
    for t in onsets[:15]:
        lines.append(f"  sudden sound at {t:.3f} s ({db[int(round(t / SOUND_STEP))]:.0f} dB)")
        events.append((t, "sound"))
    lines.append("  (sound and picture times both come from the PC's clock; the sound also "
                 "spends about 50 ms in the Vita's buffers, not corrected: compare reaction "
                 "times between builds rather than as absolute values)")
    return onsets


def sound_response(onsets, t_in):
    after = [t for t in onsets if t_in < t <= t_in + 1.5]
    return f"sound reacts {(after[0] - t_in) * 1000:.0f} ms later" if after \
        else "no new sound within 1.5 s"


SMALL_W, SMALL_H = 120, 68   # analysis works on small grey frames


def small_frames(name):
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-i", name,
                        "-vf", f"scale={SMALL_W}:{SMALL_H},format=gray", "-fps_mode", "passthrough",
                        "-f", "rawvideo", "-"], capture_output=True)
    size = SMALL_W * SMALL_H
    return [r.stdout[i:i + size] for i in range(0, len(r.stdout) - size + 1, size)]


def changed(a, b):
    """Share of pixels that changed clearly between two small frames."""
    return sum(1 for x, y in zip(a, b) if abs(x - y) > 24) / len(a)


def input_response(times, small, t_in, events):
    """When the screen first reacts to an input: the first frame that differs
    from the one at the input by more than the screen changes by itself in
    the 0.3 s before it."""
    if not small:
        return "no frame comparison"
    ref = max((i for i, t in enumerate(times) if t <= t_in), default=None)
    if ref is None:
        return "sent before the recording started"
    start = max((i for i, t in enumerate(times) if t <= t_in - 0.3), default=0)
    by_itself = max((changed(small[start], small[j]) for j in range(start, ref + 1)), default=0)
    for j in range(ref + 1, len(times)):
        if times[j] > t_in + 1.5:
            break
        if changed(small[ref], small[j]) > max(by_itself + 0.02, 0.02):
            events.append((times[j], "response"))
            return f"screen reacts {(times[j] - t_in) * 1000:.0f} ms later"
    return "no visible reaction within 1.5 s"


def analyze(name):
    if not os.path.exists(name):
        raise VitaError(f"no such file: {name}")
    info = {}
    try:
        with open(name + ".json") as f:
            info = json.load(f)
    except (OSError, ValueError):
        pass
    times = frame_times(name)
    if len(times) < 2:
        raise VitaError("fewer than 2 frames in the recording")
    duration = times[-1] - times[0]
    gaps = [b - a for a, b in zip(times, times[1:])]
    typical = sorted(gaps)[len(gaps) // 2]

    # Pictures that differ from the one before (mpdecimate drops repeats).
    unique = []
    for line in filter_times(name, "mpdecimate,showinfo", "pts_time:"):
        try:
            unique.append(float(line.split("pts_time:")[1].split()[0]))
        except (IndexError, ValueError):
            pass
    # Sudden changes: a quarter of the screen or more changing in one frame
    # (cuts, flashes, a dialog appearing).
    small = small_frames(name)
    if len(small) != len(times):
        small = []
    changes = [(times[i], changed(small[i - 1], small[i]))
               for i in range(1, len(small)) if changed(small[i - 1], small[i]) >= 0.25]

    lines = [f"{name}: {len(times)} frames over {duration:.2f} s"]
    lines.append(f"stream: {len(times) / duration:.1f} frames/s delivered, "
                 f"typical gap {typical * 1000:.0f} ms, longest {max(gaps) * 1000:.0f} ms")
    late = [(times[i], g) for i, g in enumerate(gaps) if g > max(2.5 * typical, 0.05)]
    for t, g in late[:10]:
        lines.append(f"  stream gap: {g * 1000:.0f} ms after {t:.3f} s (frames lost or late on USB)")
    events = []
    if unique:
        lines.append(f"picture: {len(unique)} different pictures, "
                     f"{len(unique) / duration:.1f}/s on average (the game's real frame rate, "
                     "if the screen moves all the time)")
        per_s = {}
        for t in unique:
            per_s[int(t)] = per_s.get(int(t), 0) + 1
        lines.append("  per second: " + " ".join(f"{per_s.get(s, 0)}" for s in range(int(duration) + 1)))
        holds = [(a, b - a) for a, b in zip(unique, unique[1:] + [times[-1]]) if b - a >= 0.1]
        for t, h in holds[:15]:
            lines.append(f"  unchanged for {h * 1000:.0f} ms from {t:.3f} s")
            events.append((t + h, "after-still"))
    for t, share in changes[:15]:
        lines.append(f"  sudden change at {t:.3f} s ({share * 100:.0f}% of the screen)")
        events.append((max(t - typical, 0), "before-change"))
        events.append((t, "change"))
    onsets = sound_section(name, info, lines, events, duration) if has_audio(name) else None
    for inp in info.get("inputs", []):
        lines.append(f"  input at {inp['t']:.3f} s: {inp['input']}, " +
                     input_response(times, small, inp["t"], events) +
                     (", " + sound_response(onsets, inp["t"]) if onsets is not None else ""))

    if not small:
        lines.append("  (frame comparison skipped: frame count mismatch)")
    out = os.path.splitext(name)[0] + "-analysis"
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for i, (t, kind) in enumerate(sorted(set(events))[:24], 1):
        run_quiet(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                   "-ss", f"{t:.3f}", "-i", name, "-frames:v", "1",
                   os.path.join(out, f"{i:02d}-{t:07.3f}s-{kind}.png")], "saving a frame")
    # One overview sheet: 16 frames spread over the recording, 4x4, 960x544.
    step = max(len(times) // 16, 1)
    run_quiet(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", name,
               "-vf", f"select='not(mod(n\\,{step}))',scale=240:136,tile=4x4",
               "-fps_mode", "passthrough", "-frames:v", "1", os.path.join(out, "sheet.png")],
              "making the overview sheet")
    lines.append(f"sheet.png: 16 frames, one every {step * duration / len(times):.2f} s, "
                 "left to right, top to bottom")
    if onsets is not None:
        run_quiet(["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", name,
                   "-filter_complex",
                   # Square-root scale so quiet sounds show; black background.
                   f"color=c=black:s=960x200[bg];"
                   f"[0:a]aformat=channel_layouts=mono,"
                   f"showwavespic=s=960x200:colors=white:scale=sqrt[w];"
                   f"[bg][w]overlay=format=auto,"
                   f"drawgrid=w={max(960 / max(duration, 0.1), 2):.2f}:h=200:color=gray@0.6",
                   "-frames:v", "1", os.path.join(out, "audio.png")], "drawing the waveform")
        lines.append("audio.png: the waveform, a grey line every second")
    lines.append(f"pictures and this report: {out}/")
    with open(os.path.join(out, "report.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


# ---- input ----

# Report layout (64 bytes): 0 report ID, 1-2 buttons, 3-6 sticks, 7 touch
# flags, 8-10 front finger 1, 11 kernel buttons, 12-14 front finger 2,
# 15-17 rear finger 1, 18 kill, 19-21 rear finger 2, 22/23 front/rear force.
# Each finger: 12-bit X and Y in panel units, packed in 3 bytes.
FINGER_SLOTS = {FRONT: ((0x01, 8), (0x02, 12)), REAR: ((0x04, 15), (0x08, 19))}
FORCE_BYTE = {FRONT: 22, REAR: 23}


def report(buttons=0, lx=128, ly=128, rx=128, ry=128, fingers=None, forces=None,
           kill=False):
    """fingers: {FRONT|REAR: [(x, y), ...]} in panel units, at most 2 per panel.
    forces: {FRONT|REAR: 1-128}; missing means the plugin's default (128)."""
    r = bytearray(64)
    r[0] = REPORT_ID_OUTPUT
    r[1], r[2] = buttons & 0xFF, (buttons >> 8) & 0xFF
    r[3], r[4], r[5], r[6] = lx, ly, rx, ry
    for port, points in (fingers or {}).items():
        for (flag, off), (tx, ty) in zip(FINGER_SLOTS[port], points):
            r[7] |= flag
            r[off], r[off + 1] = tx & 0xFF, ty & 0xFF
            r[off + 2] = ((tx >> 8) & 0x0F) | (((ty >> 8) & 0x0F) << 4)
    for port, force in (forces or {}).items():
        r[FORCE_BYTE[port]] = force
    for bit, flag in KERNEL_BUTTON_BITS.items():
        if buttons & bit:
            r[11] |= flag
    r[18] = 1 if kill else 0
    return bytes(r)


NEUTRAL = report()


def send(steps):
    """steps: list of (report, seconds). Each report is resent every 100 ms
    for its duration; a neutral report always ends the command."""
    fd = open_hidraw(need_hidraw(), os.O_WRONLY)
    try:
        for rep, secs in steps:
            end = time.time() + secs
            while True:
                os.write(fd, rep)
                left = end - time.time()
                if left <= 0:
                    break
                time.sleep(min(RESEND_S, left))
    finally:
        os.write(fd, NEUTRAL)
        os.close(fd)


def parse_buttons(names):
    mask = 0
    for n in names:
        bit = BUTTONS.get(n.upper())
        if bit is None:
            raise VitaError(f"unknown button {n!r}; known: {' '.join(BUTTONS)}")
        mask |= bit
    return mask


def parse_int(s, what, lo, hi):
    try:
        v = int(s)
    except ValueError:
        raise VitaError(f"{what} must be a number, not {s!r}")
    if not lo <= v <= hi:
        raise VitaError(f"{what} must be {lo}-{hi}, not {v}")
    return v


def parse_ms(s):
    return parse_int(s, "duration (ms)", 1, 600000) / 1000


def cmd_press(args, default_ms):
    """BTN... [ms]: a trailing number is the duration."""
    if args and args[-1].isdigit():
        secs, names = parse_ms(args[-1]), args[:-1]
    elif default_ms is None:
        raise VitaError("hold needs a duration in ms: hold BTN... ms")
    else:
        secs, names = default_ms / 1000, args
    if not names:
        raise VitaError("no button given")
    send([(report(buttons=parse_buttons(names)), secs)])


def cmd_stick(args):
    if len(args) != 4 or args[0].upper() not in ("L", "R"):
        raise VitaError("usage: stick L|R x y ms")
    x = parse_int(args[1], "x", 0, 255)
    y = parse_int(args[2], "y", 0, 255)
    secs = parse_ms(args[3])
    rep = report(lx=x, ly=y) if args[0].upper() == "L" else report(rx=x, ry=y)
    send([(rep, secs)])


def rear_bounds():
    """Rear touchpad active area (min X, max X, min Y, max Y) in panel units,
    as the plugin reads it from the Vita (heartbeat bytes 38-45, build 91+).
    Cached in the state folder: it never changes."""
    cache = state_file("rear_bounds")
    try:
        with open(cache) as f:
            b = tuple(int(v) for v in f.read().split())
        if len(b) == 4:
            return b
    except (OSError, ValueError):
        pass
    hb = read_heartbeat(need_hidraw())
    if not hb:
        # No heartbeat: the values a Vita reported on 2026-11-21
        # (build 92 debug log: X 0-1919, Y 108-889).
        return REAR_BOUNDS_FALLBACK
    b = tuple(int.from_bytes(hb[i:i + 2], "little", signed=True) for i in (38, 40, 42, 44))
    if hb[63] < 91 or b[1] <= b[0] or b[3] <= b[2]:
        raise VitaError(f"the plugin (build {hb[63]}) doesn't report the rear touchpad; "
                        "--rear needs build 91 or later")
    with open(cache, "w") as f:
        f.write(" ".join(str(v) for v in b))
    return b


def to_panel(port, x, y):
    """Screenshot pixel -> panel units. The rear maps the whole screen onto
    the rear's active area: the point behind that screen pixel."""
    x = min(max(round(x), 0), SCREEN_W - 1)
    y = min(max(round(y), 0), SCREEN_H - 1)
    if port == FRONT:
        return min(x * TOUCH_SCALE, FRONT_MAX_X), min(y * TOUCH_SCALE, FRONT_MAX_Y)
    min_x, max_x, min_y, max_y = rear_bounds()
    return (min_x + round(x * (max_x - min_x) / (SCREEN_W - 1)),
            min_y + round(y * (max_y - min_y) / (SCREEN_H - 1)))


def parse_touch_args(args, usage, counts):
    """Pull --rear and --force N out of args; check the positional count."""
    port, forces, pos = FRONT, {}, []
    it = iter(args)
    for a in it:
        if a == "--rear":
            port = REAR
        elif a == "--force":
            forces = {"force": parse_int(next(it, ""), "force", 1, 128)}
        else:
            pos.append(a)
    if len(pos) not in counts:
        raise VitaError("usage: " + usage)
    return port, {port: forces["force"]} if forces else {}, pos


def parse_point(xs, ys):
    return (parse_int(xs, "x", 0, SCREEN_W - 1), parse_int(ys, "y", 0, SCREEN_H - 1))


def touch_steps(port, forces, path, secs, step_s=0.016):
    """Reports for fingers following path(t) -> [(x, y), ...] for t in 0..1."""
    n = max(1, round(secs / step_s))
    steps = []
    for i in range(n + 1):
        pts = [to_panel(port, x, y) for x, y in path(i / n)]
        steps.append((report(fingers={port: pts}, forces=forces), secs / (n + 1)))
    return steps


def cmd_touch(args):
    port, forces, pos = parse_touch_args(
        args, "touch [--rear] [--force N] x y [ms]  (screenshot pixels 960x544)", (2, 3))
    p = parse_point(pos[0], pos[1])
    secs = parse_ms(pos[2]) if len(pos) == 3 else 0.15
    send([(report(fingers={port: [to_panel(port, *p)]}, forces=forces), secs)])


def cmd_touch2(args):
    port, forces, pos = parse_touch_args(
        args, "touch2 [--rear] [--force N] x1 y1 x2 y2 [ms]", (4, 5))
    p1, p2 = parse_point(pos[0], pos[1]), parse_point(pos[2], pos[3])
    secs = parse_ms(pos[4]) if len(pos) == 5 else 0.15
    pts = [to_panel(port, *p1), to_panel(port, *p2)]
    send([(report(fingers={port: pts}, forces=forces), secs)])


def cmd_swipe(args):
    port, forces, pos = parse_touch_args(
        args, "swipe [--rear] [--force N] x1 y1 x2 y2 [ms]", (4, 5))
    (x1, y1), (x2, y2) = parse_point(pos[0], pos[1]), parse_point(pos[2], pos[3])
    secs = parse_ms(pos[4]) if len(pos) == 5 else 0.3
    send(touch_steps(port, forces,
                     lambda t: [(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t)], secs))


def cmd_pinch(args):
    port, forces, pos = parse_touch_args(
        args, "pinch [--rear] [--force N] x y d1 d2 [ms]  "
              "(d1 -> d2: horizontal spacing in pixels; d2 > d1 zooms in)", (4, 5))
    x, y = parse_point(pos[0], pos[1])
    d1 = parse_int(pos[2], "d1", 0, SCREEN_W - 1)
    d2 = parse_int(pos[3], "d2", 0, SCREEN_W - 1)
    secs = parse_ms(pos[4]) if len(pos) == 5 else 0.5

    def path(t):
        half = (d1 + (d2 - d1) * t) / 2
        return [(x - half, y), (x + half, y)]

    send(touch_steps(port, forces, path, secs))


def cmd_kill():
    # The plugin only kills once it has seen the command at rest since the
    # last kill, and it acts on its own thread: neutral, command, neutral.
    send([(NEUTRAL, 0.3), (report(kill=True), 0.7), (NEUTRAL, 0.3)])
    print("kill sent; check with a shot (it only works in games that read touch)")


# ---- status ----

def read_heartbeat(dev, timeout=1.5):
    fd = open_hidraw(dev, os.O_RDONLY)
    try:
        end = time.time() + timeout
        while time.time() < end:
            ready, _, _ = select.select([fd], [], [], end - time.time())
            if not ready:
                break
            buf = os.read(fd, 64)
            if len(buf) >= 64 and buf[0] == REPORT_ID_HEARTBEAT and buf[1] == HEARTBEAT_MAGIC:
                return buf
    finally:
        os.close(fd)
    return None


def button_names(mask):
    return " ".join(n for n, b in BUTTONS.items() if mask & b) or "none"


def cmd_status():
    hid, video = find_hidraw(), find_video()
    print(f"hidraw: {hid or 'not found'}")
    print(f"video:  {video or 'not found'}")
    print(f"watch:  {watch_state()}")
    if not hid:
        return
    hb = read_heartbeat(hid)
    if not hb:
        note = "" if watch_pid() else " (it may only arrive while video streams: watch start)"
        print(f"heartbeat: none within 1.5 s{note}")
        return
    # Byte 29: bit 0 front finger, 1 PS, 2 VOL+, 3 VOL-, 4 rear finger.
    held = hb[7] | (hb[8] << 8)
    for bit, name in ((0x02, "PS"), (0x04, "VOLUP"), (0x08, "VOLDOWN")):
        if hb[29] & bit:
            held |= BUTTONS[name]
    build = hb[63]
    print(f"plugin: build {build}, video stream {'on' if hb[21] else 'off'}, "
          f"input {'active' if hb[2] else 'idle'}, {hb[3] | (hb[4] << 8)} reports received")
    if build < 91:
        print(f"  (build {build} predates " +
              ("PS, " if build < 90 else "") +
              "VOLUP/VOLDOWN, two fingers, the rear touchpad and pressure)")
    print(f"held by vitactl: {button_names(held)}; "
          f"front touch {'down' if hb[29] & 0x01 else 'up'}" +
          (f", rear touch {'down' if hb[29] & 0x10 else 'up'}" if build >= 91 else ""))
    if build >= 91:
        b = [int.from_bytes(hb[i:i + 2], "little", signed=True) for i in (38, 40, 42, 44)]
        print(f"touch reads patched in the last 0.5 s: front {hb[30]}, rear {hb[31]}; "
              f"rear touchpad X {b[0]}-{b[1]}, Y {b[2]}-{b[3]}")
    # Before build 96, hb[9] held a debug probe, so SELECT, START and the
    # D-pad (bits 0-7) weren't reported.
    console = (hb[10] << 8) | (hb[11] << 16) | (hb[12] << 24)
    if build >= 96:
        console |= hb[9]
    note = "" if build >= 96 else " (SELECT, START and D-pad not reported)"
    print(f"console sees: {button_names(console & 0xFFFF)}{note}; "
          f"sticks L {hb[13]},{hb[14]} R {hb[15]},{hb[16]}")
    if build >= 97:
        st = hb[19]
        words = [w for bit, w in ((0x01, "PC listening"), (0x02, "streaming"),
                                  (0x08, "capturing the Vita's sound"),
                                  (0x10, "route taken back once"),
                                  (0x20, "monitor functions not found, silence"),
                                  (0x40, "capture failed, silence"),
                                  (0x80, "a stream had to start with old requests queued")
                                  if build >= 99 else (0, ""))
                 if st & bit]
        if not st & 0x04:
            words.insert(0, "NOT READY")
        if build >= 98:
            errors = f"{hb[62]} failed, last code ends in {hb[18]:02X}" if hb[62] \
                else "no failures"
        else:
            errors = f"{hb[62] >> 4} failed completions, {hb[62] & 0x0F} failed sends"
        print(f"audio: {', '.join(words) or 'idle'}; last 0.5 s: {hb[60] * 4} packets, "
              f"{errors}, {hb[59] >> 4} underruns/overflows")


# ---- dispatch ----

def run_command(argv):
    if not argv:
        raise VitaError("no command; see vitactl.py --help")
    cmd, args = argv[0].lower(), argv[1:]
    if cmd in INPUT_COMMANDS:
        log_input(argv)
    if cmd == "watch":
        sub = args[0] if args else ""
        if sub == "start":
            rest, size, audio = args[1:], "960x544", False
            while rest:
                if rest[0] == "--size" and len(rest) >= 2:
                    size, rest = rest[1], rest[2:]
                elif rest[0] == "--audio":
                    audio, rest = True, rest[1:]
                else:
                    raise VitaError("usage: watch start [--size WxH] [--audio]")
            watch_start(size, audio)
        elif sub == "stop":
            watch_stop()
        elif sub == "status":
            print(watch_state())
        else:
            raise VitaError("usage: watch start [--size WxH] [--audio] | stop | status")
    elif cmd == "press":
        cmd_press(args, 150)
    elif cmd == "hold":
        cmd_press(args, None)
    elif cmd == "stick":
        cmd_stick(args)
    elif cmd == "touch":
        cmd_touch(args)
    elif cmd == "touch2":
        cmd_touch2(args)
    elif cmd == "swipe":
        cmd_swipe(args)
    elif cmd == "pinch":
        cmd_pinch(args)
    elif cmd == "shot":
        if len(args) != 1:
            raise VitaError("usage: shot NAME")
        shot(args[0])
    elif cmd == "record":
        cmd_record(args)
    elif cmd == "analyze":
        if len(args) != 1:
            raise VitaError("usage: analyze FILE.mkv")
        analyze(args[0])
    elif cmd == "kill":
        cmd_kill()
    elif cmd == "status":
        cmd_status()
    elif cmd == "wait":
        if len(args) != 1:
            raise VitaError("usage: wait ms")
        time.sleep(parse_ms(args[0]))
    elif cmd == "run":
        if len(args) != 1:
            raise VitaError("usage: run FILE")
        run_file(args[0])
    else:
        raise VitaError(f"unknown command {cmd!r}; see vitactl.py --help")


def run_file(path):
    try:
        with open(path) as f:
            lines = f.readlines()
    except OSError as e:
        raise VitaError(f"cannot read {path}: {e.strerror}")
    for num, line in enumerate(lines, 1):
        words = shlex.split(line, comments=True)
        if not words:
            continue
        if words[0].lower() == "run":
            raise VitaError(f"{path}:{num}: run inside run is not supported")
        print(f"> {' '.join(words)}")
        try:
            run_command(words)
        except VitaError as e:
            raise VitaError(f"{path}:{num}: {e}")


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0
    try:
        run_command(sys.argv[1:])
    except VitaError as e:
        print(f"vitactl: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
