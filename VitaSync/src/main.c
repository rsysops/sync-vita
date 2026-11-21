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

#define ALIGN(x, a)			(((x) + ((a) - 1)) & ~((a) - 1))
#define UNUSED(x)			((void)x)

#define UVC_DRIVER_NAME			"VITASYNC"
#define UVC_USB_PID			0x1337

#define MAX_UVC_VIDEO_FRAME_SIZE	VIDEO_FRAME_SIZE_NV12(1280, 720)

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
static int stream;

/* Worker audio d�di� (architecture psvita-usb-audio-midi) : l'envoi
 * USB ne doit PAS vivre dans uvc_thread � ce thread ne se r�veille
 * qu'au rythme des frames vid�o (~30 Hz), et son contexte est li� au
 * callback vblank. Un worker ind�pendant boucle � intervalle fixe. */
static SceUID audio_thread_id = -1;
static volatile int audio_thread_run;

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
static volatile unsigned char pc_lx = 128, pc_ly = 128, pc_rx = 128, pc_ry = 128;
static volatile unsigned int pc_hid_recv_ok;
static volatile unsigned int pc_hid_recv_err;
static volatile int pc_ret_button_emu;
static volatile int pc_ret_analog_emu;

/* ---- Touch injection (PC mouse click -> Vita front touchscreen) ---- */
#define TOUCHSCREEN_WIDTH  1920
#define TOUCHSCREEN_HEIGHT 1088

static volatile int pc_touch_active;
static volatile unsigned int pc_touch_x;
static volatile unsigned int pc_touch_y;
static volatile unsigned char pc_touch_id;
static volatile unsigned char h_peek, h_peekR, h_read, h_readR, h_peek2, h_read2;
static volatile unsigned int touch_patch_hits;

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

#define ENABLE_AUDIO_CAPTURE 1 /* gardé pour declarations */
#define ENABLE_AUDIO_PUSH 0
#define ENABLE_AUDIO_PIDGRAB 0
#define ENABLE_AUDIO_HOOKTRY 0
#define ENABLE_AUDIO_BISECT_PASSTHRU 0
#define ENABLE_AUDIO_RECORDERS 0
#define ENABLE_AUDIO_STREAM 0
#define ENABLE_AUDIO_MUTE 0
#define ENABLE_AUDIO_INJECT 0
#define AUD_BISECT_SKIP_RING 0 /* terminé : ring retiré du chemin data */
#define AUD_BISECT_SILENT_PUBLISH 0 /* terminé */
#define AUD_BISECT_PROBE_ONLY 1 /* bisecte 75 : 1 seule lecture pBuffer */
#if ENABLE_AUDIO_CAPTURE
/* ---- Audio redirection ----
 * Hook sceAudioOutOutput: convert the game's PCM to 24 kHz mono s16,
 * queue it for the HID IN pipe (report id 1), then silence the local
 * buffer so audio plays ONLY on the PC. */
static tai_hook_ref_t audio_out_ref;
static SceUID audio_out_hook_uid = -1;
/* SIGNATURE RÉELLE (cf. VITA2PC) : sceAudioOutOutput n'a que DEUX
 * paramètres — la taille vient du GRANULE du port fixé à l'OpenPort.
 * Notre ancien 3e argument fantôme lisait un registre résiduel :
 * valeurs absurdes (etat=6) et lectures hors buffer = panics. */
typedef int (*audio_out_fn_t)(int, const void *);

/* GetConfig : source de vérité du driver (NID 0x9C8EDAEA).
 * Déclaré tôt car l'hook Output l'appelle dans le contexte du jeu. */
typedef int (*audio_getcfg_fn_t)(int, int);
static tai_hook_ref_t audio_getcfg_ref;
static SceUID audio_getcfg_uid = -1;
#define ARING_SIZE 8192
#define ARING_MASK (ARING_SIZE - 1)
/* Petit anneau statique (8 Ko ~ 170 ms) : reste sous les limites de
 * chargement du module, contrairement aux 32 Ko qui faisaient �chouer
 * l'�num�ration. */
static unsigned char aring[ARING_SIZE] __attribute__((aligned(64)));
static volatile unsigned int ar_head, ar_tail;
/* Mutex kernel : le hook tourne sur les c�urs des threads du jeu, le
 * worker audio sur un autre. Les barri�res compilateur seules ont
 * montr� leurs limites (hook voyait � plein �, worker voyait � vide �
 * simultan�ment) � le mutex SceThreadmgr impose la coh�rence r�elle
 * entre c�urs autour de chaque section critique. */
static SceUID aring_mutex = -1;
/* M�moire des param�tres par NUM�RO DE PORT (pas par PID).
 * Le pilote audio est unique : tous les processus qui jouent sur
 * le m�me num�ro de port utilisent la m�me config mat�rielle.
 * Quand Output cr�e un slot fra�chement (pid inconnu), il emprunte
 * ces valeurs au lieu de rester � ch=0 fr=0 ? etat=2. */
static int port_ch_mem[8];  /* 0 = jamais vu */
static int port_fr_mem[8];
static volatile int audio_capture_on;
static unsigned char audio_seq;
static volatile int audio_mod_found;
static tai_hook_ref_t audio_open_ref, audio_setcfg_ref;
static SceUID audio_open_uid = -1, audio_setcfg_uid = -1;
typedef int (*audio_open_fn_t)(int, int, int, int);
typedef int (*audio_setcfg_fn_t)(int, unsigned int, int, int);

/* Diag audio (hb[59] bas): 0=pas d'appel 1=port invalide 2=params absents
 * 3=taille non confirm�e 4=nouvelle taille apprise 5=lecture OK */
static volatile unsigned char aud_diag;
static volatile unsigned char aud_diag_port;
static volatile unsigned int aud_pushes;
static volatile unsigned char usb_audio_ok_cnt;  /* successful EP2 sends per 500ms */
static volatile unsigned char usb_audio_ring;    /* ring fill level at drain time */
static volatile unsigned char n_real_calls;      /* appels Output avec vrai buffer / fenêtre */
static volatile unsigned char n_push_ok;         /* pushes ayant réellement écrit / fenêtre */
static volatile unsigned char n_null_calls;      /* appels bloquants NULL / fenêtre */
static volatile unsigned char n_pub_chunks;      /* chunks publiés par le hook / fenêtre */
static volatile unsigned char n_enc_frames;      /* frames vidéo encodées avec payload / fenêtre */

