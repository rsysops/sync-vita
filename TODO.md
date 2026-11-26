# TODO

## Sound: what's left

Sound works since plugin build 101 (`src/audio.c`); `record` corrects the
sound/picture offset (17 ms, `AUDIO_DELAY_S` in `vitactl.py`). Still to do:

- **Bluetooth headset breaks with sound capture** (since build 101; seen
  2026-11-23 on build 107). With a Bluetooth headset connected, the capture
  gets pure silence and the headset sounds distorted; after `watch stop` the
  headset sounds "monotonous", and once it's disconnected the speaker is
  distorted too, until a reboot. Likely cause: the Vita feeds Bluetooth by
  reading the mix in readback mode itself (vitastream's notes: HDMI uses
  mode 1, readback 2, local outputs 0), and `capture_stream()` always puts
  path 0 back to mode 0 at stop. Fix: read the mode before taking the route
  (vitastream reads SrcMix1's mode byte in SceAudio's data, 0xBB28; check
  its source first), give back that mode at stop, and don't capture at all
  when the mix is already in readback or HDMI mode (`status` says why).
  Then, optionally, hook SceAVConfig's set-path-mode during capture (as
  vitastream does) so a headset connected mid-capture gets its route at
  stop. Until then: no Bluetooth headset in a boot with sound capture.
- **Long captures.** 3 minutes showed no drift problem; check 30 minutes or
  more (the ring skips or rebuffers, counted in `status`).
