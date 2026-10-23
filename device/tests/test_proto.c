/* Host-side tests for the modules that do not depend on Bluetooth or Linux:
 * wire format, GATT payloads, format table, encrypted profile, jitter policy,
 * G.711 and the resampler. Run with `make test`.
 *
 * Byte vectors are copied from PROTOCOL.md §14 and §14.1. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "g711.h"
#include "jitter.h"
#include "netcfg.h"
#include "sec.h"
#include "proto.h"
#include "resample.h"
#include "stats.h"
#include "util.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static void check_bytes(const uint8_t *got, size_t gotlen,
                        const uint8_t *want, size_t wantlen, const char *what)
{
    int ok = (gotlen == wantlen) && memcmp(got, want, wantlen) == 0;
    check(ok, what);
    if (!ok) {
        printf("   want:");
        for (size_t i = 0; i < wantlen; i++)
            printf(" %02x", want[i]);
        printf("\n   got :");
        for (size_t i = 0; i < gotlen; i++)
            printf(" %02x", got[i]);
        printf("\n");
    }
}

/* ------------------------------------------------ §14 vectors: encoding --- */

static void test_build_hello(void)
{
    /* Device HELLO, v4: session_id 0x11223344, all formats and durations,
     * fixed public key and tag, firmware "hera-rpi 0.4.0". */
    uint8_t pub[32], tag[16];
    for (int i = 0; i < 32; i++) pub[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < 16; i++) tag[i] = (uint8_t)(0xA0 + i);

    uint8_t out[HERA_MAX_FRAME];
    size_t n = proto_build_hello(out, FMT_ALL, DUR_ALL, 0, 0x11223344,
                                 pub, tag, "hera-rpi 0.4.0");

    check(n == HERA_HDR_LEN + HERA_HELLO_LEN + 14, "HELLO is 82 bytes");
    check(out[0] == T_HELLO, "HELLO type");
    check(get_u16le(out + 2) == HERA_HELLO_LEN + 14, "HELLO LEN");

    const uint8_t *p = out + HERA_HDR_LEN;
    check(memcmp(p, "HERA", 4) == 0, "HERA magic");
    check(p[4] == 4, "version 4");
    check(p[5] == 1, "device role");
    check(p[6] == FMT_ALL, "format bitmap");
    check(p[7] == DUR_ALL, "duration bitmap");
    check(p[9] == 0 && p[10] == 0 && p[11] == 0, "reserved bytes are zero");
    check(get_u32le(p + 12) == 0x11223344, "session_id");
    check(memcmp(p + 16, pub, 32) == 0, "public key at offset 16");
    check(memcmp(p + 48, tag, 16) == 0, "tag at offset 48");
    check(memcmp(p + 64, "hera-rpi 0.4.0", 14) == 0, "firmware string after 64 bytes");

    /* The token must not appear anywhere in the frame (§11.4). */
    uint8_t token[HERA_TOKEN_LEN];
    for (int i = 0; i < HERA_TOKEN_LEN; i++) token[i] = (uint8_t)i;
    int found = 0;
    for (size_t i = 0; i + HERA_TOKEN_LEN <= n; i++)
        if (memcmp(out + i, token, HERA_TOKEN_LEN) == 0) found = 1;
    check(!found, "token does not appear in the frame");
}

static void test_build_events(void)
{
    uint8_t out[HERA_MAX_FRAME];
    size_t n;

    /* EVENT READY: a1 = 4, a2 = FMT_PCM16_8K. */
    static const uint8_t ready[] = {
        0x20, 0x00, 0x12, 0x00, 0x60, 0x04, 0x01, 0x00,
        0x68, 0x65, 0x72, 0x61, 0x2d, 0x72, 0x70, 0x69,
        0x20, 0x30, 0x2e, 0x33, 0x2e, 0x30,
    };
    n = proto_build_event(out, EV_READY, HERA_VERSION, FMT_PCM16_8K,
                          "hera-rpi 0.3.0");
    check_bytes(out, n, ready, sizeof ready, "EVENT READY");

    /* EVENT NET_MODE: AP, SSID "HERA-4C21". */
    static const uint8_t net[] = {
        0x20, 0x00, 0x0d, 0x00, 0x70, 0x01, 0x00, 0x00,
        0x48, 0x45, 0x52, 0x41, 0x2d, 0x34, 0x43, 0x32, 0x31,
    };
    n = proto_build_event(out, EV_NET_MODE, NET_MODE_AP, 0, "HERA-4C21");
    check_bytes(out, n, net, sizeof net, "EVENT NET_MODE");

    /* EVENT AUDIO_OPEN: CVSD at 8000 Hz, i.e. a2 = 80. */
    static const uint8_t open_[] = {
        0x20, 0x00, 0x04, 0x00, 0x10, 0x01, 0x50, 0x00,
    };
    n = proto_build_event(out, EV_AUDIO_OPEN, SCO_CODEC_CVSD, 8000 / 100, NULL);
    check_bytes(out, n, open_, sizeof open_, "EVENT AUDIO_OPEN");

    /* EVENT AUDIO_FMT: PCM16 at 8 kHz, 20 ms, epoch 1. */
    static const uint8_t fmt[] = {
        0x20, 0x00, 0x04, 0x00, 0x12, 0x01, 0x14, 0x01,
    };
    n = proto_build_event(out, EV_AUDIO_FMT, FMT_PCM16_8K,
                          DUR_20MS | (1 << 8), NULL);
    check_bytes(out, n, fmt, sizeof fmt, "EVENT AUDIO_FMT");

    /* EVENT CLIP "+390512345678". */
    static const uint8_t clip[] = {
        0x20, 0x00, 0x11, 0x00, 0x21, 0x00, 0x00, 0x00,
        0x2b, 0x33, 0x39, 0x30, 0x35, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38,
    };
    n = proto_build_event(out, EV_CLIP, 0, 0, "+390512345678");
    check_bytes(out, n, clip, sizeof clip, "EVENT CLIP");
}

static void test_build_bye(void)
{
    static const uint8_t want[] = {
        0x05, 0x00, 0x05, 0x00, 0x01, 0x73, 0x74, 0x6f, 0x70,
    };
    uint8_t out[HERA_MAX_FRAME];
    size_t n = proto_build_bye(out, BYE_NORMAL, "stop");
    check_bytes(out, n, want, sizeof want, "BYE");
}

static void test_build_stats(void)
{
    /* §14 vector: uptime 12345 ms, 600 sent, 601 received, 3 lost, 2 filled,
     * 30 ms buffered, 4 ms jitter, CVSD, flags 0x0F, CPU 12%, RSSI -47, AP,
     * PCM16 at 8 kHz. */
    static const uint8_t want[] = {
        0x31, 0x00, 0x2c, 0x00,
        0x39, 0x30, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00,
        0x59, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x1e, 0x00, 0x04, 0x00, 0x01, 0x0f, 0x0c, 0xd1,
        0x01, 0x01, 0x00, 0x00,
    };
    memset(&g_stats, 0, sizeof g_stats);
    STAT_ADD(dl_blocks, 600);
    STAT_ADD(ul_blocks, 601);
    STAT_ADD(ul_lost, 3);
    STAT_ADD(ul_fill, 2);

    stats_live_t live = {
        .ul_buffered_ms = 30,
        .ul_jitter_ms   = 4,
        .sco_codec      = SCO_CODEC_CVSD,
        .flags          = STF_SLC | STF_SCO_OPEN | STF_IN_CALL | STF_PROTECTED,
        .cpu_pct        = 12,
        .rssi           = -47,
        .net_mode       = NET_MODE_AP,
        .fmt            = FMT_PCM16_8K,
    };
    uint8_t payload[HERA_STATS_LEN];
    stats_serialise(payload, 12345, &live);

    uint8_t out[HERA_MAX_FRAME];
    size_t n = proto_build_stats(out, payload);
    check_bytes(out, n, want, sizeof want, "STATS");
}

