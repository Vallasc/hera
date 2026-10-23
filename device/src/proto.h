#ifndef HERA_PROTO_H
#define HERA_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Wire format of the HERA protocol, version 4 (PROTOCOL.md §5-§10).
 * This module only serialises and parses: no I/O, no locking, no globals. It
 * therefore builds and runs on any host (`make test`). */

#define HERA_VERSION      4

/* §5.1 -- TYPE, FLAGS, LEN. There is no sequence number and no CRC: the
 * control channel runs over TCP, and audio loss is detected from `ts`. */
#define HERA_HDR_LEN      4
#define HERA_MAX_PAYLOAD  1024
#define HERA_MAX_FRAME    (HERA_HDR_LEN + HERA_MAX_PAYLOAD)

/* §5.3 -- fixed part of HELLO; an optional UTF-8 string may follow.
 *
 * The token is not carried in HELLO. It is only used as the HMAC key that
 * authenticates it (§11.4). */
#define HERA_HELLO_LEN    64
#define HERA_TOKEN_LEN    16

/* §6.3 -- audio payload header. */
#define HERA_AUDIO_HDR_LEN 8

/* §4.2 -- session prefix of a UDP datagram. */
#define HERA_SID_LEN      4

/* §6.2 -- block duration, negotiated among 20, 10 and 5 ms. It appears four
 * times in the latency budget (§7). 20 ms is mandatory for both peers. */
enum { DUR_20MS = 20, DUR_10MS = 10, DUR_5MS = 5 };

#define DUR_BIT(ms)   ((unsigned)((ms) == DUR_20MS ? 0x01 : \
                                  (ms) == DUR_10MS ? 0x02 : \
                                  (ms) == DUR_5MS  ? 0x04 : 0))
#define DUR_MANDATORY DUR_BIT(DUR_20MS)
#define DUR_ALL       (DUR_BIT(DUR_20MS) | DUR_BIT(DUR_10MS) | DUR_BIT(DUR_5MS))

/* The longest block, which is what every fixed-size buffer must hold. */
#define PROTO_MAX_FRAME_MS  DUR_20MS

/* True when `ms` is one of the three. */
int dur_valid(unsigned ms);

/* Select the block duration (§6.2): the shortest one offered by the client,
 * unless `forced_ms` names an offered duration. Falls back to 20 ms, which is
 * mandatory. */
unsigned proto_pick_duration(uint8_t offered, int forced_ms);

/* §10 -- payload sizes. */
#define HERA_STATS_LEN    44
#define HERA_CSTATS_LEN   20

/* Default ports (§4.5). */
#define HERA_CTRL_PORT    7420
#define HERA_AUDIO_PORT   7421

/* ----- frame types (§5.2) ----- */
enum {
    T_HELLO    = 0x01,
    T_PING     = 0x02,
    T_PONG     = 0x03,
    T_BYE      = 0x05,
    T_AUDIO_RX = 0x10,
    T_AUDIO_TX = 0x11,
    T_EVENT    = 0x20,
    T_CMD      = 0x21,
    T_ACK      = 0x22,
    T_LOG      = 0x30,
    T_STATS    = 0x31,
    T_CSTATS   = 0x32,
};

/* ----- audio formats (§6.2) -----
 * Linear PCM or G.711; no other codec is defined. The bit in the HELLO
 * `formats` bitmap is `id - 1`. */
enum {
    FMT_PCM16_8K  = 0x01,
    FMT_PCM16_16K = 0x02,
    FMT_G711A     = 0x03,
    FMT_G711U     = 0x04,
};

#define FMT_BIT(id)   (1u << ((id) - 1))
#define FMT_MANDATORY (FMT_BIT(FMT_PCM16_8K) | FMT_BIT(FMT_G711A))
#define FMT_ALL       (FMT_MANDATORY | FMT_BIT(FMT_PCM16_16K) | FMT_BIT(FMT_G711U))

/* Sample rate of a format, or 0 if the id is unknown. */
unsigned fmt_rate(uint8_t fmt);
/* Bytes in one block of `frame_ms`, or 0 if either argument is unknown. */
unsigned fmt_block_bytes(uint8_t fmt, unsigned frame_ms);
/* Samples in one block of `frame_ms`. */
unsigned fmt_block_samples(uint8_t fmt, unsigned frame_ms);
/* How many blocks fit in one payload, capped at 4 (§6.3.1). */
unsigned fmt_max_blocks(uint8_t fmt, unsigned frame_ms);
/* The byte that means silence: 0x00 for PCM, 0xD5 for A-law, 0xFF for mu-law. */
uint8_t  fmt_silence(uint8_t fmt);
/* True when the format carries 8-bit G.711 rather than 16-bit PCM. */
int      fmt_is_g711(uint8_t fmt);

