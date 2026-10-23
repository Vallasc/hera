#ifndef HERA_G711_H
#define HERA_G711_H

#include <stddef.h>
#include <stdint.h>

/* ITU-T G.711 A-law and mu-law (PROTOCOL.md §6.2). Per-sample companding,
 * 16-bit linear to 8 bits and back. */

uint8_t g711_alaw_encode(int16_t sample);
int16_t g711_alaw_decode(uint8_t byte);
uint8_t g711_ulaw_encode(int16_t sample);
int16_t g711_ulaw_decode(uint8_t byte);

/* Block helpers. `n` is the number of samples; the byte buffers hold `n` bytes. */
void g711_alaw_encode_block(const int16_t *in, size_t n, uint8_t *out);
void g711_alaw_decode_block(const uint8_t *in, size_t n, int16_t *out);
void g711_ulaw_encode_block(const int16_t *in, size_t n, uint8_t *out);
void g711_ulaw_decode_block(const uint8_t *in, size_t n, int16_t *out);

#endif /* HERA_G711_H */