static void test_build_audio(void)
{
    /* AUDIO_RX: PCM16 at 8 kHz, count 1, epoch 1, ts 0. The header is the §14
     * vector; samples are not compared. */
    static const uint8_t head[] = {
        0x10, 0x00, 0x48, 0x01, 0x01, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t samples[320];
    memset(samples, 0, sizeof samples);
    uint8_t out[HERA_MAX_FRAME];
    size_t n = proto_build_audio(out, FMT_PCM16_8K, DUR_20MS, 1, 1, 0, 0, samples);
    check(n == 332, "AUDIO_RX is 332 bytes");
    check_bytes(out, 12, head, sizeof head, "AUDIO_RX header");

    /* A count above the limit is rejected: PCM16 at 16 kHz allows a single
     * 20 ms block per payload (§6.3.1). */
    check(proto_build_audio(out, FMT_PCM16_16K, DUR_20MS, 2, 0, 0, 0, samples) == 0,
          "count above the limit rejected");
}

static void test_wrap_datagram(void)
{
    /* §14 datagram: AUDIO_TX, G.711 A-law silence, ts 48000, session_id
     * 0x11223344. */
    static const uint8_t head[] = {
        0x44, 0x33, 0x22, 0x11,
        0x11, 0x00, 0xa8, 0x00, 0x03, 0x14, 0x11, 0x01, 0x80, 0xbb, 0x00, 0x00,
    };
    uint8_t frame[HERA_MAX_FRAME];
    uint8_t samples[160];
    memset(samples, 0xD5, sizeof samples);
    size_t flen = proto_build_audio(frame, FMT_G711A, DUR_20MS, 1, 1, 1, 48000, samples);
    /* The builder writes AUDIO_RX; the vector is an AUDIO_TX frame. */
    frame[0] = T_AUDIO_TX;

    uint8_t out[HERA_SID_LEN + HERA_MAX_FRAME];
    size_t n = proto_wrap_datagram(out, 0x11223344, frame, flen);
    check(n == 176, "datagram is 176 bytes");
    check_bytes(out, 16, head, sizeof head, "datagram header");
}

/* ------------------------------------------------ §14 vectors: decoding --- */

static void test_parse_hello(void)
{
    /* Client HELLO, v4: new session, all formats, all durations. */
    uint8_t payload[HERA_HELLO_LEN + 11];
    memset(payload, 0, sizeof payload);
    memcpy(payload, "HERA", 4);
    payload[4] = HERA_VERSION;
    payload[5] = 0;                     /* client */
    payload[6] = FMT_ALL;
    payload[7] = DUR_ALL;
    payload[8] = 0;                     /* flags */
    put_u32le(payload + 12, 0);
    for (int i = 0; i < 32; i++) payload[16 + i] = (uint8_t)(0x20 + i);
    for (int i = 0; i < 16; i++) payload[48 + i] = (uint8_t)(0xB0 + i);
    memcpy(payload + HERA_HELLO_LEN, "hera_client", 11);

    hera_hello_t hello;
    int ok = proto_parse_hello(payload, sizeof payload, &hello) == 0;
    check(ok, "client HELLO accepted");
    if (!ok)
        return;
    check(hello.version == HERA_VERSION, "HELLO version 4");
    check(hello.role == 0, "HELLO client role");
    check(hello.formats == FMT_ALL, "HELLO all formats");
    check(hello.durations == DUR_ALL, "HELLO all durations");
    check(!hello.resume, "HELLO new session");
    check(hello.session_id == 0, "HELLO session_id is zero");
    check(hello.pubkey[0] == 0x20 && hello.pubkey[31] == 0x3f, "public key");
    check(hello.tag[0] == 0xB0 && hello.tag[15] == 0xBF, "tag");
    check(hello.ident_len == 11 &&
          memcmp(hello.ident, "hera_client", 11) == 0, "HELLO software string");

    /* Resume: flags bit 0 and the session_id to resume. `flags` is kept
     * verbatim because it is part of the HMAC input (§11.4). */
    payload[8] = 0x01;
    put_u32le(payload + 12, 0x11223344);
    ok = proto_parse_hello(payload, sizeof payload, &hello) == 0;
    check(ok && hello.resume && hello.session_id == 0x11223344 &&
          hello.flags == 0x01, "resume HELLO");
}

static void test_reject_bad_hello(void)
{
    uint8_t payload[HERA_HELLO_LEN];
    hera_hello_t hello;

    memset(payload, 0, sizeof payload);
    memcpy(payload, "HERA", 4);
    payload[4] = HERA_VERSION;
    payload[6] = FMT_ALL;
    payload[7] = DUR_ALL;
    check(proto_parse_hello(payload, sizeof payload, &hello) == 0,
          "minimal HELLO is valid");

    payload[4] = 3;
    check(proto_parse_hello(payload, sizeof payload, &hello) != 0,
          "previous version rejected");

    payload[4] = HERA_VERSION;
    memcpy(payload, "XERA", 4);
    check(proto_parse_hello(payload, sizeof payload, &hello) != 0,
          "wrong magic rejected");

    memcpy(payload, "HERA", 4);
    payload[6] = FMT_BIT(FMT_PCM16_16K);   /* no mandatory formats */
    check(proto_parse_hello(payload, sizeof payload, &hello) != 0,
          "missing mandatory formats rejected");

    payload[6] = FMT_ALL;
    payload[7] = DUR_BIT(DUR_5MS);          /* no 20 ms */
    check(proto_parse_hello(payload, sizeof payload, &hello) != 0,
          "missing mandatory 20 ms rejected");

    payload[7] = DUR_ALL;
    check(proto_parse_hello(payload, HERA_HELLO_LEN - 1, &hello) != 0,
          "truncated HELLO rejected");
}

static void test_parse_commands(void)
{
    /* CMD ANSWER, CMD DIAL "0512345", CMD SET_FORMAT G.711 A-law. */
    static const uint8_t answer[] = { 0x21, 0x00, 0x04, 0x00,
                                      0x01, 0x00, 0x00, 0x00 };
    static const uint8_t dial[] = { 0x21, 0x00, 0x0b, 0x00,
                                    0x03, 0x00, 0x00, 0x00,
                                    0x30, 0x35, 0x31, 0x32, 0x33, 0x34, 0x35 };
    static const uint8_t setfmt[] = { 0x21, 0x00, 0x04, 0x00,
                                      0x60, 0x03, 0x00, 0x00 };
    hera_tlv_t tlv;
    char text[32];

    check(proto_parse_tlv(answer + HERA_HDR_LEN, 4, &tlv) == 0 &&
          tlv.id == CMD_ANSWER, "CMD ANSWER");

    check(proto_parse_tlv(dial + HERA_HDR_LEN, 11, &tlv) == 0 &&
          tlv.id == CMD_DIAL, "CMD DIAL");
    proto_tlv_string(&tlv, text, sizeof text);
    check(strcmp(text, "0512345") == 0, "CMD DIAL number");

    check(proto_parse_tlv(setfmt + HERA_HDR_LEN, 4, &tlv) == 0 &&
          tlv.id == CMD_SET_FORMAT && tlv.a1 == FMT_G711A, "CMD SET_FORMAT");
}

static void test_parse_audio(void)
{
    uint8_t payload[HERA_AUDIO_HDR_LEN + 160];
    memset(payload, 0, sizeof payload);
    payload[0] = FMT_G711A;
    payload[1] = DUR_20MS;
    payload[2] = 0x11;                  /* filler + epoch 1 */
    payload[3] = 1;
    put_u32le(payload + 4, 48000);
    memset(payload + HERA_AUDIO_HDR_LEN, 0xD5, 160);

    hera_audio_t block;
    int ok = proto_parse_audio(payload, sizeof payload, FMT_G711A, DUR_20MS, &block) == 0;
    check(ok, "G.711 AUDIO_TX accepted");
    if (ok) {
        check(block.count == 1, "AUDIO_TX count");
        check(block.epoch == 1, "AUDIO_TX epoch");
        check(block.filler, "AUDIO_TX filler");
        check(block.ts == 48000, "AUDIO_TX ts");
        check(block.bytes == 160, "AUDIO_TX bytes");
    }

    /* Blocks that are dropped without closing the channel (§6.3). */
    check(proto_parse_audio(payload, sizeof payload, FMT_PCM16_8K, DUR_20MS, &block) != 0,
          "format not negotiated: dropped");
    payload[1] = 10;
    check(proto_parse_audio(payload, sizeof payload, FMT_G711A, DUR_20MS, &block) != 0,
          "frame_ms other than 20: dropped");
    payload[1] = DUR_20MS;
    payload[3] = 0;
    check(proto_parse_audio(payload, sizeof payload, FMT_G711A, DUR_20MS, &block) != 0,
          "zero count: dropped");
    payload[3] = 2;
    check(proto_parse_audio(payload, sizeof payload, FMT_G711A, DUR_20MS, &block) != 0,
          "inconsistent length: dropped");
    payload[3] = 1;
    check(proto_parse_audio(payload, HERA_AUDIO_HDR_LEN - 1, FMT_G711A, DUR_20MS, &block) != 0, "truncated payload: dropped");
}

static void test_parse_cstats(void)
{
    /* §14 vector: uptime 12345, 600 received, 2 lost, 601 sent, 40 ms
     * buffered, RTT 7 ms, flags 0x03. */
    static const uint8_t frame[] = {
        0x32, 0x00, 0x14, 0x00,
        0x39, 0x30, 0x00, 0x00, 0x58, 0x02, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x59, 0x02, 0x00, 0x00,
        0x28, 0x00, 0x07, 0x03,
    };
    hera_cstats_t cs;
    int ok = proto_parse_cstats(frame + HERA_HDR_LEN, HERA_CSTATS_LEN, &cs) == 0;
    check(ok, "CSTATS accepted");
    if (!ok)
        return;
    check(cs.uptime_ms == 12345, "CSTATS uptime");
    check(cs.dl_blocks == 600 && cs.dl_lost == 2, "CSTATS downlink");
    check(cs.ul_blocks == 601, "CSTATS uplink");
    check(cs.dl_buffered_ms == 40 && cs.rtt_ms == 7, "CSTATS buffer and RTT");
    check(cs.flags == 0x03, "CSTATS flags");
}

static void test_peel_datagram(void)
{
    uint8_t out[HERA_SID_LEN + HERA_MAX_FRAME];
    uint8_t frame[HERA_MAX_FRAME];
    uint8_t samples[160];
    memset(samples, 0xD5, sizeof samples);
    size_t flen = proto_build_audio(frame, FMT_G711A, DUR_20MS, 1, 0, 1, 0, samples);
    size_t n = proto_wrap_datagram(out, 0x11223344, frame, flen);

    const uint8_t *inner;
    size_t inner_len;
    check(proto_peel_datagram(out, n, 0x11223344, &inner, &inner_len) == 0 &&
          inner_len == flen, "datagram for this session accepted");

    /* Invalid datagrams are discarded (§4.2). */
    check(proto_peel_datagram(out, n, 0x99999999, &inner, &inner_len) != 0,
          "wrong session: dropped");
    check(proto_peel_datagram(out, HERA_SID_LEN + 2, 0x11223344, &inner,
                              &inner_len) != 0, "short datagram: dropped");
    check(proto_peel_datagram(out, n - 1, 0x11223344, &inner, &inner_len) != 0,
          "inconsistent length: dropped");
}

/* ----------------------------------------------------------- format table --- */

static void test_formats(void)
{
    check(fmt_rate(FMT_PCM16_8K) == 8000 && fmt_rate(FMT_PCM16_16K) == 16000 &&
          fmt_rate(FMT_G711A) == 8000, "format sample rates");
    check(fmt_block_bytes(FMT_PCM16_8K, DUR_20MS) == 320, "PCM16 8 kHz: 320 B/block");
    check(fmt_block_bytes(FMT_PCM16_16K, DUR_20MS) == 640, "PCM16 16 kHz: 640 B/block");
    check(fmt_block_bytes(FMT_G711A, DUR_20MS) == 160, "G.711: 160 B/block");
    check(fmt_block_samples(FMT_PCM16_16K, DUR_20MS) == 320, "samples per block");

    /* §6.3.1: the limit depends on the format. */
    check(fmt_max_blocks(FMT_PCM16_16K, DUR_20MS) == 1, "PCM16 16 kHz: 1 block");
    check(fmt_max_blocks(FMT_PCM16_8K, DUR_20MS) == 3, "PCM16 8 kHz: 3 blocks");
    check(fmt_max_blocks(FMT_G711A, DUR_20MS) == 4, "G.711: 4 blocks");

    check(fmt_silence(FMT_PCM16_8K) == 0x00 && fmt_silence(FMT_G711A) == 0xD5 &&
          fmt_silence(FMT_G711U) == 0xFF, "silence byte per format");
    check(fmt_rate(0x7F) == 0, "unknown format");
}

static void test_durations(void)
{
    check(dur_valid(20) && dur_valid(10) && dur_valid(5), "the three durations");
    check(!dur_valid(15) && !dur_valid(0) && !dur_valid(40), "other values are invalid");

    /* Bytes per block scale with the duration (§6.2). */
    check(fmt_block_bytes(FMT_PCM16_8K, DUR_10MS) == 160, "PCM16/8k at 10 ms");
    check(fmt_block_bytes(FMT_PCM16_8K, DUR_5MS) == 80, "PCM16/8k at 5 ms");
    check(fmt_block_bytes(FMT_G711A, DUR_5MS) == 40, "G.711 at 5 ms");
    check(fmt_block_samples(FMT_PCM16_8K, DUR_5MS) == 40, "samples at 5 ms");
    check(fmt_block_bytes(FMT_PCM16_8K, 15) == 0, "invalid duration");

    /* §6.3.1: the limit also depends on the duration. */
    check(fmt_max_blocks(FMT_PCM16_16K, DUR_20MS) == 1, "PCM16/16k at 20 ms: 1");
    check(fmt_max_blocks(FMT_PCM16_16K, DUR_10MS) == 3, "PCM16/16k at 10 ms: 3");
    check(fmt_max_blocks(FMT_PCM16_16K, DUR_5MS) == 4, "PCM16/16k at 5 ms: 4");

    /* Selection: the shortest duration offered (§6.2). */
    check(proto_pick_duration(DUR_ALL, 0) == DUR_5MS,
          "selects the shortest");
    check(proto_pick_duration(DUR_MANDATORY, 0) == DUR_20MS,
          "mandatory only: 20 ms");
    check(proto_pick_duration(DUR_BIT(DUR_20MS) | DUR_BIT(DUR_10MS), 0)
          == DUR_10MS, "without 5 ms: 10 ms");
    /* Forced by configuration, if offered by the client. */
    check(proto_pick_duration(DUR_ALL, 10) == DUR_10MS, "forced to 10 ms");
    check(proto_pick_duration(DUR_MANDATORY, 5) == DUR_20MS,
          "forced 5 ms not offered: falls back to 20");
    check(proto_pick_duration(0, 0) == DUR_20MS,
          "empty bitmap falls back to mandatory");

    /* A block declaring a duration other than the negotiated one is dropped
     * (§6.3). */
    uint8_t payload[HERA_AUDIO_HDR_LEN + 80];
    memset(payload, 0, sizeof payload);
    payload[0] = FMT_PCM16_8K;
    payload[1] = DUR_5MS;
    payload[3] = 1;
    hera_audio_t block;
    check(proto_parse_audio(payload, sizeof payload, FMT_PCM16_8K, DUR_5MS,
                            &block) == 0, "5 ms block accepted");
    check(block.frame_ms == DUR_5MS, "duration is read from the block");
    check(proto_parse_audio(payload, sizeof payload, FMT_PCM16_8K, DUR_20MS,
                            &block) != 0, "duration not negotiated: dropped");
}

static void test_pick_format(void)
{
    /* The format matching the SCO rate is preferred. */
    check(proto_pick_format(FMT_ALL, 8000, 0) == FMT_PCM16_8K,
          "selection with 8 kHz SCO");
    check(proto_pick_format(FMT_ALL, 16000, 0) == FMT_PCM16_16K,
          "selection with 16 kHz SCO");
    /* A client offering only the mandatory formats. */
    check(proto_pick_format(FMT_MANDATORY, 16000, 0) == FMT_PCM16_8K,
          "selection with mandatory formats only");

    /* Forced by HERA_FMT; honoured only if offered by the client. */
    check(proto_pick_format(FMT_ALL, 16000, FMT_G711A) == FMT_G711A,
          "HERA_FMT forces the format");
    check(proto_pick_format(FMT_MANDATORY, 16000, FMT_PCM16_16K) == FMT_PCM16_8K,
          "forced but not offered: falls back");
    /* Empty bitmap: PCM16 at 8 kHz is mandatory. */
    check(proto_pick_format(0, 8000, 0) == FMT_PCM16_8K,
          "empty bitmap falls back to mandatory");
}

/* -------------------------------------------------------------- resampler --- */

static void test_resample(void)
{
    /* A 300 Hz sine through 8 -> 16 -> 8 kHz keeps its energy.
     *
     * Input is fed in 20 ms blocks: both functions accept at most RS_MAX_BLOCK
     * samples per call and write nothing beyond that. */
    enum { BLK = 160, BLOCKS = 5, N = BLK * BLOCKS };
    int16_t in[N], up[N * 2], down[N];
    for (int i = 0; i < N; i++)
        in[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 300.0 * i / 8000.0));

    resample_up_t up_state;
    resample_down_t down_state;
    resample_up_reset(&up_state);
    resample_down_reset(&down_state);
    for (int b = 0; b < BLOCKS; b++) {
        resample_up(&up_state, in + b * BLK, BLK, up + b * BLK * 2);
        resample_down(&down_state, up + b * BLK * 2, BLK * 2, down + b * BLK);
    }

    /* Compare the second half, after the filter transient. */
    double energy_in = 0, energy_out = 0;
    for (int i = N / 2; i < N; i++) {
        energy_in += (double)in[i] * in[i];
        energy_out += (double)down[i] * down[i];
    }
    double ratio = energy_out / energy_in;
    check(ratio > 0.9 && ratio < 1.1, "round trip preserves signal energy");

    /* Splitting the input across calls must not change the output. */
    int16_t one_up[BLK * 2], two_up[BLK * 2];
    resample_up_t a, b2;
    resample_up_reset(&a);
    resample_up_reset(&b2);
    resample_up(&a, in, BLK, one_up);
    resample_up(&b2, in, BLK / 2, two_up);
    resample_up(&b2, in + BLK / 2, BLK / 2, two_up + BLK);
    check(memcmp(one_up, two_up, sizeof one_up) == 0,
          "split input gives identical output");

    /* Input longer than RS_MAX_BLOCK writes nothing. */
    int16_t guard[4] = { 1, 2, 3, 4 };
    int16_t too_big[RS_MAX_BLOCK + 1];
    memset(too_big, 0, sizeof too_big);
    resample_up_t c;
    resample_up_reset(&c);
    resample_up(&c, too_big, RS_MAX_BLOCK + 1, guard);
    check(guard[0] == 1 && guard[3] == 4,
          "oversized input writes nothing");
}

