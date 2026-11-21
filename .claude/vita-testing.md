# Testing VitaSync on the Vita

Project facts for the `vita-testing` skill.

## Folder on ux0:

`dump/` (`ux0:dump/`), where debug builds write their log.

## Flavours

Default: **debug**.

| flavour | build command | build output |
|---|---|---|
| release | `./build-vita.sh` | `VitaSync/VitaSync.skprx` |
| debug | `./build-vita.sh DEBUG=1` | `VitaSync/VitaSync.skprx` |

Both flavours write the same file, so the last build wins. There's no `.vpk`,
`.self`, title ID or app: VitaSync is a kernel plugin.

Building the release flavour prints 4 warnings that are already in the code
(a dropped `const` and an always-true check in `main.c`, among others), plus
unused-code warnings from the disabled audio code. Only new warnings count.

## Destination

Copy the `.skprx` to `downloads/`. The user then:

1. moves it over `ur0:tai/VitaSync.skprx` in VitaShell
   (`config.txt` already loads it in `*KERNEL`);
2. powers the Vita off, powers it on, and runs henlo with "Mount sd2vita and
   exit".

The plugin takes over USB about 40 s after that (`lsusb`: `054c:1337`, video
on the `/dev/video*` named "PSVita", HID on a `/dev/hidraw*`).

Opening VitaShell's USB mode stops the plugin. Closing it brings the plugin
back by itself within a few seconds (`1337` again), with no reboot needed.
Builds without this (before 2026-11-21 evening) needed a reboot and henlo.

## Results

`RESULTS="VitaSync_log.txt"`, `DEST=/tmp/sync-vita-...`

- `VitaSync_log.txt` (debug flavour only): the plugin's startup and USB events.
  It's written from scratch each time the plugin loads, so it only covers the
  latest load.

## Never touched

`ur0:tai/config.txt` and the other plugins in `ur0:tai/` (henkaku, taihen,
gamesd, PSVshell, kubridge). Nothing else on `ux0:` belongs to this project.

## What to test from

`~/Desktop/vitasync-handoff.md`, "How to check it works".

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
  takeover. Later `states:` lines are compared with these, and are written
  whenever a state or the driver's start/stop callbacks change;
- `retaking USB`, its steps, then `retake OK` or `retake failed`;
- events: `attach` and `detach`, `stream on` (with format, frame index and
  frame interval in 100 ns units) and `stream off`, `first HID report
  received`, `HID receive errors: N` (one per detach is normal), and
  `send_frame = <error>`.

Lines produced while USB mode is open are held in memory and written after it
closes, so the card is never written while the PC has it mounted.

## Keeping captures

None: logs stay in `/tmp`.
