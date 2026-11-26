#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/udcd.h>
#include <psp2kern/display.h>
#include <psp2kern/appmgr.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/lowio/iftu.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <taihen.h>
#include <string.h>
#include <psp2/touch.h>
#include "usb_descriptors.h"
#include "uvc.h"
#include "audio.h"
#include "speaker.h"

/* Debug log (make DEBUG=1): lines are buffered and written to
 * ux0:dump/VitaSync_log.txt by DFLUSH(), or by DFLUSH_SAFE() once VitaShell's
 * USB transfer may be open (see dbg_flush_if_safe). Only call these from
 * module_start and uvc_thread, never from USB or hook callbacks. DCALL(expr) evaluates
 * expr, logs its return code and yields it; without DEBUG it is just expr. */
#ifdef DEBUG
#include "log.h"
#define DLOG(...)	LOG_TO_FILE(__VA_ARGS__)
#define DFLUSH()	log_flush()
#else
#define DLOG(...)	((void)0)
#define DFLUSH()	((void)0)
#endif
#define DCALL(expr)	({ int r__ = (int)(expr); \
			   DLOG(#expr " = 0x%08X\n", r__); r__; })

/* Heartbeat (report ID 1 to the PC every ~500 ms), read by vitactl.py
 * status. Build 93 turned it off to test whether it caused the USB transfer
 * freeze (see TODO.md): it froze anyway, so it is on again. */
#ifndef ENABLE_HEARTBEAT
#define ENABLE_HEARTBEAT 1
#endif

/* Breadcrumbs (DEBUG): USB callbacks and uvc_thread record where they are;
 * a logger thread writes every change to the log. Added to find where
 * VitaShell's USB transfer freezes the Vita after the PC used the HID
 * interface. No file IO here: callbacks only store numbers. */
#ifdef DEBUG
static volatile unsigned int bc_attach_step, bc_detach_step;
static volatile unsigned int bc_abort_n, bc_abort_step, bc_uvc_where;
static volatile unsigned int bc_ep3_done_n;
static volatile int bc_ep3_last_rc;
struct bc_ep0_req {
	unsigned char type, req;
	unsigned short value, index, length;
};
#define BC_EP0_RING 32
static struct bc_ep0_req bc_ep0_ring[BC_EP0_RING];
static volatile unsigned int bc_ep0_head;
#define BC(var, v)	((var) = (v))
#else
#define BC(var, v)	((void)0)
#endif

#define ALIGN(x, a)			(((x) + ((a) - 1)) & ~((a) - 1))
#define UNUSED(x)			((void)x)

#define UVC_DRIVER_NAME			"VITASYNC"
#define UVC_USB_PID			0x1337

#define MAX_UVC_VIDEO_FRAME_SIZE	VIDEO_FRAME_SIZE_NV12(960, 544)

#define UVC_PAYLOAD_HEADER_SIZE		12
#define UVC_PAYLOAD_SIZE(frame_size)	(UVC_PAYLOAD_HEADER_SIZE + (frame_size))
#define MAX_UVC_PAYLOAD_TRANSFER_SIZE	UVC_PAYLOAD_SIZE(MAX_UVC_VIDEO_FRAME_SIZE)

#define SCE_DISPLAY_PIXELFORMAT_BGRA5551 0x50000000

/* Standard HID class request, used by Windows to push an Output Report
 * over the control endpoint (EP0) instead of the interrupt OUT endpoint. */
#ifndef HID_SET_REPORT
#define HID_SET_REPORT 0x09
#endif

int ksceOledDisplayOn();
int ksceOledDisplayOff();
int ksceOledGetBrightness();
int ksceOledSetBrightness(int brightness);

int ksceLcdDisplayOn();
int ksceLcdDisplayOff();
int ksceLcdGetBrightness();
int ksceLcdSetBrightness(int brightness);

/* Copied from DolceSDK */
typedef struct SceIftuPlaneState_updated {
	SceIftuFrameBuf fb;
	unsigned int unk20;
	unsigned int unk24;
	unsigned int unk28;
	unsigned int src_w;
	unsigned int src_h;
	unsigned int dst_x;
	unsigned int dst_y;
	unsigned int src_x;
	unsigned int src_y;
	unsigned int crop_top;
	unsigned int crop_bot;
	unsigned int crop_left;
	unsigned int crop_right;
} SceIftuPlaneState_updated;

#define UVC_FRAME_PADDING_SIZE (16 - UVC_PAYLOAD_HEADER_SIZE)
struct uvc_frame {
	unsigned char padding[UVC_FRAME_PADDING_SIZE];
	unsigned char header[UVC_PAYLOAD_HEADER_SIZE];
	unsigned char data[];
} __attribute__((packed));

static const struct uvc_streaming_control uvc_probe_control_setting_default = {
	.bmHint				= 0,
	.bFormatIndex			= FORMAT_INDEX_UNCOMPRESSED_NV12,
	.bFrameIndex			= 1,
	.dwFrameInterval		= FPS_TO_INTERVAL(60),
	.wKeyFrameRate			= 0,
	.wPFrameRate			= 0,
	.wCompQuality			= 0,
	.wCompWindowSize		= 0,
	.wDelay				= 0,
	.dwMaxVideoFrameSize		= MAX_UVC_VIDEO_FRAME_SIZE,
	.dwMaxPayloadTransferSize	= MAX_UVC_PAYLOAD_TRANSFER_SIZE,
	.dwClockFrequency		= 0,
	.bmFramingInfo			= 0,
	.bPreferedVersion		= 1,
	.bMinVersion			= 0,
	.bMaxVersion			= 0,
};

static struct uvc_streaming_control uvc_probe_control_setting;

static struct {
	unsigned char buffer[64];
	SceUdcdEP0DeviceRequest ep0_req;
} pending_recv;

static SceUID uvc_thread_id;
static SceUID uvc_event_flag_id;
static int uvc_thread_run;
/* Set from USB callbacks and hooks, read by uvc_thread. */
static volatile int stream;
/* Set when something is about to take USB from us (VitaShell's USB
 * transfer, see usb_yield), cleared when we take it back (usb_retake).
 * While set, nothing of ours goes out on USB and our driver answers no
 * request. */
static volatile int usb_yielded;

static SceUID uvc_frame_buffer_uid = -1;
static struct uvc_frame *uvc_frame_buffer_addr;
SceUID uvc_frame_req_evflag;

#define PC_HID_INTERFACE CONTROL_HID_INTERFACE
#define PC_HID_EP_IN HID_EP_IN
#define PC_HID_EP_OUT HID_EP_OUT
#define PC_HID_REPORT_ID_OUTPUT HID_REPORT_ID_OUTPUT
#define PC_HID_REPORT_SIZE 64

static unsigned char pc_hid_report[PC_HID_REPORT_SIZE] __attribute__((aligned(64)));
static SceUdcdDeviceRequest pc_hid_recv_req;
static volatile int pc_control_active;
static volatile unsigned int pc_buttons;
/* Kernel buttons from report byte 11 (bit 0 PS, bit 1 VOL+, bit 2 VOL-),
 * as SceCtrl bits. They can only be emulated through the kernelButtons
 * mask, never the user one. */
static volatile unsigned int pc_kbuttons;
static volatile unsigned char pc_lx = 128, pc_ly = 128, pc_rx = 128, pc_ry = 128;
static volatile unsigned int pc_hid_recv_ok;
static volatile unsigned int pc_hid_recv_err;
static volatile int pc_ret_button_emu;
static volatile int pc_ret_analog_emu;

/* ---- Touch injection (PC -> Vita front screen and rear touchpad) ----
 * Up to two fingers per panel. Coordinates are raw panel units: the
 * front is 1920x1088; the rear's active area is read from the Vita
 * (rear_panel_info) and the PC maps onto it. */
#define TOUCHSCREEN_WIDTH  1920
#define TOUCHSCREEN_HEIGHT 1088
#define PC_TOUCH_PORTS     2	/* SCE_TOUCH_PORT_FRONT, SCE_TOUCH_PORT_BACK */
#define PC_TOUCH_FINGERS   2
#define PC_TOUCH_FORCE_DEFAULT 128

struct pc_finger {
	volatile unsigned char down;
	volatile unsigned char id;
	volatile unsigned short x, y;
};
static struct pc_finger pc_fingers[PC_TOUCH_PORTS][PC_TOUCH_FINGERS];
static volatile unsigned char pc_touch_force[PC_TOUCH_PORTS];
static unsigned char pc_touch_next_id;
static volatile unsigned char h_peek, h_peekR, h_read, h_readR, h_peek2, h_read2;
static volatile unsigned int touch_patch_hits;
/* Reads patched per panel since the last heartbeat: shows whether the
 * running app reads that panel at all. */
static volatile unsigned char touch_patched_reads[PC_TOUCH_PORTS];

static int pc_touch_any(int port)
{
	int f;

	for (f = 0; f < PC_TOUCH_FINGERS; f++)
		if (pc_fingers[port][f].down)
			return 1;
	return 0;
}

static void pc_touch_release_all(void)
{
	int p, f;

	for (p = 0; p < PC_TOUCH_PORTS; p++)
		for (f = 0; f < PC_TOUCH_FINGERS; f++)
			pc_fingers[p][f].down = 0;
}

/* ---- Exit game (remplace le bouton PS : �chap maintenu sur le PC) ----
 * Le PID du jeu est captur� dans patch_touch_data (seul endroit o� l'on
 * conna�t le processus APPELANT = le jeu au premier plan). Le kill est
 * TOUJOURS ex�cut� depuis uvc_thread, jamais depuis un rappel USB ou un
 * hook tactile : tuer son propre h�te depuis son contexte = plantage. */
static volatile SceUID game_pid = -1;      /* dernier poller tactile vu */
static volatile int exit_game_request;     /* commande PC re�ue (report[18]) */
static volatile int exit_armed = 1;        /* kill autoris� */
static volatile int exit_seen_idle = 1;    /* �chap revu rel�ch� depuis dernier kill */
static volatile unsigned int kill_result;  /* diag: 0=rien 1=ok 2=�chec 3=refus� */
static volatile unsigned char exit_req_seen;   /* diag: commande re�ue au moins 1x */
static volatile unsigned char exit_block;      /* diag: raison du blocage (voir thread) */
static unsigned int exit_cooldown;         /* boucles uvc_thread (~8 ms) */
/* S�quence de kill en phases (ex�cution diff�r�e, jamais dans un rappel) :
 * 1 = gel vid�o + rendu des �mulations, 2 = kill tir�, syst�me au repos,
 * 0 = repos, vid�o reprise. Le gel �vite de lire le framebuffer d'un
 * processus en cours de d�montage (DMA IFTU sur adresse lib�r�e = panic). */
static volatile int exit_phase;
static volatile SceUID exit_target_pid;
static unsigned int kill_hold;             /* boucles de gel vid�o restantes */

static volatile unsigned char touch_edge_history;
static volatile unsigned char last_read_count;
static volatile unsigned int rx_sequence;

/* Ground truth: last REAL touch seen flowing through the driver
 * (physical finger), captured when we're not injecting. */
static volatile unsigned short real_touch_x;
static volatile unsigned short real_touch_y;
static volatile unsigned char real_touch_seen;
static volatile unsigned char real_touch_down;

/* Rear touchpad active area as reported by the panel driver, sent in the
 * heartbeat so the PC can map positions onto it. Read once from
 * uvc_thread (thread context), not from a hook. */
static SceTouchPanelInfo rear_panel_info;
extern int ksceTouchGetPanelInfo(unsigned int port, SceTouchPanelInfo *info);

static void rear_panel_info_read(void)
{
	memset(&rear_panel_info, 0, sizeof(rear_panel_info));
	if (ksceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &rear_panel_info) < 0)
		memset(&rear_panel_info, 0, sizeof(rear_panel_info));
}

/* Hooking the SceTouch driver's read paths globally (same technique as
 * VitaControl/ds34vita): every process asking for touch data gets our
 * injected report instead of nothing. */
static tai_hook_ref_t touch_peek_ref;
static tai_hook_ref_t touch_peek_region_ref;
static tai_hook_ref_t touch_read_ref;
static tai_hook_ref_t touch_read_region_ref;
static tai_hook_ref_t touch_peek2_ref;
static tai_hook_ref_t touch_read2_ref;
static SceUID touch_peek_hook_uid = -1;
static SceUID touch_peek_region_hook_uid = -1;
static SceUID touch_read_hook_uid = -1;
static SceUID touch_read_region_hook_uid = -1;
static SceUID touch_peek2_hook_uid = -1;
static SceUID touch_read2_hook_uid = -1;
static tai_hook_ref_t svc_peek_ref;
static tai_hook_ref_t svc_peek_region_ref;
static tai_hook_ref_t svc_read_ref;
static tai_hook_ref_t svc_read_region_ref;
static SceUID svc_peek_hook_uid = -1;
static SceUID svc_peek_region_hook_uid = -1;
static SceUID svc_read_hook_uid = -1;
static SceUID svc_read_region_hook_uid = -1;

/* Touch hooks are installed once and kept for the module lifetime:
 * runtime taiHookRelease while a game thread executes the patched
 * function caused intermittent kernel panics. Between sessions they
 * simply pass real digitizer data through untouched. */
static volatile int touch_hooks_installed;
static volatile int touch_hooks_wanted;
#define TOUCH_HOOKS_ENABLED 1

