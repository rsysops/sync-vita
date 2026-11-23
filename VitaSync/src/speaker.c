/*
 * The Vita's speaker during sound capture.
 *
 * Readback (audio.c) sends the final mix to RAM instead of to the speaker or
 * headphones. To keep them playing, each captured block is copied to the
 * active local output ourselves: a DMA from our own buffer to that output's
 * I2S FIFO, which takes the data at the playback rate.
 *
 * The technique and the values below (I2S register addresses, DMA request
 * values, where SceAudio keeps the active output, SceAudio's module NIDs
 * for firmwares 3.60 to 3.74) come from vitastream by devnoname120
 * (github.com/devnoname120/vitastream, src/audio_relay.c, src/firmware.c,
 * include/firmware_profiles.h), which confirmed it on a retail 3.60 Vita.
 *
 * Nothing is touched unless SceAudio is one of those known builds and its
 * own copy of the I2S register table is where we expect it; otherwise the
 * capture works as before, with the speaker muted.
 */
#include <psp2kern/kernel/dmac.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>
#include <stdint.h>
#include <string.h>
#include "speaker.h"

/* taiHEN's module utilities (libtaihenModuleUtils_stub); not declared in
 * vitasdk's headers. */
int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset,
		      uintptr_t *addr);

/* The firmware takes the timeout by value; vitasdk declares a pointer. Same
 * import, with the signature the firmware expects. */
int speaker_dma_sync(SceKernelDmaOpId op, int mode, SceUInt32 timeout_us,
		     SceKernelDmaOpTag **error_tag) __asm__("ksceKernelDmaOpSync");

#define BLOCK_BYTES	2048u	/* 512 stereo 16-bit frames */
#define TAIL_BYTES	128u	/* the last burst, sent with the end flag */
#define BUF_SIZE	4096u
#define DMA_CHANNEL	15u	/* DMAC4; SceAudio uses channels 0-8 */
#define WRITE_TIMEOUT_US 100000u

/* SceAudio, the same on every known firmware. */
#define AUDIO_TEXT_SIZE		0xD1D4u
#define AUDIO_DATA_SIZE		0xBD24u
#define AUDIO_I2S_TABLE		0x7FD8u	/* in text: SceAudio's copy of i2s_phys */
#define AUDIO_SINK_ENABLED	0xBD21u	/* in data: bitmask of usable outputs */
#define AUDIO_SINK_INDEX	0xBD22u	/* in data: the output in use */

/* Physical I2S register blocks by output index. 0-2 are capture ports,
 * 6 has none. */
static const uint32_t i2s_phys[8] = {
	0xE0420000u, 0xE0430000u, 0xE0440000u, 0xE0450000u,
	0xE0470000u, 0xE0460000u, 0, 0xE0490000u
};

static const uint32_t audio_nids[] = {
	0x0DA7714Au,	/* 3.60 */
	0x5FA65AB0u,	/* 3.61 */
	0xB18B03DEu,	/* 3.63 */
	0xBA583E43u,	/* 3.65 */
	0x0086EFADu,	/* 3.65, internal build */
	0xFE1A2BC1u,	/* 3.67 */
	0x596C0CAAu,	/* 3.68 */
	0xCDD1D342u,	/* 3.69 */
	0x914AA642u,	/* 3.70 */
	0x9439ED79u,	/* 3.71 */
	0xDEFAF2FBu,	/* 3.72 */
	0x0802F6F9u,	/* 3.73 */
	0x0BF11C75u,	/* 3.74 */
};

static int checked, supported;
static const volatile unsigned char *audio_data;	/* SceAudio's data segment */

static SceUID buf_uid = -1;
static unsigned char *buf;
static SceKernelDmaOpId op = -1;
static int pending;		/* our chain may still be running */
static volatile int state;	/* SPEAKER_* */
static SceKernelDmaOpTag tags[2] __attribute__((aligned(64)));

static int check_firmware(void)
{
	tai_module_info_t info;
	uintptr_t text, data, end;
	unsigned int i;

	if (checked)
		return supported;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceAudio", &info) < 0)
		return 0;	/* not loaded yet: ask again next time */
	checked = 1;
	for (i = 0; i < sizeof(audio_nids) / sizeof(audio_nids[0]); i++)
		if (info.module_nid == audio_nids[i])
			break;
	if (i == sizeof(audio_nids) / sizeof(audio_nids[0]))
		return 0;
	/* Both segments must be at least as long as on the known builds. */
	if (module_get_offset(KERNEL_PID, info.modid, 0, 0, &text) < 0 ||
	    module_get_offset(KERNEL_PID, info.modid, 0, AUDIO_TEXT_SIZE, &end) < 0 ||
	    module_get_offset(KERNEL_PID, info.modid, 1, 0, &data) < 0 ||
	    module_get_offset(KERNEL_PID, info.modid, 1, AUDIO_DATA_SIZE, &end) < 0 ||
	    !text || !data)
		return 0;
	if (memcmp((const void *)(text + AUDIO_I2S_TABLE), i2s_phys, sizeof(i2s_phys)) != 0)
		return 0;
	audio_data = (const volatile unsigned char *)data;
	supported = 1;
	return 1;
}

