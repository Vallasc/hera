#include "netcfg.h"

#include <string.h>

#include "util.h"

size_t netcfg_encode_state(uint8_t *out, size_t outsz,
                           const net_state_t *state, int ctrl_port,
                           int audio_port)
{
    size_t ssid_len = strnlen(state->ssid, sizeof state->ssid - 1);
    size_t total = 13 + ssid_len;
    if (outsz < total)
        return 0;

    out[0] = HERA_VERSION;
    out[1] = state->mode;
    out[2] = (uint8_t)state->phase;
    out[3] = state->failure;
    /* Dotted-quad order, not little endian (§3.2). */
    memcpy(out + 4, state->ipv4, 4);
    put_u16le(out + 8, (uint16_t)ctrl_port);
    put_u16le(out + 10, (uint16_t)audio_port);
    out[12] = (uint8_t)ssid_len;
    if (ssid_len)
        memcpy(out + 13, state->ssid, ssid_len);
    return total;
}

size_t netcfg_encode_session(uint8_t *out, size_t outsz,
                             const uint8_t token[HERA_TOKEN_LEN],
                             const char *passphrase)
{
    size_t psk_len = passphrase ? strlen(passphrase) : 0;
    if (psk_len > 255)
        psk_len = 255;
    size_t total = 2 + HERA_TOKEN_LEN + psk_len;
    if (outsz < total)
        return 0;

    out[0] = HERA_VERSION;
    memcpy(out + 1, token, HERA_TOKEN_LEN);
    out[1 + HERA_TOKEN_LEN] = (uint8_t)psk_len;
    if (psk_len)
        memcpy(out + 2 + HERA_TOKEN_LEN, passphrase, psk_len);
    return total;
}

int netcfg_decode(const uint8_t *in, size_t len,
                  char *ssid, size_t ssidsz, char *psk, size_t psksz)
{
    ssid[0] = psk[0] = '\0';
    if (len < 4 || in[0] != HERA_VERSION)
        return 0;

    int op = in[1];
    size_t ssid_len = in[2];
    size_t psk_len = in[3];
    /* Declared lengths must fit in the payload. */
    if (len < 4 + ssid_len + psk_len)
        return 0;
    if (ssid_len >= ssidsz || psk_len >= psksz)
        return 0;

    if (ssid_len)
        memcpy(ssid, in + 4, ssid_len);
    ssid[ssid_len] = '\0';
    if (psk_len)
        memcpy(psk, in + 4 + ssid_len, psk_len);
    psk[psk_len] = '\0';

    /* JOIN_STA requires an SSID. */
    if (op == NETOP_JOIN_STA && ssid_len == 0)
        return 0;
    if (op < NETOP_START_AP || op > NETOP_RESTART)
        return 0;
    return op;
}
