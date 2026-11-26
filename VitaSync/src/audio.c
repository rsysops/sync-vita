/*
 * USB audio stream: the Vita's sound to the PC (USB Audio Class 1.0).
 *
 * The PC opens the audio device by selecting alternate setting 1 of the
 * streaming interface; from then on it polls the isochronous IN endpoint
 * every millisecond for one 192-byte packet (48 stereo 16-bit frames).
 * Two threads run while it does:
 *
 * - the capture thread switches the Vita's final mix (SrcMix1 main, path 0)
 *   to "readback" with the kernel's monitor functions and reads it in blocks
 *   of 512 frames into a ring. Readback takes the mix away from the speaker
 *   and headphones, so the thread also plays each block on them itself
 *   (speaker.c); on an unknown firmware the Vita is silent until the PC
 *   closes the device and the normal route is restored.
 * - the USB thread keeps exactly ONE request in flight, carrying 8 packets
 *   (8 ms), refilled from the ring and resubmitted on each completion.
 *   Queueing several requests ahead made SceUdcd fail about half of them,
 *   with code -5, from the second stream after boot on (builds 97-100);
 *   psvita-usb-audio-midi reports the same and also keeps one in flight.
 *
 * Packets are always 48 frames. The drift between the Vita's audio clock
 * and USB's is absorbed by the ring: it skips ahead when it runs too full
 * and rebuffers when it runs dry (both counted).
 *
 * The monitor functions are documented in vitasdk (psp2kern/audioout.h,
 * library SceAudioForDriver). They are looked up when a stream starts, not
 * imported: SceAudio may load after this plugin.
 */
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/udcd.h>
#include <taihen.h>
#include <stdint.h>
#include <string.h>
#include "uac.h"
#include "audio.h"
#include "speaker.h"

/* ---- USB side ---- */
#define REQ_PACKETS	8			/* packets per request (8 ms) */
#define REQ_FRAMES	(REQ_PACKETS * UAC_PACKET_FRAMES)
#define REQ_BYTES	(REQ_FRAMES * UAC_FRAME_BYTES)	/* 1536, cache aligned */

/* ---- capture side ---- */
#define CAPTURE_FRAMES	512	/* per monitor read (~10.7 ms) */
#define SETTLE_BLOCKS	2	/* blocks dropped after the route changes */
#define MAX_RECOVERIES	4	/* route resets survived per stream */
#define RING_FRAMES	8192	/* ~170 ms */
#define RING_MASK	(RING_FRAMES - 1)
#define RING_TARGET	1536	/* ~32 ms kept queued */
#define RING_HIGH	(RING_TARGET + 2048)	/* above this, skip back to target */

#define MONITOR_PATH		0	/* SrcMix1 main output */
#define MONITOR_MODE_NORMAL	0
#define MONITOR_MODE_READBACK	2
#define MONITOR_STEREO		2
#define MONITOR_NOT_READBACK	(-2)	/* Read: the route was reset */

#define SCE_AUDIO_FOR_DRIVER	0x15D711C1
#define NID_SET_PATH_MODE	0xAC9383D0
#define NID_SET_SAMPLE_RATE	0x134C96C1
#define NID_SET_CHANNEL_MODE	0xFC375BAC
#define NID_MONITOR_READ	0x81EB0AE5

#define EV_WAKE		0x01
#define EV_DONE		0x02
#define EV_EXIT		0x04

#define STOP_WAIT_MS	500	/* module_stop: wait for a cancelled request */
#define AWAKE_TICK_US	(10 * 1000 * 1000)
#define START_WAIT_MS	1000	/* stream start: wait for a cancelled request */

/* State bits (heartbeat). */
#define ST_WANTED	0x01
#define ST_STREAMING	0x02
#define ST_READY	0x04
#define ST_CAPTURING	0x08
#define ST_RECOVERED	0x10
#define ST_NO_MONITOR	0x20
#define ST_FAILED	0x40
#define ST_STALE	0x80	/* a stream had to start with an old request queued */

typedef short frame_t[UAC_CHANNELS];

/* One physically contiguous block: DMA targets for capture and USB. */
struct audio_mem {
	frame_t capture[2][CAPTURE_FRAMES];
	frame_t ring[RING_FRAMES];
	unsigned char req_buf[2][REQ_BYTES];	/* alternate between requests */
};
#define MEM_SIZE	((sizeof(struct audio_mem) + 0xFFF) & ~0xFFF)

