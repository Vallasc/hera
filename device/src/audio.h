#ifndef HERA_AUDIO_H
#define HERA_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "link.h"

/* Audio engine: the only real-time path in the service.
 *
 * A single thread owns the SCO socket and is clocked by it: every packet read
 * from the phone is answered with a packet of the same size.
 *
 *   phone mic  -> PCM16 8 kHz -> accumulate one block -> AUDIO_RX
 *   AUDIO_TX   -> jitter buffer -> PCM16 8 kHz         -> phone speaker
 *
 * No codec is involved (PROTOCOL.md §6.1). With FMT_PCM16_8K, which is selected
 * whenever the SCO link is narrowband, conversion is a memcpy in both
 * directions. G.711 is table-driven, and the resampler only runs if the client
 * asked for a rate other than the SCO rate.
 *
 * The audio path does not allocate: jitter buffer slots, resampler state and
 * work buffers are preallocated. Blocks from the client are converted to SCO
 * PCM on the link thread when they are pushed, so the real-time thread only
 * copies samples out of the jitter buffer. */

typedef struct audio audio_t;

/* `on_closed` fires on the audio thread when the phone drops the SCO link. It
 * must not call audio_close(): the thread is still running at that point. */
audio_t *audio_create(link_t *link, void (*on_closed)(void *ud), void *ud);
void     audio_destroy(audio_t *a);

/* Take ownership of a connected SCO socket and start the audio thread. Resets
 * the resamplers, the jitter buffer and the `ts` counter, as §6.3 requires at
 * every AUDIO_OPEN. Returns 0 on success. */
int  audio_open(audio_t *a, int sco_fd);
void audio_close(audio_t *a);
int  audio_is_open(audio_t *a);

/* Set the wire format and block duration (§6.2). Call before the first block
 * arrives. The format may change during a call (CMD SET_FORMAT); the jitter
 * buffer is flushed when it does. */
void audio_set_format(audio_t *a, uint8_t fmt, unsigned frame_ms);

/* Queue one audio block from the client. Called on the link thread; converts
 * the samples to SCO PCM and appends them to the jitter buffer. */
void audio_push_block(audio_t *a, const hera_audio_t *block);

/* Insert `blocks` filler blocks for a `ts` gap (§6.4). */
void audio_push_gap(audio_t *a, unsigned blocks);

/* CMD MUTE_UL (§9): send silence to the phone instead of the client's audio. */
void audio_set_mute_ul(audio_t *a, int mute);

/* Milliseconds of client audio buffered towards the phone (STATS §10.1). */
unsigned audio_buffered_ms(audio_t *a);

/* Uplink interarrival jitter estimate, in ms (STATS §10.1). */
unsigned audio_jitter_ms(audio_t *a);

/* Current target depth of the adaptive jitter buffer, in ms (STATS §10.1). */
unsigned audio_target_ms(audio_t *a);

#endif /* HERA_AUDIO_H */
