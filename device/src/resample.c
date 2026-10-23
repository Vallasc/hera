#include "resample.h"

#include <string.h>

/* Half-band low-pass, 19 taps, Q15, unity DC gain. Indexed from the centre:
 * g[0] = 16384, g[+-1] = 10087, g[+-3] = -2559, g[+-5] = 864, g[+-7] = -238,
 * g[+-9] = 38; all other even offsets are zero. */
enum { G0 = 16384, G1 = 10087, G3 = -2559, G5 = 864, G7 = -238, G9 = 38 };

static inline int16_t sat16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

void resample_up_reset(resample_up_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
}

void resample_down_reset(resample_down_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
}

void resample_up(resample_up_t *r, const int16_t *in, size_t n, int16_t *out)
{
    if (n == 0 || n > RS_MAX_BLOCK)
        return;

    /* History followed by the new block. */
    int16_t c[RS_UP_HIST + RS_MAX_BLOCK];
    memcpy(c, r->hist, sizeof(r->hist));
    memcpy(c + RS_UP_HIST, in, n * sizeof(int16_t));

    /* With zero-stuffing, even outputs equal the input samples and odd
     * outputs are interpolated. The factor 2 restores the gain lost to the
     * inserted zeros. */
    for (size_t i = 0; i < n; i++) {
        const int16_t *p = c + 4 + i;          /* centre of the output pair */
        int32_t odd = (int32_t)G1 * (p[0] + p[1])
                    + (int32_t)G3 * (p[-1] + p[2])
                    + (int32_t)G5 * (p[-2] + p[3])
                    + (int32_t)G7 * (p[-3] + p[4])
                    + (int32_t)G9 * (p[-4] + p[5]);
        out[2 * i]     = p[0];
        out[2 * i + 1] = sat16((odd * 2) >> 15);
    }
    memcpy(r->hist, c + n, sizeof(r->hist));
}

void resample_down(resample_down_t *r, const int16_t *in, size_t n, int16_t *out)
{
    if (n == 0 || (n & 1) || n > RS_MAX_BLOCK)
        return;

    int16_t c[RS_DOWN_HIST + RS_MAX_BLOCK];
    memcpy(c, r->hist, sizeof(r->hist));
    memcpy(c + RS_DOWN_HIST, in, n * sizeof(int16_t));

    /* Band-limit to 4 kHz and keep one sample in two. */
    size_t outn = n / 2;
    for (size_t i = 0; i < outn; i++) {
        const int16_t *p = c + 9 + 2 * i;
        int32_t acc = (int32_t)G0 * p[0]
                    + (int32_t)G1 * (p[-1] + p[1])
                    + (int32_t)G3 * (p[-3] + p[3])
                    + (int32_t)G5 * (p[-5] + p[5])
                    + (int32_t)G7 * (p[-7] + p[7])
                    + (int32_t)G9 * (p[-9] + p[9]);
        out[i] = sat16(acc >> 15);
    }
    memcpy(r->hist, c + n, sizeof(r->hist));
}