/* taiHEN's module utilities (libtaihenModuleUtils_stub); not declared in
 * vitasdk's headers. */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
			   uint32_t funcnid, uintptr_t *func);

typedef int (*set_path_mode_fn)(int path, int mode);
typedef int (*set_int_fn)(int value);
typedef int (*monitor_read_fn)(int path, void *pcm, int frames);

static set_path_mode_fn mon_set_path_mode;
static set_int_fn mon_set_sample_rate;
static set_int_fn mon_set_channel_mode;
static monitor_read_fn mon_read;

static SceUdcdEndpoint *audio_ep;
static SceUID mem_uid = -1;
static struct audio_mem *mem;
static SceUID ev_id = -1;		/* USB thread */
static SceUID cap_ev_id = -1;		/* capture thread */
static SceUID thread_id = -1;
static SceUID cap_thread_id = -1;

static SceUdcdDeviceRequest req;
static volatile int owned;		/* SceUdcd holds req */
static volatile int req_done;		/* req completed, not yet handled */
static volatile int wanted;		/* the PC selected alternate 1 */
static volatile int streaming;		/* our requests go out */
static volatile int exiting;
static volatile unsigned int state;	/* ST_CAPTURING and the sticky bits */

/* Ring: the capture thread writes, the USB thread reads. Free-running
 * indices; the writer publishes ring_w after the data, the reader ring_r. */
static volatile unsigned int ring_w, ring_r;
static int rebuffering = 1;		/* USB thread only */

/* Counted per heartbeat window (~500 ms). */
static volatile unsigned int stat_packets;
static volatile unsigned int stat_errors;	/* failed requests */
static volatile unsigned int stat_xruns;
static volatile int last_error;			/* last failure code */

static void count(volatile unsigned int *c, unsigned int n)
{
	__atomic_fetch_add(c, n, __ATOMIC_RELAXED);
}

/* ---- ring ---- */

static void ring_push(const frame_t *src, unsigned int n)
{
	unsigned int w = ring_w;
	unsigned int r = __atomic_load_n(&ring_r, __ATOMIC_ACQUIRE);
	unsigned int first;

	if (w - r + n > RING_FRAMES) {	/* the PC isn't reading */
		count(&stat_xruns, 1);
		return;
	}
	first = RING_FRAMES - (w & RING_MASK);
	if (first > n)
		first = n;
	memcpy(mem->ring[w & RING_MASK], src, first * sizeof(frame_t));
	if (n > first)
		memcpy(mem->ring[0], src + first, (n - first) * sizeof(frame_t));
	__atomic_store_n(&ring_w, w + n, __ATOMIC_RELEASE);
}

/* Fill one request's worth of frames (silence while buffering). */
static void ring_pop_request(unsigned char *dst)
{
	unsigned int r = ring_r;
	unsigned int used = __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE) - r;
	unsigned int first;

	if (rebuffering) {
		if (used < RING_TARGET)
			goto silence;
		rebuffering = 0;
	}
	if (used < REQ_FRAMES) {	/* the Vita's clock is a bit slow */
		count(&stat_xruns, 1);
		rebuffering = 1;
		goto silence;
	}
	if (used > RING_HIGH) {		/* a bit fast: skip, keep latency low */
		count(&stat_xruns, 1);
		r += used - RING_TARGET;
	}
	first = RING_FRAMES - (r & RING_MASK);
	if (first > REQ_FRAMES)
		first = REQ_FRAMES;
	memcpy(dst, mem->ring[r & RING_MASK], first * sizeof(frame_t));
	if (first < REQ_FRAMES)
		memcpy(dst + first * sizeof(frame_t), mem->ring[0],
		       (REQ_FRAMES - first) * sizeof(frame_t));
	__atomic_store_n(&ring_r, r + REQ_FRAMES, __ATOMIC_RELEASE);
	return;

silence:
	memset(dst, 0, REQ_BYTES);
}

/* ---- USB thread ---- */

static void on_complete(SceUdcdDeviceRequest *r)
{
	(void)r;
	__atomic_store_n(&req_done, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&owned, 0, __ATOMIC_SEQ_CST);
	ksceKernelSetEventFlag(ev_id, EV_DONE);
}

static int submit(unsigned char *data)
{
	int ret;

	ksceKernelDcacheCleanRange(data, REQ_BYTES);
	req = (SceUdcdDeviceRequest){
		.endpoint = audio_ep,
		.data = data,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = REQ_BYTES,
		.isControlRequest = 0,
		.onComplete = on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};
	__atomic_store_n(&owned, 1, __ATOMIC_SEQ_CST);
	ret = ksceUdcdReqSend(&req);
	if (ret < 0) {
		__atomic_store_n(&owned, 0, __ATOMIC_SEQ_CST);
		count(&stat_errors, 1);
		last_error = ret;
	}
	return ret;
}