static void patch_touch_data(int port, SceTouchData *data, int count)
{
	int i;

	/* Capture du PID du processus qui lit le tactile (le jeu au premier
	 * plan pendant une session PC). Syscall trivial, contexte thread jeu
	 * prouv� s�r (on y ex�cute d�j� tout le reste du hook). */
	if (pc_control_active)
		game_pid = ksceKernelGetProcessId();

	if ((port != SCE_TOUCH_PORT_FRONT && port != SCE_TOUCH_PORT_BACK) ||
	    !data || count <= 0)
		return;

	/* Ground truth capture + passthrough when idle: while the PC is not
	 * pressing, the REAL digitizer data must flow untouched � writing
	 * empty history here would erase physical finger input. */
	if (!pc_touch_any(port)) {
		int any = 0;
		/* The ground-truth capture below is for the front only. */
		if (port != SCE_TOUCH_PORT_FRONT)
			return;
		for (i = 0; i < count; i++) {
			if (data[i].reportNum > 0) {
				real_touch_x = data[i].report[0].x;
				real_touch_y = data[i].report[0].y;
				real_touch_seen = 1;
				any = 1;
				break;
			}
		}
		real_touch_down = any ? 1 : 0;

		touch_edge_history = (unsigned char)(touch_edge_history << 1);
		last_read_count = (unsigned char)count;
		return;
	}

	touch_patch_hits++;
	if (touch_patched_reads[port] < 255)
		touch_patched_reads[port]++;
	if (port == SCE_TOUCH_PORT_FRONT) {
		touch_edge_history = (unsigned char)((touch_edge_history << 1) | 1);
		last_read_count = (unsigned char)count;
	}

	/* PROVEN configuration: steady pressed fingers at the given points,
	 * preserving each buffer's timestamp/status. One report per finger
	 * held, each with the id it got when it touched down (a new contact
	 * gets a new id, like a real digitizer: some games ignore repeated
	 * contacts reusing an old id). */
	for (i = 0; i < count; i++) {
		SceUInt64 ts = data[i].timeStamp;
		SceUInt32 st = data[i].status;
		int f, n = 0;

		memset(&data[i], 0, sizeof(SceTouchData));
		data[i].timeStamp = ts;
		data[i].status = st;
		for (f = 0; f < PC_TOUCH_FINGERS; f++) {
			const struct pc_finger *pf = &pc_fingers[port][f];

			if (!pf->down)
				continue;
			data[i].report[n].id = pf->id ? pf->id : 1;
			data[i].report[n].force = pc_touch_force[port];
			data[i].report[n].x = (short)pf->x;
			data[i].report[n].y = (short)pf->y;
			data[i].report[n].info = 0;
			n++;
		}
		data[i].reportNum = n;
	}
}

typedef int (*touch_call3_t)(int, SceTouchData *, int);
typedef int (*touch_call4_t)(int, SceTouchData *, int, int);

/* Chain-aware original call: if another plugin hooked the same function
 * AFTER us, we must call ITS replacement first (psvshell/osyscall6 are
 * co-installed here). */
#define TOUCH_CALL_OLD(ref, type, ...) \
	({ struct _tai_hook_user *cur__ = (struct _tai_hook_user *)(ref); \
	   struct _tai_hook_user *nxt__ = \
		(struct _tai_hook_user *)cur__->next; \
	   nxt__ ? ((type)nxt__->func)(__VA_ARGS__) \
	         : ((type)cur__->old)(__VA_ARGS__); })

static int ksceTouchPeek_patched(int port, SceTouchData *data, int count)
{
	if (h_peek < 255) h_peek++;
	int ret = TOUCH_CALL_OLD(touch_peek_ref, touch_call3_t, port, data, count);
	patch_touch_data(port, data, count);
	return ret;
}

static int ksceTouchPeekRegion_patched(int port, SceTouchData *data, int count, int region)
{
	if (h_peekR < 255) h_peekR++;
	int ret = TOUCH_CALL_OLD(touch_peek_region_ref, touch_call4_t, port, data, count, region);
	patch_touch_data(port, data, count);
	return ret;
}

static int ksceTouchRead_patched(int port, SceTouchData *data, int count)
{
	if (h_read < 255) h_read++;
	int ret = TOUCH_CALL_OLD(touch_read_ref, touch_call3_t, port, data, count);
	patch_touch_data(port, data, count);
	return ret;
}

static int ksceTouchReadRegion_patched(int port, SceTouchData *data, int count, int region)
{
	if (h_readR < 255) h_readR++;
	int ret = TOUCH_CALL_OLD(touch_read_region_ref, touch_call4_t, port, data, count, region);
	patch_touch_data(port, data, count);
	return ret;
}

/* The "2" variants share the same signature and are what some SDK
 * versions of games call instead. */
static int sceTouchPeek2_patched(int port, SceTouchData *data, int count)
{
	if (h_peek2 < 255) h_peek2++;
	int ret = TOUCH_CALL_OLD(touch_peek2_ref, touch_call3_t, port, data, count);
	patch_touch_data(port, data, count);
	return ret;
}

static int sceTouchRead2_patched(int port, SceTouchData *data, int count)
{
	if (h_read2 < 255) h_read2++;
	int ret = TOUCH_CALL_OLD(touch_read2_ref, touch_call3_t, port, data, count);
	patch_touch_data(port, data, count);
	return ret;
}

/* Install/remove the six SceTouch interceptors. Removal fully restores
 * the stock touchscreen (hooks released, not just neutralized). */
static void touch_hooks_install(void)
{
#if TOUCH_HOOKS_ENABLED
	if (touch_hooks_installed)
		return;

	touch_peek_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&touch_peek_ref, "SceTouch", TAI_ANY_LIBRARY, 0xBAD1960B,
		ksceTouchPeek_patched);
	touch_peek_region_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&touch_peek_region_ref, "SceTouch", TAI_ANY_LIBRARY, 0x9B3F7207,
		ksceTouchPeekRegion_patched);
	touch_read_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&touch_read_ref, "SceTouch", TAI_ANY_LIBRARY, 0x70C8AACE,
		ksceTouchRead_patched);
	touch_read_region_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&touch_read_region_ref, "SceTouch", TAI_ANY_LIBRARY, 0x9A91F624,
		ksceTouchReadRegion_patched);
	touch_peek2_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&touch_peek2_ref, "SceTouch", TAI_ANY_LIBRARY, 0x3AD3D0A1,
		sceTouchPeek2_patched);
	touch_read2_hook_uid = taiHookFunctionExportForKernel(KERNEL_PID,
		&touch_read2_ref, "SceTouch", TAI_ANY_LIBRARY, 0x39401BEA,
		sceTouchRead2_patched);

	touch_hooks_installed = 1;
#endif /* TOUCH_HOOKS_ENABLED */
}

static void touch_hooks_remove(void)
{
	if (!touch_hooks_installed)
		return;

	if (touch_peek_hook_uid > 0) {
		taiHookReleaseForKernel(touch_peek_hook_uid, touch_peek_ref);
		touch_peek_hook_uid = -1;
	}
	if (touch_peek_region_hook_uid > 0) {
		taiHookReleaseForKernel(touch_peek_region_hook_uid, touch_peek_region_ref);
		touch_peek_region_hook_uid = -1;
	}
	if (touch_read_hook_uid > 0) {
		taiHookReleaseForKernel(touch_read_hook_uid, touch_read_ref);
		touch_read_hook_uid = -1;
	}
	if (touch_read_region_hook_uid > 0) {
		taiHookReleaseForKernel(touch_read_region_hook_uid, touch_read_region_ref);
		touch_read_region_hook_uid = -1;
	}
	if (touch_peek2_hook_uid > 0) {
		taiHookReleaseForKernel(touch_peek2_hook_uid, touch_peek2_ref);
		touch_peek2_hook_uid = -1;
	}
	if (touch_read2_hook_uid > 0) {
		taiHookReleaseForKernel(touch_read2_hook_uid, touch_read2_ref);
		touch_read2_hook_uid = -1;
	}

	touch_hooks_installed = 0;
}

/* Real hardware rest position of both sticks, measured at attach time.
 * Forcing an absolute "center = 128" creates a small permanent offset
 * against the console's own calibrated neutral point, which shows up as
 * a constant drift inside games. */
static unsigned char pc_neutral_lx = 128, pc_neutral_ly = 128;
static unsigned char pc_neutral_rx = 128, pc_neutral_ry = 128;

#define PC_CLAMP8(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))

static void pc_ctrl_capture_neutral(void)
{
	SceCtrlData pad;
	int sum_lx = 0, sum_ly = 0, sum_rx = 0, sum_ry = 0;
	int i, n = 0;

	for (i = 0; i < 8; i++) {
		memset(&pad, 0, sizeof(pad));
		if (ksceCtrlPeekBufferPositive(0, &pad, 1) < 0)
			break;
		sum_lx += pad.lx; sum_ly += pad.ly;
		sum_rx += pad.rx; sum_ry += pad.ry;
		n++;
	}

	if (n > 0) {
		pc_neutral_lx = (unsigned char)(sum_lx / n);
		pc_neutral_ly = (unsigned char)(sum_ly / n);
		pc_neutral_rx = (unsigned char)(sum_rx / n);
		pc_neutral_ry = (unsigned char)(sum_ry / n);
	}
}

static void pc_control_apply(void)
{
    int ret_a, ret_b;
    int slot;

    if (!pc_control_active)
        return;

    /* Separate slots: writing the analog emulation into the same slot as
     * the buttons appears to wipe the button part of the slot. Values are
     * expressed relative to the console's own stick rest position. */
    ret_a = ksceCtrlSetAnalogEmulation(0, 0,
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_lx + (int)pc_lx - 128),
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_ly + (int)pc_ly - 128),
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_rx + (int)pc_rx - 128),
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_ry + (int)pc_ry - 128),
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_lx + (int)pc_lx - 128),
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_ly + (int)pc_ly - 128),
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_rx + (int)pc_rx - 128),
                                       (unsigned char)PC_CLAMP8((int)pc_neutral_ry + (int)pc_ry - 128),
                                       2);

    /* Try everything at once: every available slot (they combine), both
     * the user-space and kernel-space masks, and a long uiMake. The
     * shoulder-trigger bits (0x100/0x200) seem to be dropped by the
     * firmware's merge step, so also inject their L1/R1 aliases
     * (0x400/0x800) as a fallback. */
    {
        unsigned int b = pc_buttons | ((pc_buttons & 0x0300u) << 2);
        /* PS and volume only in the kernel mask: user masks can't carry
         * them. */
        unsigned int kb = b | pc_kbuttons;
        ret_b = 0;
        for (slot = 1; slot <= 3; slot++) {
            ret_b |= ksceCtrlSetButtonEmulation(0, (unsigned char)slot,
                                                b, kb, 60);
        }
    }
    pc_ret_button_emu = ret_b;
    pc_ret_analog_emu = ret_a;
}

static void pc_control_release(void)
{
	int slot;

	pc_control_active = 0;
	pc_buttons = 0;
	pc_kbuttons = 0;
	pc_touch_release_all();
	pc_lx = pc_ly = pc_rx = pc_ry = 128;
	/* Session termin�e : on r�autorise un kill et on oublie l'ancien
	 * PID (le jeu au premier plan peut avoir chang� entre-temps).
	 * exit_seen_idle repart � 0 : une touche �chap d�j� enfonc�e au
	 * moment de la reconnexion ne peut rien tuer d'embl�e. */
	exit_armed = 1;
	exit_seen_idle = 0;
	game_pid = -1;
	exit_req_seen = 0;
	exit_block = 0;
	kill_result = 0;
	for (slot = 1; slot <= 3; slot++) {
		ksceCtrlSetButtonEmulation(0, (unsigned char)slot, 0, 0, 60);
	}
	ksceCtrlSetAnalogEmulation(0, 0, 128, 128, 128, 128,
	                           0, 0, 0, 0, 4);
}

/* One finger from the report: 3 bytes, 12-bit X then 12-bit Y (byte 0 =
 * X low, byte 1 = Y low, byte 2 = X high nibble | Y high nibble << 4). */
static void pc_apply_finger(int port, int f, int down, const unsigned char *p)
{
	struct pc_finger *pf = &pc_fingers[port][f];
	unsigned int tx = (unsigned int)p[0] | (((unsigned int)p[2] & 0x0F) << 8);
	unsigned int ty = (unsigned int)p[1] | ((((unsigned int)p[2] >> 4) & 0x0F) << 8);

	if (tx >= TOUCHSCREEN_WIDTH)  tx = TOUCHSCREEN_WIDTH - 1;
	if (ty >= TOUCHSCREEN_HEIGHT) ty = TOUCHSCREEN_HEIGHT - 1;
	pf->x = (unsigned short)tx;
	pf->y = (unsigned short)ty;
	if (down && !pf->down) {
		/* New contact -> new finger id, never 0. */
		if (++pc_touch_next_id == 0)
			pc_touch_next_id = 1;
		pf->id = pc_touch_next_id;
	}
	pf->down = down ? 1 : 0;
}

/* Shared helper: apply a freshly received 64-byte PC report, whichever
 * transport (interrupt endpoint or EP0 SET_REPORT) it arrived through.
 * Layout: 1-2 buttons, 3-6 sticks, 7 touch flags (bit 0/1 front fingers,
 * bit 2/3 rear fingers), 8-10 front finger 1, 11 kernel buttons (bit 0 PS,
 * bit 1 VOL+, bit 2 VOL-), 12-14 front finger 2, 15-17 rear finger 1,
 * 18 exit game, 19-21 rear finger 2, 22/23 front/rear force (0 = 128). */
static void pc_hid_apply_report(const unsigned char *report)
{
	if (report[0] != PC_HID_REPORT_ID_OUTPUT)
		return;

	pc_buttons = (unsigned int)report[1] |
	             ((unsigned int)report[2] << 8);
	pc_kbuttons = ((report[11] & 0x01) ? SCE_CTRL_PSBUTTON : 0) |
	              ((report[11] & 0x02) ? SCE_CTRL_VOLUP : 0) |
	              ((report[11] & 0x04) ? SCE_CTRL_VOLDOWN : 0);
	pc_lx = report[3];
	pc_ly = report[4];
	pc_rx = report[5];
	pc_ry = report[6];

	{
		unsigned char fl = report[7];

		pc_touch_force[SCE_TOUCH_PORT_FRONT] =
			report[22] ? report[22] : PC_TOUCH_FORCE_DEFAULT;
		pc_touch_force[SCE_TOUCH_PORT_BACK] =
			report[23] ? report[23] : PC_TOUCH_FORCE_DEFAULT;
		pc_apply_finger(SCE_TOUCH_PORT_FRONT, 0, fl & 0x01, &report[8]);
		pc_apply_finger(SCE_TOUCH_PORT_FRONT, 1, fl & 0x02, &report[12]);
		pc_apply_finger(SCE_TOUCH_PORT_BACK, 0, fl & 0x04, &report[15]);
		pc_apply_finger(SCE_TOUCH_PORT_BACK, 1, fl & 0x08, &report[19]);
	}

	/* Commande "quitter le jeu" : le PC ne l'envoie que pendant la
	 * dur�e de maintien d'�chap (>600 ms). On arme un drapeau consomm�
	 * par uvc_thread. On note aussi le retour au repos : le r�armement
	 * apr�s un kill EXIGE d'avoir revu la commande � 0, donc impossible
	 * de tuer deux processus avec un seul maintien de touche. */
	if (report[18])
		exit_game_request = 1;
	else
		exit_seen_idle = 1;

	pc_control_active = 1;
	pc_control_apply();
}