/* ------------------------------------------- GATT payloads: §14 vectors --- */

static void test_net_state(void)
{
    /* NET_STATE: AP, up, fail 0, IP 192.168.73.1, control 7420, audio 7421,
     * SSID "HERA-4C21". */
    static const uint8_t want[] = {
        0x04, 0x01, 0x02, 0x00, 0xc0, 0xa8, 0x49, 0x01,
        0xfc, 0x1c, 0xfd, 0x1c, 0x09,
        0x48, 0x45, 0x52, 0x41, 0x2d, 0x34, 0x43, 0x32, 0x31,
    };
    net_state_t state = {
        .mode = NET_MODE_AP,
        .phase = NET_UP,
        .failure = NETFAIL_NONE,
        .ipv4 = { 192, 168, 73, 1 },
    };
    snprintf(state.ssid, sizeof state.ssid, "HERA-4C21");

    uint8_t out[NET_STATE_MAX];
    size_t n = netcfg_encode_state(out, sizeof out, &state, 7420, 7421);
    check_bytes(out, n, want, sizeof want, "NET_STATE");

    /* No address: the four bytes are zero. */
    memset(state.ipv4, 0, sizeof state.ipv4);
    state.phase = NET_FAILED;
    state.failure = NETFAIL_AUTH;
    n = netcfg_encode_state(out, sizeof out, &state, 7420, 7421);
    check(n && out[2] == NET_FAILED && out[3] == NETFAIL_AUTH &&
          out[4] == 0 && out[7] == 0, "NET_STATE without an address");

    check(netcfg_encode_state(out, 4, &state, 7420, 7421) == 0,
          "NET_STATE into a short buffer writes nothing");
}

