#include "proto.h"

#include <string.h>

#include "util.h"

/* Offsets inside the 4-byte frame header (§5.1). */
enum { H_TYPE = 0, H_FLAGS = 1, H_LEN = 2 };

/* Offsets inside the HELLO payload (§5.3). */
enum {
    HL_MAGIC = 0, HL_VER = 4, HL_ROLE = 5, HL_FMT = 6, HL_DUR = 7,
    HL_FLAGS = 8, HL_RSVD = 9, HL_SID = 12, HL_PUB = 16, HL_TAG = 48,
};

/* Offsets inside the audio payload header (§6.3). */
enum { A_FMT = 0, A_MS = 1, A_FLAGS = 2, A_COUNT = 3, A_TS = 4 };

void proto_put_header(uint8_t *out, uint8_t type, uint16_t len)
{
    out[H_TYPE]  = type;
    out[H_FLAGS] = 0;                       /* reserved, always 0 */
    put_u16le(out + H_LEN, len);
}

/* Copy at most `max` bytes of `s` into the payload; returns bytes written. */
static size_t put_str(uint8_t *out, const char *s, size_t max)
{
    if (!s)
        return 0;
    size_t n = strlen(s);
    if (n > max)
        n = max;
    memcpy(out, s, n);
    return n;
}

/* --- formats ---------------------------------------------------------- */

unsigned fmt_rate(uint8_t fmt)
{
    switch (fmt) {
    case FMT_PCM16_8K:
    case FMT_G711A:
    case FMT_G711U:     return 8000;
    case FMT_PCM16_16K: return 16000;
    default:            return 0;
    }
}

int fmt_is_g711(uint8_t fmt)
{
    return fmt == FMT_G711A || fmt == FMT_G711U;
}

int dur_valid(unsigned ms)
{
    return ms == DUR_20MS || ms == DUR_10MS || ms == DUR_5MS;
}

unsigned proto_pick_duration(uint8_t offered, int forced_ms)
{
    if (dur_valid((unsigned)forced_ms) &&
        (offered & DUR_BIT((unsigned)forced_ms)))
        return (unsigned)forced_ms;

    /* Prefer the shortest duration (§6.2). */
    if (offered & DUR_BIT(DUR_5MS))  return DUR_5MS;
    if (offered & DUR_BIT(DUR_10MS)) return DUR_10MS;
    /* 20 ms is mandatory (§6.2). */
    return DUR_20MS;
}

unsigned fmt_block_samples(uint8_t fmt, unsigned frame_ms)
{
    if (!dur_valid(frame_ms))
        return 0;
    return fmt_rate(fmt) * frame_ms / 1000;
}

unsigned fmt_block_bytes(uint8_t fmt, unsigned frame_ms)
{
    unsigned samples = fmt_block_samples(fmt, frame_ms);
    return fmt_is_g711(fmt) ? samples : samples * 2;
}

unsigned fmt_max_blocks(uint8_t fmt, unsigned frame_ms)
{
    unsigned bytes = fmt_block_bytes(fmt, frame_ms);
    if (!bytes)
        return 0;
    unsigned fits = (HERA_MAX_PAYLOAD - HERA_AUDIO_HDR_LEN) / bytes;
    return fits > 4 ? 4 : fits;
}

uint8_t fmt_silence(uint8_t fmt)
{
    switch (fmt) {
    case FMT_G711A: return 0xD5;
    case FMT_G711U: return 0xFF;
    default:        return 0x00;
    }
}

uint8_t proto_pick_format(uint8_t offered, unsigned sco_rate, uint8_t forced)
{
    /* A configured format wins over the automatic choice, but only if the
     * client offered it. Same rule as proto_pick_duration(). */
    if (forced && (offered & FMT_BIT(forced)))
        return forced;

    /* Precedence of §6.2: the format that matches the SCO rate, and therefore
     * needs no conversion, comes first. */
    static const uint8_t ip8[]  = { FMT_PCM16_8K, FMT_PCM16_16K, FMT_G711A, FMT_G711U };
    static const uint8_t ip16[] = { FMT_PCM16_16K, FMT_PCM16_8K, FMT_G711A, FMT_G711U };

    const uint8_t *order = sco_rate >= 16000 ? ip16 : ip8;
    for (size_t i = 0; i < 4; i++) {
        if (offered & FMT_BIT(order[i]))
            return order[i];
    }
    /* FMT_PCM16_8K is mandatory (§6.2). */
    return FMT_PCM16_8K;
}

/* --- builders --------------------------------------------------------- */