/* ----- events (§8) ----- */
enum {
    EV_BT_CONNECTED    = 0x01,
    EV_BT_DISCONNECTED = 0x02,
    EV_SLC_UP          = 0x03,
    EV_AUDIO_OPEN      = 0x10,
    EV_AUDIO_CLOSE     = 0x11,
    EV_AUDIO_FMT       = 0x12,
    EV_RING            = 0x20,
    EV_CLIP            = 0x21,
    EV_CALL            = 0x22,
    EV_CALL_SETUP      = 0x23,
    EV_CALL_HELD       = 0x24,
    EV_VOLUME          = 0x30,
    EV_VR              = 0x40,
    EV_SERVICE         = 0x50,
    EV_SIGNAL          = 0x51,
    EV_BATTERY         = 0x52,
    EV_OPER            = 0x53,
    EV_READY           = 0x60,
    EV_NET_MODE        = 0x70,
    EV_ERROR           = 0x7F,
};

/* ----- ERROR codes (§8.2) ----- */
enum {
    ERR_BAD_HELLO     = 1,
    ERR_SHORT_PAYLOAD = 2,
    ERR_SCO           = 3,
    ERR_HFP           = 4,
    ERR_BAD_TOKEN     = 5,
    ERR_RESUME        = 6,
    ERR_NETWORK       = 7,
};

/* ----- BYE reasons (§5.6) ----- */
enum {
    BYE_NORMAL        = 1,
    BYE_BUSY          = 2,
    BYE_TAKEN_OVER    = 3,
    BYE_RECONFIGURING = 4,
};

/* ----- commands (§9) ----- */
enum {
    CMD_ANSWER        = 0x01,
    CMD_HANGUP        = 0x02,
    CMD_DIAL          = 0x03,
    CMD_REDIAL        = 0x04,
    CMD_AUDIO_CONNECT = 0x10,
    CMD_AUDIO_DISC    = 0x11,
    CMD_VOL_SPK       = 0x20,
    CMD_VOL_MIC       = 0x21,
    CMD_AUTO_ANSWER   = 0x30,
    CMD_CONNECT       = 0x40,
    CMD_DISCONNECT    = 0x41,
    CMD_VR_START      = 0x50,
    CMD_VR_STOP       = 0x51,
    CMD_SET_FORMAT    = 0x60,
    CMD_MUTE_UL       = 0x61,
    CMD_STATS         = 0x70,
    CMD_LOG_LEVEL     = 0x71,
    CMD_NET_MODE      = 0x72,
    CMD_REBOOT        = 0x7F,
};

/* SCO codec reported by AUDIO_OPEN / STATS. */
enum { SCO_CODEC_NONE = 0, SCO_CODEC_CVSD = 1, SCO_CODEC_MSBC = 2 };

/* Network modes (§2). Mode 3 (USB) is not implemented by this device; the
 * value is left undefined here so that it is never mistaken for another mode.
 *
 * NET_MODE_NONE is not a protocol value and never appears in NET_STATE. It is
 * a development mode in which the device leaves the network configuration
 * alone; clients see it as NET_MODE_STA, since the device is attached to an
 * existing network (§2.2). */
enum { NET_MODE_AP = 1, NET_MODE_STA = 2 };
#define NET_MODE_NONE 0

/* STATS flag bits (§10.1). */
enum {
    STF_SLC       = 1u << 0,
    STF_SCO_OPEN  = 1u << 1,
    STF_IN_CALL   = 1u << 2,
    STF_PROTECTED = 1u << 3,
};

/* Decoded 4-byte EVENT/CMD/ACK header plus the optional trailing string. */
typedef struct {
    uint8_t     id;
    uint8_t     a1;
    uint16_t    a2;
    const char *str;    /* NOT NUL-terminated; points into the payload */
    size_t      slen;
} hera_tlv_t;

/* Decoded HELLO (§5.3). */
typedef struct {
    uint8_t  version;
    uint8_t  role;          /* 0 client, 1 device */
    uint8_t  formats;       /* bitmap, see FMT_BIT */
    uint8_t  durations;     /* bitmap, see DUR_BIT */
    uint8_t  flags;         /* as it arrived, for the HMAC transcript */
    int      resume;        /* flags bit 0 */
    uint32_t session_id;
    uint8_t  pubkey[32];    /* ephemeral X25519 (§11.3) */
    uint8_t  tag[16];       /* HMAC over the signed fields (§11.4) */
    const char *ident;      /* NOT NUL-terminated */
    size_t   ident_len;
} hera_hello_t;

