# VitaSync — PS Vita streaming & control over USB

See and hear the PS Vita on a Linux PC **and** control the console (buttons,
sticks, front and rear touch, PS and volume) over USB, from the command line.

Prototype based on a modified fork of **`vita-udcd-uvc` 1.7** (`xerpi/vita-udcd-uvc`, MIT license).
Keeping the Vita's speaker on during sound capture uses the technique and
hardware values found by **vitastream** (`devnoname120/vitastream`, GPL-3.0).

## What's in it

- `VitaSync/` — Vita kernel plugin (`VitaSync.skprx`): UVC video, USB audio
  (the Vita's sound), vendor HID (input) and file transfer. Device name
  `PSVita`, USB ID `054c:1337`.
- `linux/` — `vitactl.py`, the command-line tool (screen, recordings, input,
  files), and the udev rule it needs.
- `TESTING.md` — an end-to-end test scenario; `TODO.md` — known problems and
  next steps.
- `.claude/skills/vita-testing/` — a Claude Code skill to test homebrew on the
  Vita with all this.

## Installing the plugin on the Vita

1. Build it: `./build-vita.sh` (see below).
2. Copy `VitaSync/VitaSync.skprx` into `ur0:tai/` (VitaShell).
3. Add to `ur0:tai/config.txt`:

   ```
   *KERNEL
   ur0:tai/VitaSync.skprx
   ```

4. Reboot the Vita completely (and run henlo, if that's how plugins load).

The plugin takes USB over about 40 s after boot.

## Video troubleshooting

If `vitactl.py watch start` fails, follow the advice it prints and try once
more; don't retry in a loop (repeated quick attempts once crashed the Vita).
Unplugging and replugging the cable often helps; otherwise power the Vita off
and on.

## Linux

On Linux, build the plugin with `./build-vita.sh` (Docker, `vitasdk/vitasdk:2026.08`;
`./build-vita.sh DEBUG=1` adds a log in `ur0:data/VitaSync_log.txt`).

`linux/vitactl.py` (Python 3, standard library and `ffmpeg`) saves the Vita's
screen, records it, and sends input.

1. Once, let your user send input and copy files, and keep the Vita's USB
   awake (reinstall it if you installed it before build 119):

   ```bash
   sudo cp linux/70-vitasync.rules /etc/udev/rules.d/
   sudo udevadm control --reload && sudo udevadm trigger
   ```

2. Start the video, then send input. Input also works without the video, but
   `shot` and the heartbeat in `status` need it, and the plugin releases all
   input when the stream stops.

   ```bash
   ./linux/vitactl.py watch start     # screen kept in $XDG_RUNTIME_DIR/vitactl/latest.png
   ./linux/vitactl.py watch start --size 864x488   # smaller video, more frames/s (below)
   ./linux/vitactl.py watch start --audio          # also the Vita's sound, kept for `record`
   ./linux/vitactl.py press CROSS     # SELECT START UP RIGHT DOWN LEFT L R TRIANGLE CIRCLE CROSS SQUARE
                                      # PS VOLUP VOLDOWN, L3 R3 (PS TV and controllers only)
   ./linux/vitactl.py hold L R 2000
   ./linux/vitactl.py stick L 128 0 1000
   ./linux/vitactl.py touch 480 272   # screenshot pixels (960x544)
   ./linux/vitactl.py touch2 300 272 660 272         # two fingers
   ./linux/vitactl.py swipe 480 450 480 100          # one finger moving (300 ms)
   ./linux/vitactl.py pinch 480 272 100 400          # two fingers spreading: zoom in
   ./linux/vitactl.py touch --rear --force 64 480 272   # rear touchpad, behind that pixel
   ./linux/vitactl.py shot menu.png   # the next frame, taken after the last input
   ./linux/vitactl.py record 3 clip.mkv --before 2   # every frame (~47/s) of the next 3 s
                                      # and the 2 s before; inputs noted in clip.mkv.json
   ./linux/vitactl.py analyze clip.mkv   # frame rate, freezes, sudden changes, input delays,
                                         # with pictures of each in clip-analysis/; with
                                         # sound: loudness, silences, sudden sounds, waveform
   ./linux/vitactl.py run steps.txt   # one command per line, plus "wait ms"
   ./linux/vitactl.py status          # devices, video, and what the plugin reports
   ./linux/vitactl.py watch stop
   ```

Video sizes and the frames per second measured over USB 2 (plugin build 117,
2026-11-26). Shots are always 960x544 (scaled up); recordings keep the real size.

| `--size` | frames/s |
|---|---|
| 960x544 (default) | 53 |
| 896x504 | 60 |
| 864x488 | 60 |
| 480x272 | 60 |

Plugin builds 116 and older were slower: 47, 54, 57 and 60.

Plugin builds before 106 also offered 1280x720 (upscaled, 30 frames/s at most);
it was removed (see TODO.md).

**Sound** (plugin build 101+). The Vita is also a USB sound card: Linux lists it
as `card PSVita` (48 kHz, stereo, 16-bit), carrying the Vita's final mix (games,
system sounds, music). While the PC has it open, the plugin also plays that
mix on the Vita's own speaker or headphones (build 107+, firmwares 3.60–3.74),
so the person holding it still hears everything, and the volume buttons still
work. On a firmware it doesn't recognise, and in builds 101–106, the Vita is
silent until the PC closes it; `vitactl.py status` says which. `watch start --audio`
keeps it open for the ring buffer, so `record` saves sound with the video. For a
plain WAV:

```bash
arecord -D hw:CARD=PSVita,DEV=0 -f S16_LE -r 48000 -c 2 -d 10 sound.wav
```

**Files** (plugin build 119+). Files go to and from the Vita over the same
cable, without VitaShell, even while a game runs:

```bash
./linux/vitactl.py ls ux0:data
./linux/vitactl.py push myfile.txt ux0:data/          # into a folder
./linux/vitactl.py pull ur0:data/VitaSync_log.txt .
./linux/vitactl.py push -r myfolder ux0:data/         # a whole folder
./linux/vitactl.py mkdir|rm|rmdir|mv|stat ...
```

The Vita's own filesystem does every read and write (the card is never
shared with the PC as a disk). Only `ux0:`, `ur0:`, `uma0:`, `imc0:`, `xmc0:`
and `grw0:` are served; the plugin refuses the system partitions. Deleting or
replacing a file asks first (`[y/N]`; in scripts, `--yes`), `rmdir` only
deletes empty folders, and `-r` never replaces anything. A pushed file is
written under a temporary name and renamed at the end, so an interrupted push
never leaves a half file in its place. Measured on 2026-11-26: push 10.5 MB/s,
pull 15 MB/s, and the video keeps running (about 47 frames/s at 960x544 during a
push). Dates are shown as the Vita reports them, in UTC.

Opening VitaShell's USB transfer takes USB away from the plugin; it takes USB
back by itself about 5 s after the transfer is closed.

[TESTING.md](TESTING.md) is a test scenario that checks video, sound, input and
VitaShell's FTP together.

`.claude/skills/vita-testing/` is a Claude Code skill that tests a homebrew build on the
Vita, with or without `vitactl.py`. Install it by copying the folder to
`~/.claude/skills/`; each project describes itself in `.claude/vita-testing.md`.

> **Known issue:** up to build 110, opening VitaShell's USB transfer after a
> video capture froze the Vita until it was forced off. Since build 111 the
> plugin steps aside cleanly before VitaShell takes USB, and the card mounts in
> about 1 s again (it took 11 s with the plugin before). Twice, on 2026-11-25,
> VitaShell still hung for about 30 s after input had been sent into it during a
> capture; PS gets you out. If that happens while closing USB transfer, the
> Vita doesn't re-read the card: reboot before changing anything on `ux0:`.
> Details in [TODO.md](TODO.md).

## ⚠️ Warning

Prototype not tested as an official release. Test it on a real Vita before use.
The plugin loads in kernel — risk of hang (fallback: reboot the Vita while
holding **L** to skip plugins).