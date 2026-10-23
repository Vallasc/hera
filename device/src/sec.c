#include "sec.h"

#include <sodium.h>
#include <string.h>

#include "util.h"

/* HKDF info labels (§11.3). */
static const char *LBL_CTRL_C2D  = "hera/v4/ctrl/c2d";
static const char *LBL_CTRL_D2C  = "hera/v4/ctrl/d2c";
static const char *LBL_AUDIO_C2D = "hera/v4/audio/c2d";
static const char *LBL_AUDIO_D2C = "hera/v4/audio/d2c";

/* Prefix of the signed HELLO transcript (§11.4). */
static const char *LBL_HELLO = "hera/v4/hello";

int sec_init(void)
{
    return sodium_init() < 0 ? -1 : 0;
}

int sec_keypair(sec_keypair_t *out)
{
    randombytes_buf(out->priv, sizeof out->priv);
    return crypto_scalarmult_curve25519_base(out->pub, out->priv) == 0 ? 0 : -1;
}

void sec_keypair_clear(sec_keypair_t *kp)
{
    sodium_memzero(kp, sizeof *kp);
}

void sec_hello_tag(const uint8_t *token, size_t token_len, uint8_t role,
                   uint8_t formats, uint8_t durations, uint8_t flags,
                   uint32_t session_id, const uint8_t pub[SEC_PUBKEY_LEN],
                   uint8_t tag[SEC_TAG_LEN])
{
    /* crypto_auth_hmacsha256() takes a 32-byte key only; the streaming API
     * accepts the 16-byte token. */
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, token, token_len);
    crypto_auth_hmacsha256_update(&st, (const uint8_t *)LBL_HELLO,
                                  strlen(LBL_HELLO));

    uint8_t fields[4];
    fields[0] = role;
    fields[1] = formats;
    fields[2] = durations;
    fields[3] = flags;
    crypto_auth_hmacsha256_update(&st, fields, sizeof fields);

    uint8_t sid[4];
    put_u32le(sid, session_id);
    crypto_auth_hmacsha256_update(&st, sid, sizeof sid);
    crypto_auth_hmacsha256_update(&st, pub, SEC_PUBKEY_LEN);

    uint8_t full[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256_final(&st, full);
    memcpy(tag, full, SEC_TAG_LEN);
    sodium_memzero(full, sizeof full);
}

int sec_tag_equal(const uint8_t a[SEC_TAG_LEN], const uint8_t b[SEC_TAG_LEN])
{
    return sodium_memcmp(a, b, SEC_TAG_LEN) == 0;
}

/* HKDF-SHA256 (RFC 5869) on top of HMAC-SHA256.
 *
 * libsodium provides crypto_kdf_hkdf_sha256_* only from 1.0.19, and Raspberry
 * Pi OS (trixie) ships 1.0.18. tests/test_proto.c checks this implementation
 * against RFC 5869 Appendix A and against the §14.1 vectors, which the Dart
 * client checks as well. */
void sec_hkdf_extract(uint8_t prk[SEC_HKDF_PRK_LEN],
                      const uint8_t *salt, size_t salt_len,
                      const uint8_t *ikm, size_t ikm_len)
{
    /* PRK = HMAC(key = salt, message = IKM). */
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, salt, salt_len);
    crypto_auth_hmacsha256_update(&st, ikm, ikm_len);
    crypto_auth_hmacsha256_final(&st, prk);
}

int sec_hkdf_expand(uint8_t *out, size_t out_len,
                    const char *info, size_t info_len,
                    const uint8_t prk[SEC_HKDF_PRK_LEN])
{
    if (out_len == 0 || out_len > 255u * crypto_auth_hmacsha256_BYTES)
        return -1;

    uint8_t block[crypto_auth_hmacsha256_BYTES];
    size_t done = 0;
    uint8_t counter = 1;

    while (done < out_len) {
        crypto_auth_hmacsha256_state st;
        crypto_auth_hmacsha256_init(&st, prk, SEC_HKDF_PRK_LEN);
        /* T(i) = HMAC(PRK, T(i-1) || info || i), with T(0) empty. */
        if (counter > 1)
            crypto_auth_hmacsha256_update(&st, block, sizeof block);
        crypto_auth_hmacsha256_update(&st, (const uint8_t *)info, info_len);
        crypto_auth_hmacsha256_update(&st, &counter, 1);
        crypto_auth_hmacsha256_final(&st, block);

        size_t take = out_len - done;
        if (take > sizeof block)
            take = sizeof block;
        memcpy(out + done, block, take);
        done += take;
        counter++;
    }
    sodium_memzero(block, sizeof block);
    return 0;
}

