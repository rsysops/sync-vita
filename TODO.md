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

Next: make a normal video stop clean the same way (check `stream` before
each submit, cancel and release the wait from the abort path) before
removing the 2 ms wait again. 103's crash came from cancelling on a timeout
in uvc_thread, during streaming; the handler cancels from another thread,
once nothing more is sent, which works.

## Video: the Vita won't sleep during a capture

With `watch` running, a short press on the power button doesn't put the
Vita to sleep: the PS button stays blue and the capture goes on after
unlocking (seen 2026-11-24 on build 109). The system refuses before sending
any sleep event, so the plugin's handler never runs. Not investigated;
likely the keep-awake ticks in uvc_thread (`ksceKernelPowerTick` every
10 s while streaming) or the USB traffic itself. Harmless: `watch stop`
first to put the Vita to sleep.

## VitaShell's USB transfer freezes the Vita after the PC used the HID interface

**The problem.** With the plugin running, opening VitaShell's USB transfer
(File Manager, SELECT) sometimes freezes the Vita; it crashes 30–60 s later.
The PC never sees VitaShell's USB device (`054c:05d3`): the plugin's device
(`054c:1337`) stays listed until the crash. So the freeze happens while
VitaShell takes USB away from the plugin, before it hands the card to the PC.

**Workaround (in place).** Don't open USB transfer in a boot where
`vitactl.py` has sent input or run `status`. Video alone (`watch`, `shot`) is
fine. To copy files in such a boot, use VitaShell's FTP, or reboot first.

### What we know (2026-11-21)

| Opening | Result | What the PC had done in that boot |
|---|---|---|
| 17:49, 17:53 (2nd of the boot, after a retake) | ok | video frame grabs only |
| 18:07 | ok | video frame grabs only |
| 18:53 | ok | `status` and inputs, but 30 min before; video running |
| 19:12 | freeze | `status`, inputs, PS, then `watch stop` |
| 19:14 | ok | nothing |
| 19:18 (2nd of the boot) | freeze | `watch start`, `shot`, `status` seconds before |
| 19:23 | ok | nothing |
| 19:28 | freeze | `status`, `watch start`, `shot`, `status`, `watch stop` |
| 19:34–19:37, 4 openings in one boot | ok | nothing (build 92: no retake) |
| 19:48 | freeze | `status`, 2 presses, video running (build 93: no heartbeat) |
| 20:01, 20:14 | freeze | same (build 94 debug, logging tests) |

Every freeze followed HID use (`status` or input commands, which open
`/dev/hidraw*`) by the PC in the same boot. 18:53 is the one exception.

### Ruled out

- **The plugin's retake** after USB transfer closes: first openings freeze too
  (19:12, 19:28, 19:48), and a second opening after a retake worked (17:53).
- **The heartbeat** (the only IN traffic on the HID interface): build 93 sent
  none and still froze.
- **A Linux MTP session:** `mtp-probe` reported "not an MTP device" each time;
  nothing on the PC talked MTP to the Vita.
- **Stopping MTP through the MTP service** (`sceMtpIfStopDriver`, NID
  `0xC0CFA3BC` in module `SceMtpIfDriver`, library `0x5EFA4138`): it refuses
  calls from kernel code with `0x80010058`, so it can't be used from the plugin.

### What to check next

1. **Video vs HID.** One boot with only `watch start`, 30 s of video,
   `watch stop`, no HID use, then open USB transfer. A freeze there clears the
   HID side and points at the video path (`uvc_handle_video_abort`,
   `send_frame` waiting on `uvc_frame_req_evflag` without a timeout).
2. **HID only.** One boot with `status` and a `press`, no video at all, then
   open USB transfer.
3. **Why the debug log goes quiet.** In builds 94 (`ur0:data/VitaSync_log.txt`,
   written by a logger thread with `ksceIoSyncByFd`), the log stops about 25 s
   after the PC connects, before any of our input or video traffic is recorded,
   though the plugin handled both. Find out why (logger thread blocked? file
   writes failing? `log_buf` full?) before relying on it to catch the freeze.
   The breadcrumbs (`BC()` in `main.c`, debug builds only) record each step of
   attach, detach, video abort, EP3 receives, EP0 requests and where
   `uvc_thread` is.
4. **Suspects in the plugin's USB shutdown path**, which runs inside
   VitaShell's `ksceUdcdStopCurrentInternal(2)`:
   - `uvc_handle_video_abort` calls `pc_hid_start_receive()` (queues a new EP3
     receive) while the driver is being stopped;
   - `usb_ep0_req_recv_on_complete` applies EP0 data without checking
     `req->returnCode`;
   - `uvc_frame_req_submit_phycont` waits for the frame transfer with no
     timeout.
5. **How VitaShell opens USB transfer** (`usb.c`, `modules/usbdevice/main.c`
   in TheOfficialFloW/VitaShell): `taiLoadStartKernelModule(usbdevice.skprx)`
   (its `module_start` calls `ksceUdcdStopCurrentInternal(2)`), then
   `sceMtpIfStopDriver(1)`, then `sceUsbstorVStorStart`. Find which of these
   never returns.

To fetch a debug log after a freeze: boot with **L** held when leaving henlo's
menu (so the plugin doesn't start a new log), copy `ur0:data/VitaSync_log.txt`
to `ux0:` in VitaShell, then open USB transfer.

##
* Rework readme (add a thank you section at the top to thank the original creator; ...)
* Try to fix USB file transfert after recording