/* ---- Registre de ports audio SPARSES (architecture EQVita) ----
 * Les IDs de ports Vita ne sont pas bornés à 0..7 : des valeurs comme
 * 256 existent. Ancien gate port<=7 = musique du jeu rejetée. Nouveau
 * registre : table plate indexée par le NUMÉRO DE PORT seul (pas de
 * PID — un syscall par appel audio dans le chemin chaud est interdit).
 * Entrées neuves : fallback stéréo 48 kHz, raffiné par les
 * enregistreurs OpenPort/SetConfig quand ils voient une ouverture. */
#define APORT_MAX 16
struct aport_state {
	int port;                   /* -1 = entrée libre */
	unsigned char pid;          /* propriétaire (processus) : DEUX
				     * processus peuvent utiliser LE MÊME
				     * numéro de port avec des allocations
				     * différentes — sans ce clé, l'entrée
				     * du jeu décrivait le buffer UI et
				     * inversement = data aborts 74/76 */
	int ch, fr;
	int granule;                /* frames par buffer */
	unsigned char learned;      /* 1 = config posée par NOS hooks */
	SceUInt64 last_real_us;
};
static struct aport_state aport_tab[APORT_MAX];

static struct aport_state *aport_get(int port, unsigned char pid)
{
	struct aport_state *free_s = NULL, *oldest = NULL;
	int k;

	if (aring_mutex >= 0)
		ksceKernelLockMutex(aring_mutex, 0, NULL);

	for (k = 0; k < APORT_MAX; k++) {
		if (aport_tab[k].port == port &&
		    aport_tab[k].pid == pid) {
			aport_tab[k].last_real_us =
				ksceKernelGetSystemTimeWide();
			free_s = &aport_tab[k];
			goto found;
		}
		if (aport_tab[k].port < 0 && !free_s)
			free_s = &aport_tab[k];
		if (!oldest ||
		    aport_tab[k].last_real_us < oldest->last_real_us)
			oldest = &aport_tab[k];
	}
	if (!free_s)
		free_s = oldest;
	free_s->port = port;
	free_s->pid = pid;
	free_s->ch = 2;
	free_s->fr = 48000;
	free_s->granule = 0;
	free_s->learned = 0;
	free_s->last_real_us = 0;
found:
	if (aring_mutex >= 0)
		ksceKernelUnlockMutex(aring_mutex, 0);
	return free_s;
}

static void aport_set_params(int p, int ch, int fr, int granule,
			     unsigned char pid)
{
	struct aport_state *s;

	if (p < 0)
		return;
	if (ch != 1 && ch != 2)
		ch = 2;
	if (fr < 8000 || fr > 192000)
		fr = 48000;
	s = aport_get(p, pid);
	if (s) {
		s->ch = ch;
		s->fr = fr;
		if (granule > 0 && granule <= 16384)
			s->granule = granule;
		s->learned = 1; /* config certifiée par nos hooks */
	}
	if (p <= 7) {
		port_ch_mem[p] = ch;
		port_fr_mem[p] = fr;
	}
}

static int audio_ring_ensure(void)
{
	/* Anneau statique : rien � allouer. */
	ar_head = ar_tail = 0;
	return 0;
}

static void audio_ring_free(void)
{
	ar_head = ar_tail = 0;
}

/* ---- Injection audio DANS LES FRAMES VIDÉO (option 2) ----
 * Le PCM voyage caché dans les 8 dernières lignes de la plane Y du
 * frame NV12, encodé 2 bits/pixel sur 4 niveaux de gris très espacés
 * {25,95,165,235} → survit à toute conversion YUV→RGB côté PC.
 * Capacité : 960px × 8 lignes × 30fps × 2b = ~57 Ko/s > 48 requis.
 * Zéro nouveau canal USB, zéro hook supplémentaire : on surfe sur le
 * flux vidéo, stable depuis le jour un. */
#define AUD_ROWS        8
#define AUD_PAYLOAD_CAP 1728   /* octets PCM par frame (≈51 Ko/s @30fps) */
#define AUD_HDR_LEN     6      /* 'A','V',seq,len_lo,len_hi,sum */

static unsigned char aud_frame_seq;
static unsigned char aud_inject_hdr[AUD_HDR_LEN];
static unsigned char aud_bounce[2048 * 2]; /* copie protégée de pBuffer */

/* Double-buffer publié par le hook audio, consommé par le chemin
 * vidéo SANS AUCUN VERROU. Lock-free SPSC. */
static unsigned char aud_stage[2][AUD_HDR_LEN + AUD_PAYLOAD_CAP];
static volatile unsigned int aud_stage_len[2];
static volatile unsigned int aud_stage_seq;
static unsigned int aud_stage_read_seq;