static void test_session_payload(void)
{
    /* SESSION: token 00..0f, passphrase "abcd1234efgh5678". */
    static const uint8_t want[] = {
        0x04,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10,
        0x61, 0x62, 0x63, 0x64, 0x31, 0x32, 0x33, 0x34,
        0x65, 0x66, 0x67, 0x68, 0x35, 0x36, 0x37, 0x38,
    };
    uint8_t token[HERA_TOKEN_LEN];
    for (int i = 0; i < HERA_TOKEN_LEN; i++)
        token[i] = (uint8_t)i;

    uint8_t out[SESSION_MAX];
    size_t n = netcfg_encode_session(out, sizeof out, token, "abcd1234efgh5678");
    check_bytes(out, n, want, sizeof want, "SESSION");

    /* Outside AP mode there is no passphrase (§3.4). */
    n = netcfg_encode_session(out, sizeof out, token, "");
    check(n == 18 && out[17] == 0, "SESSION without passphrase");
}

static void test_net_cfg(void)
{
    /* NET_CFG: JOIN_STA, SSID "CasaWiFi", passphrase "segreto123". */
    static const uint8_t in[] = {
        0x04, 0x02, 0x08, 0x0a,
        0x43, 0x61, 0x73, 0x61, 0x57, 0x69, 0x46, 0x69,
        0x73, 0x65, 0x67, 0x72, 0x65, 0x74, 0x6f, 0x31, 0x32, 0x33,
    };
    char ssid[64], psk[128];
    int op = netcfg_decode(in, sizeof in, ssid, sizeof ssid, psk, sizeof psk);
    check(op == NETOP_JOIN_STA, "NET_CFG JOIN_STA");
    check(strcmp(ssid, "CasaWiFi") == 0, "NET_CFG SSID");
    check(strcmp(psk, "segreto123") == 0, "NET_CFG passphrase");

    /* START_AP has no fields. */
    static const uint8_t ap[] = { 0x04, 0x01, 0x00, 0x00 };
    check(netcfg_decode(ap, sizeof ap, ssid, sizeof ssid, psk, sizeof psk) ==
          NETOP_START_AP, "NET_CFG START_AP");

    /* Malformed writes are rejected. */
    static const uint8_t short_[] = { 0x04, 0x02, 0x08, 0x0a, 0x43 };
    check(netcfg_decode(short_, sizeof short_, ssid, sizeof ssid, psk,
                        sizeof psk) == 0, "declared lengths exceed the payload");

    static const uint8_t v3[] = { 0x03, 0x01, 0x00, 0x00 };
    check(netcfg_decode(v3, sizeof v3, ssid, sizeof ssid, psk, sizeof psk) == 0,
          "previous version rejected");

    static const uint8_t empty_ssid[] = { 0x04, 0x02, 0x00, 0x00 };
    check(netcfg_decode(empty_ssid, sizeof empty_ssid, ssid, sizeof ssid, psk,
                        sizeof psk) == 0, "JOIN_STA without SSID rejected");

    static const uint8_t bad_op[] = { 0x04, 0x63, 0x00, 0x00 };
    check(netcfg_decode(bad_op, sizeof bad_op, ssid, sizeof ssid, psk,
                        sizeof psk) == 0, "unknown operation rejected");

    /* An SSID longer than the output buffer is rejected. */
    uint8_t huge[4 + 200];
    memset(huge, 'x', sizeof huge);
    huge[0] = HERA_VERSION;
    huge[1] = NETOP_JOIN_STA;
    huge[2] = 200;
    huge[3] = 0;
    char tiny[8];
    check(netcfg_decode(huge, sizeof huge, tiny, sizeof tiny, psk,
                        sizeof psk) == 0, "SSID longer than the buffer rejected");
}

