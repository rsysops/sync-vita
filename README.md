# VitaSync — PS Vita streaming & control over USB

See the PS Vita's screen on a Linux PC **and** control the console (buttons,
sticks, front and rear touch, PS and volume) over USB, from the command line.

Prototype based on a modified fork of **`vita-udcd-uvc` 1.7** (`xerpi/vita-udcd-uvc`, MIT license).

## What's in it

- `VitaSync/` — Vita kernel plugin (`VitaSync.skprx`): UVC video + vendor HID
  USB interface. UVC device name: `PSVita`, USB ID `054c:1337`.
- `linux/` — `vitactl.py`, the command-line tool (screen, recordings, input),
  and the udev rule it needs.
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

1. Once, let your user send input and keep the Vita's USB awake:

   ```bash
   sudo cp linux/70-vitasync.rules /etc/udev/rules.d/
   sudo udevadm control --reload && sudo udevadm trigger
   ```

2. Start the video, then send input. Input also works without the video, but
   `shot` and the heartbeat in `status` need it, and the plugin releases all
   input when the stream stops.

   ```bash
   ./linux/vitactl.py watch start     # screen kept in $XDG_RUNTIME_DIR/vitactl/latest.png
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
                                         # with pictures of each in clip-analysis/
   ./linux/vitactl.py run steps.txt   # one command per line, plus "wait ms"
   ./linux/vitactl.py status          # devices, video, and what the plugin reports
   ./linux/vitactl.py watch stop
   ```

Opening VitaShell's USB transfer takes USB away from the plugin; it takes USB
back by itself a few seconds after the transfer is closed.

[TESTING.md](TESTING.md) is a test scenario that checks video, input and
VitaShell's FTP together.

`.claude/skills/vita-testing/` is a Claude Code skill that tests a homebrew build on the
Vita, with or without `vitactl.py`. Install it by copying the folder to
`~/.claude/skills/`; each project describes itself in `.claude/vita-testing.md`.

> **Known issue:** once `vitactl.py` has sent input or run `status` in a boot,
> opening VitaShell's USB transfer can freeze the Vita (it crashes 30–60 s
> later). Video alone is fine. In such a boot, copy files over VitaShell's FTP,
> or reboot before opening USB transfer. Details and leads in [TODO.md](TODO.md).

## ⚠️ Warning

Prototype not tested as an official release. Test it on a real Vita before use.
The plugin loads in kernel — risk of hang (fallback: reboot the Vita while
holding **L** to skip plugins).