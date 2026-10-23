#ifndef HERA_SEC_H
#define HERA_SEC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "proto.h"

/* Encrypted profile of PROTOCOL.md §11.
 *
 * - Ephemeral X25519 per control connection.
 * - HKDF-SHA256 expands the shared secret into four keys (channel x direction).
 * - ChaCha20-Poly1305 (IETF, 96-bit nonce) seals every control record and
 *   every audio datagram.
 * - Audio datagrams go through a 64-entry anti-replay window.
 *
 * The token is never transmitted. It keys the HMAC that authenticates each
 * HELLO (§11.4), which binds the ephemeral public key to the token and
 * prevents key substitution by an on-path attacker.
 *
 * ChaCha20-Poly1305 is used instead of AES-GCM because the ARMv6 core of the
 * Pi Zero W has no AES instructions; a software ChaCha20 is faster there and
 * has no table lookups. */

#define SEC_PUBKEY_LEN  32
#define SEC_TAG_LEN     16      /* truncated HMAC in HELLO, and Poly1305 tag */
#define SEC_COUNTER_LEN  8

/* Envelope sizes: §11.2. */
#define SEC_RECORD_OVERHEAD   (2 + SEC_COUNTER_LEN + SEC_TAG_LEN)   /* 26 */
#define SEC_DATAGRAM_OVERHEAD (HERA_SID_LEN + SEC_COUNTER_LEN + SEC_TAG_LEN) /* 28 */

/* A control record carries at most two maximum frames (§11.2). */
#define SEC_MAX_PLAIN   (2 * HERA_MAX_FRAME)
#define SEC_MAX_RECORD  (SEC_MAX_PLAIN + SEC_RECORD_OVERHEAD)

typedef struct {
    uint8_t priv[SEC_PUBKEY_LEN];
    uint8_t pub[SEC_PUBKEY_LEN];
} sec_keypair_t;

/* Sending half of one direction of one channel. */
typedef struct {
    uint8_t  key[32];
    uint64_t counter;           /* the next one to use; never goes back */
} sec_tx_t;

/* Receiving half, with the replay window.
 *
 * `highest` is the largest counter accepted so far and `window` is a bitmap of
 * the 64 counters ending at `highest`. The window is used on the audio channel
 * only; the control channel requires strictly increasing counters. */
typedef struct {
    uint8_t  key[32];
    uint64_t highest;
    uint64_t window;
    int      primed;
} sec_rx_t;

typedef struct {
    sec_tx_t ctrl_tx, audio_tx;
    sec_rx_t ctrl_rx, audio_rx;
    int      ready;
} sec_session_t;

/* Initialise libsodium. Must be called before any other function here. */
int sec_init(void);

/* Generate an ephemeral keypair (§11.3). */
int sec_keypair(sec_keypair_t *out);

/* Wipe a keypair. */
void sec_keypair_clear(sec_keypair_t *kp);

/* The HELLO tag of §11.4: HMAC-SHA256 over the signed fields, truncated to 16
 * bytes, keyed with the token. */
void sec_hello_tag(const uint8_t *token, size_t token_len, uint8_t role,
                   uint8_t formats, uint8_t durations, uint8_t flags,
                   uint32_t session_id, const uint8_t pub[SEC_PUBKEY_LEN],
                   uint8_t tag[SEC_TAG_LEN]);

/* Constant-time comparison of two tags. Returns 1 when equal. */
int sec_tag_equal(const uint8_t a[SEC_TAG_LEN], const uint8_t b[SEC_TAG_LEN]);

/* HKDF-SHA256 (RFC 5869), extract and expand.
 *
 * Implemented on top of HMAC-SHA256 because crypto_kdf_hkdf_sha256_* requires
 * libsodium 1.0.19 and Raspberry Pi OS ships 1.0.18. Exported so that the
 * test suite can check them against RFC 5869 Appendix A.
 *
 * `prk` is SEC_HKDF_PRK_LEN bytes. sec_hkdf_expand() returns -1 if `out_len`
 * is 0 or exceeds 255 * 32 bytes. */
#define SEC_HKDF_PRK_LEN 32

void sec_hkdf_extract(uint8_t prk[SEC_HKDF_PRK_LEN],
                      const uint8_t *salt, size_t salt_len,
                      const uint8_t *ikm, size_t ikm_len);
int sec_hkdf_expand(uint8_t *out, size_t out_len,
                    const char *info, size_t info_len,
                    const uint8_t prk[SEC_HKDF_PRK_LEN]);

/* Derive the four session keys (§11.3).
 *
 * The salt is always `pub_client || pub_device`; `we_are_device` tells which of
 * `own` and `peer_pub` is which. Returns 0 on success, or -1 if the peer key is
 * a low-order point. */
int sec_derive(sec_session_t *s, const sec_keypair_t *own,
               const uint8_t peer_pub[SEC_PUBKEY_LEN], int we_are_device);

/* Wipe all key material. */
void sec_clear(sec_session_t *s);

/* ----- control channel: a record layer over the frame stream (§11.2) ----- */

/* Seal `n` plaintext bytes into `out`. Returns the record length, or 0 if the
 * plaintext is empty or too large, or `out` is too small. */
size_t sec_seal_record(sec_tx_t *tx, const uint8_t *plain, size_t n,
                       uint8_t *out, size_t outsz);

/* Open one complete record. Returns the plaintext length, or -1 on a length,
 * counter or authentication failure. The caller must close the control
 * channel on -1 (§11.2). */
ssize_t sec_open_record(sec_rx_t *rx, const uint8_t *rec, size_t reclen,
                        uint8_t *out, size_t outsz);

/* Total length of the record starting at `rec`; at least 2 bytes must be
 * readable. Returns 0 if the declared length is out of range. */
size_t sec_record_len(const uint8_t *rec);

/* ----- audio channel: one sealed datagram per frame (§11.2) ----- */

/* Seal one frame into a datagram. Returns the datagram length, or 0 on error. */
size_t sec_seal_datagram(sec_tx_t *tx, uint32_t session_id,
                         const uint8_t *plain, size_t n,
                         uint8_t *out, size_t outsz);

/* Open a datagram. Returns the plaintext length, or -1 if the session id does
 * not match, the counter is a replay or the tag does not verify. The caller
 * drops the datagram silently (§4.2). */
ssize_t sec_open_datagram(sec_rx_t *rx, uint32_t session_id,
                          const uint8_t *dg, size_t dglen,
                          uint8_t *out, size_t outsz);

#endif /* HERA_SEC_H */
