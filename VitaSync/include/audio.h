#ifndef AUDIO_H
#define AUDIO_H

#include <psp2kern/udcd.h>

/* The USB audio stream (Vita sound to the PC), on its own thread. */

/* module_start: memory, event flag and thread. ep = the isochronous IN
 * endpoint. Returns < 0 on failure; the rest of the plugin works without. */
int audio_init(SceUdcdEndpoint *ep);

/* module_stop: stops the stream and frees everything. */
void audio_fini(void);

/* The PC selected alternate setting 1 (on = 1) or 0 of the streaming
 * interface, or the cable went away (on = 0). Safe in USB callbacks. */
void audio_set_streaming(int on);

/* Heartbeat fields, counted since the last call (~500 ms):
 * state: 1 PC wants sound, 2 streaming, 4 ready, 8 capturing the Vita's
 *        mix, 16 route taken back after a reset, 32 monitor functions not
 *        found (silence sent), 64 capture failed (silence sent),
 * packets_div4: packets sent / 4 (max 255);
 *        128 a stream had to start with an old request queued (stays
 *        until reboot);
 * packets_div4 counts 1 ms packets; requests carry 8 of them;
 * errors: failed requests (max 255, counted only while streaming);
 * xruns: the ring ran dry, skipped ahead or overflowed (max 15);
 * last_error_low: low byte of the last failure code (0x8024xxxx). */
void audio_heartbeat(unsigned char *state, unsigned char *packets_div4,
		     unsigned char *errors, unsigned char *xruns,
		     unsigned char *last_error_low);

#endif