static void pc_hid_recv_on_complete(SceUdcdDeviceRequest *req)
{
	BC(bc_ep3_done_n, bc_ep3_done_n + 1);
	BC(bc_ep3_last_rc, req->returnCode);
	if (req->returnCode < 0) {
		if (pc_hid_recv_err < 255)
			pc_hid_recv_err++;
		/* Flush whatever half-received packet is stuck in the
		 * endpoint before trying again. */
		ksceUdcdReqCancelAll(&endpoints[3]);
		ksceUdcdClearFIFO(&endpoints[3]);
	} else {
		if (pc_hid_recv_ok < 0xFFFF)
			pc_hid_recv_ok++;
		if (req->transmitted >= 7) {
			/* While the USB DMA was writing the report, the CPU
			 * may have speculatively re-cached the buffer's stale
			 * contents. Discard the cache before reading, or we
			 * apply zeros instead of the real report. */
			ksceKernelDcacheInvalidateRange(pc_hid_report,
							sizeof(pc_hid_report));
			rx_sequence++;
			touch_hooks_wanted = 1;
			pc_hid_apply_report(pc_hid_report);
		}
	}

	/* Always re-arm, even after an error: a single bad transfer must
	 * not kill the control channel permanently. Except once USB is being
	 * given away: the cancel lands here, and a new request would be left
	 * behind for whoever takes USB. Attach arms it again. */
	if (usb_yielded)
		return;
	memset(pc_hid_report, 0, sizeof(pc_hid_report));
	pc_hid_report[0] = PC_HID_REPORT_ID_OUTPUT;
	ksceKernelDcacheCleanInvalidateRange(pc_hid_report, sizeof(pc_hid_report));
	pc_hid_recv_req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[3],
		.data = pc_hid_report,
		.attributes = 0,
		.size = sizeof(pc_hid_report),
		.isControlRequest = 0,
		.onComplete = &pc_hid_recv_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceUdcdReqRecv(&pc_hid_recv_req);
}

static int pc_hid_start_receive(void)
{
	memset(pc_hid_report, 0, sizeof(pc_hid_report));
	pc_hid_report[0] = PC_HID_REPORT_ID_OUTPUT;
	pc_hid_recv_req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[3],
		.data = pc_hid_report,
		.attributes = 0,
		.size = sizeof(pc_hid_report),
		.isControlRequest = 0,
		.onComplete = &pc_hid_recv_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};
	ksceKernelDcacheCleanInvalidateRange(pc_hid_report, sizeof(pc_hid_report));
	return ksceUdcdReqRecv(&pc_hid_recv_req);
}

/* ---- EP2 (HID IN): one request at a time ----
 * The heartbeat is sent here. UDCD does not tolerate nested requests on
 * the same endpoint (that caused a panic when a game started). */
static unsigned char ep2_pkt[64] __attribute__((aligned(64)));
static SceUdcdDeviceRequest ep2_req;
static volatile int ep2_busy;

static void ep2_on_complete(SceUdcdDeviceRequest *req)
{
	(void)req;
	ep2_busy = 0;
}

/* Copie pkt puis envoie ; -1 si le canal est encore occup�. */
static int ep2_try_send(const unsigned char *pkt)
{
	/* Atomic test-and-set: only the winner touches ep2_req/ep2_pkt, so two
	 * callers can never put nested requests on EP2. */
	int old = __sync_val_compare_and_swap(&ep2_busy, 0, 1);
	if (old != 0)
		return -1;

	memcpy(ep2_pkt, pkt, 64);

	ep2_req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[2],
		.data = ep2_pkt,
		.attributes = 0,
		.size = 64,
		.isControlRequest = 0,
		.onComplete = &ep2_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelDcacheCleanRange(ep2_pkt, 64);
	int r = ksceUdcdReqSend(&ep2_req);
	if (r != 0) {
		/* Requ�te possiblement � moiti� enregistr�e c�t� driver :
		 * purge EP2 AVANT de rendre le jeton, sinon la compl�tion
		 * tardive de l'orphelin retombe pendant le remplissage du
		 * prochain paquet (requ�tes imbriqu�es = panique). */
		ksceUdcdReqCancelAll(&endpoints[2]);
		ep2_busy = 0;
	}
	return r;
}

/*
 * Sleep, power off and reboot: no video frame may be pending when the
 * system takes USB down. Before build 110, a reboot with the cable in hung
 * for ~20 s and then powered the Vita off whenever the video had been used
 * in that boot (TODO.md): either frames were still being sent, or, after a
 * video stop, uvc_thread was stuck waiting on a frame that never went out.
 * A reboot that never used the video, or with the cable pulled, was fine.
 *
 * Build 108 recorded what the kernel sends (2026-11-24): a reboot and a
 * sleep send the same "going down" events, 0x100, 0x102, ..., 0x202, 0x201,
 * 0x200, 0x400, then 0x401 every ~10 ms for 3.6 s until USB goes; a wake
 * sends "resume" events (0x100002, 0x400000). The ids don't tell sleep from
 * reboot, so this does the same for both: on each "going down" event, stop
 * the video, cancel the frame request and release uvc_thread if it waits
 * on one; on "resume", streaming may start again. No waiting here, no file
 * IO (the debug build that waited 500 ms here blocked the shutdown for
 * good). The events repeat, which covers a frame queued between two.
 */
int ksceKernelUnregisterSysEventHandler(SceUID uid);	/* in the stub, not the header */

static SceUID sysevent_uid = -1;
static volatile int going_down;	/* between a "going down" event and a resume */
/* Set while uvc_thread waits for a frame transfer to complete (TODO.md:
 * after a video stop it can wait there forever). */
static volatile int uvc_frame_waiting;
/* Set from just before a frame request is queued until it completes. */
static volatile int uvc_frame_pending;
/* When that frame was queued (ms since boot, 32 bits so another thread
 * reads it in one go). See uvc_watchdog_thread. */
static volatile unsigned int uvc_frame_sent_ms;

/* Cancel the frame request still queued, if any, and let uvc_thread go on
 * if it waits for it. The caller has set stream = 0 (or going_down, or
 * usb_yielded) first, so uvc_frame_req_submit_phycont queues no new one.
 * Without this, a frame queued just as the PC stops reading never
 * completes: uvc_thread waits forever, and whoever stops USB next (a
 * reboot, VitaShell's USB transfer) hangs on it. Called from other threads
 * than uvc_thread (the 103 crash came from cancelling in uvc_thread while
 * streaming). No waiting, no file IO. */
static void uvc_frame_release(void)
{
	__sync_synchronize();
	if (uvc_frame_pending || uvc_frame_waiting) {
		ksceUdcdReqCancelAll(&endpoints[1]);
		/* A cancel may not call the completion. */
		uvc_frame_pending = 0;
		ksceKernelSetEventFlag(uvc_frame_req_evflag, 1);
	}
}

static int sysevent_handler(int resume, int eventid, void *args, void *opt)
{
	(void)eventid;
	(void)args;
	(void)opt;
	if (resume) {
		going_down = 0;
	} else {
		going_down = 1;		/* uvc_thread sends no more frames */
		stream = 0;
		/* Each repeated event cancels again while the frame hasn't
		 * completed. */
		uvc_frame_release();
	}
	return 0;
}

static void pc_hid_send_input_heartbeat(void)
{
	static unsigned char hb[PC_HID_REPORT_SIZE] __attribute__((aligned(64)));
	SceCtrlData pad;
	static unsigned char seq;

	memset(hb, 0, sizeof(hb));

	/* Snapshot of the REAL controller state as the console sees it
	 * right now (physical presses + whatever the ctrl driver merged),
	 * so the PC app can display ground truth for diagnosis. */
	memset(&pad, 0, sizeof(pad));
	ksceCtrlPeekBufferPositive(0, &pad, 1);

	hb[0] = HID_REPORT_ID_INPUT; /* diag rides on report id 1 for now */
	hb[1] = 0xC7; /* magic */
	hb[2] = pc_control_active ? 1 : 0;
	hb[3] = (unsigned char)(pc_hid_recv_ok & 0xFF);
	hb[4] = (unsigned char)((pc_hid_recv_ok >> 8) & 0xFF);
	hb[5] = exit_block;     /* raison du dernier refus exit (sticky) */
	hb[6] = exit_req_seen;  /* une commande exit est d�j� arriv�e */
	hb[7] = (unsigned char)(pc_buttons & 0xFF);
	hb[8] = (unsigned char)((pc_buttons >> 8) & 0xFF);
	hb[9]  = (unsigned char)(pad.buttons & 0xFF);
	hb[10] = (unsigned char)((pad.buttons >> 8) & 0xFF);
	hb[11] = (unsigned char)((pad.buttons >> 16) & 0xFF);
	hb[12] = (unsigned char)((pad.buttons >> 24) & 0xFF);
	hb[13] = pad.lx;
	hb[14] = pad.ly;
	hb[15] = pad.rx;
	hb[16] = pad.ry;
	hb[17] = (unsigned char)pc_ret_button_emu;
	hb[18] = 0; /* audio: last failure code, set below */
	hb[19] = 0; /* audio state: set below */
	hb[20] = ++seq;

	/* Video-side ground truth: is the UVC stream active and what did
	 * the host negotiate? */
	hb[21] = stream ? 1 : 0;
	hb[22] = uvc_probe_control_setting.bFormatIndex;
	hb[23] = uvc_probe_control_setting.bFrameIndex;
	hb[24] = (unsigned char)(uvc_probe_control_setting.dwFrameInterval & 0xFF);
	hb[25] = (unsigned char)((uvc_probe_control_setting.dwFrameInterval >> 8) & 0xFF);
	hb[26] = (unsigned char)((uvc_probe_control_setting.dwFrameInterval >> 16) & 0xFF);
	hb[27] = (unsigned char)((uvc_probe_control_setting.dwFrameInterval >> 24) & 0xFF);

	/* Touch-injection ground truth: hook install status + current
	 * injected state. */
	hb[28] = (unsigned char)((touch_peek_hook_uid > 0 ? 1 : 0) |
	                         (touch_peek_region_hook_uid > 0 ? 2 : 0) |
	                         (touch_read_hook_uid > 0 ? 4 : 0) |
	                         (touch_read_region_hook_uid > 0 ? 8 : 0) |
	                         (touch_peek2_hook_uid > 0 ? 16 : 0) |
	                         (touch_read2_hook_uid > 0 ? 32 : 0));
	/* bit 0 front finger, 1 PS, 2 VOL+, 3 VOL-, 4 rear finger */
	hb[29] = (pc_touch_any(SCE_TOUCH_PORT_FRONT) ? 0x01 : 0) |
	         ((pc_kbuttons & SCE_CTRL_PSBUTTON) ? 0x02 : 0) |
	         ((pc_kbuttons & SCE_CTRL_VOLUP) ? 0x04 : 0) |
	         ((pc_kbuttons & SCE_CTRL_VOLDOWN) ? 0x08 : 0) |
	         (pc_touch_any(SCE_TOUCH_PORT_BACK) ? 0x10 : 0);
	/* Reads patched per panel since the last heartbeat. */
	hb[30] = touch_patched_reads[SCE_TOUCH_PORT_FRONT];
	hb[31] = touch_patched_reads[SCE_TOUCH_PORT_BACK];
	hb[32] = h_peek;
	hb[33] = h_peekR;
	hb[34] = h_read;
	hb[35] = h_readR;
	hb[36] = h_peek2;
	hb[37] = h_read2;
	/* Rear touchpad active area (little-endian): min X, max X, min Y, max Y. */
	hb[38] = (unsigned char)(rear_panel_info.minAaX & 0xFF);
	hb[39] = (unsigned char)(((unsigned short)rear_panel_info.minAaX >> 8) & 0xFF);
	hb[40] = (unsigned char)((unsigned short)rear_panel_info.maxAaX & 0xFF);
	hb[41] = (unsigned char)(((unsigned short)rear_panel_info.maxAaX >> 8) & 0xFF);
	hb[42] = (unsigned char)(rear_panel_info.minAaY & 0xFF);
	hb[43] = (unsigned char)(((unsigned short)rear_panel_info.minAaY >> 8) & 0xFF);
	hb[44] = (unsigned char)((unsigned short)rear_panel_info.maxAaY & 0xFF);
	hb[45] = (unsigned char)(((unsigned short)rear_panel_info.maxAaY >> 8) & 0xFF);
	hb[46] = (unsigned char)((svc_peek_hook_uid > 0 ? 1 : 0) |
	                         (svc_peek_region_hook_uid > 0 ? 2 : 0) |
	                         (svc_read_hook_uid > 0 ? 4 : 0) |
	                         (svc_read_region_hook_uid > 0 ? 8 : 0));
	hb[47] = (unsigned char)((real_touch_seen ? 1 : 0) | (real_touch_down ? 2 : 0));
	hb[48] = (unsigned char)(real_touch_x & 0xFF);
	hb[49] = (unsigned char)((real_touch_x >> 8) & 0xFF);
	hb[50] = (unsigned char)(real_touch_y & 0xFF);
	hb[51] = (unsigned char)((real_touch_y >> 8) & 0xFF);
	hb[52] = touch_edge_history;
	hb[53] = last_read_count;
	hb[54] = (unsigned char)(rx_sequence & 0xFF);
	hb[55] = (unsigned char)((rx_sequence >> 8) & 0xFF);
	hb[56] = (unsigned char)((rx_sequence >> 16) & 0xFF);
	hb[57] = (unsigned char)((rx_sequence >> 24) & 0xFF);
	/* Exit-game diagnostics: PID of the latest touch poller (2 bytes,
	 * les PIDs tiennent largement dans 16 bits) + kill state. */
	hb[58] = (unsigned char)((unsigned int)game_pid & 0xFF);
	hb[59] = (unsigned char)(((unsigned int)game_pid >> 8) & 0x0F);
	{
		/* USB audio: state, packets/4, errors, underruns, last failure
		 * code (audio.h). */
		unsigned char xruns;

		audio_heartbeat(&hb[19], &hb[60], &hb[62], &xruns, &hb[18]);
		hb[59] |= (unsigned char)(xruns << 4);
	}
	/* Bits 0-1: the Vita's speaker during sound capture (speaker.h). */
	hb[61] = (unsigned char)((speaker_state() & 3) |
	                         (exit_game_request ? 8 : 0) |
	                         ((kill_result & 3) << 4) |
	                         ((exit_phase & 3) << 6));
	hb[63] =
#ifdef USB_PROBE
		116;
#else
		115;
#endif
	/* build id: 90 PS, 91 all inputs, 95 debug breadcrumbs, 96 USB audio (silence), 97 Vita sound, 101 one audio request in flight, 105 = 101 + awake during sound capture (102-104 withdrawn: video hangs), 106 no 1280x720, 107 speaker plays during sound capture, 108 system event probe (test only), 109 = probe + 110's fix (test only), 110 no video frame pending at sleep/reboot, 111 steps aside for VitaShell's USB transfer (StopCurrentInternal hook), frame released at video stop, 112 USB probe (test only), 113 retake 5 s after MTP restarts, 114 = 113 + USB probe (test only), 115 video stop detected (frame unread 1 s), 116 = 115 + USB probe (test only) */

	ep2_try_send(hb);
	touch_patched_reads[SCE_TOUCH_PORT_FRONT] = 0;
	touch_patched_reads[SCE_TOUCH_PORT_BACK] = 0;
}