int sec_derive(sec_session_t *s, const sec_keypair_t *own,
               const uint8_t peer_pub[SEC_PUBKEY_LEN], int we_are_device)
{
    uint8_t shared[crypto_scalarmult_curve25519_BYTES];
    /* Rejects low-order peer keys, which would yield an all-zero secret. */
    if (crypto_scalarmult_curve25519(shared, own->priv, peer_pub) != 0)
        return -1;

    /* salt = pub_client || pub_device, regardless of role (§11.3). */
    uint8_t salt[2 * SEC_PUBKEY_LEN];
    const uint8_t *client_pub = we_are_device ? peer_pub : own->pub;
    const uint8_t *device_pub = we_are_device ? own->pub : peer_pub;
    memcpy(salt, client_pub, SEC_PUBKEY_LEN);
    memcpy(salt + SEC_PUBKEY_LEN, device_pub, SEC_PUBKEY_LEN);

    uint8_t prk[SEC_HKDF_PRK_LEN];
    sec_hkdf_extract(prk, salt, sizeof salt, shared, sizeof shared);
    sodium_memzero(shared, sizeof shared);

    memset(s, 0, sizeof *s);
    /* One key and one counter per channel and direction (§11.2). */
    const char *tx_ctrl  = we_are_device ? LBL_CTRL_D2C  : LBL_CTRL_C2D;
    const char *rx_ctrl  = we_are_device ? LBL_CTRL_C2D  : LBL_CTRL_D2C;
    const char *tx_audio = we_are_device ? LBL_AUDIO_D2C : LBL_AUDIO_C2D;
    const char *rx_audio = we_are_device ? LBL_AUDIO_C2D : LBL_AUDIO_D2C;

    int rc = 0;
    rc |= sec_hkdf_expand(s->ctrl_tx.key, 32, tx_ctrl, strlen(tx_ctrl), prk);
    rc |= sec_hkdf_expand(s->ctrl_rx.key, 32, rx_ctrl, strlen(rx_ctrl), prk);
    rc |= sec_hkdf_expand(s->audio_tx.key, 32, tx_audio, strlen(tx_audio), prk);
    rc |= sec_hkdf_expand(s->audio_rx.key, 32, rx_audio, strlen(rx_audio), prk);
    sodium_memzero(prk, sizeof prk);
    if (rc != 0)
        return -1;
    s->ready = 1;
    return 0;
}

void sec_clear(sec_session_t *s)
{
    sodium_memzero(s, sizeof *s);
}

/* nonce = 00 00 00 00 || counter (u64 LE). */
static void nonce_of(uint64_t counter, uint8_t out[12])
{
    memset(out, 0, 4);
    for (int i = 0; i < 8; i++)
        out[4 + i] = (uint8_t)(counter >> (8 * i));
}

static void put_u64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Sliding anti-replay window (§11.2), as in RFC 4303 §3.4.3. */
static int replay_accept(sec_rx_t *rx, uint64_t counter)
{
    if (!rx->primed) {
        rx->primed = 1;
        rx->highest = counter;
        rx->window = 1;
        return 1;
    }
    if (counter > rx->highest) {
        uint64_t advance = counter - rx->highest;
        rx->window = advance >= 64 ? 1 : ((rx->window << advance) | 1);
        rx->highest = counter;
        return 1;
    }
    uint64_t behind = rx->highest - counter;
    if (behind >= 64)
        return 0;                   /* left of the window */
    uint64_t bit = (uint64_t)1 << behind;
    if (rx->window & bit)
        return 0;                   /* already seen */
    rx->window |= bit;
    return 1;
}

/* ------------------------------------------------- control records (§11.2) */

size_t sec_record_len(const uint8_t *rec)
{
    size_t declared = (size_t)rec[0] | ((size_t)rec[1] << 8);
    if (declared < SEC_COUNTER_LEN + SEC_TAG_LEN ||
        declared > SEC_MAX_PLAIN + SEC_COUNTER_LEN + SEC_TAG_LEN)
        return 0;
    return 2 + declared;
}

