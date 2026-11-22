/*
 * USB audio stream: the Vita's sound to the PC (USB Audio Class 1.0).
 *
 * The PC opens the audio device by selecting alternate setting 1 of the
 * streaming interface; from then on it expects one 192-byte packet (48
 * stereo 16-bit frames) every millisecond on the isochronous IN endpoint.
 * A small queue of one-packet requests is kept in flight: each completion
 * (USB interrupt context) only marks its slot done and wakes the thread,
 * which refills and resubmits it.
 *
 * Step 1 sends silence. Step 2 fills the packets from the Vita's final mix.
 */
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/udcd.h>
#include <string.h>
#include "uac.h"
#include "audio.h"

#define QUEUE		8	/* one-millisecond requests in flight */
#define SLOT_STRIDE	256	/* 192 bytes, rounded to cache lines */

#define EV_WAKE		0x01
#define EV_DONE		0x02
#define EV_EXIT		0x04

#define STOP_WAIT_MS	200

static SceUdcdEndpoint *audio_ep;
static SceUID mem_uid = -1;
static unsigned char *slot_data;	/* QUEUE * SLOT_STRIDE, physically contiguous */
static SceUID ev_id = -1;
static SceUID thread_id = -1;

static SceUdcdDeviceRequest reqs[QUEUE];
static volatile unsigned int done_mask;		/* completed, not yet handled */
static volatile int in_flight;
static volatile int wanted;			/* the PC selected alternate 1 */
static volatile int streaming;			/* our requests are queued */
static volatile int exiting;

static volatile unsigned int stat_packets;	/* since the last heartbeat */
static volatile unsigned int stat_errors;	/* since start */

static void on_complete(SceUdcdDeviceRequest *req)
{
	int slot = req - reqs;

	if (slot < 0 || slot >= QUEUE)
		return;
	__atomic_fetch_or(&done_mask, 1u << slot, __ATOMIC_SEQ_CST);
	__atomic_fetch_sub(&in_flight, 1, __ATOMIC_SEQ_CST);
	ksceKernelSetEventFlag(ev_id, EV_DONE);
}

static int submit(int slot)
{
	unsigned char *data = slot_data + slot * SLOT_STRIDE;
	int ret;

	memset(data, 0, UAC_PACKET_BYTES);	/* step 1: silence */
	ksceKernelDcacheCleanRange(data, SLOT_STRIDE);

	reqs[slot] = (SceUdcdDeviceRequest){
		.endpoint = audio_ep,
		.data = data,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = UAC_PACKET_BYTES,
		.isControlRequest = 0,
		.onComplete = on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	__atomic_fetch_add(&in_flight, 1, __ATOMIC_SEQ_CST);
	ret = ksceUdcdReqSend(&reqs[slot]);
	if (ret < 0) {
		__atomic_fetch_sub(&in_flight, 1, __ATOMIC_SEQ_CST);
		stat_errors++;
	}
	return ret;
}

/* Cancel everything in flight and wait (bounded) for the completions. */
static void stop_stream(void)
{
	int ms;

	ksceUdcdReqCancelAll(audio_ep);
	for (ms = 0; ms < STOP_WAIT_MS &&
	     __atomic_load_n(&in_flight, __ATOMIC_SEQ_CST) > 0; ms++)
		ksceKernelDelayThread(1000);
	ksceUdcdClearFIFO(audio_ep);
	__atomic_store_n(&done_mask, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&in_flight, 0, __ATOMIC_SEQ_CST);
	streaming = 0;
}

static int audio_thread(SceSize args, void *argp)
{
	unsigned int idle = 0;	/* slots not in flight while streaming */
	int slot;

	(void)args;
	(void)argp;

	for (;;) {
		unsigned int bits = 0, done;
		SceUInt timeout = 10000;	/* retry idle slots every 10 ms */

		ksceKernelWaitEventFlag(ev_id, EV_WAKE | EV_DONE | EV_EXIT,
					SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
					&bits, &timeout);
		if (exiting)
			break;

		if (!wanted) {
			if (streaming)
				stop_stream();
			idle = 0;
			continue;
		}

		if (!streaming) {
			__atomic_store_n(&done_mask, 0, __ATOMIC_SEQ_CST);
			streaming = 1;
			idle = (1u << QUEUE) - 1;
		}

		done = __atomic_exchange_n(&done_mask, 0, __ATOMIC_SEQ_CST);
		for (slot = 0; slot < QUEUE; slot++) {
			if (!(done & (1u << slot)))
				continue;
			if (reqs[slot].returnCode < 0)
				stat_errors++;
			else
				stat_packets++;
			idle |= 1u << slot;
		}

		for (slot = 0; slot < QUEUE && wanted; slot++) {
			if ((idle & (1u << slot)) && submit(slot) >= 0)
				idle &= ~(1u << slot);
		}
	}

	if (streaming)
		stop_stream();
	return 0;
}

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
	mem_uid = ksceKernelAllocMemBlock("vitasync_audio", 0x10208006,
					  4 * 1024, &opt);
	if (mem_uid < 0)
		return mem_uid;
	ret = ksceKernelGetMemBlockBase(mem_uid, (void **)&slot_data);
	if (ret < 0)
		goto err_mem;

	ev_id = ksceKernelCreateEventFlag("vitasync_audio_ev", 0, 0, NULL);
	if (ev_id < 0) {
		ret = ev_id;
		goto err_mem;
	}

	thread_id = ksceKernelCreateThread("vitasync_audio", audio_thread,
					   0x3C, 0x1000, 0, 0x10000, 0);
	if (thread_id < 0) {
		ret = thread_id;
		goto err_ev;
	}
	ret = ksceKernelStartThread(thread_id, 0, NULL);
	if (ret < 0)
		goto err_thread;
	return 0;

err_thread:
	ksceKernelDeleteThread(thread_id);
	thread_id = -1;
err_ev:
	ksceKernelDeleteEventFlag(ev_id);
	ev_id = -1;
err_mem:
	ksceKernelFreeMemBlock(mem_uid);
	mem_uid = -1;
	slot_data = NULL;
	return ret;
}

void audio_fini(void)
{
	wanted = 0;
	if (thread_id >= 0) {
		exiting = 1;
		ksceKernelSetEventFlag(ev_id, EV_EXIT);
		ksceKernelWaitThreadEnd(thread_id, NULL, NULL);
		ksceKernelDeleteThread(thread_id);
		thread_id = -1;
	}
	if (ev_id >= 0) {
		ksceKernelDeleteEventFlag(ev_id);
		ev_id = -1;
	}
	if (mem_uid >= 0) {
		ksceKernelFreeMemBlock(mem_uid);
		mem_uid = -1;
		slot_data = NULL;
	}
}

void audio_set_streaming(int on)
{
	wanted = on ? 1 : 0;
	if (ev_id >= 0)
		ksceKernelSetEventFlag(ev_id, EV_WAKE);
}

void audio_heartbeat(unsigned char *state, unsigned char *packets_div4,
		     unsigned char *errors)
{
	unsigned int packets = __atomic_exchange_n(&stat_packets, 0,
						   __ATOMIC_SEQ_CST) / 4;
	unsigned int err = stat_errors;

	*state = (unsigned char)((wanted ? 1 : 0) | (streaming ? 2 : 0) |
				 (thread_id >= 0 ? 4 : 0));
	*packets_div4 = (unsigned char)(packets > 255 ? 255 : packets);
	*errors = (unsigned char)(err > 255 ? 255 : err);
}
