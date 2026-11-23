#ifndef SPEAKER_H
#define SPEAKER_H

/* Plays the captured mix on the Vita's own speaker or headphones while the
 * PC records it (readback takes the mix away from them). All calls come
 * from the capture thread, except speaker_state(). */

#define SPEAKER_OFF		0	/* not capturing */
#define SPEAKER_PLAYING		1	/* the captured sound goes to the speaker */
#define SPEAKER_UNSUPPORTED	2	/* unknown SceAudio: muted, as before */
#define SPEAKER_FAILED		3	/* setup or a transfer failed: muted */

/* Before the first block of a stream. Returns < 0 if the speaker stays
 * muted (the PC still gets the sound). */
int speaker_start(void);

/* One captured block of 512 stereo 16-bit frames, played on the active
 * output. Waits until the output has taken it (about 10 ms). Returns < 0
 * on failure; call speaker_fail() then. */
int speaker_write(const void *block);

/* Stops our transfers. Must run before the normal route is put back. */
void speaker_stop(void);

/* speaker_stop(), and the speaker reports SPEAKER_FAILED. */
void speaker_fail(void);

int speaker_state(void);

#endif
