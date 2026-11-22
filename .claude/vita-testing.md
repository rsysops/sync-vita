# Testing VitaSync on the Vita

Project facts for the `vita-testing` skill.

## Connection

```bash
MOUNT=/media/ryan/352C-A00F        # ux0: over VitaShell's USB mode (lsusb 054c:05d3)
VITA_IP=192.168.1.241              # VitaShell's FTP server
FTP_PORT=1337
VITACTL=/home/ryan/Documents/Loisirs/Jeux/PSVita/Dev/sync-vita/linux/vitactl.py
```

## Folder on ux0:

`downloads/` (`ux0:downloads/`). Debug builds write their log to
`ur0:data/VitaSync_log.txt`, which USB transfer doesn't expose: the user
copies it to `ux0:downloads/` in VitaShell before opening USB transfer.

## Flavours

Default: **release**.

| flavour | build command | build output |
|---|---|---|
| release | `./build-vita.sh` | `VitaSync/VitaSync.skprx` |
| debug | `./build-vita.sh DEBUG=1` | `VitaSync/VitaSync.skprx` |

Both flavours write the same file, so the last build wins. There's no `.vpk`,
`.self`, title ID or app: VitaSync is a kernel plugin.

Both flavours print 4 warnings that are already in the code (a dropped `const`
and always-true checks in `main.c`, among others), plus unused-code warnings
from the disabled audio code. Only new warnings count. `build-vita.sh` fails
if the plugin imports the user-mode `SceLibKernel`.

## Destination

Copy the `.skprx` to `downloads/`. The user then:

1. moves it over `ur0:tai/VitaSync.skprx` in VitaShell
   (`config.txt` already loads it in `*KERNEL`);
2. powers the Vita off, powers it on, and runs henlo with "Mount sd2vita and
   exit".

The plugin takes over USB about 40 s after that (`lsusb`: `054c:1337`, video
on the `/dev/video*` named "PSVita", HID on a `/dev/hidraw*`). With the
release build, `vitactl.py status` shows the build number (105 as of
2026-11-21).

Opening VitaShell's USB transfer stops the plugin. Closing it brings the plugin
back by itself within a few seconds (`1337` again), with no reboot needed.

**Known issue (TODO.md):** once `vitactl.py` has sent input or run `status` in
a boot, opening USB transfer can freeze the Vita, which crashes 30–60 s later.
Video alone is fine. In such a boot, copy over VitaShell's FTP or reboot first.
After a freeze: hold power to force it off.

## Results

`RESULTS="VitaSync_log.txt"`, `DEST=/tmp/sync-vita-...`

- `VitaSync_log.txt` (debug flavour only), copied by the user from
  `ur0:data/` to `ux0:downloads/`: the plugin's startup and USB events. Each
  plugin load starts it afresh, so after a freeze the user boots with **L**
  held when leaving henlo's menu (no plugins), then copies it.

## Never touched

`ur0:tai/config.txt` and the other plugins in `ur0:tai/` (henkaku, taihen,
gamesd, PSVshell, kubridge). Nothing else on `ux0:` belongs to this project.

## What to test from

`~/Desktop/vitasync-handoff.md`, "How to check it works". The end-to-end
scenario (video, input, FTP): `TESTING.md`. For the USB transfer freeze:
`TODO.md`.

## Reading results

`VitaSync_log.txt`, in order:

- the build date and time (UTC);
- each `module_start` step with its return code. Return codes are hex:
  `0x00000000` means OK, and a hook or object UID (`0x0001xxxx`) also means
  success;
- `uvc_thread: started`, then the 15 s wait;
- each USB step in `uvc_start`. Stopping `USBPSPCommunicationDriver` and
  `USBSerDriver` returns `0x80243002` when they weren't running, which is
  normal. `uvc_start() = 0x00000000` means the plugin owns USB;
- `reference states`: the raw driver states recorded right after the boot
  takeover (`0x02` running, `0x01` stopped). Later `states:` lines are
  written whenever a state or the driver's start/stop callbacks change;
- `rear panel`: the rear touchpad's active area, read from the Vita;
- `retaking USB`, its steps, then `retake OK` or `retake failed`;
- events: `attach` and `detach`, `stream on` (with format, frame index and
  frame interval in 100 ns units) and `stream off`, `first HID report
  received`, `HID receive errors: N` (one per detach is normal), and
  `send_frame = <error>`;
- `bc:` lines, from the breadcrumb logger thread: `bc: ep0 ...` for each
  setup request from the PC, and a summary (attach/detach/video-abort steps,
  driver start/stop counts, EP3 receives, where `uvc_thread` is) on each
  change and every 5 s. As of build 94 these stop about 25 s after the PC
  connects; see TODO.md.

## Keeping captures

None: logs stay in `/tmp`.