static int uvc_frame_init(unsigned int size);
static int uvc_frame_term();

#if defined(DISPLAY_OFF_OLED) || defined(DISPLAY_OFF_LCD)
static int prev_brightness;
#endif

static int usb_ep0_req_send(const void *data, unsigned int size)
{
	static SceUdcdDeviceRequest req;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)data,
		.attributes = 0,
		.size = size,
		.isControlRequest = 0,
		.onComplete = NULL,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelDcacheCleanRange(data, size);

	return ksceUdcdReqSend(&req);
}

static void usb_ep0_req_recv_on_complete(SceUdcdDeviceRequest *req);

static int usb_ep0_enqueue_recv_for_req(const SceUdcdEP0DeviceRequest *ep0_req)
{
	static SceUdcdDeviceRequest req;

	pending_recv.ep0_req = *ep0_req;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[0],
		.data = (void *)pending_recv.buffer,
		.attributes = 0,
		.size = pending_recv.ep0_req.wLength,
		.isControlRequest = 0,
		.onComplete = &usb_ep0_req_recv_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelDcacheCleanInvalidateRange(pending_recv.buffer,
		pending_recv.ep0_req.wLength);

	return ksceUdcdReqRecv(&req);
}

static int uvc_frame_req_init(void)
{
	uvc_frame_req_evflag = ksceKernelCreateEventFlag("uvc_frame_req_evflag", 0, 0, NULL);
	if (uvc_frame_req_evflag < 0) {
		return uvc_frame_req_evflag;
	}

	return 0;
}

static int uvc_frame_req_fini(void)
{
	int ret;

	ret = ksceKernelDeleteEventFlag(uvc_frame_req_evflag);
	if (ret < 0)
		return ret;

	return 0;
}

static void uvc_frame_req_submit_phycont_on_complete(SceUdcdDeviceRequest *req)
{
	uvc_frame_pending = 0;
	ksceKernelSetEventFlag(uvc_frame_req_evflag, 1);
}

static int uvc_frame_req_submit_phycont(const void *data, unsigned int size)
{
	static SceUdcdDeviceRequest req;
	int ret;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[1],
		.data = (void *)data,
		.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT,
		.size = size,
		.isControlRequest = 0,
		.onComplete = uvc_frame_req_submit_phycont_on_complete,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	/* Not once the video has stopped, the system is going down
	 * (sysevent_handler) or USB is being given away (usb_yield). Pending
	 * is set before the check and the stoppers clear stream before
	 * testing pending (uvc_frame_release), so either we see the stop or
	 * they see this frame. */
	uvc_frame_pending = 1;
	__sync_synchronize();
	if (!stream || going_down || usb_yielded) {
		uvc_frame_pending = 0;
		return 0;
	}
	uvc_frame_sent_ms = (unsigned int)(ksceKernelGetSystemTimeWide() / 1000);
	/* A release while nothing waited leaves the flag set: it must not
	 * end this frame's wait early. (A poll that clears what it finds;
	 * it fails harmlessly when the flag isn't set.) */
	{
		unsigned int stale;

		ksceKernelPollEventFlag(uvc_frame_req_evflag, 1,
					SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
					&stale);
	}
	ret = ksceUdcdReqSend(&req);
	if (ret < 0) {
		uvc_frame_pending = 0;
		return ret;
	}

	BC(bc_uvc_where, 5);
	uvc_frame_waiting = 1;
	ret = ksceKernelWaitEventFlagCB(uvc_frame_req_evflag, 1, SCE_EVENT_WAITOR |
					SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);
	uvc_frame_waiting = 0;
	BC(bc_uvc_where, 6);

	return ret;
}

static void uvc_handle_video_streaming_req_recv(const SceUdcdEP0DeviceRequest *req)
{
	struct uvc_streaming_control *streaming_control =
		(struct uvc_streaming_control *)pending_recv.buffer;

	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_SET_CUR:
			uvc_probe_control_setting.bFormatIndex = streaming_control->bFormatIndex;
			uvc_probe_control_setting.bFrameIndex = streaming_control->bFrameIndex;
			uvc_probe_control_setting.dwFrameInterval = streaming_control->dwFrameInterval;

			stream = 1;
			ksceKernelSetEventFlag(uvc_event_flag_id, 1);
			break;
		}
		break;
	}
}

/* Handles the completion of an EP0 receive we ourselves queued. Used both
 * for the UVC probe/commit SET_CUR flow and for HID SET_REPORT (PC control
 * data pushed over the control endpoint instead of the interrupt OUT one). */
void usb_ep0_req_recv_on_complete(SceUdcdDeviceRequest *req)
{
	switch (pending_recv.ep0_req.wIndex & 0xFF) {
	case STREAM_INTERFACE:
		uvc_handle_video_streaming_req_recv(&pending_recv.ep0_req);
		break;
	case PC_HID_INTERFACE:
		if (pending_recv.ep0_req.bRequest == HID_SET_REPORT &&
		    req->transmitted >= 7) {
			ksceKernelDcacheInvalidateRange(pending_recv.buffer,
							req->transmitted);
			pc_hid_apply_report(pending_recv.buffer);
		}
		break;
	}
}

static void uvc_handle_interface_ctrl_req(const SceUdcdEP0DeviceRequest *req)
{
}

static void uvc_handle_input_terminal_req(const SceUdcdEP0DeviceRequest *req)
{
}

static void uvc_handle_output_terminal_req(const SceUdcdEP0DeviceRequest *req)
{
}

static void uvc_handle_video_streaming_req(const SceUdcdEP0DeviceRequest *req)
{
	switch (req->wValue >> 8) {
	case UVC_VS_PROBE_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_MIN:
		case UVC_GET_MAX:
		case UVC_GET_DEF:
			usb_ep0_req_send(&uvc_probe_control_setting_default,
					 sizeof(uvc_probe_control_setting_default));
			break;
		case UVC_GET_CUR:
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	case UVC_VS_COMMIT_CONTROL:
		switch (req->bRequest) {
		case UVC_GET_INFO:
			break;
		case UVC_GET_LEN:
			break;
		case UVC_GET_CUR:
			usb_ep0_req_send(&uvc_probe_control_setting,
					 sizeof(uvc_probe_control_setting));
			break;
		case UVC_SET_CUR:
			usb_ep0_enqueue_recv_for_req(req);
			break;
		}
		break;
	}
}

static void uvc_handle_video_abort(void)
{
	BC(bc_abort_n, bc_abort_n + 1);
	BC(bc_abort_step, 1);
	if (stream) {
		stream = 0;

		ksceUdcdClearFIFO(&endpoints[1]);
		BC(bc_abort_step, 2);
		pc_control_release();
		BC(bc_abort_step, 3);
		/* HID is left alone: the EP3 receive is still queued and
		 * re-arms itself. Queuing it again here (as before build 111)
		 * put the same request on EP3 twice. */
		exit_phase = 0;
		kill_hold = 0;
	}
	/* Even when stream was already 0: after a video stop the last frame
	 * can stay queued, with uvc_thread waiting on it. */
	uvc_frame_release();
	BC(bc_abort_step, 9);
}

static void uvc_handle_set_interface(const SceUdcdEP0DeviceRequest *req)
{
	if ((req->wIndex == STREAM_INTERFACE) && (req->wValue == 0))
		uvc_handle_video_abort();
	if (req->wIndex == AUDIO_STREAM_INTERFACE)
		audio_set_streaming(req->wValue == 1);
}

static void uvc_handle_clear_feature(const SceUdcdEP0DeviceRequest *req)
{
	switch (req->wValue) {
	case USB_FEATURE_ENDPOINT_HALT:
		if ((req->wIndex & USB_ENDPOINT_ADDRESS_MASK) ==
		    endpoints[1].endpointNumber) {
			uvc_handle_video_abort();
		}
		break;
	}
}

static volatile int usb_driver_running;	/* defined with uvc_driver_start */
#ifdef USB_PROBE
/* Callback counts, for the probe's lines (no file IO in callbacks). */
static volatile unsigned int probe_attach_n, probe_detach_n;
static volatile unsigned int probe_drv_start_n, probe_drv_stop_n;
#endif