/* ------------------------------------------------------------------- G.711 */

/* Signal-to-noise ratio between the original and the reconstruction, in dB. */
static double snr_db(const int16_t *original, const int16_t *rebuilt, size_t n)
{
    double signal = 0, noise = 0;
    for (size_t i = 0; i < n; i++) {
        double o = original[i];
        double d = (double)rebuilt[i] - o;
        signal += o * o;
        noise += d * d;
    }
    if (noise == 0)
        return 1e9;
    return 10.0 * log10(signal / noise);
}

static void test_g711(void)
{
    /* Silence codes (fmt_silence): a zero byte is not silence in G.711. */
    check(g711_alaw_encode(0) == 0xD5, "A-law: zero encodes to 0xD5");
    check(g711_ulaw_encode(0) == 0xFF, "mu-law: zero encodes to 0xFF");
    /* A-law has no exact zero: 0xD5 decodes to +8. mu-law 0xFF decodes to 0. */
    int16_t alaw_idle = g711_alaw_decode(0xD5);
    int16_t ulaw_idle = g711_ulaw_decode(0xFF);
    check(alaw_idle <= 8 && alaw_idle >= -8, "A-law: 0xD5 is the smallest magnitude");
    check(ulaw_idle == 0, "mu-law: 0xFF decodes to zero");

    /* Decoded values stay in range and the round trip is monotonic. */
    int ok_range = 1, ok_monotone = 1;
    int16_t previous = -32768;
    for (int b = 0; b < 256; b++) {
        int16_t a = g711_alaw_decode((uint8_t)b);
        int16_t u = g711_ulaw_decode((uint8_t)b);
        if (a == -32768 && b != 0x2A)
            ok_range = 0;
        (void)u;
    }
    for (int level = 0; level <= 255; level++) {
        int16_t decoded = g711_ulaw_decode(g711_ulaw_encode((int16_t)(level * 100)));
        if (decoded < previous - 200)
            ok_monotone = 0;
        previous = decoded;
    }
    check(ok_range, "A-law: no code decodes out of range");
    check(ok_monotone, "mu-law: round trip is monotonic");

    /* 300 Hz sine through both laws; expect SNR above 30 dB at this level. */
    enum { N = 800 };
    int16_t in[N], back[N];
    uint8_t coded[N];
    for (int i = 0; i < N; i++)
        in[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * 300.0 * i / 8000.0));

    g711_alaw_encode_block(in, N, coded);
    g711_alaw_decode_block(coded, N, back);
    double alaw = snr_db(in, back, N);
    check(alaw > 30.0, "A-law: SNR above 30 dB");

    g711_ulaw_encode_block(in, N, coded);
    g711_ulaw_decode_block(coded, N, back);
    double ulaw = snr_db(in, back, N);
    check(ulaw > 30.0, "mu-law: SNR above 30 dB");

    /* Full scale must saturate, not wrap. */
    int16_t extremes[] = { 32767, -32768, 32000, -32000 };
    int ok_sat = 1;
    for (size_t i = 0; i < sizeof extremes / sizeof *extremes; i++) {
        int16_t a = g711_alaw_decode(g711_alaw_encode(extremes[i]));
        int16_t u = g711_ulaw_decode(g711_ulaw_encode(extremes[i]));
        if ((extremes[i] > 0 && (a < 0 || u < 0)) ||
            (extremes[i] < 0 && (a > 0 || u > 0)))
            ok_sat = 0;
    }
    check(ok_sat, "full scale saturates without sign change");
}

/* ------------------------------------------------------- WiFi credentials --- */

static void test_credential_ok(void)
{
    /* IEEE 802.11 lengths: 1..32 for an SSID, 8..63 for a passphrase. */
    check(credential_ok("CasaWiFi", 1, 32), "plain SSID");
    check(credential_ok("segreto123", 8, 63), "plain passphrase");
    check(credential_ok("x", 1, 32), "one-character SSID");
    check(!credential_ok("", 1, 32), "empty SSID rejected");
    check(!credential_ok("corta", 8, 63), "passphrase under 8 characters rejected");

    char long_ssid[34];
    memset(long_ssid, 'a', sizeof long_ssid);
    long_ssid[32] = '\0';
    check(credential_ok(long_ssid, 1, 32), "32-character SSID");
    long_ssid[32] = 'a';
    long_ssid[33] = '\0';
    check(!credential_ok(long_ssid, 1, 32), "33-character SSID rejected");

    /* Quotes and backslashes would break the hostapd / wpa_supplicant
     * directive. */
    check(!credential_ok("ab\"cdefgh", 8, 63), "double quote rejected");
    check(!credential_ok("ab\\cdefgh", 8, 63), "backslash rejected");
    check(!credential_ok("line\nbreak", 8, 63), "newline rejected");
    check(!credential_ok("tab\there1", 8, 63), "tab rejected");

    /* Non-ASCII is rejected, although 802.11 allows UTF-8 SSIDs. */
    check(!credential_ok("caff\xc3\xa8", 1, 32), "UTF-8 rejected");
    check(!credential_ok("ctrl\x01here", 8, 63), "control character rejected");

    check(!credential_ok(NULL, 1, 32), "NULL rejected");

    /* All other printable ASCII is accepted, including spaces. */
    check(credential_ok("pa ss-w0rd!#$%&'()*+,-./:;<=>?@[]^_`{|}~", 8, 63),
          "spaces and symbols accepted");
}

/* ------------------------------------- encrypted profile: §14.1 vectors --- */

/* Key pairs from RFC 7748 §6.1. */
static const char *RFC7748_A_PRIV =
    "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
static const char *RFC7748_A_PUB =
    "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
static const char *RFC7748_B_PRIV =
    "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
static const char *RFC7748_B_PUB =
    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";

static void from_hex(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned byte = 0;
        sscanf(hex + 2 * i, "%2x", &byte);
        out[i] = (uint8_t)byte;
    }
}

static int hex_equal(const uint8_t *got, const char *want_hex, size_t n)
{
    uint8_t want[64];
    from_hex(want_hex, want, n);
    return memcmp(got, want, n) == 0;
}

/* Derive a client and a device session from the RFC 7748 key pairs. */
static void make_pair(sec_session_t *client, sec_session_t *device)
{
    sec_keypair_t c, d;
    from_hex(RFC7748_A_PRIV, c.priv, 32);
    from_hex(RFC7748_A_PUB, c.pub, 32);
    from_hex(RFC7748_B_PRIV, d.priv, 32);
    from_hex(RFC7748_B_PUB, d.pub, 32);
    check(sec_derive(client, &c, d.pub, 0) == 0, "client-side derivation");
    check(sec_derive(device, &d, c.pub, 1) == 0, "device-side derivation");
}