size_t proto_build_hello(uint8_t *out, uint8_t formats, uint8_t durations,
                         uint8_t flags, uint32_t session_id,
                         const uint8_t pubkey[32], const uint8_t tag[16],
                         const char *ident)
{
    uint8_t *p = out + HERA_HDR_LEN;
    memset(p, 0, HERA_HELLO_LEN);
    memcpy(p + HL_MAGIC, "HERA", 4);
    p[HL_VER]   = HERA_VERSION;
    p[HL_ROLE]  = 1;                        /* device */
    p[HL_FMT]   = formats;
    p[HL_DUR]   = durations;
    p[HL_FLAGS] = flags;
    put_u32le(p + HL_SID, session_id);
    memcpy(p + HL_PUB, pubkey, 32);
    memcpy(p + HL_TAG, tag, 16);
    size_t len = HERA_HELLO_LEN +
                 put_str(p + HERA_HELLO_LEN, ident,
                         HERA_MAX_PAYLOAD - HERA_HELLO_LEN);
    proto_put_header(out, T_HELLO, (uint16_t)len);
    return HERA_HDR_LEN + len;
}

size_t proto_build_pong(uint8_t *out, const uint8_t *payload, size_t len)
{
    if (len > HERA_MAX_PAYLOAD)
        len = HERA_MAX_PAYLOAD;
    if (len)
        memcpy(out + HERA_HDR_LEN, payload, len);
    proto_put_header(out, T_PONG, (uint16_t)len);
    return HERA_HDR_LEN + len;
}

/* EVENT, CMD and ACK share the same 4-byte header + optional string (§8, §9). */
static size_t build_tlv(uint8_t *out, uint8_t type, uint8_t id, uint8_t a1,
                        uint16_t a2, const char *str)
{
    uint8_t *p = out + HERA_HDR_LEN;
    p[0] = id;
    p[1] = a1;
    put_u16le(p + 2, a2);
    size_t len = 4 + put_str(p + 4, str, HERA_MAX_PAYLOAD - 4);
    proto_put_header(out, type, (uint16_t)len);
    return HERA_HDR_LEN + len;
}

size_t proto_build_event(uint8_t *out, uint8_t id, uint8_t a1, uint16_t a2,
                         const char *str)
{
    return build_tlv(out, T_EVENT, id, a1, a2, str);
}

size_t proto_build_ack(uint8_t *out, uint8_t id, uint8_t a1)
{
    return build_tlv(out, T_ACK, id, a1, 0, NULL);
}

size_t proto_build_bye(uint8_t *out, uint8_t reason, const char *str)
{
    uint8_t *p = out + HERA_HDR_LEN;
    p[0] = reason;
    size_t len = 1 + put_str(p + 1, str, HERA_MAX_PAYLOAD - 1);
    proto_put_header(out, T_BYE, (uint16_t)len);
    return HERA_HDR_LEN + len;
}

size_t proto_build_log(uint8_t *out, const char *text)
{
    size_t len = put_str(out + HERA_HDR_LEN, text, 256);
    proto_put_header(out, T_LOG, (uint16_t)len);
    return HERA_HDR_LEN + len;
}

size_t proto_build_stats(uint8_t *out, const uint8_t stats[HERA_STATS_LEN])
{
    memcpy(out + HERA_HDR_LEN, stats, HERA_STATS_LEN);
    proto_put_header(out, T_STATS, HERA_STATS_LEN);
    return HERA_HDR_LEN + HERA_STATS_LEN;
}

size_t proto_build_audio(uint8_t *out, uint8_t fmt, unsigned frame_ms,
                         uint8_t count, uint8_t epoch, int filler, uint32_t ts,
                         const uint8_t *samples)
{
    unsigned block = fmt_block_bytes(fmt, frame_ms);
    if (!block || !count || count > fmt_max_blocks(fmt, frame_ms))
        return 0;

    uint8_t *p = out + HERA_HDR_LEN;
    p[A_FMT]   = fmt;
    p[A_MS]    = (uint8_t)frame_ms;
    p[A_FLAGS] = (uint8_t)((filler ? 0x01 : 0x00) | ((epoch & 0x0F) << 4));
    p[A_COUNT] = count;
    put_u32le(p + A_TS, ts);
    size_t bytes = (size_t)count * block;
    memcpy(p + HERA_AUDIO_HDR_LEN, samples, bytes);

    size_t len = HERA_AUDIO_HDR_LEN + bytes;
    proto_put_header(out, T_AUDIO_RX, (uint16_t)len);
    return HERA_HDR_LEN + len;
}

size_t proto_wrap_datagram(uint8_t *out, uint32_t session_id,
                           const uint8_t *frame, size_t frame_len)
{
    put_u32le(out, session_id);
    memcpy(out + HERA_SID_LEN, frame, frame_len);
    return HERA_SID_LEN + frame_len;
}