/*
 * A stream only works if it starts on a clean endpoint, with no request
 * left from an earlier stream. When the PC closes the device the request is
 * cancelled; when it opens it again, the stream starts once that request
 * has come back, after clearing the endpoint. If that takes over a second,
 * it starts anyway and says so (ST_STALE).
 */
static int usb_thread(SceSize args, void *argp)
{
	SceUInt64 wait_since = 0;	/* start: waiting for a leftover since */
	SceUInt64 next_awake = 0;	/* next keep-awake tick */
	int buf = 0;			/* req_buf to fill next */
	int resend = 0;			/* last submission failed: same data */

	(void)args;
	(void)argp;

	for (;;) {
		unsigned int bits = 0;
		SceUInt timeout = 10000;	/* retries and start waits */

		ksceKernelWaitEventFlag(ev_id, EV_WAKE | EV_DONE | EV_EXIT,
					SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
					&bits, &timeout);
		if (exiting)
			break;

		if (__atomic_exchange_n(&req_done, 0, __ATOMIC_SEQ_CST) && streaming) {
			if (req.returnCode >= 0) {
				count(&stat_packets, REQ_PACKETS);
			} else {
				count(&stat_errors, 1);
				last_error = req.returnCode;
			}
		}

		if (!wanted) {
			if (streaming) {
				ksceUdcdReqCancelAll(audio_ep);
				streaming = 0;
			}
			wait_since = 0;
			continue;
		}

		if (!streaming) {
			SceUInt64 now = ksceKernelGetSystemTimeWide();

			if (!wait_since) {
				wait_since = now;
				if (__atomic_load_n(&owned, __ATOMIC_SEQ_CST))
					ksceUdcdReqCancelAll(audio_ep);
			}
			if (__atomic_load_n(&owned, __ATOMIC_SEQ_CST)) {
				if (now - wait_since < START_WAIT_MS * 1000ULL)
					continue;	/* the 10 ms timeout brings us back */
				__atomic_fetch_or(&state, ST_STALE, __ATOMIC_RELAXED);
			}
			ksceUdcdClearFIFO(audio_ep);
			/* Drop what an earlier stream left in the ring. */
			__atomic_store_n(&ring_r, __atomic_load_n(&ring_w, __ATOMIC_ACQUIRE),
					 __ATOMIC_RELEASE);
			rebuffering = 1;
			resend = 0;
			wait_since = 0;
			streaming = 1;
			/* Counts as activity: lights a screen that had gone dim
			 * or dark, so the capture isn't of a sleeping Vita. */
			ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
			next_awake = 0;
		}

		/* Keep the Vita awake while the PC captures its sound: cancel
		 * the power-save timers every 10 s. Done here and not in
		 * uvc_thread, which can stall after the video stops (TODO.md);
		 * uvc_thread does the same while the video streams. */
		{
			SceUInt64 now = ksceKernelGetSystemTimeWide();

			if (now >= next_awake) {
				ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
				ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
				ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
				next_awake = now + AWAKE_TICK_US;
			}
		}

		if (__atomic_load_n(&owned, __ATOMIC_SEQ_CST))
			continue;
		if (!resend) {
			buf ^= 1;
			ring_pop_request(mem->req_buf[buf]);
		}
		resend = submit(mem->req_buf[buf]) < 0;	/* keep its samples */
	}
	return 0;
}

/* ---- capture thread ---- */

static int resolve_monitor(void)
{
	if (mon_set_path_mode && mon_set_sample_rate && mon_set_channel_mode && mon_read)
		return 1;
	if (module_get_export_func(KERNEL_PID, "SceAudio", SCE_AUDIO_FOR_DRIVER,
				   NID_SET_PATH_MODE, (uintptr_t *)&mon_set_path_mode) < 0 ||
	    module_get_export_func(KERNEL_PID, "SceAudio", SCE_AUDIO_FOR_DRIVER,
				   NID_SET_SAMPLE_RATE, (uintptr_t *)&mon_set_sample_rate) < 0 ||
	    module_get_export_func(KERNEL_PID, "SceAudio", SCE_AUDIO_FOR_DRIVER,
				   NID_SET_CHANNEL_MODE, (uintptr_t *)&mon_set_channel_mode) < 0 ||
	    module_get_export_func(KERNEL_PID, "SceAudio", SCE_AUDIO_FOR_DRIVER,
				   NID_MONITOR_READ, (uintptr_t *)&mon_read) < 0) {
		mon_set_path_mode = NULL;
		mon_read = NULL;
		return 0;
	}
	return 1;
}