- **Wake the Vita when the video starts.** A Vita already asleep when
  `watch start` runs records a frozen screen (sound capture wakes it since
  build 105; the video path doesn't).

## Video: frame rate, and uvc_thread stalling after a stop (both done)

`uvc_frame_transfer`'s caller waited 2 ms after each IFTU conversion (from
the build that hid audio in the frames) until build 117. Experimental builds
102-104 (2026-11-22, withdrawn; build 105 = 101's video path) found:

- **Without the wait** (102): 960x544 went from 47 to 52 frames/s, 896x504
  to 59.6, 864x488 to 59.8, and a fast scroll showed no torn frame, so the
  conversion is done when `ksceIftuCsc` returns. But a video start after a
  few stop/starts went unanswered (`probe control: -110`), the heartbeat
  stopped, and the Vita crashed about a minute later.
- **A 250 ms limit on the frame send's wait, cancelling the request on
  timeout from uvc_thread** (103): worse, a single stop led to a freeze and
  a crash.
- **uvc_thread stalls after a video stop** (fixed in build 115, see below;
  confirmed 2026-11-24 with probe
  build 108: the heartbeat stops the moment `watch stop` runs and comes
  back only on the next start, a reboot, or an unplug).
  `uvc_frame_req_submit_phycont` waits for its completion with no limit,
  and a frame queued just after the PC stops reading never completes. With
  the 2 ms wait this stays harmless (10 stop/starts in a row fine on 105),
  and the next start unblocks it. It also made a reboot hang and power the
  Vita off (until build 110, see below). And PC input does nothing in the
  meantime: `pc_control_apply` and the heartbeat run in uvc_thread, so after
  a `watch stop` a `press` is lost until the next `watch start` (seen
  2026-11-24 on build 110). Input in a boot where the video never ran works.

**Fixed in build 110: reboot after the video was used.** Before, with the
cable in, VitaDeploy > Reboot hung ~20 s and then powered the Vita off
whenever `watch` had run in that boot (running or stopped). A boot without
video, or a reboot with the cable pulled, was fine. Build 110 has a system
event handler (`sysevent_handler` in `main.c`): before a sleep or reboot it
stops the video, cancels the pending frame and releases uvc_thread. Build
108 found that a reboot and a sleep send the same events, so the handler
can't tell them apart. Notes: the cancel doesn't call the frame's
completion. Waiting or writing files in that handler blocks the shutdown
for good (a debug build that waited 500 ms there did). Debug builds can't
be used to study a shutdown: their logger goes quiet ~60 s after the PC
connects, and the shutdown then hangs too.

**Why a video stop is never seen (found 2026-11-25).** The video endpoint
is bulk, so Linux's uvcvideo stops a stream with CLEAR_FEATURE(ENDPOINT_HALT)
on EP 0x81, not SET_INTERFACE (`uvc_video_stop_streaming`). udcd handles that
standard request itself and never passes it to `uvc_udcd_process_request`,
so `uvc_handle_video_abort` doesn't run, `stream` stays 1 and the last frame
stays queued (probe 112: `stream 1 wait 1 pend 1` tens of seconds after
`watch stop`).

**Fixed in build 115: the stop is detected, input works after it.** While
the PC streams, uvcvideo keeps reads queued and a frame goes out in ~25 ms.
A watchdog thread (`uvc_watchdog_thread`, every 100 ms) treats a frame left
unread for 1 s (`UVC_STOP_TIMEOUT_MS`) as a stop: `stream = 0`, clear the
EP1 FIFO, `uvc_frame_release()` (cancel EP1, set the event flag), as the
abort path would. The cancel comes from that thread after `stream = 0`, as
in `sysevent_handler` and `usb_yield`; 103's crash came from cancelling in
uvc_thread while it kept streaming. PC input isn't released. Tested
2026-11-26: heartbeat and input right after `watch stop` (3 presses moved
VitaShell's cursor), 10 start/stops at ~47.7 frames/s with the heartbeat
back after each, 60 s recording with sound on a still screen with no false
stop (two 50 ms gaps), 480x272 at 60.1 frames/s, SELECT pressed from the PC
right after a stop (mount 1.04 s), reboot after video.

**Done in build 117: no more 2 ms wait** (`IFTU_WAIT_US` 0; 2000 brings
it back). Tested 2026-11-26: 960x544 53.1 frames/s (was 47), 896x504 60.0
(54), 864x488 60.0 (57), 480x272 60.1; no torn frame during LiveArea page
swipes; 20 start/stops alternating 960x544 and 864x488, half restarted at
once (inside the 1 s stop window), every start answered (102 failed
there), heartbeat back after each stop; alive and taking input 2 min later;
3 min with sound on a moving screen at 51.6 frames/s (ten 50 ms gaps);
SELECT from the PC right after a stop; reboot after video. 102's failure
was most likely the frame left queued after a stop, which a frame sent
with no pause always is; build 115 releases it.

## Video: the Vita won't sleep during a capture

With `watch` running, a short press on the power button doesn't put the
Vita to sleep: the PS button stays blue and the capture goes on after
unlocking (seen 2026-11-24 on build 109). The system refuses before sending
any sleep event, so the plugin's handler never runs. Not investigated;
likely the keep-awake ticks in uvc_thread (`ksceKernelPowerTick` every
10 s while streaming) or the USB traffic itself. Harmless: `watch stop`
first to put the Vita to sleep.

## VitaShell's USB transfer: fixed in build 111, one hang left

**Up to build 110.** With the plugin running, opening VitaShell's USB
transfer (VitaDeploy > File Manager, SELECT) froze the Vita whenever a video
capture had been started and stopped in that boot. It stayed frozen until
forced off (it crashed on its own only once or twice). The PC kept seeing
`054c:1337`. It was first blamed on HID use (table of 2026-11-21 in git
history); on 2026-11-25 a plain `watch start` + `watch stop` was enough. And
every USB transfer took ~15 s to mount instead of ~3 s.

**What VitaShell does** (`usb.c` and `modules/usbdevice/main.c` in
TheOfficialFloW/VitaShell; VitaDeploy just runs a stock VitaShell 2.02).
Opening: `sceAppMgrDestroyOtherApp`, load `usbdevice.skprx` (its
`module_start` calls `ksceUdcdStopCurrentInternal(2)`, which stops our
driver), `sceMtpIfStopDriver(1)` (returns `0x8054360C`, accepted),
`sceUsbstorVStorStart` (PC sees `054c:05d3`). Closing: `sceUsbstorVStorStop`,
`sceMtpIfStartDriver(1)` (PC sees `04e4`), unload the module, then remount
`ux0:` so the Vita sees what the PC wrote. `stopUsb()` returns early on any
error and then skips the remount. VitaShell locks PS only once USB transfer
is up.

**Causes and fixes (build 111).**

- **Freeze:** after `watch stop` the last frame stayed queued on EP1 with
  uvc_thread waiting on it (the stop wasn't seen until build 115, see
  "Video" above), and
  VitaShell's stop hung on it, as reboots did before build 110. The plugin
  now hooks `ksceUdcdStopCurrentInternal` (`SceUdcdForDriver` 0xBC05A8FB, NID
  0x0F3595AE) and steps aside first (`usb_yield`): stops video, sound and
  heartbeat, releases the frame, waits for uvc_thread (200 ms at most; a sound
  request stays queued until the driver stops, so the wait always runs out),
  cancels EP1-EP4, then answers nothing (`usb_yielded`) until `usb_retake`.
- **Same path:** the video abort queued the EP3 receive a second time while
  it was still queued (removed).
- **15 s mount, two causes found with usbmon and `lsusb -v` (2026-11-25):**
  our descriptor hook added the video IAD to every config descriptor,
  VitaShell's storage one included (now only ours: `confdesc_hi/full`), and
  udcd still calls our stopped driver, which took the storage request
  GET_MAX_LUN (0xA1 0xFE, interface 0) for a UVC one and answered nothing:
  the PC waited its 10 s timeout. Half of those mounts also logged `device
  descriptor read ... error -32`. Now 1.0 s, no error.
- **Build 113:** the retake waits 5 s of MTP running instead of 1.5 s, so
  VitaShell can unload its module and remount `ux0:` first.

Tested on 2026-11-25/26 (builds 111-113): capture then stop, recording with
sound, video and sound running, input only, 10 start/stops, reboot after
video, input into VitaShell during a capture (11 runs): no freeze but the
one below.

**Left: VitaShell hangs ~30 s.** Twice on 2026-11-25 (build 111), after
input sent into VitaShell during a capture then `watch stop`, pressing
SELECT left VitaShell stuck for ~30 s; the PC saw no change (so before our
driver stopped), then VitaShell came back without USB transfer. PS frees it
earlier, with its state intact. Once it hung while closing instead: the
remount was skipped, and files the PC had copied didn't show until a reboot
(writing to `ux0:` from the Vita in that state risks the card). Not seen
again in the 11 runs that followed (2026-11-25 evening and 2026-11-26, builds
111-114), including a replay of the boot that froze
(soft reboot after 10 start/stops, input on LiveArea, a USB transfer, then
the capture), on 111 and on the probes. Leads: something before
`StopCurrentInternal` in VitaShell's sequence (`sceAppMgrDestroyOtherApp`,
the module load), or a race with something on the PC side then (the PC has
rebooted since).

**If it comes back:** `./build-vita.sh PROBE=1` (build 114) logs each step
of VitaShell's `startUsb()`/`stopUsb()` (MtpIf, VStor, AppMgr syscalls),
every `ksceUdcdStart/Stop/Activate/Deactivate`, our yield and each change
the retake check sees, with durations, to `ur0:data/VitaSync_probe.txt`
(synced at once). Read it over VitaShell's FTP (`ur0:/data/`): USB transfer
doesn't expose `ur0:`. A first probe with a status thread and a hook on the
cable-state check (VitaShell polls it ~60 times a second while USB transfer
is open) never saw the hang either; the `*Internal` udcd exports are never
called, hook the public ones.

**Debug log** (`./build-vita.sh DEBUG=1`): it goes quiet ~25-60 s after the
PC connects (logger thread blocked? `log_buf` full?), so it can't catch a
hang; the probe writes its lines directly instead.

## Done in build 119-121: VitaSync's own file transfer over USB

File-level, not mass storage: vendor interface 5 with two bulk endpoints,
one kernel thread doing all the IO (`src/files.c`, protocol in
`include/files.h`), and `vitactl.py ls/stat/pull/push/mkdir/rm/rmdir/mv`
over usbdevfs. Allowed devices only (`ux0:` `ur0:` `uma0:` `imc0:` `xmc0:`
`grw0:`); PUT and RENAME refuse an existing target unless the request says
overwrite, and vitactl.py asks before any deletion or overwrite (`--yes`
without a terminal); nothing recursive deletes or overwrites. Notes:

- udcd numbers the endpoints itself (the descriptor's 0x85/0x06 came out as
  0x87 IN, 0x04 OUT; HID is 0x83/0x02, sound 0x85): vitactl.py reads them
  from `/sys/bus/usb/devices/*/*:1.5/ep_*`.
- Tested 2026-11-26: round trips at 0 B to 200 MiB including 511/512/513 and
  256 KiB +/-1 (sha256 identical), push 10.5 MB/s, pull 15 MB/s, 50 MiB push
  during video+sound (video 47 frames/s, no gap over 34 ms), refusals (system
  partitions, overwrite without the bit, non-empty rmdir), VitaShell's USB
  transfer opened mid-pull (no freeze, retake, next command fine), a push
  interrupted with Ctrl-C (no leftover; the next command waits ~6 s while the
  plugin drops the push: 5 s without data, build 121), and build 121 itself
  installed into `ur0:tai/` with `push --yes` then booted.
- Dates are the Vita's, in UTC.

Ideas: show the PC's local time; `push`/`pull` resuming at an offset (GET
already takes one).

## Small things

* Rework readme (add a thank you section at the top to thank the original creator; ...)
* The USB product ID `0x1337` comes from vita-udcd-uvc (`UVC_USB_PID`); it
  could become VitaSync's own (any value Sony doesn't use, e.g. `0x5653`).
  Needs `vitactl.py` (`find_hidraw`), `linux/70-vitasync.rules` (reinstall)
  and the docs.