/* The output SceAudio plays to right now (speaker or headphones). */
static int active_output(uint32_t *fifo, uint32_t *request)
{
	unsigned int index = audio_data[AUDIO_SINK_INDEX];
	unsigned int enabled = audio_data[AUDIO_SINK_ENABLED];

	if (index < 3 || index >= 8 || !i2s_phys[index] || !(enabled & (1u << index)))
		return -1;
	*fifo = i2s_phys[index] + 0x100u;
	*request = index == 4 ? 0x00900000u : 0x00A00000u;
	return 0;
}

static void release(void)
{
	if (op >= 0 && pending) {
		ksceKernelDmaOpDeQueue(op);
		if (ksceKernelDmaOpQuit(op) < 0)
			return;	/* still running: keep the op and the buffer */
		pending = 0;
	}
	if (op >= 0) {
		ksceKernelDmaOpFree(op);
		op = -1;
	}
	if (buf_uid >= 0) {
		ksceKernelFreeMemBlock(buf_uid);
		buf_uid = -1;
		buf = NULL;
	}
}

int speaker_start(void)
{
	SceKernelAllocMemBlockKernelOpt opt;
	int ret;

	release();
	if (pending)
		goto failed;
	if (!check_firmware()) {
		state = SPEAKER_UNSUPPORTED;
		return -1;
	}
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
		   SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = BUF_SIZE;
	buf_uid = ksceKernelAllocMemBlock("vitasync_speaker",
					  SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW,
					  BUF_SIZE, &opt);
	if (buf_uid < 0)
		goto failed;
	if (ksceKernelGetMemBlockBase(buf_uid, (void **)&buf) < 0)
		goto failed;
	op = ksceKernelDmaOpAlloc("vitasync_speaker");
	if (op < 0)
		goto failed;
	ret = ksceKernelDmaOpAssign(op, SCE_KERNEL_DMAC_ID_DMAC4, DMA_CHANNEL);
	if (ret < 0)
		goto failed;
	state = SPEAKER_PLAYING;
	return 0;

failed:
	release();
	state = SPEAKER_FAILED;
	return -1;
}

int speaker_write(const void *block)
{
	static const SceKernelDmaOpChainParam param = {
		.size = sizeof(SceKernelDmaOpChainParam),
		.coherencyMask = 0x0003FFFF,
		.setValue = 0
	};
	uint32_t fifo, request;
	int ret;

	if (!buf || op < 0 || pending)
		return -1;
	if (active_output(&fifo, &request) < 0)
		return 0;	/* no local output right now: nothing to play */
	/* Our own copy: the capture buffer is refilled while this plays. */
	memcpy(buf, block, BLOCK_BYTES);
	/* RAM (incrementing) to the I2S FIFO, paced by the output, in 128-byte
	 * bursts; the last burst carries the end flag. */
	tags[0] = (SceKernelDmaOpTag){
		.src = buf,
		.dst = (void *)fifo,
		.len = 0x50000000u | (BLOCK_BYTES - TAIL_BYTES),
		.cmd = request,
		.blockSize = 0x00800000u,
		.pNext = &tags[1]
	};
	tags[1] = (SceKernelDmaOpTag){
		.src = buf + BLOCK_BYTES - TAIL_BYTES,
		.dst = (void *)fifo,
		.len = 0x50000000u | TAIL_BYTES,
		.cmd = request | 0x2000u,
		.blockSize = 0x00800000u,
		.pNext = SCE_KERNEL_DMAC_CHAIN_END
	};
	ret = ksceKernelDmaOpSetupChain(op, tags, (SceKernelDmaOpChainParam *)&param,
					SCE_KERNEL_DMA_OP_VIRTUAL_SRC_ADDR);
	if (ret < 0)
		return ret;
	pending = 1;	/* from here, only release() may reuse the op */
	ret = ksceKernelDmaOpEnQueue(op);
	if (ret < 0)
		return ret;
	ret = speaker_dma_sync(op, SCE_KERNEL_DMA_OP_SYNC_TIMED_WAIT, WRITE_TIMEOUT_US, NULL);
	if (ret < 0)
		return ret;
	pending = 0;
	return 0;
}

void speaker_stop(void)
{
	release();
	if (state == SPEAKER_PLAYING)
		state = SPEAKER_OFF;
}

void speaker_fail(void)
{
	release();
	state = SPEAKER_FAILED;
}

int speaker_state(void)
{
	return state;
}