static int set_readback(void)
{
	int ret = mon_set_path_mode(MONITOR_PATH, MONITOR_MODE_READBACK);

	if (ret >= 0)
		ret = mon_set_sample_rate(UAC_RATE);
	if (ret >= 0)
		ret = mon_set_channel_mode(MONITOR_STEREO);
	return ret;
}

static int read_block(int which)
{
	ksceKernelDcacheInvalidateRange(mem->capture[which], sizeof(mem->capture[which]));
	return mon_read(MONITOR_PATH, mem->capture[which], CAPTURE_FRAMES);
}

/* Capture until the PC closes the device. */
static void capture_stream(void)
{
	int cur, next, ret, discard, recoveries = 0, started = 0;

	if (!resolve_monitor()) {
		__atomic_fetch_or(&state, ST_NO_MONITOR, __ATOMIC_RELAXED);
		return;		/* the USB thread streams silence */
	}
	if (set_readback() < 0)
		goto failed;

restart:
	speaker_start();	/* on failure the speaker stays muted, capture goes on */
	cur = 0;
	discard = SETTLE_BLOCKS;
	ret = read_block(cur);
	if (ret != 0)
		goto read_failed;
	started = 1;
	__atomic_fetch_or(&state, ST_CAPTURING, __ATOMIC_RELAXED);

	while (wanted && !exiting) {
		next = cur ^ 1;
		/* Queues buf[next]; returns once buf[cur] is full. */
		ret = read_block(next);
		if (ret != 0)
			goto read_failed;
		ksceKernelDcacheInvalidateRange(mem->capture[cur], sizeof(mem->capture[cur]));
		if (discard) {
			discard--;	/* Sony's queued blocks may still reach I2S */
		} else {
			ring_push((const frame_t *)mem->capture[cur], CAPTURE_FRAMES);
			/* Then the speaker: done before the next read, while the
			 * monitor fills buf[next]. */
			if (speaker_state() == SPEAKER_PLAYING &&
			    speaker_write(mem->capture[cur]) < 0)
				speaker_fail();	/* muted; the PC keeps its sound */
		}
		cur = next;
	}
	goto stop;

read_failed:
	/* Our transfers stop before the system's playback can resume. */
	speaker_stop();
	if (ret == MONITOR_NOT_READBACK && recoveries < MAX_RECOVERIES &&
	    wanted && !exiting) {
		/* The system put the normal route back: take it again. */
		recoveries++;
		__atomic_fetch_or(&state, ST_RECOVERED, __ATOMIC_RELAXED);
		started = 0;
		if (set_readback() >= 0)
			goto restart;
	}
failed:
	__atomic_fetch_or(&state, ST_FAILED, __ATOMIC_RELAXED);
stop:
	speaker_stop();		/* before the normal route comes back */
	__atomic_fetch_and(&state, ~ST_CAPTURING, __ATOMIC_RELAXED);
	if (started)
		mon_read(MONITOR_PATH, NULL, CAPTURE_FRAMES);	/* drain the queued block */
	mon_set_path_mode(MONITOR_PATH, MONITOR_MODE_NORMAL);
}

static int capture_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	for (;;) {
		unsigned int bits = 0;
		SceUInt timeout = 50000;

		ksceKernelWaitEventFlag(cap_ev_id, EV_WAKE | EV_EXIT,
					SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
					&bits, &timeout);
		if (exiting)
			break;
		if (!wanted)
			continue;

		__atomic_fetch_and(&state, ~(ST_RECOVERED | ST_NO_MONITOR | ST_FAILED),
				   __ATOMIC_RELAXED);	/* ST_STALE stays until reboot */
		capture_stream();
		/* After a failure or with no monitor, wait for the PC to close. */
		while (wanted && !exiting)
			ksceKernelDelayThread(20000);
	}
	return 0;
}

/* ---- interface ---- */

