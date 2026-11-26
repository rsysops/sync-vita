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

## Video: faster frames, and uvc_thread stalling after a stop

`uvc_frame_transfer`'s caller waits 2 ms after each IFTU conversion (from
the build that hid audio in the frames). Experimental builds 102-104
(2026-11-22, withdrawn; build 105 = 101's video path) found:

- **Without the wait** (102): 960x544 went from 47 to 52 frames/s, 896x504
  to 59.6, 864x488 to 59.8, and a fast scroll showed no torn frame, so the
  conversion is done when `ksceIftuCsc` returns. But a video start after a
  few stop/starts went unanswered (`probe control: -110`), the heartbeat
  stopped, and the Vita crashed about a minute later.
- **A 250 ms limit on the frame send's wait, cancelling the request on
  timeout from uvc_thread** (103): worse, a single stop led to a freeze and
  a crash.
- **uvc_thread stalls after a video stop** (confirmed 2026-11-24 with probe
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
`watch stop`). Build 111 added the release (`uvc_frame_release`: cancel EP1,
set the event flag) to the abort path, which therefore doesn't help here,
and to the USB transfer yield, which does.

Next: detect the stop another way, then remove the 2 ms wait again. Idea: a
frame unread for over ~0.5 s means the PC stopped reading; release it from
another thread than uvc_thread (as `sysevent_handler` and `usb_yield` do;
103's crash came from cancelling in uvc_thread during streaming). That would
also bring input back after `watch stop`.

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

- **Freeze:** after `watch stop` the last frame stays queued on EP1 with
  uvc_thread waiting on it (the stop is never seen, see "Video" above), and
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

## Next: VitaSync's own file transfer over USB

File-level, not mass storage (the PC and the Vita would both write the same
exFAT): a vendor bulk interface next to video, HID and sound, a plugin
thread doing `ksceIoOpen/Read/Write`, and `vitactl.py ls/push/pull/rm` over
usbdevfs (standard library only). It would work while a game runs, without
VitaShell, and the vita-testing skill could deploy builds and fetch logs
with no steps from the user. Needs its own plan.

## Small things

* Rework readme (add a thank you section at the top to thank the original creator; ...)
* The USB product ID `0x1337` comes from vita-udcd-uvc (`UVC_USB_PID`); it
  could become VitaSync's own (any value Sony doesn't use, e.g. `0x5653`).
  Needs `vitactl.py` (`find_hidraw`), `linux/70-vitasync.rules` (reinstall)
  and the docs.