/* --- parsers ---------------------------------------------------------- */

int proto_parse_hello(const uint8_t *payload, size_t len, hera_hello_t *out)
{
    if (len < HERA_HELLO_LEN)
        return -1;
    if (memcmp(payload + HL_MAGIC, "HERA", 4) != 0)
        return -1;
    if (payload[HL_VER] != HERA_VERSION)
        return -1;

    uint8_t formats = payload[HL_FMT];
    /* Both mandatory formats must be offered (§5.3). */
    if ((formats & FMT_MANDATORY) != FMT_MANDATORY)
        return -1;
    uint8_t durations = payload[HL_DUR];
    /* 20 ms is mandatory (§6.2). */
    if ((durations & DUR_MANDATORY) != DUR_MANDATORY)
        return -1;

    out->version    = payload[HL_VER];
    out->role       = payload[HL_ROLE];
    out->formats    = formats;
    out->durations  = durations;
    out->flags      = payload[HL_FLAGS];
    out->resume     = (payload[HL_FLAGS] & 0x01) != 0;
    out->session_id = get_u32le(payload + HL_SID);
    memcpy(out->pubkey, payload + HL_PUB, 32);
    memcpy(out->tag, payload + HL_TAG, 16);
    out->ident      = (const char *)payload + HERA_HELLO_LEN;
    out->ident_len  = len - HERA_HELLO_LEN;
    return 0;
}

int proto_parse_tlv(const uint8_t *payload, size_t len, hera_tlv_t *out)
{
    if (len < 4)
        return -1;
    out->id   = payload[0];
    out->a1   = payload[1];
    out->a2   = get_u16le(payload + 2);
    out->str  = (const char *)payload + 4;
    out->slen = len - 4;
    return 0;
}

void proto_tlv_string(const hera_tlv_t *t, char *out, size_t outsz)
{
    size_t n = t->slen;
    if (n >= outsz)
        n = outsz - 1;
    if (n)
        memcpy(out, t->str, n);
    out[n] = '\0';
}

int proto_parse_audio(const uint8_t *payload, size_t len, uint8_t negotiated,
                      unsigned frame_ms, hera_audio_t *out)
{
    if (len < HERA_AUDIO_HDR_LEN)
        return -1;

    uint8_t fmt = payload[A_FMT];
    /* A block in a format other than the negotiated one is dropped; the
     * channel stays open (§6.3). */
    if (fmt != negotiated)
        return -1;
    if (payload[A_MS] != (uint8_t)frame_ms)
        return -1;

    uint8_t count = payload[A_COUNT];
    unsigned block = fmt_block_bytes(fmt, frame_ms);
    if (!block || !count || count > fmt_max_blocks(fmt, frame_ms))
        return -1;

    size_t bytes = (size_t)count * block;
    if (len != HERA_AUDIO_HDR_LEN + bytes)
        return -1;

    out->fmt      = fmt;
    out->frame_ms = (uint8_t)frame_ms;
    out->count    = count;
    out->epoch   = (payload[A_FLAGS] >> 4) & 0x0F;
    out->filler  = (payload[A_FLAGS] & 0x01) != 0;
    out->ts      = get_u32le(payload + A_TS);
    out->samples = payload + HERA_AUDIO_HDR_LEN;
    out->bytes   = bytes;
    return 0;
}

int proto_parse_cstats(const uint8_t *payload, size_t len, hera_cstats_t *out)
{
    if (len < HERA_CSTATS_LEN)
        return -1;
    out->uptime_ms      = get_u32le(payload + 0);
    out->dl_blocks      = get_u32le(payload + 4);
    out->dl_lost        = get_u32le(payload + 8);
    out->ul_blocks      = get_u32le(payload + 12);
    out->dl_buffered_ms = get_u16le(payload + 16);
    out->rtt_ms         = payload[18];
    out->flags          = payload[19];
    return 0;
}

int proto_peel_datagram(const uint8_t *datagram, size_t len,
                        uint32_t session_id, const uint8_t **frame,
                        size_t *frame_len)
{
    if (len < HERA_SID_LEN + HERA_HDR_LEN)
        return -1;
    if (get_u32le(datagram) != session_id)
        return -1;

    const uint8_t *f = datagram + HERA_SID_LEN;
    size_t flen = len - HERA_SID_LEN;
    /* Exactly one frame per datagram (§4.2). */
    if (flen != (size_t)HERA_HDR_LEN + get_u16le(f + H_LEN))
        return -1;

    *frame     = f;
    *frame_len = flen;
    return 0;
}
