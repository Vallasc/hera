#include "g711.h"

/* ITU-T G.711, following the CCITT reference implementation (segment tables
 * and XOR masks included). The Dart implementation in hera_audio uses the same
 * algorithm.
 *
 * Each code is a sign bit, a 3-bit segment number and a 4-bit mantissa. The
 * XOR masks are the even-bit inversion (A-law) and full inversion (mu-law)
 * defined by the standard. */

#define BIAS       0x84
#define ULAW_CLIP  8159    /* already in the 14-bit domain */
#define ALAW_CLIP  4095    /* already in the 13-bit domain */

static const int ulaw_segment_end[8] = {
    0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF,
};

static const int alaw_segment_end[8] = {
    0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF,
};

static int segment_of(int value, const int *ends)
{
    for (int i = 0; i < 8; i++) {
        if (value <= ends[i])
            return i;
    }
    return 8;
}

uint8_t g711_ulaw_encode(int16_t sample)
{
    int value = sample >> 2;               /* 14 bits */
    int mask;
    if (value < 0) {
        value = -value;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }
    if (value > ULAW_CLIP)
        value = ULAW_CLIP;
    value += BIAS >> 2;

    int segment = segment_of(value, ulaw_segment_end);
    if (segment >= 8)
        return (uint8_t)(0x7F ^ mask);
    return (uint8_t)(((segment << 4) | ((value >> (segment + 1)) & 0x0F)) ^ mask);
}

int16_t g711_ulaw_decode(uint8_t byte)
{
    int value = (~byte) & 0xFF;
    int magnitude = ((value & 0x0F) << 3) + BIAS;
    magnitude <<= (value & 0x70) >> 4;
    return (int16_t)((value & 0x80) ? BIAS - magnitude : magnitude - BIAS);
}

uint8_t g711_alaw_encode(int16_t sample)
{
    int value = sample >> 3;               /* 13 bits */
    int mask;
    if (value >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        value = -value - 1;
    }
    if (value > ALAW_CLIP)
        value = ALAW_CLIP;

    int segment = segment_of(value, alaw_segment_end);
    if (segment >= 8)
        return (uint8_t)(0x7F ^ mask);

    int encoded = segment << 4;
    encoded |= (segment < 2) ? ((value >> 1) & 0x0F) : ((value >> segment) & 0x0F);
    return (uint8_t)(encoded ^ mask);
}

int16_t g711_alaw_decode(uint8_t byte)
{
    int value = byte ^ 0x55;
    int magnitude = (value & 0x0F) << 4;
    int segment = (value & 0x70) >> 4;
    if (segment == 0) {
        magnitude += 8;
    } else if (segment == 1) {
        magnitude += 0x108;
    } else {
        magnitude += 0x108;
        magnitude <<= segment - 1;
    }
    return (int16_t)((value & 0x80) ? magnitude : -magnitude);
}

void g711_alaw_encode_block(const int16_t *in, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_alaw_encode(in[i]);
}

void g711_alaw_decode_block(const uint8_t *in, size_t n, int16_t *out)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_alaw_decode(in[i]);
}

void g711_ulaw_encode_block(const int16_t *in, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_ulaw_encode(in[i]);
}

void g711_ulaw_decode_block(const uint8_t *in, size_t n, int16_t *out)
{
    for (size_t i = 0; i < n; i++)
        out[i] = g711_ulaw_decode(in[i]);
}
