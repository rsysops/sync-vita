#!/usr/bin/env python3
"""Drive a PS Vita running the VitaSync plugin from Linux: see its screen and
send it buttons, sticks and touches over USB.

  vitactl.py watch start|stop|status   keep the screen in latest.png (3 fps)
  vitactl.py press BTN... [ms]         tap buttons (default 150 ms)
  vitactl.py hold BTN... ms            hold buttons
  vitactl.py stick L|R x y ms          move a stick (0-255, 128 = centre)
  vitactl.py touch [T] x y [ms]        one finger (default 150 ms)
  vitactl.py touch2 [T] x1 y1 x2 y2 [ms]   two fingers
  vitactl.py swipe [T] x1 y1 x2 y2 [ms]    one finger moving (default 300 ms)
  vitactl.py pinch [T] x y d1 d2 [ms]      two fingers spreading from d1 to d2 apart
  vitactl.py shot NAME                 save the next screen frame as NAME
  vitactl.py run FILE                  run commands from FILE (plus "wait ms")
  vitactl.py kill                      quit the game (only games that read touch)
  vitactl.py status                    devices, watch, and what the Vita reports

Buttons: SELECT START UP RIGHT DOWN LEFT L R TRIANGLE CIRCLE CROSS SQUARE,
PS VOLUP VOLDOWN (plugin build 90+/91+), L3 R3 (PS TV and controllers only).

Touch positions are screenshot pixels (960x544). T is optional:
  --rear      the rear touchpad, at the point behind that screen pixel (build 91+)
  --force N   pressure 1-128 (default 128)

Input works with or without `watch`; `shot` needs it. The plugin releases
all input when the video stream stops, so don't stop `watch` in the middle
of an input command. Standard library only.
"""

import glob
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
TOUCH_SCALE = 2           # touch grid is 1920x1088
FRAME_MAX_AGE_S = 3.0     # older than this, the stream is considered stopped
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


def watch_start():
    if watch_pid():
        print(f"watch already running; screen in {state_file('latest.png')}")
        return
    video = find_video()
    if not video:
        raise VitaError("Vita video not found (no /dev/video* named PSVita). Is the plugin running?")
    latest = state_file("latest.png")
    log_path = state_file("ffmpeg.log")
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostdin", "-y",
           "-f", "v4l2", "-input_format", "nv12", "-video_size", f"{SCREEN_W}x{SCREEN_H}",
           "-i", video, "-vf", "fps=3",
           "-f", "image2", "-update", "1", "-atomic_writing", "1", latest]
    # A single attempt: on 2026-11-21, three quick retries after the Vita
    # stopped answering video setup requests were followed by a crash.
    started = time.time()
    with open(log_path, "w") as log:
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
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
    return f"running (pid {pid}), last frame {age:.1f} s ago, {state_file('latest.png')}"


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
    # hb[9] is reused by the plugin for a debug probe, so only bits 8+ of
    # what the console sees are reliable: L and R (0x100, 0x200), L1/R1 copies,
    # and the face buttons.
    console = (hb[10] << 8) | (hb[11] << 16) | (hb[12] << 24)
    print(f"console sees: {button_names(console & 0xFF00)} (SELECT, START and D-pad not reported); "
          f"sticks L {hb[13]},{hb[14]} R {hb[15]},{hb[16]}")


# ---- dispatch ----

def run_command(argv):
    if not argv:
        raise VitaError("no command; see vitactl.py --help")
    cmd, args = argv[0].lower(), argv[1:]
    if cmd == "watch":
        sub = args[0] if args else ""
        if sub == "start":
            watch_start()
        elif sub == "stop":
            watch_stop()
        elif sub == "status":
            print(watch_state())
        else:
            raise VitaError("usage: watch start|stop|status")
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
