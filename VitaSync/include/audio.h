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

/* Heartbeat fields: state bits (1 = PC wants sound, 2 = streaming,
 * 4 = ready), packets sent since the last call (divided by 4, max 255),
 * errors since start (max 255). */
void audio_heartbeat(unsigned char *state, unsigned char *packets_div4,
		     unsigned char *errors);

#endif