static void inject_audio_rows(unsigned char *nv12, int W, int H)
{
	unsigned int total, si, di;
	int row, col;
	unsigned char *ybase;
	unsigned char crc;
	unsigned int s0, idx0, idx1, pay_len;
	const unsigned char *b0, *b1;
	unsigned int len0, len1;

	if (!nv12 || W < 320 || H <= AUD_ROWS)
		return;

	/* Lecture lock-free des 2 slots du double-buffer. */
	s0 = aud_stage_seq;
	idx0 = s0 & 1;
	len0 = aud_stage_len[idx0];
	b0 = aud_stage[idx0];
	idx1 = (s0 - 1) & 1;
	len1 = (s0 > 0) ? aud_stage_len[idx1] : 0;
	b1 = (s0 > 0 && len1 > 0) ? aud_stage[idx1] : NULL;

	pay_len = 0;
	if (b0 != NULL && len0 > 0) {
		unsigned int j;
		for (j = 0; j < len0 && pay_len < AUD_PAYLOAD_CAP; j++)
			aud_bounce[pay_len++] = b0[AUD_HDR_LEN + j];
	}
	if (b1 != NULL && len1 > 0) {
		unsigned int j;
		for (j = 0; j < len1 && pay_len < AUD_PAYLOAD_CAP; j++)
			aud_bounce[pay_len++] = b1[AUD_HDR_LEN + j];
	}
	if (pay_len > 0 && n_enc_frames < 255)
		n_enc_frames++;

	crc = 0;
	for (si = 0; si < pay_len; si++)
		crc += aud_bounce[si];

	aud_inject_hdr[0] = 'A';
	aud_inject_hdr[1] = 'V';
	aud_inject_hdr[2] = aud_frame_seq++;
	aud_inject_hdr[3] = (unsigned char)(pay_len & 0xFF);
	aud_inject_hdr[4] = (unsigned char)(pay_len >> 8);
	aud_inject_hdr[5] = crc;
	total = AUD_HDR_LEN + pay_len;

	/* Encoder en 2 bits/pixel dans les dernières lignes Y. */
	ybase = nv12 + (size_t)(H - AUD_ROWS) * W;
	di = 0;
	for (row = 0; row < AUD_ROWS; row++) {
		unsigned char *line = ybase + (size_t)row * W;

		for (col = 0; col < W && di < total; col += 4) {
			unsigned char sym[4];
			unsigned char b;

			b = (di < AUD_HDR_LEN) ? aud_inject_hdr[di]
					       : aud_bounce[di - AUD_HDR_LEN];
			sym[0] = (b >> 6) & 3;
			sym[1] = (b >> 4) & 3;
			sym[2] = (b >> 2) & 3;
			sym[3] = b & 3;
			di++;
			line[col + 0] =
				(unsigned char)(25 + sym[0] * 70);
			line[col + 1] =
				(unsigned char)(25 + sym[1] * 70);
			line[col + 2] =
				(unsigned char)(25 + sym[2] * 70);
			line[col + 3] =
				(unsigned char)(25 + sym[3] * 70);
		}
	}

	/* BALISE DE FIN (build 87) : 'T','S','E','L' + len_lo,len_hi,crc
	 * APRÈS les données. Le PC s'ancre sur TSEL et lit les N octets
	 * d'avant : insensible à tout décalage de fenêtre. */
	{
		static const unsigned char tail[4] = { 'T', 'S', 'E', 'L' };
		unsigned char meta[3];
		unsigned int base = total * 4;
		unsigned int j;

		meta[0] = (unsigned char)(pay_len & 0xFF);
		meta[1] = (unsigned char)(pay_len >> 8);
		meta[2] = crc;

		if (base + 28 <= (unsigned int)(AUD_ROWS * W)) {
			for (j = 0; j < 4; j++) {
				unsigned char tb = tail[j];

				ybase[base + j * 4 + 0] =
				  (unsigned char)(25 + ((tb >> 6) & 3) * 70);
				ybase[base + j * 4 + 1] =
				  (unsigned char)(25 + ((tb >> 4) & 3) * 70);
				ybase[base + j * 4 + 2] =
				  (unsigned char)(25 + ((tb >> 2) & 3) * 70);
				ybase[base + j * 4 + 3] =
				  (unsigned char)(25 + (tb & 3) * 70);
			}
			base += 16;
			for (j = 0; j < 3; j++) {
				unsigned char mb = meta[j];

				ybase[base + j * 4 + 0] =
				  (unsigned char)(25 + ((mb >> 6) & 3) * 70);
				ybase[base + j * 4 + 1] =
				  (unsigned char)(25 + ((mb >> 4) & 3) * 70);
				ybase[base + j * 4 + 2] =
				  (unsigned char)(25 + ((mb >> 2) & 3) * 70);
				ybase[base + j * 4 + 3] =
				  (unsigned char)(25 + (mb & 3) * 70);
			}
		}
	}

	/* NEUTRALISER LA CHROMA des mêmes lignes : sans ça, U/V issus
	 * de l'image du jeu teintent nos gris et le canal G côté PC
	 * ne reflète plus les symboles (magies AV/s=0, build 81).
	 * 8 lignes Y → 4 lignes UV à U=V=128, juste après le plan Y
	 * en mémoire (NV12) → zone contiguë avec nos lignes. */
	{
		unsigned char *uv = nv12 + (size_t)W * H +
				    (size_t)((H - AUD_ROWS) / 2) * W;
		unsigned int n = (AUD_ROWS / 2) * W;

		for (si = 0; si < n; si++)
			uv[si] = 128;
	}
}
#endif /* ENABLE_AUDIO_CAPTURE */
static volatile unsigned char touch_edge_history;
static volatile unsigned char last_read_count;
static volatile unsigned int rx_sequence;

/* Ground truth: last REAL touch seen flowing through the driver
 * (physical finger), captured when we're not injecting. */
static volatile unsigned short real_touch_x;
static volatile unsigned short real_touch_y;
static volatile unsigned char real_touch_seen;
static volatile unsigned char real_touch_down;

/* Real active-area bounds reported by the panel driver; injected
 * coordinates are remapped into this range so clicks land where the
 * system expects them. */
static SceTouchPanelInfo touch_panel_info;
static int touch_panel_inited;
extern int ksceTouchGetPanelInfo(unsigned int port, SceTouchPanelInfo *info);