/* Decoded audio block header (§6.3). The samples follow it in the payload. */
typedef struct {
    uint8_t  fmt;
    uint8_t  frame_ms;      /* 20, 10 or 5 (§6.2) */
    uint8_t  count;         /* 1..fmt_max_blocks(fmt, frame_ms) */
    uint8_t  epoch;         /* flags bits 4-7 */
    int      filler;        /* flags bit 0 */
    uint32_t ts;
    const uint8_t *samples;
    size_t   bytes;
} hera_audio_t;

/* Decoded CSTATS (§10.2). Diagnostic only; the device does not depend on it. */
typedef struct {
    uint32_t uptime_ms;
    uint32_t dl_blocks;
    uint32_t dl_lost;
    uint32_t ul_blocks;
    uint16_t dl_buffered_ms;
    uint8_t  rtt_ms;
    uint8_t  flags;         /* bit0 playing, bit1 mic live, bit2 free-running */
} hera_cstats_t;

/* --- frame header ---------------------------------------------------- */

void proto_put_header(uint8_t *out, uint8_t type, uint16_t len);

/* --- builders: write a whole frame into `out` (>= HERA_MAX_FRAME bytes)
 *     and return its total length. --- */

size_t proto_build_hello(uint8_t *out, uint8_t formats, uint8_t durations,
                         uint8_t flags, uint32_t session_id,
                         const uint8_t pubkey[32], const uint8_t tag[16],
                         const char *ident);
size_t proto_build_pong(uint8_t *out, const uint8_t *payload, size_t len);
size_t proto_build_event(uint8_t *out, uint8_t id, uint8_t a1, uint16_t a2,
                         const char *str);
size_t proto_build_ack(uint8_t *out, uint8_t id, uint8_t a1);
size_t proto_build_bye(uint8_t *out, uint8_t reason, const char *str);
size_t proto_build_log(uint8_t *out, const char *text);
size_t proto_build_stats(uint8_t *out, const uint8_t stats[HERA_STATS_LEN]);

/* AUDIO_RX carrying `count` blocks of `fmt` (§6.3). The device sends
 * count = 1; batching would add latency for no useful saving on IP. */
size_t proto_build_audio(uint8_t *out, uint8_t fmt, unsigned frame_ms,
                         uint8_t count, uint8_t epoch, int filler, uint32_t ts,
                         const uint8_t *samples);

/* Wrap a frame in a UDP datagram's session prefix (§4.2). Returns the total
 * length; `out` must hold HERA_SID_LEN + frame_len bytes. */
size_t proto_wrap_datagram(uint8_t *out, uint32_t session_id,
                           const uint8_t *frame, size_t frame_len);

/* --- parsers ---------------------------------------------------------- */

/* Validate and split a HELLO payload. Returns 0 on success, -1 if it is short,
 * malformed, of the wrong version, or missing a mandatory format. */
int proto_parse_hello(const uint8_t *payload, size_t len, hera_hello_t *out);

/* Split a 4-byte EVENT/CMD/ACK payload. Returns 0 on success, -1 if short. */
int proto_parse_tlv(const uint8_t *payload, size_t len, hera_tlv_t *out);

/* Copy a hera_tlv_t string into a NUL-terminated buffer (may truncate). */
void proto_tlv_string(const hera_tlv_t *t, char *out, size_t outsz);

/* Split an audio payload (§6.3). Returns 0 on success, or -1 if the block is
 * malformed; the caller drops it and counts it in ul_bad, but does not close
 * the channel. `negotiated` is the format in use; blocks in any other format
 * are rejected. */
int proto_parse_audio(const uint8_t *payload, size_t len, uint8_t negotiated,
                      unsigned frame_ms, hera_audio_t *out);

/* Split a CSTATS payload. Returns 0 on success, -1 if short. */
int proto_parse_cstats(const uint8_t *payload, size_t len,
                       hera_cstats_t *out);

/* Unwrap a plaintext UDP datagram: check the length and the session id and
 * return the frame it carries. Returns 0 on success, or -1 if the datagram
 * does not belong to this session (§4.2). */
int proto_peel_datagram(const uint8_t *datagram, size_t len,
                        uint32_t session_id, const uint8_t **frame,
                        size_t *frame_len);

/* Select the audio format (§6.2).
 *
 * `offered` is the client's bitmap and `sco_rate` the SCO sample rate (8000 or
 * 16000). `forced` is a format set by configuration, or 0; it is honoured only
 * if the client offered it. Always returns a usable id, falling back to
 * FMT_PCM16_8K, which every client is required to support. */
uint8_t proto_pick_format(uint8_t offered, unsigned sco_rate, uint8_t forced);

#endif /* HERA_PROTO_H */
