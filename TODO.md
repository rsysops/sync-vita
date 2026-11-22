# TODO

## Sound: what's left

Sound works since plugin build 101 (`src/audio.c`). Still to do:

- **Measure the sound/picture offset.** Both are stamped with the PC's clock,
  but the sound spends about 50 ms in the Vita's buffers (plugin ring, 8 ms
  USB request, 10.7 ms read block). Measure it, e.g. VOL+'s click against the
  volume bar appearing, in a quiet place, and correct it in `record`.
- **Long captures.** 3 minutes showed no drift problem; check 30 minutes or
  more (the ring skips or rebuffers, counted in `status`).
- **Keep the Vita awake while recording.** The plugin keeps it awake only
  while input flows; a recording made after a while without input can show
  the screen off (frozen picture, silence).

## Video: the 2 ms wait after each frame conversion

`uvc_frame_transfer`'s caller waits 2 ms after the IFTU conversion. The wait
came from the build that hid audio in the frames; its comment says the IFTU
DMA finishes after the call returns. Try without it (or with a shorter
wait), and measure the frame rate (`watch status`) and look for torn frames.

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
     `req->returnCode` or the frame index;
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