static int uvc_udcd_process_request(int recipient, int arg, SceUdcdEP0DeviceRequest *req, void *user_data)
{
	if (arg < 0)
		return -1;
	/* udcd keeps calling us after VitaShell's USB transfer has replaced
	 * our device. We used to take its storage class request GET_MAX_LUN
	 * (0xA1 0xFE, interface 0) for one of ours and answer nothing: the PC
	 * waited 10 s before mounting (usbmon, 2026-11-25). */
	if (usb_yielded || !usb_driver_running)
		return -1;

#ifdef DEBUG
	{
		unsigned int h = bc_ep0_head;
		struct bc_ep0_req *e = &bc_ep0_ring[h % BC_EP0_RING];

		e->type = req->bmRequestType;
		e->req = req->bRequest;
		e->value = req->wValue;
		e->index = req->wIndex;
		e->length = req->wLength;
		bc_ep0_head = h + 1;
	}
#endif

	switch (req->bmRequestType) {
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0xA1 */
		switch (req->wIndex & 0xFF) {
		case CONTROL_INTERFACE:
			switch (req->wIndex >> 8) {
			case INTERFACE_CTRL_ID:
				uvc_handle_interface_ctrl_req(req);
				break;
			case INPUT_TERMINAL_ID:
				uvc_handle_input_terminal_req(req);
				break;
			case OUTPUT_TERMINAL_ID:
				uvc_handle_output_terminal_req(req);
				break;
			}
			break;
		case STREAM_INTERFACE:
			uvc_handle_video_streaming_req(req);
			break;
		case PC_HID_INTERFACE:
			if (req->bRequest == USB_REQ_GET_DESCRIPTOR &&
				(req->wValue >> 8) == HID_DESCRIPTOR_REPORT) {
				usb_ep0_req_send(hid_report_descriptor, sizeof(hid_report_descriptor));
			}
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_CLASS |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x21 */
		switch (req->wIndex & 0xFF) {
		case CONTROL_INTERFACE:
			switch (req->wIndex >> 8) {
			case INTERFACE_CTRL_ID:
				uvc_handle_interface_ctrl_req(req);
				break;
			case INPUT_TERMINAL_ID:
				uvc_handle_input_terminal_req(req);
				break;
			case OUTPUT_TERMINAL_ID:
				uvc_handle_output_terminal_req(req);
				break;
			}
			break;
		case STREAM_INTERFACE:
			uvc_handle_video_streaming_req(req);
			break;
		case PC_HID_INTERFACE:
			/*
			 * Windows may deliver HID Output Reports via a
			 * SET_REPORT class request on EP0 instead of (or in
			 * addition to) the interrupt OUT endpoint. Handle it
			 * here so PC control data isn't silently dropped if
			 * that's the transport being used.
			 */
			if (req->bRequest == HID_SET_REPORT) {
				usb_ep0_enqueue_recv_for_req(req);
			}
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x81 */
		if ((req->wIndex & 0xFF) == PC_HID_INTERFACE &&
		    req->bRequest == USB_REQ_GET_DESCRIPTOR &&
		    ((req->wValue >> 8) & 0xFF) == HID_DESCRIPTOR_REPORT) {
			usb_ep0_req_send(hid_report_descriptor, sizeof(hid_report_descriptor));
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_INTERFACE: /* 0x01 */
		switch (req->bRequest) {
		case USB_REQ_SET_INTERFACE:
			uvc_handle_set_interface(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_HOST2DEVICE |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_ENDPOINT: /* 0x02 */
		switch (req->bRequest) {
		case USB_REQ_CLEAR_FEATURE:
			uvc_handle_clear_feature(req);
			break;
		}
		break;
	case USB_CTRLTYPE_DIR_DEVICE2HOST |
	     USB_CTRLTYPE_TYPE_STANDARD |
	     USB_CTRLTYPE_REC_DEVICE: /* 0x80 */
		switch (req->wValue >> 8) {
		case 0x0A: /* USB_DT_DEBUG */
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int uvc_udcd_change_setting(int interfaceNumber, int alternateSetting, int bus)
{
	/* The PC opens (alternate 1) or closes (alternate 0) the audio device.
	 * Also seen as SET_INTERFACE in uvc_handle_set_interface; both are
	 * harmless together. */
	if (interfaceNumber == AUDIO_STREAM_INTERFACE)
		audio_set_streaming(alternateSetting == 1);
	return 0;
}

#ifdef DEBUG
/* Counted here, logged by uvc_thread (no file IO in USB callbacks). */
static volatile unsigned int dbg_attach_n, dbg_detach_n;
static volatile int dbg_attach_usb_version;
#endif

static int uvc_udcd_attach(int usb_version, void *user_data)
{
#ifdef USB_PROBE
	probe_attach_n++;
#endif
#ifdef DEBUG
	dbg_attach_usb_version = usb_version;
	dbg_attach_n++;
#endif
	/* Measure the sticks' rest position BEFORE enabling anything, while
	 * the values read are still the untouched hardware ones. */
	BC(bc_attach_step, 1);
	pc_ctrl_capture_neutral();
	BC(bc_attach_step, 2);

	ksceUdcdClearFIFO(&endpoints[1]);
	ksceUdcdClearFIFO(&endpoints[2]);
	ksceUdcdClearFIFO(&endpoints[3]);
	BC(bc_attach_step, 3);
	pc_control_release();
	BC(bc_attach_step, 4);
	pc_hid_start_receive();
	BC(bc_attach_step, 9);

#if defined(DISPLAY_OFF_OLED)
	prev_brightness = ksceOledGetBrightness();
	ksceOledDisplayOff();
#elif defined(DISPLAY_OFF_LCD)
	prev_brightness = ksceLcdGetBrightness();
	ksceLcdDisplayOff();
#endif

	return 0;
}

static void uvc_udcd_detach(void *user_data)
{
#ifdef USB_PROBE
	probe_detach_n++;
#endif
#ifdef DEBUG
	dbg_detach_n++;
#endif
	BC(bc_detach_step, 1);
	audio_set_streaming(0);
	uvc_handle_video_abort();
	BC(bc_detach_step, 2);
	ksceUdcdReqCancelAll(&endpoints[3]);
	BC(bc_detach_step, 3);
	pc_control_release();
	BC(bc_detach_step, 9);

#if defined(DISPLAY_OFF_OLED)
	ksceOledDisplayOn();
	ksceOledSetBrightness(prev_brightness);
#elif defined(DISPLAY_OFF_LCD)
	ksceLcdDisplayOn();
	ksceLcdSetBrightness(prev_brightness);
#endif
}

static void uvc_udcd_configure(int usb_version, int desc_count, SceUdcdInterfaceSettings *settings, void *user_data)
{
}

/* udcd calls these when our driver is started or stopped, including when
 * VitaShell's USB transfer stops every driver as it opens. One of the two
 * signals the retake check in uvc_thread relies on. (usb_driver_running is
 * declared above uvc_udcd_process_request.) */
#ifdef DEBUG
static volatile unsigned int dbg_drv_start_n, dbg_drv_stop_n;
#endif

static int uvc_driver_start(int size, void *p, void *user_data)
{
	usb_driver_running = 1;
#ifdef USB_PROBE
	probe_drv_start_n++;
#endif
#ifdef DEBUG
	dbg_drv_start_n++;
#endif
	return 0;
}

static int uvc_driver_stop(int size, void *p, void *user_data)
{
	usb_driver_running = 0;
#ifdef USB_PROBE
	probe_drv_stop_n++;
#endif
	stream = 0;
	uvc_frame_release();
#ifdef DEBUG
	dbg_drv_stop_n++;
#endif
	return 0;
}

static SceUdcdDriver uvc_udcd_driver = {
	.driverName			= UVC_DRIVER_NAME,
	.numEndpoints			= 5,
	.endpoints			= endpoints,
	.interface			= &interface,
	.descriptor_hi			= &devdesc_hi,
	.configuration_hi		= &config_hi,
	.descriptor			= &devdesc_full,
	.configuration			= &config_full,
	.stringDescriptors		= NULL,
	.stringDescriptorProduct	= &string_descriptor_product,
	.stringDescriptorSerial		= &string_descriptor_serial,
	.processRequest			= &uvc_udcd_process_request,
	.changeSetting			= &uvc_udcd_change_setting,
	.attach				= &uvc_udcd_attach,
	.detach				= &uvc_udcd_detach,
	.configure			= &uvc_udcd_configure,
	.start				= &uvc_driver_start,
	.stop				= &uvc_driver_stop,
	.user_data			= NULL
};

static unsigned int uvc_frame_transfer(struct uvc_frame *frame,
				       unsigned int frame_size,
				       int fid, int eof)
{
	int ret;

	frame->header[0] = UVC_PAYLOAD_HEADER_SIZE;
	frame->header[1] = UVC_STREAM_EOH;

	if (fid)
		frame->header[1] |= UVC_STREAM_FID;
	if (eof)
		frame->header[1] |= UVC_STREAM_EOF;

	ret = uvc_frame_req_submit_phycont(frame->header, frame_size);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int uvc_start(void);
int uvc_stop(void);
static int usb_takeover(void);
static void usb_restore_mtp(void);

static inline unsigned int display_to_iftu_pixelformat(unsigned int fmt)
{
	switch (fmt) {
	case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
	default:
		return SCE_IFTU_PIXELFORMAT_BGRX8888;
	case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
		return SCE_IFTU_PIXELFORMAT_BGRA5551;
	}
}

static inline unsigned int display_pixelformat_bpp(unsigned int fmt)
{
	switch (fmt) {
	case SCE_DISPLAY_PIXELFORMAT_A8B8G8R8:
	default:
		return 4;
	case SCE_DISPLAY_PIXELFORMAT_BGRA5551:
		return 2;
	}
}

static int frame_convert_to_nv12(int fid, const SceDisplayFrameBufInfo *fb_info,
					int dst_width, int dst_height)
{
	uintptr_t dst_paddr;
	uintptr_t src_paddr = fb_info->paddr;
	unsigned int src_width = fb_info->framebuf.width;
	unsigned int src_width_aligned = ALIGN(src_width, 16);
	unsigned int src_pitch = fb_info->framebuf.pitch;
	unsigned int src_height = fb_info->framebuf.height;
	unsigned int src_pixelfmt = fb_info->framebuf.pixelformat;
	unsigned int src_pixelfmt_bpp = display_pixelformat_bpp(src_pixelfmt);
	unsigned char *uvc_frame_data = uvc_frame_buffer_addr->data;

	static SceIftuCscParams RGB_to_YCbCr_JPEG_csc_params = {
		0, 0x202, 0x3FF,
		0, 0x3FF,     0,
		{
			{ 0x99, 0x12C,  0x3A},
			{0xFAA, 0xF57, 0x100},
			{0x100, 0xF2A, 0xFD7}
		}
	};

	ksceKernelGetPaddr(uvc_frame_data, &dst_paddr);

	SceIftuConvParams params;
	memset(&params, 0, sizeof(params));
	params.size = sizeof(params);
	params.unk04 = 1;
	params.csc_params1 = &RGB_to_YCbCr_JPEG_csc_params;
	params.csc_params2 = NULL;
	params.csc_control = 1;
	params.unk14 = 0;
	params.unk18 = 0;
	params.unk1C = 0;
	params.alpha = 0xFF;
	params.unk24 = 0;

	SceIftuPlaneState_updated src;
	memset(&src, 0, sizeof(src));
	src.fb.pixelformat = display_to_iftu_pixelformat(src_pixelfmt);
	src.fb.width = src_width_aligned;
	src.fb.height = src_height;
	src.fb.leftover_stride = (src_pitch - src_width_aligned) * src_pixelfmt_bpp;
	src.fb.leftover_align = 0;
	src.fb.paddr0 = src_paddr;
	src.unk20 = 0;
	src.unk24 = 0;
	src.unk28 = 0;
	src.src_w = (src_width * 0x10000) / dst_width;
	src.src_h = (src_height * 0x10000) / dst_height;
	src.dst_x = 0;
	src.dst_y = 0;
	src.src_x = 0;
	src.src_y = 0;
	src.crop_top = 0;
	src.crop_bot = 0;
	src.crop_left = 0;
	src.crop_right = 0;

	SceIftuFrameBuf dst;
	memset(&dst, 0, sizeof(dst));
	dst.pixelformat = SCE_IFTU_PIXELFORMAT_NV12;
	dst.width = dst_width;
	dst.height = dst_height;
	dst.leftover_stride = 0;
	dst.leftover_align = 0;
	dst.paddr0 = dst_paddr;
	dst.paddr1 = dst_paddr + dst_width * dst_height;

	return ksceIftuCsc(&dst, (SceIftuPlaneState *)&src, &params);
}

static int convert_and_send_frame_nv12(int fid, const SceDisplayFrameBufInfo *fb_info,
					int dst_width, int dst_height)
{
	int ret;

	ret = frame_convert_to_nv12(fid, fb_info, dst_width, dst_height);
	if (ret < 0)
		return ret;

	/* IFTU is asynchronous: its DMA finishes writing the buffer after the
	 * call returns. Wait before USB sends the frame. (Kept from the build
	 * that embedded audio in the frames; untested without it.) */
	ksceKernelDelayThreadCB(2000);

	ret = uvc_frame_transfer(uvc_frame_buffer_addr,
				 UVC_PAYLOAD_SIZE(VIDEO_FRAME_SIZE_NV12(dst_width, dst_height)),
				 fid, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static int send_frame(void)
{
	static int fid = 0;

	/* Frame buffer pas prêt ? On saute ce frame (LiveArea boot). */
	if (uvc_frame_buffer_uid < 0 || !uvc_frame_buffer_addr || !uvc_frame_buffer_addr->data)
		return 0;

	int ret;
	SceDisplayFrameBufInfo fb_info;
	int head = ksceDisplayGetPrimaryHead();

	memset(&fb_info, 0, sizeof(fb_info));
	fb_info.size = sizeof(fb_info);
	ret = ksceDisplayGetProcFrameBufInternal(-1, head, 0, &fb_info);
	if (ret < 0 || fb_info.paddr == 0)
		ret = ksceDisplayGetProcFrameBufInternal(-1, head, 1, &fb_info);
	if (ret < 0)
		return ret;

	switch (uvc_probe_control_setting.bFormatIndex) {
	case FORMAT_INDEX_UNCOMPRESSED_NV12: {
		const struct UVC_FRAME_UNCOMPRESSED(2) *frames =
			video_streaming_descriptors.frames_uncompressed_nv12;
		int cur_frame_index = uvc_probe_control_setting.bFrameIndex;
		int frame_count = sizeof(video_streaming_descriptors.frames_uncompressed_nv12) /
				  sizeof(video_streaming_descriptors.frames_uncompressed_nv12[0]);
		/* The index comes from the PC: never read past the table. */
		if (cur_frame_index < 1 || cur_frame_index > frame_count)
			cur_frame_index = 1;
		int dst_width = frames[cur_frame_index - 1].wWidth;
		int dst_height = frames[cur_frame_index - 1].wHeight;

		static int last_frame_index = 0;
		if (uvc_frame_buffer_uid < 0 || cur_frame_index != last_frame_index) {
			uvc_frame_term();
			ret = uvc_frame_init(UVC_FRAME_PADDING_SIZE + VIDEO_FRAME_SIZE_NV12(dst_width, dst_height));
			if (ret < 0) {
				return ret;
			} else {
				last_frame_index = cur_frame_index;
			}
		}

		ret = convert_and_send_frame_nv12(fid, &fb_info, dst_width, dst_height);
		if (ret < 0) {
			return ret;
		}

		break;
	}
	}

	if (ret < 0) {
		stream = 0;
		return ret;
	}

	fid ^= 1;

	return 0;
}

static int display_vblank_cb_func(int notifyId, int notifyCount, int notifyArg, void *common)
{
	static unsigned int frames = 0;
	unsigned int elapsed;

	if (!stream)
		return 0;

	frames += notifyCount;
	elapsed = FPS_TO_INTERVAL(60 / frames);

	if (elapsed >= uvc_probe_control_setting.dwFrameInterval) {
		ksceKernelSetEventFlag(uvc_event_flag_id, 1);
		frames = 0;
	}

	return 0;
}

/* Raw ksceUdcdGetDrvState values read right after the boot takeover, when
 * our driver is known to be running and MTP known to be stopped. Later
 * states are compared with these instead of being decoded: the meaning of
 * the bits did not match vitasdk's documentation on hardware (build of
 * 2026-11-21 read our running driver as stopped and retook USB from
 * itself, which hung USB until reboot). */
static int usb_states_known;
static int usb_ours_running_state, usb_mtp_stopped_state;

static void usb_record_states(void)
{
	usb_ours_running_state = ksceUdcdGetDrvState(UVC_DRIVER_NAME);
	usb_mtp_stopped_state = ksceUdcdGetDrvState("USB_MTP_Driver");
	usb_states_known = usb_ours_running_state >= 0 &&
			   usb_mtp_stopped_state >= 0;
	DLOG("reference states: VITASYNC 0x%08X (running), "
	     "USB_MTP_Driver 0x%08X (stopped), callback says running %d\n",
	     usb_ours_running_state, usb_mtp_stopped_state, usb_driver_running);
}


/* Our driver was stopped: both signals must agree. If either one doesn't
 * behave as expected, no retake happens, which is safe. */
static int usb_ours_evicted(void)
{
	int st = ksceUdcdGetDrvState(UVC_DRIVER_NAME);

	return !usb_driver_running && st >= 0 && st != usb_ours_running_state;
}

static int usb_mtp_restarted(void)
{
	int st = ksceUdcdGetDrvState("USB_MTP_Driver");

	return st >= 0 && st != usb_mtp_stopped_state;
}

/*
 * Stepping aside for VitaShell's USB transfer (build 111).
 *
 * VitaShell (also VitaDeploy's File Manager) opens USB transfer by loading
 * its usbdevice.skprx, whose module_start calls
 * ksceUdcdStopCurrentInternal(2) to stop the current USB drivers, ours
 * included; then it stops MTP and starts its storage device (05d3). Before
 * build 111 it stopped us in whatever state we were in: after a video stop
 * a frame request stayed queued forever, and the Vita froze there.
 *
 * Our hook on ksceUdcdStopCurrentInternal runs first, in the caller's
 * thread. It stops everything we send and cancels everything we have
 * queued, waits (200 ms at most) for uvc_thread to leave a frame or
 * heartbeat it may be in the middle of, then lets the call go on. From then
 * on we queue nothing and answer no request (usb_yielded), until
 * usb_retake takes USB back after VitaShell's transfer closes.
 */
static SceUID udcd_stop_current_hook_uid = -1;
static tai_hook_ref_t udcd_stop_current_ref;
/* Set by uvc_thread while it may queue something on USB (a frame or a
 * heartbeat), after checking usb_yielded. */
static volatile int uvc_busy;
#ifdef DEBUG
static volatile unsigned int dbg_yield_n, dbg_yield_wait_ms, dbg_unyield_n;
#endif

#define USB_YIELD_WAIT_MS	200

#ifdef USB_PROBE
/*
 * USB probe (make PROBE=1, test only, build id 116). Build 111 still left
 * VitaShell stuck for ~30 s (PS frees it) when USB transfer opened or
 * closed after input sent into VitaShell during a capture. A first probe
 * with a status thread and a hook on the cable-state check never saw it
 * happen (4 tries), so this one only writes a line when something is
 * called: the steps of VitaShell's startUsb()/stopUsb(), every udcd
 * start/stop/activate/deactivate (ours, VStor's, MTP's), our yield, and
 * each change the retake check sees. Lines go to
 * ur0:data/VitaSync_probe.txt, synced at once so a forced power-off keeps
 * them. Not while the system goes down (file IO blocks a shutdown).
 */
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <stdio.h>

#define PROBE_FILE	"ur0:data/VitaSync_probe.txt"

static void probe_write(const char *buf, int len, int truncate)
{
	SceUID fd;
	int st;

	ksceIoMkdir("ur0:data", 6);
	fd = ksceIoOpen(PROBE_FILE, SCE_O_WRONLY | SCE_O_CREAT |
			(truncate ? SCE_O_TRUNC : SCE_O_APPEND), 6);
	if (fd < 0)
		return;
	ksceIoWrite(fd, buf, len);
	ksceIoSyncByFd(fd, &st);
	ksceIoClose(fd);
}

/* Time in ms since boot, then the line, then our state. */
#define PROBE(...) do { \
	if (!going_down) { \
		char b__[256]; \
		int n__ = snprintf(b__, sizeof(b__), "%8u ", \
			(unsigned int)(ksceKernelGetSystemTimeWide() / 1000)); \
		n__ += snprintf(b__ + n__, sizeof(b__) - n__, __VA_ARGS__); \
		n__ += snprintf(b__ + n__, sizeof(b__) - n__, \
			" | busy %d wait %d pend %d stream %d yielded %d " \
			"running %d ctrl %d audio_idle %d ep2 %d | attach %u " \
			"detach %u drvstart %u drvstop %u\n", \
			uvc_busy, uvc_frame_waiting, uvc_frame_pending, stream, \
			usb_yielded, usb_driver_running, pc_control_active, \
			audio_idle(), ep2_busy, probe_attach_n, probe_detach_n, \
			probe_drv_start_n, probe_drv_stop_n); \
		if (n__ > (int)sizeof(b__) - 1) \
			n__ = sizeof(b__) - 1; \
		probe_write(b__, n__, 0); \
	} \
} while (0)

/*
 * One pass-through hook per function: up to 4 arguments go through
 * unchanged (all of these take 0 to 3). A line before and after, with the
 * time the call took. arg 0 is shown as a string for ksceUdcdStart/Stop
 * (a kernel pointer to the driver name), as a number otherwise.
 */
struct probe_hook {
	const char *module;
	unsigned int lib, nid;
	const char *name;
	int name_arg;		/* arg 0 is the driver name */
};

static const struct probe_hook probe_hooks[] = {
	{ "SceUdcd", 0xBC05A8FB, 0x9FD733EA, "ksceUdcdStart", 1 },
	{ "SceUdcd", 0xBC05A8FB, 0x1494293B, "ksceUdcdStop", 1 },
	{ "SceUdcd", 0xBC05A8FB, 0x4FDEA423, "ksceUdcdActivate", 0 },
	{ "SceUdcd", 0xBC05A8FB, 0x8AE87657, "ksceUdcdDeactivate", 0 },
	{ "SceMtpIfDriver", 0x5EFA4138, 0xC5327CAC, "sceMtpIfStartDriver", 0 },
	{ "SceMtpIfDriver", 0x5EFA4138, 0xC0CFA3BC, "sceMtpIfStopDriver", 0 },
	{ "SceUsbstorVStorDriver", 0x17F294B9, 0xB606F1AF, "sceUsbstorVStorStart", 0 },
	{ "SceUsbstorVStorDriver", 0x17F294B9, 0x0FD67059, "sceUsbstorVStorStop", 0 },
	{ "SceAppMgr", 0x8AF17416, 0x3EF143BF, "sceAppMgrDestroyOtherApp", 0 },
};
#define PROBE_HOOKS	(sizeof(probe_hooks) / sizeof(probe_hooks[0]))

static SceUID probe_hook_uid[PROBE_HOOKS];
static tai_hook_ref_t probe_ref[PROBE_HOOKS];

typedef int (*probe_fn)(int, int, int, int);

static int probe_call(int i, int a0, int a1, int a2, int a3)
{
	const struct probe_hook *h = &probe_hooks[i];
	struct _tai_hook_user *cur = (struct _tai_hook_user *)probe_ref[i];
	struct _tai_hook_user *nxt = (struct _tai_hook_user *)cur->next;
	SceUInt64 t = ksceKernelGetSystemTimeWide();
	int ret;

	if (h->name_arg && a0)
		PROBE("> %s(%s)", h->name, (const char *)a0);
	else
		PROBE("> %s(0x%X)", h->name, a0);
	ret = nxt ? ((probe_fn)nxt->func)(a0, a1, a2, a3)
		  : ((probe_fn)cur->old)(a0, a1, a2, a3);
	PROBE("< %s = 0x%08X in %u ms", h->name, ret,
	      (unsigned int)((ksceKernelGetSystemTimeWide() - t) / 1000));
	return ret;
}

#define PROBE_HOOK_FN(i) \
	static int probe_hook_##i(int a0, int a1, int a2, int a3) \
	{ return probe_call(i, a0, a1, a2, a3); }
PROBE_HOOK_FN(0) PROBE_HOOK_FN(1) PROBE_HOOK_FN(2) PROBE_HOOK_FN(3)
PROBE_HOOK_FN(4) PROBE_HOOK_FN(5) PROBE_HOOK_FN(6) PROBE_HOOK_FN(7)
PROBE_HOOK_FN(8)

static void probe_install(void)
{
	static const probe_fn fns[] = {
		probe_hook_0, probe_hook_1, probe_hook_2, probe_hook_3,
		probe_hook_4, probe_hook_5, probe_hook_6, probe_hook_7,
		probe_hook_8,
	};
	char line[160];
	unsigned int i;
	int n;

	_Static_assert(sizeof(fns) / sizeof(fns[0]) == PROBE_HOOKS,
		       "one hook function per probe_hooks entry");
	n = snprintf(line, sizeof(line), "%8u probe build 116\n",
		     (unsigned int)(ksceKernelGetSystemTimeWide() / 1000));
	probe_write(line, n, 1);
	for (i = 0; i < PROBE_HOOKS; i++) {
		probe_hook_uid[i] = taiHookFunctionExportForKernel(KERNEL_PID,
			&probe_ref[i], probe_hooks[i].module, probe_hooks[i].lib,
			probe_hooks[i].nid, fns[i]);
		n = snprintf(line, sizeof(line), "         hook %s: 0x%08X\n",
			     probe_hooks[i].name, probe_hook_uid[i]);
		probe_write(line, n, 0);
	}
}

static void probe_remove(void)
{
	unsigned int i;

	for (i = 0; i < PROBE_HOOKS; i++)
		if (probe_hook_uid[i] > 0)
			taiHookReleaseForKernel(probe_hook_uid[i], probe_ref[i]);
}
#else
#define PROBE(...)	((void)0)
#endif

static void usb_yield(void)
{
	int ms;

	usb_yielded = 1;
	stream = 0;
	audio_set_streaming(0);
	pc_control_release();
	/* Lets uvc_thread out if it waits for a frame (with the barrier
	 * that pairs with its uvc_busy). */
	uvc_frame_release();
	PROBE("  yield: stopped sending, frame released");

	for (ms = 0; ms < USB_YIELD_WAIT_MS && (uvc_busy || !audio_idle()); ms++)
		ksceKernelDelayThread(1000);
	PROBE("  yield: waited %d ms", ms);
#ifdef DEBUG
	dbg_yield_n++;
	dbg_yield_wait_ms = ms;
#endif

	/* Nothing of ours is being queued any more: cancel what is left.
	 * EP1 again for a frame queued in the instant before the stop. The
	 * EP3 cancel doesn't re-arm (pc_hid_recv_on_complete). */
	ksceUdcdReqCancelAll(&endpoints[1]);
	uvc_frame_pending = 0;
	ksceKernelSetEventFlag(uvc_frame_req_evflag, 1);
	ksceUdcdReqCancelAll(&endpoints[2]);
	ep2_busy = 0;
	ksceUdcdReqCancelAll(&endpoints[3]);
	ksceUdcdReqCancelAll(&endpoints[4]);
	PROBE("  yield: all requests cancelled");
}

static int udcd_stop_current_hook(int bus)
{
	int ret;
#ifdef USB_PROBE
	SceUInt64 t = ksceKernelGetSystemTimeWide();
#endif

	PROBE("> StopCurrentInternal(bus %d)", bus);
	usb_yield();
	/* TAI_CONTINUE by hand: its (int (*)()) cast means "no arguments"
	 * to this compiler (same as the descriptor hook). */
	{
		struct _tai_hook_user *cur =
			(struct _tai_hook_user *)udcd_stop_current_ref;
		struct _tai_hook_user *nxt =
			(struct _tai_hook_user *)cur->next;

		ret = nxt ? ((int (*)(int))nxt->func)(bus)
			  : ((int (*)(int))cur->old)(bus);
	}
	PROBE("< StopCurrentInternal = 0x%08X in %u ms", ret,
	      (unsigned int)((ksceKernelGetSystemTimeWide() - t) / 1000));
	/* If that didn't stop our driver after all, carry on: arm the HID
	 * receive again (attach won't, nothing reconnects). */
	if (usb_driver_running) {
		usb_yielded = 0;
		pc_hid_start_receive();
#ifdef DEBUG
		dbg_unyield_n++;
#endif
	}
	return ret;
}

/*
 * Detecting a video stop (build 115).
 *
 * The video endpoint is bulk, so the PC's uvcvideo stops a stream with
 * CLEAR_FEATURE(ENDPOINT_HALT) on EP1, which udcd handles itself and never
 * passes to uvc_udcd_process_request: uvc_handle_video_abort doesn't run,
 * stream stays 1, and the last frame stays queued with uvc_thread waiting
 * on it. Input (pc_control_apply) and the heartbeat run in uvc_thread, so
 * they stopped too until the next start (TODO.md).
 *
 * While the PC streams, uvcvideo keeps reads queued and a frame goes out in
 * ~25 ms. A frame left unread for UVC_STOP_TIMEOUT_MS means the PC stopped
 * reading: this thread then does what the abort path would have done. The
 * cancel comes from this thread, after stream = 0, as in sysevent_handler
 * and usb_yield (build 103 crashed cancelling in uvc_thread while still
 * streaming). PC input isn't released: it must keep working.
 */
#define UVC_STOP_TIMEOUT_MS	1000
#define UVC_WATCHDOG_TICK_US	100000

static SceUID uvc_watchdog_id = -1;
static volatile int uvc_watchdog_run;
static volatile unsigned int uvc_stop_detected_n;

static int uvc_watchdog_thread(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (uvc_watchdog_run) {
		ksceKernelDelayThread(UVC_WATCHDOG_TICK_US);
		if (!stream || !uvc_frame_pending || usb_yielded || going_down)
			continue;
		if ((unsigned int)(ksceKernelGetSystemTimeWide() / 1000) -
		    uvc_frame_sent_ms < UVC_STOP_TIMEOUT_MS)
			continue;

		stream = 0;
		ksceUdcdClearFIFO(&endpoints[1]);
		uvc_frame_release();
		uvc_stop_detected_n++;
		PROBE("video stop detected: frame unread for %u ms",
		      UVC_STOP_TIMEOUT_MS);
	}
	return 0;
}

/* The debug log is on ur0:, which VitaShell's USB transfer never hands to
 * the PC, so it can be written at any time. */
#define DFLUSH_SAFE()	DFLUSH()

/* After VitaShell's USB transfer closes, the port is back in MTP mode
 * (VitaShell restarts only MTP). Clear what the eviction left behind, then
 * take the port again as at boot. If that fails, give it back to MTP so the
 * port is never left dead. */
static int usb_retake(void)
{
	int ret;

	stream = 0;
	pc_control_release();
	/* A heartbeat in flight when the port was taken may never complete,
	 * which would leave ep2_busy set and stop all later heartbeats. */
	ksceUdcdReqCancelAll(&endpoints[2]);
	ep2_busy = 0;
	memcpy(&uvc_probe_control_setting, &uvc_probe_control_setting_default,
	       sizeof(uvc_probe_control_setting));

	DLOG("retaking USB\n");
	PROBE("  retake: taking USB back");
	/* Our requests may go out again (the attach that follows arms the
	 * HID receive). */
	usb_yielded = 0;
	ret = usb_takeover();
	if (ret < 0) {
		DLOG("retake failed (0x%08X), giving USB back to MTP\n", ret);
		PROBE("  retake failed 0x%08X", ret);
		usb_yielded = 1;
		usb_restore_mtp();
	} else {
		DLOG("retake OK\n");
		PROBE("  retake OK");
	}
	DFLUSH_SAFE();
	return ret;
}

#define USB_CHECK_INTERVAL_US	1000000	/* driver states read once a second */
/* Checks in a row (one a second) before retaking. VitaShell restarts MTP
 * in the middle of closing USB transfer, then still unloads its module and
 * re-reads the memory card. Retaking 1.5 s after MTP came back (2 checks,
 * builds 95-112) came while that was going on; 5 leaves it the time. */
#define USB_FREE_CHECKS		5
#define USB_RETAKE_MAX_FAILS	3	/* then stop trying until next load */

static int uvc_thread(SceSize args, void *argp)
{
	SceUID display_vblank_cb_uid;

	stream = 0;
	DLOG("uvc_thread: started\n");
	/* Retaking the port is only armed once the boot takeover worked and
	 * the reference states could be read. */
	if (DCALL(uvc_start()) == 0)
		usb_record_states();
	rear_panel_info_read();
	DLOG("rear panel: X %d-%d, Y %d-%d, force %u-%u\n",
	     rear_panel_info.minAaX, rear_panel_info.maxAaX,
	     rear_panel_info.minAaY, rear_panel_info.maxAaY,
	     rear_panel_info.minForce, rear_panel_info.maxForce);
	int usb_free_checks = 0, usb_retake_fails = 0;
	SceUInt64 next_usb_check = 0;
	/* First write to ux0:. By now (15 s+ after start) henlo has mounted
	 * the SD2Vita, so the log lands on the card the PC mounts. */
	DFLUSH();

#ifdef DEBUG
	unsigned int dbg_attach_seen = 0, dbg_detach_seen = 0, dbg_yield_seen = 0;
	unsigned int dbg_stop_seen = 0;
	int dbg_stream_seen = 0, dbg_rx_logged = 0, dbg_last_frame_err = 0;
	unsigned int dbg_rx_err_seen = 0;
	int dbg_ours_seen = -1, dbg_mtp_seen = -1, dbg_running_seen = -1;
#endif

	display_vblank_cb_uid = ksceKernelCreateCallback("uvc_display_vblank", 0,
							 display_vblank_cb_func, NULL);

	ksceDisplayRegisterVblankStartCallback(display_vblank_cb_uid);

	SceUInt64 last_hb = ksceKernelGetSystemTimeWide();
	unsigned int seen_rx = rx_sequence;
	SceUInt64 last_rx_change = last_hb;
	SceUInt64 next_power_tick = 0;

	while (uvc_thread_run) {
		unsigned int out_bits;

		BC(bc_uvc_where, 1);
		int ret = ksceKernelWaitEventFlagCB(uvc_event_flag_id, 1,
			SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
			&out_bits, (SceUInt32[]){8000});

		/* Re-apply the emulated pad continuously: the emulation
		 * expires after uiMake sampling counts. Must run from real
		 * thread context, not from the vblank callback. */
		pc_control_apply();

		SceUInt64 now = ksceKernelGetSystemTimeWide();

		/* Garde-eveil : pendant le streaming, annule TOUS les
		 * minuteurs d'economie d'energie du systeme (veille auto,
		 * extinction OLED, assombrissement OLED). Sinon la Vita se
		 * met en standby toute seule au bout d'un moment (et l'ecran
		 * s'assombrit d'abord). Aucun impact en jeu : seule la
		 * bascule power-save est concernee, aucune entree injectee. */
		if (stream && now >= next_power_tick) {
			ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
			ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
			ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
			next_power_tick = now + 10 * 1000 * 1000;
		}

		/* (Re)install touch interceptors once PC reports flow. */
		if (touch_hooks_wanted && !touch_hooks_installed) {
			touch_hooks_install();
			touch_hooks_wanted = 0;
		}

		if (now - last_hb >= 500000) { /* heartbeat every ~500 ms */
			last_hb = now;
#if ENABLE_HEARTBEAT
			/* uvc_busy before the check pairs with usb_yield. */
			uvc_busy = 1;
			__sync_synchronize();
			if (!usb_yielded)
				pc_hid_send_input_heartbeat();
			uvc_busy = 0;
#endif
		}

		/* Take the port back once VitaShell's USB transfer has closed.
		 * Opening it stops every udcd driver (ours too) and MTP, and
		 * closing it restarts only MTP. So "ours evicted + MTP back"
		 * can only mean it has closed. */
		/* Not while the system takes USB down for a sleep or reboot. */
		if (usb_states_known && usb_retake_fails < USB_RETAKE_MAX_FAILS &&
		    !going_down && now >= next_usb_check) {
			next_usb_check = now + USB_CHECK_INTERVAL_US;
#ifdef DEBUG
			{
				int ours = ksceUdcdGetDrvState(UVC_DRIVER_NAME);
				int mtp = ksceUdcdGetDrvState("USB_MTP_Driver");

				if (ours != dbg_ours_seen || mtp != dbg_mtp_seen ||
				    usb_driver_running != dbg_running_seen) {
					dbg_ours_seen = ours;
					dbg_mtp_seen = mtp;
					dbg_running_seen = usb_driver_running;
					DLOG("states: VITASYNC 0x%08X, USB_MTP_Driver 0x%08X, "
					     "callback says running %d (starts %u, stops %u)\n",
					     ours, mtp, dbg_running_seen,
					     dbg_drv_start_n, dbg_drv_stop_n);
					DFLUSH_SAFE();
				}
			}
#endif
#ifdef USB_PROBE
			{
				static int seen_ours = -1, seen_mtp = -1, seen_run = -1;
				int ours = ksceUdcdGetDrvState(UVC_DRIVER_NAME);
				int mtp = ksceUdcdGetDrvState("USB_MTP_Driver");

				if (ours != seen_ours || mtp != seen_mtp ||
				    usb_driver_running != seen_run) {
					seen_ours = ours;
					seen_mtp = mtp;
					seen_run = usb_driver_running;
					PROBE("  retake check: VITASYNC 0x%08X (ref 0x%08X), "
					      "MTP 0x%08X (ref 0x%08X), free checks %d",
					      ours, usb_ours_running_state, mtp,
					      usb_mtp_stopped_state, usb_free_checks);
				}
			}
#endif
			if (usb_ours_evicted() && usb_mtp_restarted()) {
				if (++usb_free_checks >= USB_FREE_CHECKS) {
					usb_free_checks = 0;
					BC(bc_uvc_where, 7);
					if (usb_retake() < 0)
						usb_retake_fails++;
					BC(bc_uvc_where, 8);
				}
			} else {
				usb_free_checks = 0;
			}
		}

		/* Watchdog: if reports stop arriving for >1 s while a
		 * control session is active (app closed, crashed or
		 * unplugged), release everything. NB : on ne retire PLUS
		 * les hooks tactiles ici � taiHookReleaseForKernel sur
		 * une fonction qu'un thread du jeu est en train d'ex�cuter
		 * = use-after-free = panique al�atoire au lancement d'un
		 * jeu (le silence USB >1 s arrive pr�cis�ment pendant la
		 * fen�tre de boot du jeu). Les hooks restent pos�s et
		 * font du pass-through tant que pc_touch_active=0 ; seul
		 * module_stop les d�pose (syst�me au repos). */
		if (rx_sequence != seen_rx) {
			seen_rx = rx_sequence;
			last_rx_change = now;
		} else if (pc_control_active && now - last_rx_change > 1000000ULL) {
			pc_control_release();
			pc_touch_release_all();
			touch_hooks_wanted = 0;
			/* Une s�quence exit � moiti� faite ne doit pas tirer
			 * sur une session morte. */
			exit_phase = 0;
			kill_hold = 0;
		}

		/* Exit game diff�r� : ex�cut� ICI (notre propre thread kernel,
		 * contexte prouv� s�r) et jamais dans les rappels USB/touch.
		 * S�curit�s empil�es : �chap doit avoir �t� revu rel�ch�,
		 * PID obligatoirement propri�taire du framebuffer primaire
		 * (= processus au premier plan), cooldown entre tentatives.
		 * Chaque refus laisse une raison lisible au diagnostic :
		 * 1=verrou rel�chement 2=cooldown 3=hors session
		 * 4=PID absent (jeu ne lit pas le tactile ?) 5=refus �cran
		 * 6=�chec KillProcess. */
		if (exit_game_request) {
			exit_game_request = 0;
			exit_req_seen = 1;
			if (!pc_control_active) {
				exit_block = 3;
			} else if (game_pid <= 0) {
				exit_block = 4;
			} else if (exit_phase != 0) {
				exit_block = 0; /* s�quence d�j� en cours */
			} else if (!exit_seen_idle || !exit_armed || exit_cooldown != 0) {
				exit_block = exit_cooldown != 0 ? 2 :
					     (!exit_seen_idle ? 1 : 2);
			} else {
				int head = ksceDisplayGetPrimaryHead();
				SceDisplayFrameBufInfo fb;
				int owns = 0;

				memset(&fb, 0, sizeof(fb));
				fb.size = sizeof(fb);
				if (ksceDisplayGetProcFrameBufInternal(game_pid, head, 0, &fb) >= 0)
					owns = 1;
				else {
					memset(&fb, 0, sizeof(fb));
					fb.size = sizeof(fb);
					if (ksceDisplayGetProcFrameBufInternal(game_pid, head, 1, &fb) >= 0)
						owns = 1;
				}

				if (!owns) {
					kill_result = 3; /* pas au 1er plan : refus� */
					exit_block = 5;
					exit_cooldown = 125; /* ~1 s */
				} else {
					/* Phase 1 : figer la vid�o et rendre les
					 * �mulations MAINTENANT ; le tir part
					 * quand kill_hold tombe � z�ro. On
					 * m�morise la cible car release() purge
					 * game_pid. */
					exit_target_pid = game_pid;
					pc_control_release();
					pc_touch_release_all();
					kill_hold = 60; /* ~0,5 s de drain */
					exit_phase = 1;
					exit_block = 0;
				}
			}
		}

		if (exit_phase == 1 && kill_hold == 0) {
			int r = ksceAppMgrKillProcess(exit_target_pid);
			kill_result = (r == 0) ? 1 : 2;
			exit_block = (r == 0) ? 0 : 6;
			exit_phase = 2;
			kill_hold = 250; /* ~2 s sans vid�o : le syst�me
					  * d�monte le jeu et affiche la
					  * LiveArea en paix. */
			exit_seen_idle = 0;
			exit_cooldown = 250;
		}
		if (exit_phase == 2 && kill_hold == 0)
			exit_phase = 0; /* reprise de la vid�o */

		/* R�armement : commande revenue au repos + cooldown �coul�. */
		if (!exit_armed && exit_seen_idle && exit_cooldown == 0)
			exit_armed = 1;
		if (exit_cooldown > 0)
			exit_cooldown--;
		if (kill_hold > 0)
			kill_hold--;

		uvc_busy = 1;
		__sync_synchronize();
		if (ret == 0 && stream && kill_hold == 0 && !going_down &&
		    !usb_yielded) {
			BC(bc_uvc_where, 3);
			int sf = send_frame();
			uvc_busy = 0;
			BC(bc_uvc_where, 4);
#ifdef DEBUG
			/* Log each new error code once, not every frame. */
			if (sf < 0 && sf != dbg_last_frame_err) {
				DLOG("send_frame = 0x%08X\n", sf);
				DFLUSH_SAFE();
			}
			if (sf < 0)
				dbg_last_frame_err = sf;
#else
			(void)sf;
#endif
		}
		uvc_busy = 0;

#ifdef DEBUG
		if (uvc_stop_detected_n != dbg_stop_seen) {
			dbg_stop_seen = uvc_stop_detected_n;
			DLOG("video stop detected #%u (frame unread for %u ms)\n",
			     dbg_stop_seen, UVC_STOP_TIMEOUT_MS);
			DFLUSH_SAFE();
		}
		if (dbg_yield_n != dbg_yield_seen) {
			dbg_yield_seen = dbg_yield_n;
			DLOG("yielded USB #%u (StopCurrentInternal), waited %u ms, "
			     "un-yielded %u\n", dbg_yield_seen, dbg_yield_wait_ms,
			     dbg_unyield_n);
			DFLUSH_SAFE();
		}
		if (dbg_attach_n != dbg_attach_seen) {
			dbg_attach_seen = dbg_attach_n;
			DLOG("attach #%u (usb_version %d)\n",
			     dbg_attach_seen, dbg_attach_usb_version);
			DFLUSH_SAFE();
		}
		if (dbg_detach_n != dbg_detach_seen) {
			dbg_detach_seen = dbg_detach_n;
			DLOG("detach #%u\n", dbg_detach_seen);
			DFLUSH_SAFE();
		}
		if (stream != dbg_stream_seen) {
			dbg_stream_seen = stream;
			if (stream)
				DLOG("stream on: format %u frame %u interval %u\n",
				     uvc_probe_control_setting.bFormatIndex,
				     uvc_probe_control_setting.bFrameIndex,
				     (unsigned int)uvc_probe_control_setting.dwFrameInterval);
			else
				DLOG("stream off\n");
			dbg_last_frame_err = 0;
			DFLUSH_SAFE();
		}
		if (!dbg_rx_logged && pc_hid_recv_ok > 0) {
			dbg_rx_logged = 1;
			DLOG("first HID report received\n");
			DFLUSH_SAFE();
		}
		/* Receive errors can repeat fast: report at heartbeat pace. */
		if (now == last_hb && pc_hid_recv_err != dbg_rx_err_seen) {
			dbg_rx_err_seen = pc_hid_recv_err;
			DLOG("HID receive errors: %u\n", dbg_rx_err_seen);
			DFLUSH_SAFE();
		}
#endif
		/* NB : PAS de uvc_frame_term() sur timeout ! L'ancien code
		 * libérait le buffer toutes les 8 ms d'inactivité, parfois
		 * pendant que le DMA USB relisait encore le dernier frame
		 * envoyé → panic LiveArea aléatoire. Le buffer vit jusqu'à
		 * uvc_stop/module_stop, c'est ~700 Ko, la stabilité avant tout. */
	}

	ksceDisplayUnregisterVblankStartCallback(display_vblank_cb_uid);
	ksceKernelDeleteCallback(display_vblank_cb_uid);

	uvc_stop();

	return 0;
}

static int uvc_frame_init(unsigned int size)
{
	int ret;

	const int use_cdram = 0;
	SceKernelAllocMemBlockKernelOpt opt;
	SceKernelMemBlockType type;
	SceKernelAllocMemBlockKernelOpt *optp;

	if (use_cdram) {
		type = 0x40408006;
		size = ALIGN(size, 256 * 1024);
		optp = NULL;
	} else {
		type = 0x10208006;
		size = ALIGN(size, 4 * 1024);
		memset(&opt, 0, sizeof(opt));
		opt.size = sizeof(opt);
		opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT |
			   SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
		opt.alignment = 4 * 1024;
		optp = &opt;
	}

	uvc_frame_buffer_uid = ksceKernelAllocMemBlock("uvc_frame_buffer", type, size, optp);
	if (uvc_frame_buffer_uid < 0) {
		return uvc_frame_buffer_uid;
	}

	ret = ksceKernelGetMemBlockBase(uvc_frame_buffer_uid, (void **)&uvc_frame_buffer_addr);
	if (ret < 0) {
		ksceKernelFreeMemBlock(uvc_frame_buffer_uid);
		uvc_frame_buffer_uid = -1;
		return ret;
	}

	return 0;
}

static int uvc_frame_term()
{
	if (uvc_frame_buffer_uid >= 0) {
		ksceKernelFreeMemBlock(uvc_frame_buffer_uid);
		uvc_frame_buffer_uid = -1;
	}

	return 0;
}

/* Take the USB port: stop the system's drivers (MTP...) and start ours.
 * Used at boot by uvc_start, and again by usb_retake after VitaShell's USB
 * transfer had taken the port. */
static int usb_takeover(void)
{
	int ret;

	ret = DCALL(ksceUdcdDeactivate());
	if (ret < 0 && ret != SCE_UDCD_ERROR_INVALID_ARGUMENT) {
		return ret;
	}

	DCALL(ksceUdcdStop("USB_MTP_Driver", 0, NULL));
	DCALL(ksceUdcdStop("USBPSPCommunicationDriver", 0, NULL));
	DCALL(ksceUdcdStop("USBSerDriver", 0, NULL));
	DCALL(ksceUdcdStop("USBDeviceControllerDriver", 0, NULL));

	ret = DCALL(ksceUdcdStart("USBDeviceControllerDriver", 0, NULL));
	if (ret < 0) {
		return ret;
	}

	ret = DCALL(ksceUdcdStart(UVC_DRIVER_NAME, 0, NULL));
	if (ret < 0) {
		goto err_start_uvc_driver;
	}

	ret = DCALL(ksceUdcdActivate(UVC_USB_PID));
	if (ret < 0) {
		goto err_activate;
	}

	return 0;

err_activate:
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
err_start_uvc_driver:
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	return ret;
}

/* Give the port back to the system's MTP mode, as uvc_stop does. */
static void usb_restore_mtp(void)
{
	DCALL(ksceUdcdStart("USBDeviceControllerDriver", 0, NULL));
	DCALL(ksceUdcdStart("USB_MTP_Driver", 0, NULL));
	DCALL(ksceUdcdActivate(0x4E4));
}

int uvc_start(void)
{
	int ret;

	DCALL(ksceDisplayWaitSetFrameBufCB());

	ksceKernelDelayThreadCB(15 * 1000 * 1000);
	DLOG("uvc_start: 15 s delay over\n");

	ret = usb_takeover();
	if (ret < 0) {
		return ret;
	}

	ret = DCALL(uvc_frame_req_init());
	if (ret < 0) {
		goto err_alloc_uvc_frame_req;
	}

	/* Initialisation PRÉCOCE du frame buffer (AVANT 1er vblank)
	 * pour éviter tout crash sur frame LiveArea au format imprévu. */
	ret = DCALL(uvc_frame_init(UVC_FRAME_PADDING_SIZE + VIDEO_FRAME_SIZE_NV12(960, 544)));
	if (ret < 0) {
		goto err_alloc_uvc_frame_req;
	}

	memcpy(&uvc_probe_control_setting, &uvc_probe_control_setting_default,
	       sizeof(uvc_probe_control_setting));

	return 0;

err_alloc_uvc_frame_req:
	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	return ret;
}

int uvc_stop(void)
{
	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdStart("USB_MTP_Driver", 0, NULL);
	ksceUdcdActivate(0x4E4);

	uvc_frame_term();

	return 0;
}

static SceUID SceUdcd_sub_01E1128C_hook_uid = -1;
static tai_hook_ref_t SceUdcd_sub_01E1128C_ref;

static int SceUdcd_sub_01E1128C_hook_func(const SceUdcdConfigDescriptor *config_descriptor, void *desc_data)
{
	int ret;
	SceUdcdConfigDescriptor *dst = desc_data;

	/* Chain-aware: respect hooks installed after ours. */
	{
		struct _tai_hook_user *cur =
			(struct _tai_hook_user *)SceUdcd_sub_01E1128C_ref;
		struct _tai_hook_user *nxt =
			(struct _tai_hook_user *)cur->next;
		ret = nxt
			? ((int (*)(const SceUdcdConfigDescriptor *, void *))nxt->func)
			  (config_descriptor, desc_data)
			: ((int (*)(const SceUdcdConfigDescriptor *, void *))cur->old)
			  (config_descriptor, desc_data);
	}

	/* Only into our own descriptor: this function builds every driver's,
	 * and VitaShell's storage device (05d3) used to get our video IAD
	 * too (lsusb -v, 2026-11-25). */
	if ((config_descriptor == &confdesc_hi ||
	     config_descriptor == &confdesc_full) &&
	    dst->wTotalLength == config_descriptor->wTotalLength) {
		memmove(desc_data + USB_DT_CONFIG_SIZE + sizeof(interface_association_descriptor),
			desc_data + USB_DT_CONFIG_SIZE,
			config_descriptor->wTotalLength - USB_DT_CONFIG_SIZE);

		memcpy(desc_data + USB_DT_CONFIG_SIZE, interface_association_descriptor,
		       sizeof(interface_association_descriptor));

		dst->wTotalLength += sizeof(interface_association_descriptor);

		ksceKernelDcacheCleanRange(desc_data, dst->wTotalLength);
	}

	return ret;
}

#ifdef DEBUG
/* Writes breadcrumb changes and every EP0 request to the log 5 times a
 * second, plus a line every 5 s showing where uvc_thread is. Runs on its
 * own, so it keeps logging if uvc_thread or a USB callback hangs. */
static SceUID dbg_logger_id = -1;
static volatile int dbg_logger_run;

static int dbg_logger_thread(SceSize args, void *argp)
{
	unsigned int seen[9], ep0_seen = 0, ticks = 0;

	(void)args; (void)argp;
	memset(seen, 0, sizeof(seen));
	while (dbg_logger_run) {
		unsigned int now[9];
		unsigned int head = bc_ep0_head;
		int changed;

		now[0] = dbg_attach_n;   now[1] = bc_attach_step;
		now[2] = dbg_detach_n;   now[3] = bc_detach_step;
		now[4] = bc_abort_n;     now[5] = bc_abort_step;
		now[6] = dbg_drv_start_n; now[7] = dbg_drv_stop_n;
		now[8] = bc_ep3_done_n;
		changed = memcmp(now, seen, sizeof(now)) != 0;

		if (head - ep0_seen > BC_EP0_RING) {
			DLOG("bc: %u EP0 requests not logged\n",
			     head - ep0_seen - BC_EP0_RING);
			ep0_seen = head - BC_EP0_RING;
		}
		while (ep0_seen != head) {
			const struct bc_ep0_req *e = &bc_ep0_ring[ep0_seen % BC_EP0_RING];

			DLOG("bc: ep0 type %02X req %02X value %04X index %04X len %u\n",
			     e->type, e->req, e->value, e->index, e->length);
			ep0_seen++;
		}
		if (changed || ++ticks >= 25) {
			DLOG("bc: attach %u step %u, detach %u step %u, abort %u step %u, "
			     "drv start %u stop %u, ep3 done %u rc 0x%08X, uvc_thread at %u\n",
			     now[0], now[1], now[2], now[3], now[4], now[5],
			     now[6], now[7], now[8], bc_ep3_last_rc, bc_uvc_where);
			memcpy(seen, now, sizeof(now));
			ticks = 0;
		}
		log_flush();
		ksceKernelDelayThread(200000);
	}
	return 0;
}
#endif

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	int ret;
	tai_module_info_t SceUdcd_modinfo;

#ifdef DEBUG
	log_reset();
#endif
	DLOG("VitaSync debug build " __DATE__ " " __TIME__ "\n");
#ifdef DEBUG
	dbg_logger_run = 1;
	dbg_logger_id = DCALL(ksceKernelCreateThread("vitasync_log", dbg_logger_thread,
						     0x40, 0x4000, 0, 0x10000, 0));
	if (dbg_logger_id >= 0)
		ksceKernelStartThread(dbg_logger_id, 0, NULL);
#endif
	sysevent_uid = DCALL(ksceKernelRegisterSysEventHandler("vitasync_sysevent",
							       sysevent_handler, NULL));

	SceUdcd_modinfo.size = sizeof(SceUdcd_modinfo);
	DCALL(taiGetModuleInfoForKernel(KERNEL_PID, "SceUdcd", &SceUdcd_modinfo));

	SceUdcd_sub_01E1128C_hook_uid = DCALL(taiHookFunctionOffsetForKernel(KERNEL_PID,
		&SceUdcd_sub_01E1128C_ref, SceUdcd_modinfo.modid, 0,
		0x01E1128C - 0x01E10000, 1, SceUdcd_sub_01E1128C_hook_func));

	/* VitaShell's USB transfer stops USB through this: step aside first
	 * (usb_yield). SceUdcdForDriver, ksceUdcdStopCurrentInternal. */
	udcd_stop_current_hook_uid = DCALL(taiHookFunctionExportForKernel(KERNEL_PID,
		&udcd_stop_current_ref, "SceUdcd", 0xBC05A8FB, 0x0F3595AE,
		udcd_stop_current_hook));
#ifdef USB_PROBE
	probe_install();
#endif

	/* Touch hooks are installed lazily once the PC app talks to us,
	 * and fully removed again when it goes silent (watchdog below). */
	touch_hooks_install();
	touch_hooks_wanted = 1;
	DLOG("touch hooks: 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
	     touch_peek_hook_uid, touch_peek_region_hook_uid,
	     touch_read_hook_uid, touch_read_region_hook_uid,
	     touch_peek2_hook_uid, touch_read2_hook_uid);

	uvc_thread_id = DCALL(ksceKernelCreateThread("uvc_thread", uvc_thread,
					       0x3C, 0x1000, 0, 0x10000, 0));
	if (uvc_thread_id < 0) {
		goto err_return;
	}

	uvc_event_flag_id = DCALL(ksceKernelCreateEventFlag("uvc_event_flag", 0,
						      0, NULL));
	if (uvc_event_flag_id < 0) {
		goto err_destroy_thread;
	}

	ret = DCALL(ksceUdcdRegister(&uvc_udcd_driver));
	if (ret < 0) {
		goto err_delete_event_flag;
	}

	uvc_thread_run = 1;

	/* USB audio (Vita sound to the PC): optional, the rest works without. */
	ret = DCALL(audio_init(&endpoints[4]));

	/* Last line before uvc_thread runs: from here on only that thread
	 * writes to the log buffer. */
	DLOG("module_start: starting uvc_thread\n");
	uvc_thread_run = 1;
	ret = ksceKernelStartThread(uvc_thread_id, 0, NULL);
	if (ret < 0) {
		DLOG("ksceKernelStartThread(uvc_thread) = 0x%08X\n", ret);
		goto err_unregister;
	}

	/* Optional: without it, input stops after a video stop, as before
	 * build 115. */
	uvc_watchdog_run = 1;
	uvc_watchdog_id = ksceKernelCreateThread("uvc_watchdog",
						 uvc_watchdog_thread, 0x40, 0x1000,
						 0, 0x10000, 0);
	if (uvc_watchdog_id >= 0)
		ksceKernelStartThread(uvc_watchdog_id, 0, NULL);

	return SCE_KERNEL_START_SUCCESS;

err_unregister:
	ksceUdcdUnregister(&uvc_udcd_driver);
err_delete_event_flag:
	ksceKernelDeleteEventFlag(uvc_event_flag_id);
err_destroy_thread:
	ksceKernelDeleteThread(uvc_thread_id);
err_return:
	DFLUSH(); /* may fail if ux0: isn't mounted yet */
	return SCE_KERNEL_START_FAILED;
}

int module_stop(SceSize argc, const void *args)
{
	if (sysevent_uid >= 0) {
		ksceKernelUnregisterSysEventHandler(sysevent_uid);
		sysevent_uid = -1;
	}
#ifdef DEBUG
	if (dbg_logger_id >= 0) {
		dbg_logger_run = 0;
		ksceKernelWaitThreadEnd(dbg_logger_id, NULL, NULL);
		ksceKernelDeleteThread(dbg_logger_id);
		dbg_logger_id = -1;
	}
#endif
	pc_control_release();
	ksceUdcdReqCancelAll(&endpoints[3]);
	uvc_thread_run = 0;
	audio_set_streaming(0);

	ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	ksceKernelWaitThreadEnd(uvc_thread_id, NULL, NULL);

	audio_fini();

	ksceKernelDeleteEventFlag(uvc_event_flag_id);
	ksceKernelDeleteThread(uvc_thread_id);

	/* It may release a frame (uvc_frame_req_evflag): stop it first. */
	if (uvc_watchdog_id >= 0) {
		uvc_watchdog_run = 0;
		ksceKernelWaitThreadEnd(uvc_watchdog_id, NULL, NULL);
		ksceKernelDeleteThread(uvc_watchdog_id);
		uvc_watchdog_id = -1;
	}

	uvc_frame_req_fini();

	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdUnregister(&uvc_udcd_driver);

	if (SceUdcd_sub_01E1128C_hook_uid > 0) {
		taiHookReleaseForKernel(SceUdcd_sub_01E1128C_hook_uid,
			SceUdcd_sub_01E1128C_ref);
	}
#ifdef USB_PROBE
	probe_remove();
#endif
	if (udcd_stop_current_hook_uid > 0) {
		taiHookReleaseForKernel(udcd_stop_current_hook_uid,
			udcd_stop_current_ref);
		udcd_stop_current_hook_uid = -1;
	}

	if (touch_peek_hook_uid > 0)
		taiHookReleaseForKernel(touch_peek_hook_uid, touch_peek_ref);
	if (touch_peek_region_hook_uid > 0)
		taiHookReleaseForKernel(touch_peek_region_hook_uid, touch_peek_region_ref);
	if (touch_read_hook_uid > 0)
		taiHookReleaseForKernel(touch_read_hook_uid, touch_read_ref);
	if (touch_read_region_hook_uid > 0)
		taiHookReleaseForKernel(touch_read_region_hook_uid, touch_read_region_ref);
	if (touch_peek2_hook_uid > 0)
		taiHookReleaseForKernel(touch_peek2_hook_uid, touch_peek2_ref);
	if (touch_read2_hook_uid > 0)
		taiHookReleaseForKernel(touch_read2_hook_uid, touch_read2_ref);
	if (svc_peek_hook_uid > 0)
		taiHookReleaseForKernel(svc_peek_hook_uid, svc_peek_ref);
	if (svc_peek_region_hook_uid > 0)
		taiHookReleaseForKernel(svc_peek_region_hook_uid, svc_peek_region_ref);
	if (svc_read_hook_uid > 0)
		taiHookReleaseForKernel(svc_read_hook_uid, svc_read_ref);
	if (svc_read_region_hook_uid > 0)
		taiHookReleaseForKernel(svc_read_region_hook_uid, svc_read_region_ref);

	return SCE_KERNEL_STOP_SUCCESS;
}