int audio_init(SceUdcdEndpoint *ep)
{
	SceKernelAllocMemBlockKernelOpt opt;
	int ret;

	audio_ep = ep;

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		   SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = 4 * 1024;
	mem_uid = ksceKernelAllocMemBlock("vitasync_audio", 0x10208006, MEM_SIZE, &opt);
	if (mem_uid < 0)
		return mem_uid;
	ret = ksceKernelGetMemBlockBase(mem_uid, (void **)&mem);
	if (ret < 0)
		goto err;

	ev_id = ksceKernelCreateEventFlag("vitasync_audio_ev", 0, 0, NULL);
	cap_ev_id = ksceKernelCreateEventFlag("vitasync_capture_ev", 0, 0, NULL);
	if (ev_id < 0 || cap_ev_id < 0) {
		ret = ev_id < 0 ? ev_id : cap_ev_id;
		goto err;
	}

	thread_id = ksceKernelCreateThread("vitasync_audio", usb_thread,
					   0x3C, 0x1000, 0, 0x10000, 0);
	cap_thread_id = ksceKernelCreateThread("vitasync_capture", capture_thread,
					       0x3A, 0x2000, 0, 0x10000, 0);
	if (thread_id < 0 || cap_thread_id < 0) {
		ret = thread_id < 0 ? thread_id : cap_thread_id;
		goto err;
	}
	ret = ksceKernelStartThread(thread_id, 0, NULL);
	if (ret < 0)
		goto err;
	ret = ksceKernelStartThread(cap_thread_id, 0, NULL);
	if (ret < 0)
		goto err;
	return 0;

err:
	audio_fini();
	return ret;
}

void audio_fini(void)
{
	wanted = 0;
	exiting = 1;
	if (ev_id >= 0)
		ksceKernelSetEventFlag(ev_id, EV_EXIT);
	if (cap_ev_id >= 0)
		ksceKernelSetEventFlag(cap_ev_id, EV_EXIT);
	if (audio_ep && __atomic_load_n(&owned, __ATOMIC_SEQ_CST)) {
		int ms;

		ksceUdcdReqCancelAll(audio_ep);
		for (ms = 0; ms < STOP_WAIT_MS &&
		     __atomic_load_n(&owned, __ATOMIC_SEQ_CST); ms++)
			ksceKernelDelayThread(1000);
	}
	if (cap_thread_id >= 0) {
		ksceKernelWaitThreadEnd(cap_thread_id, NULL, NULL);
		ksceKernelDeleteThread(cap_thread_id);
		cap_thread_id = -1;
	}
	if (thread_id >= 0) {
		ksceKernelWaitThreadEnd(thread_id, NULL, NULL);
		ksceKernelDeleteThread(thread_id);
		thread_id = -1;
	}
	if (ev_id >= 0) {
		ksceKernelDeleteEventFlag(ev_id);
		ev_id = -1;
	}
	if (cap_ev_id >= 0) {
		ksceKernelDeleteEventFlag(cap_ev_id);
		cap_ev_id = -1;
	}
	/* Never free memory SceUdcd may still send from. */
	if (mem_uid >= 0 && !__atomic_load_n(&owned, __ATOMIC_SEQ_CST)) {
		ksceKernelFreeMemBlock(mem_uid);
		mem_uid = -1;
		mem = NULL;
	}
}

void audio_set_streaming(int on)
{
	wanted = on ? 1 : 0;
	if (ev_id >= 0)
		ksceKernelSetEventFlag(ev_id, EV_WAKE);
	if (cap_ev_id >= 0)
		ksceKernelSetEventFlag(cap_ev_id, EV_WAKE);
}

int audio_idle(void)
{
	return !__atomic_load_n(&owned, __ATOMIC_SEQ_CST);
}

static unsigned char take(volatile unsigned int *c, unsigned int div, unsigned int max)
{
	unsigned int v = __atomic_exchange_n(c, 0, __ATOMIC_RELAXED) / div;

	return (unsigned char)(v > max ? max : v);
}

void audio_heartbeat(unsigned char *state_out, unsigned char *packets_div4,
		     unsigned char *errors, unsigned char *xruns,
		     unsigned char *last_error_low)
{
	*state_out = (unsigned char)((wanted ? ST_WANTED : 0) |
				     (streaming ? ST_STREAMING : 0) |
				     (thread_id >= 0 && cap_thread_id >= 0 ? ST_READY : 0) |
				     __atomic_load_n(&state, __ATOMIC_RELAXED));
	*packets_div4 = take(&stat_packets, 4, 255);
	*errors = take(&stat_errors, 1, 255);
	*xruns = take(&stat_xruns, 1, 15);
	*last_error_low = (unsigned char)(last_error & 0xFF);
}