static void touch_ensure_panel_info(void)
{
	if (touch_panel_inited)
		return;
	memset(&touch_panel_info, 0, sizeof(touch_panel_info));
	if (ksceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &touch_panel_info) == 0 &&
	    touch_panel_info.maxAaX > 0)
		touch_panel_inited = 1;
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

	if (port != SCE_TOUCH_PORT_FRONT || !data || count <= 0)
		return;

	/* Ground truth capture + passthrough when idle: while the PC is not
	 * pressing, the REAL digitizer data must flow untouched � writing
	 * empty history here would erase physical finger input. */
	if (!pc_touch_active) {
		int any = 0;
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
	touch_edge_history = (unsigned char)((touch_edge_history << 1) | 1);
	last_read_count = (unsigned char)count;

	/* PROVEN configuration: steady pressed finger at the cursor,
	 * preserving each buffer's timestamp/status. */
	for (i = 0; i < count; i++) {
		SceUInt64 ts = data[i].timeStamp;
		SceUInt32 st = data[i].status;

		memset(&data[i], 0, sizeof(SceTouchData));
		data[i].timeStamp = ts;
		data[i].status = st;
		data[i].reportNum = 1;
		/* New contact => new finger id, like a real digitizer:
		 * some games ignore repeated contacts reusing an old id. */
		data[i].report[0].id = pc_touch_id ? pc_touch_id : 1;
		data[i].report[0].force = 128;
		data[i].report[0].x = (unsigned short)pc_touch_x;
		data[i].report[0].y = (unsigned short)pc_touch_y;
		data[i].report[0].info = 0;
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

#if ENABLE_AUDIO_CAPTURE
/* ---- Audio redirection implementation ---- */
static int audio_send_pkt(unsigned char *pkt)
{
	static SceUdcdDeviceRequest req;

	req = (SceUdcdDeviceRequest){
		.endpoint = &endpoints[2],
		.data = pkt,
		.attributes = 0,
		.size = 64,
		.isControlRequest = 0,
		.onComplete = NULL,
		.transmitted = 0,
		.returnCode = 0,
		.next = NULL,
		.unused = NULL,
		.physicalAddress = NULL
	};

	ksceKernelDcacheCleanRange(pkt, 64);
	return ksceUdcdReqSend(&req);
}

static int audio_ring_push(const short *samples, int nFrames, int ch, int dec,
			    int buf_shorts)
{
	int i, k;
	unsigned int written = 0;
	int max_iter;

	if (aring_mutex < 0)
		return 0; /* mutex pas prêt : on saute ce morceau */

	ksceKernelLockMutex(aring_mutex, 0, NULL);

	/* Place disponible (en échantillons de 2 octets), plafonnée au
	 * nombre de frames décimées réellement présentes. */
	max_iter = (int)((ARING_SIZE - 2 - (ar_head - ar_tail)) / 2);
	if (nFrames / dec < max_iter)
		max_iter = nFrames / dec;

	for (i = 0; i < max_iter; i++) {
		int acc = 0;
		unsigned h;
		int idx = i * dec * ch;

		/* GARDE ANTI-CRASH : si la config devinée (ch) dépasse
		 * la taille réelle du buffer du jeu (port mono lu comme
		 * stéréo…), STOP net — jamais de lecture hors allocation. */
		if (idx + ch > buf_shorts)
			break;

		for (k = 0; k < ch; k++)
			acc += samples[(long)idx + k];
		acc /= ch;

		h = ar_head & ARING_MASK;
		aring[h] = (unsigned char)(acc & 0xFF);
		aring[(h + 1) & ARING_MASK] = (unsigned char)((acc >> 8) & 0xFF);
		ar_head += 2;
		written += 2;
	}

	ksceKernelUnlockMutex(aring_mutex, 0);

	/* Diag fin : la boucle a-t-elle tourné au moins une fois ?
	 * 6 = rien à décimer (nFrames<dec → paramètres absurdes)
	 * 7 = ring plein dès le premier échantillon */
	if (written == 0)
		aud_diag = (nFrames / dec < 1) ? 6 : 7;
	else
		aud_diag = 5; /* lecture OK confirmée jusqu'au ring */

	return (int)written;
}

static int sceAudioOutOutput_patched(int port, const void *pBuffer)
{
	int r;

#if ENABLE_AUDIO_PIDGRAB
	/* Point de capture PID additionnel : ce hook tourne dans le thread
	 * du processus qui joue du son (= le jeu en pratique). Couvre les
	 * jeux qui ne pollent jamais le tactile. */
	if (pc_control_active)
		game_pid = ksceKernelGetProcessId();
#endif

	/* L'original JOUE D'ABORD : son code retour est notre validateur
	 * (r >= 0 = bloc joué, donc contenu lisible). Signature 2 args :
	 * la taille du bloc est le granule du port, appris à l'OpenPort. */
	r = TOUCH_CALL_OLD(audio_out_ref, audio_out_fn_t, port, pBuffer);

#if ENABLE_AUDIO_BISECT_PASSTHRU
	/* BISECTION : hook présent mais corps vide. Si ça crash encore
	 * → le problème est l'INSTALLATION/PRÉSENCE du hook lui-même
	 * (conflit chaîne taiHEN avec un autre plugin). Si ça passe →
	 * notre logique est en cause et on la réintroduit morceau par
	 * morceau. */
	return r;
#endif

#if ENABLE_AUDIO_PUSH
	if (audio_capture_on && !pBuffer) {
		aud_diag = 8; /* appel bloquant de synchro : normal */
		if (n_null_calls < 255)
			n_null_calls++;
	}
	else if (audio_capture_on && r < 0)
		aud_diag = 9;

	if (audio_capture_on && r >= 0 && pBuffer && port >= 0) {
		/* Chemin chaud conforme EQVita : zéro syscall coûteux,
		 * aucune allocation. Ports sparses (0x100 BGM…) inclus.
		 *
		 * GRANULE PAR TIMING : en lecture synchrone, l'écart
		 * entre deux appels réels ≈ durée du buffer. On déduit
		 * les frames puis on ARRONDI VERS LE BAS par multiples
		 * de 64 → lecture toujours ≤ allocation = zéro crash
		 * possible, même si l'estimation dépasse un poil. */
		struct aport_state *ps;
		int nf, dec;
		SceUID cur_pid = ksceKernelGetProcessId();
		unsigned char my_pid = (unsigned char)(cur_pid & 0xFF);

		n_real_calls++;
		aud_diag_port = (unsigned char)(port & 0xFF);
		ps = aport_get(port, my_pid);
		if (!ps) {
			return r;
		}

		/* GRANULE : uniquement depuis le registre (appris par les
		 * hooks OpenPort/SetConfig/GetConfig installés AU BOOT —
		 * ils précèdent toute ouverture de port de jeu).
		 * INTERDIT d'interroger le driver ici : on est DÉJÀ
		 * dans SceAudio (ré-entrée = verrous = panic). Granule
		 * inconnu → bypass silencieux, règle EQVita. */
		if (!ps->granule)
			return r;

		/* ANTI-CRASH STRUCTUREL (leçon des builds 65-72) : on ne
		 * lit pBuffer QUE si la config du port vient de NOS
		 * hooks OpenPort/SetConfig. Une entrée par défaut LRU
		 * (ch/granule inventés) décrirait mal l'allocation
		 * réelle du port système → lecture hors buffer → data
		 * abort dès qu'un son UI joue. */
		if (!ps->learned || ps->granule <= 0)
			return r;
		if (ps->ch != 1 && ps->ch != 2)
			return r;
		if (ps->fr < 8000 || ps->fr > 192000)
			return r;

		nf = ps->granule;
		dec = ps->fr / 24000;
		if (dec < 1)
			dec = 1;

		/* PUBLICATION DIRECTE dans le double-buffer vidéo :
		 * ZÉRO ring, ZÉRO mutex. Producteur unique (ce hook,
		 * un seul thread audio actif à la fois), consommateur
		 * unique (thread vidéo). Cohérence par séquence, et
		 * tout chunk déchiré est rejeté par le CRC côté PC.
		 * Décimation vers 24 kHz mono faite ici, hot-path
		 * borné et sans allocation (règle EQVita). */
		{
			unsigned int idx = (aud_stage_seq + 1) & 1;
			unsigned char *out = aud_stage[idx] + AUD_HDR_LEN;
			unsigned int cap = AUD_PAYLOAD_CAP;
			const short *in;
			unsigned int o = 0, i, nf_safe, copy_len;
			int cp;

			/* LECTURE PROTÉGÉE (leçon 74-77) : pBuffer peut
			 * être une VA du processus appelant non mappée
			 * dans notre contexte de lecture (sons UI joués
			 * via des workers kernel) → tout déréférencement
			 * direct = data abort. ksceKernelCopyFromUser-
			 * Proc est CONÇUE pour ça : elle renvoie une
			 * erreur au lieu de paniquer. */
			nf_safe = ((unsigned)nf > 2048) ? 2048 : (unsigned)nf;
			copy_len = nf_safe * 2;
			/* ATTENTION : le SceUID COMPLET ici — le pid
			 * tronqué &0xFF n'est qu'une clé de table, pas
			 * un handle valide (cause pub=0 du build 79). */
			cp = ksceKernelCopyFromUserProc(
				cur_pid, aud_bounce, pBuffer,
				(SceSize)copy_len);
			if (cp < 0)
				return r; /* mapping absent : chunk sauté */

			in = (const short *)aud_bounce;
			for (i = 0; i < nf_safe && o + 2 <= cap;
			     i += dec) {
				int acc = in[i];
				out[o++] = (unsigned char)(acc & 0xFF);
				out[o++] = (unsigned char)((acc >> 8) & 0xFF);
			}
			if (o > 0) {
				aud_stage_len[idx] = o;
				aud_stage_seq++; /* publication */
				n_push_ok++;
				if (n_pub_chunks < 255)
					n_pub_chunks++;
			}
		}

#if ENABLE_AUDIO_MUTE
		/* Mute the console: the PC is the speaker now. */
		memset((void *)pBuffer, 0, (size_t)nf * 2 * ps->ch);
#endif
	}
#endif /* ENABLE_AUDIO_PUSH */

	return r;
}

static int sceAudioOutOpenPort_patched(int type, int len, int freq, int mode)
{
	int ret = TOUCH_CALL_OLD(audio_open_ref, audio_open_fn_t, type, len, freq, mode);
	if (ret >= 0) {
		aport_set_params(ret, (mode == 1) ? 2 : 1, freq, len,
				 (unsigned char)(ksceKernelGetProcessId() & 0xFF));
		audio_mod_found = 1;
	}
	return ret;
}

static int sceAudioOutSetConfig_patched(int port, unsigned int len, int freq, int mode)
{
	int ret = TOUCH_CALL_OLD(audio_setcfg_ref, audio_setcfg_fn_t, port, (unsigned int)len, freq, mode);
	if (ret >= 0 && port >= 0) {
		aport_set_params(port, (mode == 1) ? 2 : 1, freq, (int)len,
				 (unsigned char)(ksceKernelGetProcessId() & 0xFF));
		audio_mod_found = 1;
	}
	return ret;
}

/* ---- GetConfig : la source de vérité du DRIVER ----
 * (déclarations en tête de fichier ; le patch vit ici). */
void aport_learn_from_driver(int port);

static int sceAudioOutGetConfig_patched(int port, int type)
{
	int r = TOUCH_CALL_OLD(audio_getcfg_ref, audio_getcfg_fn_t, port, type);
	if (r > 0 && port >= 0) {
		struct aport_state *ps = aport_get(port,
			(unsigned char)(ksceKernelGetProcessId() & 0xFF));
		if (ps && ps->learned) {
			switch (type) {
			case 0: /* LEN = granule */
				if (r <= 16384)
					ps->granule = r;
				break;
			case 1: /* FREQ */
				if (r >= 8000 && r <= 192000)
					ps->fr = r;
				break;
			case 2: /* MODE : 0=mono 1=stereo */
				if (r == 0 || r == 1)
					ps->ch = (r == 1) ? 2 : 1;
				break;
			}
		}
	}
	return r;
}

void aport_learn_from_driver(int port)
{
	if (audio_getcfg_uid <= 0 || port < 0)
		return;
	/* Trois requêtes directes ; les réponses passent par le patch
	 * ci-dessus qui remplit le registre. */
	sceAudioOutGetConfig_patched(port, 0);
	sceAudioOutGetConfig_patched(port, 1);
	sceAudioOutGetConfig_patched(port, 2);
}
#endif /* ENABLE_AUDIO_CAPTURE */

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
        ret_b = 0;
        for (slot = 1; slot <= 3; slot++) {
            ret_b |= ksceCtrlSetButtonEmulation(0, (unsigned char)slot,
                                                b, b, 60);
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

/* Shared helper: apply a freshly received 16-byte PC report, whichever
 * transport (interrupt endpoint or EP0 SET_REPORT) it arrived through. */
static void pc_hid_apply_report(const unsigned char *report)
{
	if (report[0] != PC_HID_REPORT_ID_OUTPUT)
		return;

	pc_buttons = (unsigned int)report[1] |
	             ((unsigned int)report[2] << 8);
	pc_lx = report[3];
	pc_ly = report[4];
	pc_rx = report[5];
	pc_ry = report[6];

	/* Touch packet: flags byte then 12-bit X / 12-bit Y coordinates.
	 * High nibbles live in report[10] (low nibble = X high, high
	 * nibble = Y high), exactly where the PC app writes them. */
	{
		unsigned char fl = report[7];
		unsigned int tx = (unsigned int)report[8] |
		                  (((unsigned int)report[10] & 0x0F) << 8);
		unsigned int ty = (unsigned int)report[9] |
		                  ((((unsigned int)report[10] >> 4) & 0x0F) << 8);

		if (tx >= TOUCHSCREEN_WIDTH)  tx = TOUCHSCREEN_WIDTH - 1;
		if (ty >= TOUCHSCREEN_HEIGHT) ty = TOUCHSCREEN_HEIGHT - 1;
		pc_touch_x = tx;
		pc_touch_y = ty;
		if (fl & 0x01) {
			if (!pc_touch_active) {
				pc_touch_id++; /* new contact -> new finger id */
			}
			pc_touch_active = 1;
		} else {
			pc_touch_active = 0;
		}
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
	 * not kill the control channel permanently. */
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

/* ---- Canal EP2 (rentrant) : UNE SEULE requ�te � la fois ----
 * Le battement de c�ur ET l'audio partagent cet endpoint ; le pilote
 * UDCD ne tol�re pas les requ�tes imbriqu�es sur le m�me endpoint
 * (source du panic au lancement d'un jeu). */
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
	/* Thread-safe test-and-set : le heartbeat tourne dans uvc_thread
	 * et l'audio dans son propre worker. Un simple � if (ep2_busy) �
	 * laisserait deux threads franchir la barri�re simultan�ment
	 * (requ�tes imbriqu�es sur EP2 = panique UDCD). Le CAS rend
	 * l'acquisition atomique ; seul le gagnant touche ep2_req/ep2_pkt. */
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
	hb[18] = (unsigned char)pc_ret_analog_emu;
	hb[19] = aud_diag_port; /* dernier port audio réel vu par le hook */
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
	hb[29] = pc_touch_active ? 1 : 0;
	hb[30] = (unsigned char)(pc_touch_x & 0xFF);
	hb[31] = (unsigned char)(pc_touch_y & 0xFF);
	hb[32] = h_peek;
	hb[33] = h_peekR;
	hb[34] = h_read;
	hb[35] = h_readR;
	hb[36] = h_peek2;
	hb[37] = h_read2;
	hb[38] = (unsigned char)(touch_panel_info.minAaX & 0xFF);
	hb[39] = (unsigned char)(((unsigned short)touch_panel_info.minAaX >> 8) & 0xFF);
	hb[40] = (unsigned char)((unsigned short)touch_panel_info.maxAaX & 0xFF);
	hb[41] = (unsigned char)(((unsigned short)touch_panel_info.maxAaX >> 8) & 0xFF);
	hb[42] = (unsigned char)(touch_panel_info.minAaY & 0xFF);
	hb[43] = (unsigned char)(((unsigned short)touch_panel_info.minAaY >> 8) & 0xFF);
	hb[44] = (unsigned char)((unsigned short)touch_panel_info.maxAaY & 0xFF);
	hb[45] = (unsigned char)(((unsigned short)touch_panel_info.maxAaY >> 8) & 0xFF);
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
	/* Sondes pipeline (seul octet libre : hb[9]) :
	 * haut = chunks publiés par hook, bas = frames encodées vidéo */
	hb[9] = (unsigned char)(((n_pub_chunks > 15 ? 15 : n_pub_chunks) << 4) |
				(n_enc_frames > 15 ? 15 : n_enc_frames));
	/* Exit-game diagnostics: PID of the latest touch poller (2 bytes,
	 * les PIDs tiennent largement dans 16 bits) + kill state. */
	hb[58] = (unsigned char)((unsigned int)game_pid & 0xFF);
	hb[59] = (unsigned char)((((unsigned int)game_pid >> 8) & 0x0F) |
	                         ((aud_diag & 0x0F) << 4));
#if ENABLE_AUDIO_CAPTURE
	{
		/* Sonde granule (bits 3-7 de hb[60]) :
		 * 0 = jamais appris | 1-31 = (granule/64)+1 */
		int gprobe = 0, kk;
		for (kk = 0; kk < APORT_MAX; kk++) {
			if (aport_tab[kk].port == 7 &&
			    aport_tab[kk].granule > 0) {
				gprobe = ((aport_tab[kk].granule >> 6) + 1);
				if (gprobe > 31) gprobe = 31;
				break;
			}
		}
		hb[60] = (unsigned char)((audio_out_hook_uid > 0 ? 1 : 0) |
				(audio_capture_on ? 2 : 0) |
				(audio_mod_found ? 4 : 0) |
				(gprobe << 3));
	}
#else
	hb[60] = 0;
#endif
	hb[61] = (unsigned char)((exit_game_request ? 8 : 0) |
	                         ((kill_result & 3) << 4) |
	                         ((exit_phase & 3) << 6));
	/* hb[62] : haut = appels réels (buffer non NULL) / fenêtre 500ms,
	 *          bas  = paquets EP2 envoyés / fenêtre.
	 * Décisif : distingue « le jeu ne soumet rien »
	 * de « on n'arrive pas à l'envoyer ». */
	hb[62] = (unsigned char)(((n_real_calls & 0x0F) << 4) |
				 (usb_audio_ok_cnt & 0x0F));
	/* NULLs (appels bloquants) dans le quartet haut de hb[46] */
	hb[46] = (unsigned char)((hb[46] & 0x0F) |
		 ((n_null_calls > 15 ? 15 : n_null_calls) << 4));
	hb[63] = 89; /* build id */

	ep2_try_send(hb);
	usb_audio_ok_cnt = 0;
	n_real_calls = 0;
	n_push_ok = 0;
	n_null_calls = 0;
	n_pub_chunks = 0;
	n_enc_frames = 0;
}

/* ---- Worker audio d�di� ----
 * Boucle ind�pendante : un paquet 62 octets toutes les 2 ms tant que
 * la capture est active et le ring aliment�. Ne partage RIEN avec
 * uvc_thread hormis ep2_try_send() (rendu atomique par CAS). */
static int audio_send_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;

	while (audio_thread_run) {
		int k_port;
		(void)k_port;
		/* Ring supprimé du chemin des données (build 71) : le
		 * hook publie directement dans le double-buffer vidéo.
		 * Ce worker ne sert plus qu'à respirer. */
		ksceKernelDelayThreadCB(20000);
	}
	return 0;
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

	ret = ksceUdcdReqSend(&req);
	if (ret < 0)
		return ret;

	ret = ksceKernelWaitEventFlagCB(uvc_frame_req_evflag, 1, SCE_EVENT_WAITOR |
					SCE_EVENT_WAITCLEAR_PAT, NULL, NULL);

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
	if (stream) {
		stream = 0;

		ksceUdcdClearFIFO(&endpoints[1]);
	ksceUdcdClearFIFO(&endpoints[2]);
	ksceUdcdClearFIFO(&endpoints[3]);
	pc_control_release();
	pc_hid_start_receive();
		ksceUdcdReqCancelAll(&endpoints[1]);
		exit_phase = 0;
		kill_hold = 0;
	}
}

static void uvc_handle_set_interface(const SceUdcdEP0DeviceRequest *req)
{
	if ((req->wIndex == STREAM_INTERFACE) && (req->wValue == 0))
		uvc_handle_video_abort();
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

static int uvc_udcd_process_request(int recipient, int arg, SceUdcdEP0DeviceRequest *req, void *user_data)
{
	if (arg < 0)
		return -1;

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
	return 0;
}

#ifdef DEBUG
/* Counted here, logged by uvc_thread (no file IO in USB callbacks). */
static volatile unsigned int dbg_attach_n, dbg_detach_n;
static volatile int dbg_attach_usb_version;
#endif

static int uvc_udcd_attach(int usb_version, void *user_data)
{
#ifdef DEBUG
	dbg_attach_usb_version = usb_version;
	dbg_attach_n++;
#endif
	/* Measure the sticks' rest position BEFORE enabling anything, while
	 * the values read are still the untouched hardware ones. */
	pc_ctrl_capture_neutral();

	ksceUdcdClearFIFO(&endpoints[1]);
	ksceUdcdClearFIFO(&endpoints[2]);
	ksceUdcdClearFIFO(&endpoints[3]);
	pc_control_release();
	pc_hid_start_receive();

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
#ifdef DEBUG
	dbg_detach_n++;
#endif
	uvc_handle_video_abort();
	ksceUdcdReqCancelAll(&endpoints[3]);
	pc_control_release();

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
 * signals the retake check in uvc_thread relies on. */
static volatile int usb_driver_running;
#ifdef DEBUG
static volatile unsigned int dbg_drv_start_n, dbg_drv_stop_n;
#endif

static int uvc_driver_start(int size, void *p, void *user_data)
{
	usb_driver_running = 1;
#ifdef DEBUG
	dbg_drv_start_n++;
#endif
	return 0;
}

static int uvc_driver_stop(int size, void *p, void *user_data)
{
	usb_driver_running = 0;
#ifdef DEBUG
	dbg_drv_stop_n++;
#endif
	return 0;
}

static SceUdcdDriver uvc_udcd_driver = {
	.driverName			= UVC_DRIVER_NAME,
	.numEndpoints			= 4,
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

	/* IFTU est ASYNCHRONE : sa DMA finit d'écrire le buffer APRÈS
	 * le retour de l'appel. Sans attente, elle écrase nos lignes
	 * audio → l'USB n'envoie que l'image (symptôme exact du build
	 * 80 : enc=15+, magies AV/s=0). Le crash historique attribué
	 * au délai venait en réalité du mutex dans ce même chemin,
	 * supprimé depuis. */
	ksceKernelDelayThreadCB(2000);

	/* Audio embarqué : écriture CPU puis CLEAN (writeback vers RAM
	 * pour que le DMA USB voie l'audio), borné aux 8 lignes avec
	 * alignement ligne de cache 32 o. */
#if ENABLE_AUDIO_INJECT
	if (audio_capture_on && uvc_frame_buffer_uid >= 0
	    && uvc_frame_buffer_addr && uvc_frame_buffer_addr->data) {
		uintptr_t s, e;
		unsigned char *d = uvc_frame_buffer_addr->data;

		inject_audio_rows(d, dst_width, dst_height);
		s = (uintptr_t)(d +
		    (size_t)(dst_height - AUD_ROWS) * dst_width);
		e = s + (AUD_ROWS + AUD_ROWS / 2) * dst_width;
		s &= ~(uintptr_t)0x1F;
		e = (e + 0x1F) & ~(uintptr_t)0x1F;
		ksceKernelDcacheCleanRange((void *)s,
					   (unsigned int)(e - s));
	}
#endif

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

#ifdef DEBUG
/* Our driver is still the one on the port: the callback says running and
 * its state is unchanged. */
static int usb_ours_running(void)
{
	return usb_driver_running &&
	       ksceUdcdGetDrvState(UVC_DRIVER_NAME) == usb_ours_running_state;
}
#endif

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

#ifdef DEBUG
/* While VitaShell's USB transfer is open, the PC has ux0: mounted as a raw
 * disk, and writing the log to it from here could corrupt the card.
 * VitaShell stops both our driver and MTP while the transfer is open, so
 * only write while our driver still runs or MTP is back; until then lines
 * stay buffered. */
static void dbg_flush_if_safe(void)
{
	if (usb_states_known && (usb_ours_running() || usb_mtp_restarted()))
		log_flush();
}
#define DFLUSH_SAFE()	dbg_flush_if_safe()
#else
#define DFLUSH_SAFE()	((void)0)
#endif

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
	ret = usb_takeover();
	if (ret < 0) {
		DLOG("retake failed (0x%08X), giving USB back to MTP\n", ret);
		usb_restore_mtp();
	} else {
		DLOG("retake OK\n");
	}
	DFLUSH_SAFE();
	return ret;
}

#define USB_CHECK_INTERVAL_US	1000000	/* driver states read once a second */
#define USB_FREE_CHECKS		2	/* checks in a row before retaking */
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
	int usb_free_checks = 0, usb_retake_fails = 0;
	SceUInt64 next_usb_check = 0;
	/* First write to ux0:. By now (15 s+ after start) henlo has mounted
	 * the SD2Vita, so the log lands on the card the PC mounts. */
	DFLUSH();

#ifdef DEBUG
	unsigned int dbg_attach_seen = 0, dbg_detach_seen = 0;
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
	SceUInt64 next_audio_try = 0;
	unsigned int audio_tries = 0;
	SceUInt64 next_power_tick = 0;

	while (uvc_thread_run) {
		unsigned int out_bits;

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

		/* NB : le drain audio NE VIT PLUS ICI. Il tourne dans
		 * audio_send_thread (worker d�di�, cf. plus haut) � ce
		 * thread ne se r�veille qu'au rythme vid�o (~30 Hz),
		 * bien trop lent pour l'audio, et son contexte li� au
		 * callback vblank �tait la source des panics UDCD. */

		/* Hooks audio : installés DANS module_start (au boot),
		 * comme EQVita — ainsi ils précèdent l'ouverture des
		 * ports par les jeux et les enregistreurs OpenPort ne
		 * ratent JAMAIS un granule. L'installation tardive ici
		 * faisait rater l'OpenPort de Rayman → granule inconnu
		 * à jamais → silence. */

		if (now - last_hb >= 500000) { /* heartbeat every ~500 ms */
			last_hb = now;
			pc_hid_send_input_heartbeat();
		}

		/* Take the port back once VitaShell's USB transfer has closed.
		 * Opening it stops every udcd driver (ours too) and MTP, and
		 * closing it restarts only MTP. So "ours evicted + MTP back"
		 * can only mean it has closed. */
		if (usb_states_known && usb_retake_fails < USB_RETAKE_MAX_FAILS &&
		    now >= next_usb_check) {
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
			if (usb_ours_evicted() && usb_mtp_restarted()) {
				if (++usb_free_checks >= USB_FREE_CHECKS) {
					usb_free_checks = 0;
					if (usb_retake() < 0)
						usb_retake_fails++;
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
#if ENABLE_AUDIO_CAPTURE
			if (audio_out_hook_uid > 0 && aring)
				audio_capture_on = 1;
#endif
		} else if (pc_control_active && now - last_rx_change > 1000000ULL) {
			pc_control_release();
			pc_touch_active = 0;
			touch_hooks_wanted = 0;
#if ENABLE_AUDIO_CAPTURE
			audio_capture_on = 0;
#endif
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
					pc_touch_active = 0;
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

		if (ret == 0 && stream && kill_hold == 0) {
			int sf = send_frame();
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

#ifdef DEBUG
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

	if (dst->wTotalLength == config_descriptor->wTotalLength) {
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

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	int ret;
	tai_module_info_t SceUdcd_modinfo;

#ifdef DEBUG
	log_reset();
#endif
	DLOG("VitaSync debug build " __DATE__ " " __TIME__ "\n");

	SceUdcd_modinfo.size = sizeof(SceUdcd_modinfo);
	DCALL(taiGetModuleInfoForKernel(KERNEL_PID, "SceUdcd", &SceUdcd_modinfo));

	SceUdcd_sub_01E1128C_hook_uid = DCALL(taiHookFunctionOffsetForKernel(KERNEL_PID,
		&SceUdcd_sub_01E1128C_ref, SceUdcd_modinfo.modid, 0,
		0x01E1128C - 0x01E10000, 1, SceUdcd_sub_01E1128C_hook_func));

	/* Touch hooks are installed lazily once the PC app talks to us,
	 * and fully removed again when it goes silent (watchdog below). */
	touch_hooks_install();
	touch_hooks_wanted = 1;
	DLOG("touch hooks: 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
	     touch_peek_hook_uid, touch_peek_region_hook_uid,
	     touch_read_hook_uid, touch_read_region_hook_uid,
	     touch_peek2_hook_uid, touch_read2_hook_uid);

#if ENABLE_AUDIO_CAPTURE && ENABLE_AUDIO_HOOKTRY
	/* Hooks audio AU BOOT (architecture EQVita) : SceAudio est déjà
	 * chargé par boot_config quand taiHEN lance les plugins, donc
	 * l'accroche ici est sûre — et surtout elle PRÉCÈDE toute
	 * ouverture de port par les jeux : OpenPort/SetConfig ne
	 * ratent plus jamais le granule. */
	audio_out_hook_uid = taiHookFunctionExportForKernel(
		KERNEL_PID, &audio_out_ref, "SceAudio",
		TAI_ANY_LIBRARY, 0x02DB3F5F,
		sceAudioOutOutput_patched);
	if (audio_out_hook_uid <= 0)
		audio_out_hook_uid = -1;
#if ENABLE_AUDIO_RECORDERS
	audio_open_uid = taiHookFunctionExportForKernel(
		KERNEL_PID, &audio_open_ref, "SceAudio",
		TAI_ANY_LIBRARY, 0x5BC341E4,
		sceAudioOutOpenPort_patched);
	audio_setcfg_uid = taiHookFunctionExportForKernel(
		KERNEL_PID, &audio_setcfg_ref, "SceAudio",
		TAI_ANY_LIBRARY, 0xB8BA0D07,
		sceAudioOutSetConfig_patched);
	audio_getcfg_uid = taiHookFunctionExportForKernel(
		KERNEL_PID, &audio_getcfg_ref, "SceAudio",
		TAI_ANY_LIBRARY, 0x9C8EDAEA,
		sceAudioOutGetConfig_patched);
#endif
#endif

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

	/* Worker audio d�di� : son �chec ne doit pas emp�cher le
	 * chargement du module (la vid�o/tactile continuent sans lui). */
	aring_mutex = DCALL(ksceKernelCreateMutex("aring_mut", 0, 0, NULL));
	if (aring_mutex < 0) {
		DFLUSH();
		return SCE_KERNEL_START_FAILED;
	}

	audio_thread_run = 1;
	audio_thread_id = DCALL(ksceKernelCreateThread("aud_send",
						 audio_send_thread,
						 0x3C, 0x1000, 0,
						 0x10000, 0));
	if (audio_thread_id >= 0)
		ksceKernelStartThread(audio_thread_id, 0, NULL);
	else
		audio_thread_id = -1;

	/* Last line before uvc_thread runs: from here on only that thread
	 * writes to the log buffer. */
	DLOG("module_start: starting uvc_thread\n");
	uvc_thread_run = 1;
	ret = ksceKernelStartThread(uvc_thread_id, 0, NULL);
	if (ret < 0) {
		DLOG("ksceKernelStartThread(uvc_thread) = 0x%08X\n", ret);
		goto err_unregister;
	}

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
	pc_control_release();
	ksceUdcdReqCancelAll(&endpoints[3]);
	uvc_thread_run = 0;
	audio_thread_run = 0;

	ksceKernelSetEventFlag(uvc_event_flag_id, 1);
	ksceKernelWaitThreadEnd(uvc_thread_id, NULL, NULL);

	if (audio_thread_id >= 0) {
		ksceKernelWaitThreadEnd(audio_thread_id, NULL, NULL);
		ksceKernelDeleteThread(audio_thread_id);
		audio_thread_id = -1;
	}

	if (aring_mutex >= 0) {
		ksceKernelDeleteMutex(aring_mutex);
		aring_mutex = -1;
	}

	ksceKernelDeleteEventFlag(uvc_event_flag_id);
	ksceKernelDeleteThread(uvc_thread_id);

	uvc_frame_req_fini();

	ksceUdcdDeactivate();
	ksceUdcdStop(UVC_DRIVER_NAME, 0, NULL);
	ksceUdcdStop("USBDeviceControllerDriver", 0, NULL);
	ksceUdcdUnregister(&uvc_udcd_driver);

	if (SceUdcd_sub_01E1128C_hook_uid > 0) {
		taiHookReleaseForKernel(SceUdcd_sub_01E1128C_hook_uid,
			SceUdcd_sub_01E1128C_ref);
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































