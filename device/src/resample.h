#ifndef HERA_RESAMPLE_H
#define HERA_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

/* 2x resampling between the SCO side (8 kHz) and a 16 kHz wire format.
 *
 * Used only when the negotiated format rate differs from the SCO rate (§6.2).
 * With CVSD and FMT_PCM16_8K this code is not reached.
 *
 * Both directions use the same 19-tap symmetric half-band FIR in Q15. All taps
 * at even non-zero offsets from the centre are zero, so each output sample
 * costs five multiply-accumulates. Group delay is 9 samples at 16 kHz
 * (0.56 ms).
 *
 * No allocation. Filter history is kept across calls, so processing a stream
 * in blocks gives the same output as processing it in one call. */

/* Maximum input length per call: 20 ms at 16 kHz. Longer inputs are rejected
 * and nothing is written. */
#define RS_MAX_BLOCK 320

#define RS_UP_HIST    9     /* 8 kHz samples carried between blocks  */
#define RS_DOWN_HIST 18     /* 16 kHz samples carried between blocks */

typedef struct {
    int16_t hist[RS_UP_HIST];
} resample_up_t;

typedef struct {
    int16_t hist[RS_DOWN_HIST];
} resample_down_t;

void resample_up_reset(resample_up_t *r);
void resample_down_reset(resample_down_t *r);

/* 8 kHz -> 16 kHz. Writes exactly 2*n samples into `out`. */
void resample_up(resample_up_t *r, const int16_t *in, size_t n, int16_t *out);

/* 16 kHz -> 8 kHz. `n` must be even; writes exactly n/2 samples into `out`. */
void resample_down(resample_down_t *r, const int16_t *in, size_t n, int16_t *out);

#endif /* HERA_RESAMPLE_H */