/* HKDF against RFC 5869 Appendix A (SHA-256 cases).
 *
 * The §14.1 vectors only show that the C and Dart implementations agree; these
 * check sec.c against the RFC. Case 1 requests 42 bytes, which exercises the
 * multi-block path that 32-byte keys never reach. */
static void test_hkdf_rfc5869(void)
{
    uint8_t ikm[22], salt[13], prk[SEC_HKDF_PRK_LEN], okm[42];

    /* Case 1: salt and info present. */
    memset(ikm, 0x0b, sizeof ikm);
    for (size_t i = 0; i < sizeof salt; i++)
        salt[i] = (uint8_t)i;
    const char info1[] = "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9";

    sec_hkdf_extract(prk, salt, sizeof salt, ikm, sizeof ikm);
    check(hex_equal(prk,
          "077709362c2e32df0ddc3f0dc47bba63"
          "90b6c73bb50f9c3122ec844ad7c2b3e5", 32),
          "RFC 5869 case 1: PRK");

    check(sec_hkdf_expand(okm, sizeof okm, info1, sizeof info1 - 1, prk) == 0,
          "RFC 5869 case 1: expand succeeds");
    check(hex_equal(okm,
          "3cb25f25faacd57a90434f64d0362f2a"
          "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
          "34007208d5b887185865", 42),
          "RFC 5869 case 1: 42-byte OKM");

    /* Case 3: empty salt and info. */
    sec_hkdf_extract(prk, NULL, 0, ikm, sizeof ikm);
    check(hex_equal(prk,
          "19ef24a32c717b167f33a91d6f648bdf"
          "96596776afdb6377ac434c1c293ccb04", 32),
          "RFC 5869 case 3: PRK with empty salt");

    check(sec_hkdf_expand(okm, sizeof okm, "", 0, prk) == 0,
          "RFC 5869 case 3: expand succeeds");
    check(hex_equal(okm,
          "8da4e775a563c18f715f802a063c5a31"
          "b8a11f5c5ee1879ec3454e5f3c738d2d"
          "9d201395faa4b61a96c8", 42),
          "RFC 5869 case 3: 42-byte OKM");

    /* L > 255 * HashLen is an error (RFC 5869 §2.3). */
    static uint8_t big[256 * 32];
    check(sec_hkdf_expand(big, sizeof big, "x", 1, prk) == -1,
          "more than 255 blocks rejected");
    check(sec_hkdf_expand(okm, 0, "x", 1, prk) == -1,
          "zero length rejected");
}

/* The HELLO vectors of §14, compared byte for byte. Public keys are those of
 * §14.1, so that the tags can be reproduced independently. */
static void test_hello_vectors(void)
{
    uint8_t token[16], zero[16] = {0};
    for (int i = 0; i < 16; i++) token[i] = (uint8_t)i;

    uint8_t cpub[32], dpub[32];
    from_hex(RFC7748_A_PUB, cpub, 32);
    from_hex(RFC7748_B_PUB, dpub, 32);

    uint8_t out[HERA_MAX_FRAME], tag[16];
    size_t n;

    /* 1. Client, new session. proto_build_hello() always writes role = 1,
     *    so the role byte is patched for the client vector. */
    sec_hello_tag(token, 16, 0, FMT_ALL, DUR_ALL, 0, 0, cpub, tag);
    check(hex_equal(tag, "dad8a1519105b1d27f84023495ed1f31", 16),
          "§14: client HELLO tag");
    n = proto_build_hello(out, FMT_ALL, DUR_ALL, 0, 0, cpub, tag,
                          "hera_client");
    out[HERA_HDR_LEN + 5] = 0;
    check(n == 79, "§14: client HELLO is 79 bytes");
    check(hex_equal(out, "01004b00", 4), "§14: client HELLO header");
    check(hex_equal(out + HERA_HDR_LEN,
          "4845524104000f070000000000000000", 16),
          "§14: client HELLO fixed part");

    /* 2. Device, session 0x11223344, all-zero token. */
    sec_hello_tag(zero, 16, 1, FMT_ALL, DUR_ALL, 0, 0x11223344, dpub, tag);
    check(hex_equal(tag, "4af4e67c6fc2bd17e5d88256173c89b3", 16),
          "§14: device HELLO tag");
    n = proto_build_hello(out, FMT_ALL, DUR_ALL, 0, 0x11223344, dpub, tag,
                          "hera-rpi 0.3.0");
    check(n == 82, "§14: device HELLO is 82 bytes");
    check(hex_equal(out, "01004e00", 4), "§14: device HELLO header");
    check(hex_equal(out + HERA_HDR_LEN,
          "4845524104010f070000000044332211", 16),
          "§14: device HELLO fixed part");

    /* 3. Client, resume: no string, fixed part only. */
    sec_hello_tag(token, 16, 0, FMT_ALL, DUR_ALL, 0x01, 0x11223344, cpub, tag);
    check(hex_equal(tag, "1dfe179e8ca691ce86b88c55e7a4d3ba", 16),
          "§14: resume HELLO tag");
    n = proto_build_hello(out, FMT_ALL, DUR_ALL, 0x01, 0x11223344, cpub, tag,
                          NULL);
    check(n == HERA_HDR_LEN + HERA_HELLO_LEN, "§14: resume HELLO is 68 bytes");
    check(hex_equal(out, "01004000", 4), "§14: resume HELLO header");

    /* `flags` and `session_id` are authenticated (§11.4). */
    uint8_t fresh[16];
    sec_hello_tag(token, 16, 0, FMT_ALL, DUR_ALL, 0, 0, cpub, fresh);
    check(memcmp(fresh, tag, 16) != 0,
          "resume tag differs from new-session tag");
}

static void test_sec_keys(void)
{
    sec_keypair_t kp;
    check(sec_keypair(&kp) == 0, "ephemeral keypair generated");

    /* Generation also succeeds when the struct is pre-filled. */
    sec_keypair_t fixed;
    from_hex(RFC7748_A_PRIV, fixed.priv, 32);
    sec_keypair_t derived = fixed;
    check(sec_keypair(&derived) == 0, "keypair generation succeeds");

    sec_session_t client, device;
    make_pair(&client, &device);

    /* §14.1: the four derived keys. */
    check(hex_equal(client.ctrl_tx.key,
          "9903b35c0c66fd2d1cef08bb9486cc279fa997cafac6fec911b1fb45ba2c8c2e", 32),
          "K_ctrl_c2d matches §14.1");
    check(hex_equal(client.ctrl_rx.key,
          "6870f486648a71b38c0a043f01cc5d3b97b6349b037a27038919883850be0e6c", 32),
          "K_ctrl_d2c matches §14.1");
    check(hex_equal(client.audio_tx.key,
          "6f548c424a8b48eea2689677fc6ce25d38fba71d9a20c13e5a5a81d788f21425", 32),
          "K_audio_c2d matches §14.1");
    check(hex_equal(client.audio_rx.key,
          "31e46fd34a28bd7e71f441e023e10a5d6e9e25c1a7caec91f222ffe9a42eb3a1", 32),
          "K_audio_d2c matches §14.1");

    /* Client tx keys must equal device rx keys and vice versa. */
    check(memcmp(client.ctrl_tx.key, device.ctrl_rx.key, 32) == 0,
          "ctrl: client tx = device rx");
    check(memcmp(client.ctrl_rx.key, device.ctrl_tx.key, 32) == 0,
          "ctrl: client rx = device tx");
    check(memcmp(client.audio_tx.key, device.audio_rx.key, 32) == 0,
          "audio: client tx = device rx");
    check(memcmp(client.ctrl_tx.key, client.audio_tx.key, 32) != 0,
          "control and audio keys differ");
}