size_t sec_seal_record(sec_tx_t *tx, const uint8_t *plain, size_t n,
                       uint8_t *out, size_t outsz)
{
    if (n == 0 || n > SEC_MAX_PLAIN)
        return 0;
    size_t total = n + SEC_RECORD_OVERHEAD;
    if (outsz < total)
        return 0;

    uint16_t rec_len = (uint16_t)(SEC_COUNTER_LEN + n + SEC_TAG_LEN);
    put_u16le(out, rec_len);
    put_u64le(out + 2, tx->counter);

    uint8_t nonce[12];
    nonce_of(tx->counter, nonce);

    unsigned long long written = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            out + 2 + SEC_COUNTER_LEN, &written, plain, n,
            out, 2 + SEC_COUNTER_LEN,        /* AAD: length and counter */
            NULL, nonce, tx->key) != 0)
        return 0;
    tx->counter++;
    return 2 + SEC_COUNTER_LEN + (size_t)written;
}

ssize_t sec_open_record(sec_rx_t *rx, const uint8_t *rec, size_t reclen,
                        uint8_t *out, size_t outsz)
{
    if (reclen < SEC_RECORD_OVERHEAD)
        return -1;
    if (sec_record_len(rec) != reclen)
        return -1;

    uint64_t counter = get_u64le(rec + 2);
    /* Counters on the control channel must strictly increase (§11.2). */
    if (rx->primed && counter <= rx->highest)
        return -1;

    size_t cipher_len = reclen - 2 - SEC_COUNTER_LEN;
    if (cipher_len < SEC_TAG_LEN || cipher_len - SEC_TAG_LEN > outsz)
        return -1;

    uint8_t nonce[12];
    nonce_of(counter, nonce);

    unsigned long long written = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            out, &written, NULL,
            rec + 2 + SEC_COUNTER_LEN, cipher_len,
            rec, 2 + SEC_COUNTER_LEN,
            nonce, rx->key) != 0)
        return -1;

    rx->primed = 1;
    rx->highest = counter;
    return (ssize_t)written;
}

/* ----------------------------------------------- audio datagrams (§11.2) */

size_t sec_seal_datagram(sec_tx_t *tx, uint32_t session_id,
                         const uint8_t *plain, size_t n,
                         uint8_t *out, size_t outsz)
{
    if (n == 0 || n > HERA_MAX_FRAME)
        return 0;
    size_t total = n + SEC_DATAGRAM_OVERHEAD;
    if (outsz < total)
        return 0;

    put_u32le(out, session_id);
    put_u64le(out + HERA_SID_LEN, tx->counter);

    uint8_t nonce[12];
    nonce_of(tx->counter, nonce);

    unsigned long long written = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            out + HERA_SID_LEN + SEC_COUNTER_LEN, &written, plain, n,
            out, HERA_SID_LEN + SEC_COUNTER_LEN,  /* AAD: session id, counter */
            NULL, nonce, tx->key) != 0)
        return 0;
    tx->counter++;
    return HERA_SID_LEN + SEC_COUNTER_LEN + (size_t)written;
}

ssize_t sec_open_datagram(sec_rx_t *rx, uint32_t session_id,
                          const uint8_t *dg, size_t dglen,
                          uint8_t *out, size_t outsz)
{
    if (dglen < SEC_DATAGRAM_OVERHEAD + 1)
        return -1;
    if (get_u32le(dg) != session_id)
        return -1;

    uint64_t counter = get_u64le(dg + HERA_SID_LEN);
    /* Check the window before decrypting, to reject replays cheaply. */
    if (rx->primed) {
        uint64_t behind = rx->highest >= counter ? rx->highest - counter : 0;
        if (counter <= rx->highest && behind >= 64)
            return -1;
        if (counter <= rx->highest && (rx->window & ((uint64_t)1 << behind)))
            return -1;
    }

    size_t cipher_len = dglen - HERA_SID_LEN - SEC_COUNTER_LEN;
    if (cipher_len < SEC_TAG_LEN || cipher_len - SEC_TAG_LEN > outsz)
        return -1;

    uint8_t nonce[12];
    nonce_of(counter, nonce);

    unsigned long long written = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            out, &written, NULL,
            dg + HERA_SID_LEN + SEC_COUNTER_LEN, cipher_len,
            dg, HERA_SID_LEN + SEC_COUNTER_LEN,
            nonce, rx->key) != 0)
        return -1;

    /* Update the window only after authentication succeeds, so that forged
     * counters cannot advance it. */
    replay_accept(rx, counter);
    return (ssize_t)written;
}
