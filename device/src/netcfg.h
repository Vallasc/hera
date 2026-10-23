#ifndef HERA_NETCFG_H
#define HERA_NETCFG_H

#include <stddef.h>
#include <stdint.h>

#include "netd.h"
#include "proto.h"

/* Encoding and decoding of the GATT payloads (PROTOCOL.md §3.2-§3.4).
 *
 * Kept separate from gatt.c, which depends on sd-bus, so that `make test` can
 * check them against the §14 vectors on any host. */

/* §3.3.1 -- NET_CFG operations. */
enum {
    NETOP_START_AP      = 1,
    NETOP_JOIN_STA      = 2,
    NETOP_FORGET_STA    = 3,
    /* 4 is reserved (formerly ROTATE, §3.3.1). */
    NETOP_RESTART       = 5,
};

/* The longest NET_STATE is 13 bytes plus a 32-byte SSID. */
#define NET_STATE_MAX  (13 + 32)
/* The longest SESSION is 18 bytes plus a 63-byte passphrase. */
#define SESSION_MAX    (18 + 63)

/* Write a NET_STATE payload; returns its length, or 0 if `out` is too small. */
size_t netcfg_encode_state(uint8_t *out, size_t outsz,
                           const net_state_t *state, int ctrl_port,
                           int audio_port);

/* Write a SESSION payload; returns its length, or 0 if `out` is too small. */
size_t netcfg_encode_session(uint8_t *out, size_t outsz,
                             const uint8_t token[HERA_TOKEN_LEN],
                             const char *passphrase);

/* Decode a NET_CFG payload. Returns the op, or 0 if the payload is malformed,
 * has the wrong version, or declares lengths that exceed it. */
int netcfg_decode(const uint8_t *in, size_t len,
                  char *ssid, size_t ssidsz, char *psk, size_t psksz);

#endif /* HERA_NETCFG_H */