static void test_sec_record(void)
{
    sec_session_t client, device;
    make_pair(&client, &device);

    /* §14.1 vector: CMD ANSWER, counter 0, K_ctrl_c2d. */
    static const uint8_t frame[] = { 0x21, 0x00, 0x04, 0x00,
                                     0x01, 0x00, 0x00, 0x00 };
    static const uint8_t want[] = {
        0x20, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x3d, 0x7a, 0x7d, 0x7f, 0x4a, 0x5f, 0x9c, 0x2a,
        0xe6, 0x26, 0x62, 0xcd, 0xa7, 0x1e, 0xd5, 0x97,
        0xb1, 0x81, 0x32, 0x44, 0xb2, 0x23, 0x00, 0xfc,
    };
    uint8_t rec[SEC_MAX_RECORD];
    size_t n = sec_seal_record(&client.ctrl_tx, frame, sizeof frame,
                               rec, sizeof rec);
    check_bytes(rec, n, want, sizeof want, "§14.1 control record");

    /* The device opens it. */
    uint8_t plain[SEC_MAX_PLAIN];
    ssize_t got = sec_open_record(&device.ctrl_rx, rec, n, plain, sizeof plain);
    check(got == (ssize_t)sizeof frame &&
          memcmp(plain, frame, sizeof frame) == 0, "record opens");

    /* The counter advances, so identical plaintexts seal differently. */
    uint8_t rec2[SEC_MAX_RECORD];
    size_t n2 = sec_seal_record(&client.ctrl_tx, frame, sizeof frame,
                                rec2, sizeof rec2);
    check(n == n2 && memcmp(rec, rec2, n) != 0,
          "same plaintext sealed twice differs");

    /* A modified byte fails authentication. */
    rec2[20] ^= 0x01;
    check(sec_open_record(&device.ctrl_rx, rec2, n2, plain, sizeof plain) < 0,
          "tampered record rejected");

    /* Control counters must increase; a replay is rejected (§11.2). */
    check(sec_open_record(&device.ctrl_rx, rec, n, plain, sizeof plain) < 0,
          "replayed record rejected");

    /* The wrong key does not open it. */
    sec_session_t other_c, other_d;
    make_pair(&other_c, &other_d);
    check(sec_open_record(&other_d.audio_rx, rec, n, plain, sizeof plain) < 0,
          "key of the other channel does not open the record");
}

static void test_sec_datagram(void)
{
    sec_session_t client, device;
    make_pair(&client, &device);
    const uint32_t sid = 0x11223344;

    uint8_t frame[32];
    memset(frame, 0xAB, sizeof frame);
    frame[0] = T_AUDIO_TX;

    uint8_t dg[SEC_DATAGRAM_OVERHEAD + sizeof frame];
    size_t n = sec_seal_datagram(&client.audio_tx, sid, frame, sizeof frame,
                                 dg, sizeof dg);
    check(n == sizeof frame + SEC_DATAGRAM_OVERHEAD,
          "datagram overhead is 28 bytes");

    uint8_t plain[HERA_MAX_FRAME];
    ssize_t got = sec_open_datagram(&device.audio_rx, sid, dg, n,
                                    plain, sizeof plain);
    check(got == (ssize_t)sizeof frame &&
          memcmp(plain, frame, sizeof frame) == 0, "datagram opens");

    /* Wrong session id. */
    check(sec_open_datagram(&device.audio_rx, 0x99999999, dg, n, plain,
                            sizeof plain) < 0, "wrong session rejected");

    /* The same datagram is not accepted twice. */
    check(sec_open_datagram(&device.audio_rx, sid, dg, n, plain,
                            sizeof plain) < 0, "replay rejected");

    /* Reordering within the window is accepted. */
    uint8_t a[64], b[64], c[64];
    size_t na = sec_seal_datagram(&client.audio_tx, sid, frame, 16, a, sizeof a);
    size_t nb = sec_seal_datagram(&client.audio_tx, sid, frame, 16, b, sizeof b);
    size_t nc = sec_seal_datagram(&client.audio_tx, sid, frame, 16, c, sizeof c);
    check(sec_open_datagram(&device.audio_rx, sid, c, nc, plain, sizeof plain) > 0,
          "third arrives first");
    check(sec_open_datagram(&device.audio_rx, sid, a, na, plain, sizeof plain) > 0,
          "then the first, late: accepted");
    check(sec_open_datagram(&device.audio_rx, sid, b, nb, plain, sizeof plain) > 0,
          "then the second: accepted");
    check(sec_open_datagram(&device.audio_rx, sid, a, na, plain, sizeof plain) < 0,
          "first again: rejected");

    /* Counters left of the window are rejected. */
    uint8_t far[64];
    for (int i = 0; i < 70; i++)
        sec_seal_datagram(&client.audio_tx, sid, frame, 16, far, sizeof far);
    size_t nf = sec_seal_datagram(&client.audio_tx, sid, frame, 16, far,
                                  sizeof far);
    check(sec_open_datagram(&device.audio_rx, sid, far, nf, plain,
                            sizeof plain) > 0, "forward jump accepted");
    check(sec_open_datagram(&device.audio_rx, sid, b, nb, plain,
                            sizeof plain) < 0,
          "datagram left of the window rejected");
}

static void test_sec_hello_tag(void)
{
    uint8_t token[HERA_TOKEN_LEN];
    for (int i = 0; i < HERA_TOKEN_LEN; i++)
        token[i] = (uint8_t)i;
    uint8_t pub[32];
    from_hex(RFC7748_A_PUB, pub, 32);

    uint8_t tag[SEC_TAG_LEN], same[SEC_TAG_LEN], other[SEC_TAG_LEN];
    sec_hello_tag(token, sizeof token, 0, FMT_ALL, 0x07, 0, 0, pub, tag);
    sec_hello_tag(token, sizeof token, 0, FMT_ALL, 0x07, 0, 0, pub, same);
    check(sec_tag_equal(tag, same), "same HELLO gives the same tag");

    /* Every authenticated field changes the tag (§11.4). */
    uint8_t other_pub[32];
    from_hex(RFC7748_B_PUB, other_pub, 32);
    sec_hello_tag(token, sizeof token, 0, FMT_ALL, 0x07, 0, 0, other_pub, other);
    check(!sec_tag_equal(tag, other), "public key changes the tag");

    sec_hello_tag(token, sizeof token, 1, FMT_ALL, 0x07, 0, 0, pub, other);
    check(!sec_tag_equal(tag, other), "role changes the tag");
    sec_hello_tag(token, sizeof token, 0, FMT_MANDATORY, 0x07, 0, 0, pub, other);
    check(!sec_tag_equal(tag, other), "formats change the tag");
    sec_hello_tag(token, sizeof token, 0, FMT_ALL, 0x01, 0, 0, pub, other);
    check(!sec_tag_equal(tag, other), "durations change the tag");
    sec_hello_tag(token, sizeof token, 0, FMT_ALL, 0x07, 1, 0, pub, other);
    check(!sec_tag_equal(tag, other), "flags change the tag");
    sec_hello_tag(token, sizeof token, 0, FMT_ALL, 0x07, 0, 42, pub, other);
    check(!sec_tag_equal(tag, other), "session_id changes the tag");

    /* A different token gives a different tag. */
    token[0] ^= 0xFF;
    sec_hello_tag(token, sizeof token, 0, FMT_ALL, 0x07, 0, 0, pub, other);
    check(!sec_tag_equal(tag, other), "different token gives a different tag");
}

/* ---------------------------------------------- adaptive jitter buffer --- */

/* `n` arrivals at exactly `frame_ms` intervals, starting after `t`. */
static uint64_t feed_steady(jitter_t *j, uint64_t t, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        t += j->frame_ms;
        jitter_on_arrival(j, t);
    }
    return t;
}

/* `n` arrivals alternating `dev` ms early and late. */
static uint64_t feed_jittery(jitter_t *j, uint64_t t, unsigned n, unsigned dev)
{
    for (unsigned i = 0; i < n; i++) {
        t += (i % 2) ? j->frame_ms + dev : (j->frame_ms > dev ? j->frame_ms - dev : 1);
        jitter_on_arrival(j, t);
    }
    return t;
}

static void test_jitter_adaptive(void)
{
    jitter_t j;

    /* The target starts at the floor. */
    jitter_reset(&j, 20, 20, 200);
    check(jitter_target_ms(&j) == 20, "starts at the floor");

    /* Regular arrivals keep the target low. */
    uint64_t t = feed_steady(&j, 1000, 200);
    check(jitter_target_ms(&j) <= 40, "regular arrivals: low target");

    /* 30 ms of jitter raises it. */
    jitter_reset(&j, 20, 20, 200);
    t = feed_jittery(&j, 1000, 300, 30);
    check(jitter_target_ms(&j) > 60, "jittery arrivals: target rises");
    check(jitter_target_ms(&j) <= 200, "but stays under the ceiling");

    /* It comes back down when arrivals become regular. */
    unsigned high = jitter_target_ms(&j);
    t = feed_steady(&j, t, 500);
    check(jitter_target_ms(&j) < high, "regular again: target falls");

    /* Floor and ceiling are enforced. */
    jitter_reset(&j, 20, 20, 60);
    feed_jittery(&j, 1000, 300, 100);
    check(jitter_target_ms(&j) == 60, "ceiling not exceeded");

    /* The floor is raised to one block. */
    jitter_reset(&j, 20, 5, 200);
    check(j.floor_ms == 20, "floor is at least one block");
}

static void test_jitter_small(void)
{
    /* Deviations of a few ms must still be tracked: J is kept in 1/16 ms. */
    jitter_t j;
    jitter_reset(&j, 20, 20, 200);
    feed_jittery(&j, 1000, 400, 6);
    check(j.jitter_ms >= 4 && j.jitter_ms <= 8, "6 ms jitter is measured");
    check(jitter_target_ms(&j) >= 32, "6 ms jitter raises the target");
}

static void test_jitter_underrun(void)
{
    jitter_t j;
    jitter_reset(&j, 20, 20, 200);
    feed_steady(&j, 1000, 100);
    unsigned before = jitter_target_ms(&j);

    /* An underrun raises the target immediately. */
    jitter_on_underrun(&j, 5000);
    check(jitter_target_ms(&j) >= before + 40, "underrun raises by two blocks");

    /* The increase survives later arrivals during the hold-down. */
    unsigned raised = jitter_target_ms(&j);
    uint64_t t = feed_steady(&j, 5000, 50);
    check(jitter_target_ms(&j) == raised,
          "target does not fall during hold-down");

    /* After the hold-down the estimate takes over again. */
    feed_steady(&j, t + JB_HOLDDOWN_MS + 1, 50);
    check(jitter_target_ms(&j) < raised,
          "after hold-down the estimate takes over");

    /* Shrinking is blocked during the hold-down. */
    jitter_reset(&j, 20, 20, 200);
    jitter_on_underrun(&j, 5000);
    unsigned deep = jitter_target_ms(&j) + 200;
    check(jitter_decide(&j, deep, 5000) == JB_HOLD,
          "no shrink right after an underrun");
    check(jitter_decide(&j, deep, 5000 + JB_HOLDDOWN_MS - 1) == JB_HOLD,
          "no shrink during hold-down");
    check(jitter_decide(&j, deep, 5000 + JB_HOLDDOWN_MS + 1) == JB_SHRINK,
          "shrinks after hold-down");
}

static void test_jitter_decide(void)
{
    jitter_t j;
    jitter_reset(&j, 20, 40, 200);
    /* target at the floor, 40 ms */

    check(jitter_decide(&j, 0, 10000) == JB_GROW, "empty: grow");
    check(jitter_decide(&j, 20, 10000) == JB_GROW, "one block below: grow");
    check(jitter_decide(&j, 40, 10000) == JB_HOLD, "at target: hold");
    check(jitter_decide(&j, 60, 10000) == JB_HOLD,
          "one block above: hold");
    check(jitter_decide(&j, 80, 10000) == JB_HOLD, "two blocks above: hold");
    check(jitter_decide(&j, 100, 10000) == JB_SHRINK,
          "more than two blocks above: shrink");

    /* Same thresholds with 5 ms blocks. */
    jitter_reset(&j, 5, 5, 100);
    check(jitter_decide(&j, 0, 10000) == JB_GROW, "5 ms: empty grows");
    check(jitter_decide(&j, 10, 10000) == JB_HOLD, "5 ms: one block above holds");
    check(jitter_decide(&j, 20, 10000) == JB_SHRINK, "5 ms: three blocks above shrinks");
}

static void test_jitter_restart(void)
{
    /* A gap of seconds is clamped and does not inflate the estimate. */
    jitter_t j;
    jitter_reset(&j, 20, 20, 400);
    uint64_t t = feed_steady(&j, 1000, 100);
    unsigned calm = jitter_target_ms(&j);

    t += 3000;                       /* 3 s without arrivals */
    jitter_on_arrival(&j, t);
    check(jitter_target_ms(&j) <= calm + 120,
          "long gap is clamped");

    /* The target recovers after regular arrivals. */
    feed_steady(&j, t, 200);
    check(jitter_target_ms(&j) <= calm + 20, "target recovers");
}

static void test_recovery(void)
{
    /* §2.5: three failed starts within five minutes force AP. */
    const int64_t now = 100000;
    int64_t two[] = { now - 100, now - 50 };
    int64_t three[] = { now - 250, now - 100, now - 50 };
    int64_t old[] = { now - 900, now - 100, now - 50 };
    check(!recovery_forces_ap(NULL, 0, now, 300, 3), "no earlier starts");
    check(!recovery_forces_ap(two, 2, now, 300, 3), "two failed starts");
    check(recovery_forces_ap(three, 3, now, 300, 3),
          "three failed starts in five minutes");
    check(!recovery_forces_ap(old, 3, now, 300, 3),
          "starts older than the window are ignored");
}

int main(void)
{
    printf("--- encoding (PROTOCOL.md §14 vectors) ---\n");
    test_build_hello();
    test_build_events();
    test_build_bye();
    test_build_stats();
    test_build_audio();
    test_wrap_datagram();

    printf("\n--- decoding ---\n");
    test_parse_hello();
    test_reject_bad_hello();
    test_parse_commands();
    test_parse_audio();
    test_parse_cstats();
    test_peel_datagram();

    printf("\n--- GATT payloads (§3) ---\n");
    test_net_state();
    test_session_payload();
    test_net_cfg();

    printf("\n--- formats ---\n");
    test_formats();
    test_pick_format();
    test_durations();

    printf("\n--- encrypted profile (§11, §14.1 vectors) ---\n");
    if (sec_init() != 0) {
        printf("cannot initialise libsodium\n");
        return 1;
    }
    test_hkdf_rfc5869();
    test_hello_vectors();
    test_sec_keys();
    test_sec_hello_tag();
    test_sec_record();
    test_sec_datagram();

    printf("\n--- credentials and recovery ---\n");
    test_credential_ok();
    test_recovery();

    printf("\n--- adaptive jitter buffer (§6.5) ---\n");
    test_jitter_adaptive();
    test_jitter_small();
    test_jitter_underrun();
    test_jitter_decide();
    test_jitter_restart();

    printf("\n--- G.711 ---\n");
    test_g711();

    printf("\n--- resampler ---\n");
    test_resample();

    printf("\n%s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
