/* hera_probe -- bring-up client for the HERA device.
 *
 * Opens both channels, performs the handshake, prints EVENT / ACK / STATS and
 * answers every AUDIO_RX with a filler AUDIO_TX (PROTOCOL.md §6.4), so that the
 * uplink counters advance as well. Useful to tell device problems from client
 * problems.
 *
 * Build:  make tools
 * Run:    ./tools/hera_probe 192.168.73.1 <token-hex>
 *         ./tools/hera_probe 192.168.73.1 <token-hex> answer
 *
 * The token argument is the 16-byte session token (§11.4) as 32 hex digits.
 * With the default configuration it is stored in /var/lib/hera/token:
 *
 *     xxd -p /var/lib/hera/token
 *
 * BLE is not used: the host must already be on the device's network and the
 * token must be known.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#include "sec.h"
#include "util.h"

static const char *event_name(uint8_t id)
{
    switch (id) {
    case EV_BT_CONNECTED:    return "BT_CONNECTED";
    case EV_BT_DISCONNECTED: return "BT_DISCONNECTED";
    case EV_SLC_UP:          return "SLC_UP";
    case EV_AUDIO_OPEN:      return "AUDIO_OPEN";
    case EV_AUDIO_CLOSE:     return "AUDIO_CLOSE";
    case EV_AUDIO_FMT:       return "AUDIO_FMT";
    case EV_RING:            return "RING";
    case EV_CLIP:            return "CLIP";
    case EV_CALL:            return "CALL";
    case EV_CALL_SETUP:      return "CALL_SETUP";
    case EV_CALL_HELD:       return "CALL_HELD";
    case EV_VOLUME:          return "VOLUME";
    case EV_VR:              return "VR";
    case EV_SERVICE:         return "SERVICE";
    case EV_SIGNAL:          return "SIGNAL";
    case EV_BATTERY:         return "BATTERY";
    case EV_OPER:            return "OPER";
    case EV_READY:           return "READY";
    case EV_NET_MODE:        return "NET_MODE";
    case EV_ERROR:           return "ERROR";
    default:                 return "?";
    }
}

static const char *fmt_name(uint8_t fmt)
{
    switch (fmt) {
    case FMT_PCM16_8K:  return "PCM16/8k";
    case FMT_PCM16_16K: return "PCM16/16k";
    case FMT_G711A:     return "G711A";
    case FMT_G711U:     return "G711U";
    default:            return "?";
    }
}

static int parse_token(const char *hex, uint8_t out[HERA_TOKEN_LEN])
{
    if (strlen(hex) != HERA_TOKEN_LEN * 2)
        return -1;
    for (int i = 0; i < HERA_TOKEN_LEN; i++) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1)
            return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}

/* ------------------------------------------------------------------ state */

static uint32_t session_id;
static uint8_t  fmt = FMT_PCM16_8K;
static unsigned frame_ms = DUR_20MS;
static sec_keypair_t kp;
static sec_session_t sec;
static uint8_t  epoch;
static uint32_t ul_ts;
static unsigned blocks_in, blocks_out, gaps;
static uint32_t expected_ts;
static int      audio_open;

/* Previous STATS, to print per-second rates (§13). */
static int      have_previous;
static uint32_t prev_uptime, prev_dl, prev_ul, prev_drop, prev_lost, prev_late;
static uint32_t prev_fill, prev_bad;

static void print_stats(const uint8_t *p)
{
    uint32_t uptime = get_u32le(p + 0);
    uint32_t dl = get_u32le(p + 4), ul = get_u32le(p + 8);
    uint32_t drop = get_u32le(p + 12), lost = get_u32le(p + 16);
    uint32_t late = get_u32le(p + 20), fill = get_u32le(p + 24);
    uint32_t bad = get_u32le(p + 28);
    unsigned buffered = get_u16le(p + 32), jitter = get_u16le(p + 34);
    uint8_t sco = p[36], flags = p[37], cpu = p[38];
    int8_t rssi = (int8_t)p[39];
    uint8_t mode = p[40], wire = p[41];

    if (!have_previous || uptime <= prev_uptime) {
        have_previous = 1;
        goto save;
    }

    double secs = (uptime - prev_uptime) / 1000.0;
#define RATE(now, before) ((unsigned)(((now) - (before)) / secs + 0.5))
    printf("STATS  dl %u/s  ul %u/s  | drop %u  lost %u  late %u  "
           "fill %u  bad %u | buffer %ums jitter %ums cpu %u%% "
           "rssi %ddBm | %s %s %s%s%s%s\n",
           RATE(dl, prev_dl), RATE(ul, prev_ul), RATE(drop, prev_drop),
           RATE(lost, prev_lost), RATE(late, prev_late), RATE(fill, prev_fill),
           RATE(bad, prev_bad), buffered, jitter, cpu, rssi,
           mode == NET_MODE_AP ? "hotspot" : mode == NET_MODE_STA ? "wifi" : "?",
           fmt_name(wire),
           (flags & STF_SLC) ? "slc " : "",
           (flags & STF_SCO_OPEN) ? "sco " : "",
           (flags & STF_IN_CALL) ? "call " : "",
           (flags & STF_PROTECTED) ? "wpa2" : "open-link");
    /* With 20 ms blocks and SCO open, dl_blocks grows by 50/s (§13). */
    if ((flags & STF_SCO_OPEN) && RATE(dl, prev_dl) < 45)
        printf("       ^ downlink below 50/s: see §13.1\n");
    if (sco == SCO_CODEC_NONE && (flags & STF_IN_CALL))
        printf("       ^ call active but no SCO codec\n");
#undef RATE

save:
    prev_uptime = uptime; prev_dl = dl; prev_ul = ul; prev_drop = drop;
    prev_lost = lost; prev_late = late; prev_fill = fill; prev_bad = bad;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <address> <token-hex> [answer]\n"
                "  the token is 32 hex digits: "
                "xxd -p /var/lib/hera/token\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    uint8_t token[HERA_TOKEN_LEN];
    if (parse_token(argv[2], token) != 0) {
        fprintf(stderr, "the token must be %d hex digits\n",
                HERA_TOKEN_LEN * 2);
        return 2;
    }
    int want_answer = (argc > 3 && strcmp(argv[3], "answer") == 0);

    struct sockaddr_in ctrl_addr = { .sin_family = AF_INET,
                                     .sin_port = htons(HERA_CTRL_PORT) };
    struct sockaddr_in audio_addr = { .sin_family = AF_INET,
                                      .sin_port = htons(HERA_AUDIO_PORT) };
    if (inet_pton(AF_INET, host, &ctrl_addr.sin_addr) != 1) {
        fprintf(stderr, "invalid address: %s\n", host);
        return 2;
    }
    audio_addr.sin_addr = ctrl_addr.sin_addr;

    int ctrl = socket(AF_INET, SOCK_STREAM, 0);
    int audio = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctrl < 0 || audio < 0) {
        perror("socket");
        return 1;
    }
    if (connect(ctrl, (struct sockaddr *)&ctrl_addr, sizeof ctrl_addr) != 0) {
        perror("connect");
        return 1;
    }
    int one = 1;
    setsockopt(ctrl, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("connected to %s: control tcp/%d, audio udp/%d\n", host,
           HERA_CTRL_PORT, HERA_AUDIO_PORT);

    if (sec_init() != 0 || sec_keypair(&kp) != 0) {
        fprintf(stderr, "cannot initialise libsodium\n");
        return 1;
    }

    /* Send HELLO without waiting for the device's (§5.3). All formats and
     * durations are offered, so the device's choice is visible in the output.
     * The token is only used to compute the tag (§11.4). */
    uint8_t frame[HERA_MAX_FRAME];
    uint8_t *p = frame + HERA_HDR_LEN;
    memset(p, 0, HERA_HELLO_LEN);
    memcpy(p, "HERA", 4);
    p[4] = HERA_VERSION;
    p[5] = 0;                       /* role: client */
    p[6] = FMT_ALL;
    p[7] = DUR_ALL;
    p[8] = 0;                       /* flags */
    memcpy(p + 16, kp.pub, 32);

    uint8_t tag[SEC_TAG_LEN];
    sec_hello_tag(token, HERA_TOKEN_LEN, 0, FMT_ALL, DUR_ALL, 0, 0, kp.pub, tag);
    memcpy(p + 48, tag, SEC_TAG_LEN);

    const char *ident = "hera_probe";
    size_t ilen = strlen(ident);
    memcpy(p + HERA_HELLO_LEN, ident, ilen);
    proto_put_header(frame, T_HELLO, (uint16_t)(HERA_HELLO_LEN + ilen));
    if (write(ctrl, frame, HERA_HDR_LEN + HERA_HELLO_LEN + ilen) < 0) {
        perror("write HELLO");
        return 1;
    }

    uint8_t rx[4096];
    size_t rx_len = 0;
    int64_t last_ping = 0;

    for (;;) {
        struct pollfd fds[2] = {
            { .fd = ctrl, .events = POLLIN },
            { .fd = audio, .events = POLLIN },
        };
        if (poll(fds, 2, 200) < 0 && errno != EINTR)
            break;

        /* Periodic PING on the audio channel binds the audio flow
         * (§4.2.1). */
        int64_t now = (int64_t)now_ms();
        if (sec.ready && now - last_ping > 500) {
            last_ping = now;
            uint8_t ping[HERA_HDR_LEN];
            proto_put_header(ping, T_PING, 0);
            uint8_t dg[SEC_DATAGRAM_OVERHEAD + HERA_HDR_LEN];
            size_t dlen = sec_seal_datagram(&sec.audio_tx, session_id, ping,
                                            sizeof ping, dg, sizeof dg);
            if (dlen)
                sendto(audio, dg, dlen, 0,
                       (struct sockaddr *)&audio_addr, sizeof audio_addr);
        }

        /* ---- audio channel ---- */
        if (fds[1].revents & POLLIN) {
            uint8_t dg[SEC_DATAGRAM_OVERHEAD + HERA_MAX_FRAME];
            ssize_t n = recv(audio, dg, sizeof dg, 0);
            uint8_t inner[HERA_MAX_FRAME];
            ssize_t ilen2 = (n > 0 && sec.ready)
                ? sec_open_datagram(&sec.audio_rx, session_id, dg, (size_t)n,
                                    inner, sizeof inner)
                : -1;
            if (ilen2 > HERA_HDR_LEN && inner[0] == T_AUDIO_RX) {
                hera_audio_t block;
                if (proto_parse_audio(inner + HERA_HDR_LEN,
                                      (size_t)ilen2 - HERA_HDR_LEN, fmt,
                                      frame_ms, &block) == 0) {
                    blocks_in += block.count;
                    if (block.epoch != epoch) {
                        epoch = block.epoch;
                        ul_ts = 0;
                        expected_ts = 0;
                    }
                    unsigned per = fmt_block_samples(block.fmt, block.frame_ms);
                    unsigned missing = 0;
                    if (expected_ts && block.ts > expected_ts && per)
                        missing = (block.ts - expected_ts) / per;
                    if (missing && missing <= 8)
                        gaps += missing;
                    expected_ts = block.ts + block.count * per;

                    /* One block out per block in, plus one per lost block
                     * (§6.4). Filled with the format's silence byte. */
                    unsigned send_count = block.count +
                                          (missing <= 8 ? missing : 0);
                    unsigned cap = fmt_max_blocks(fmt, frame_ms);
                    while (send_count) {
                        unsigned batch = send_count > cap ? cap : send_count;
                        unsigned bytes = batch * fmt_block_bytes(fmt, frame_ms);
                        uint8_t out[HERA_MAX_FRAME];
                        uint8_t *op = out + HERA_HDR_LEN;
                        op[0] = fmt;
                        op[1] = (uint8_t)frame_ms;
                        op[2] = (uint8_t)(0x01 | (epoch << 4));  /* filler */
                        op[3] = (uint8_t)batch;
                        put_u32le(op + 4, ul_ts);
                        memset(op + HERA_AUDIO_HDR_LEN, fmt_silence(fmt), bytes);
                        proto_put_header(out, T_AUDIO_TX,
                                         (uint16_t)(HERA_AUDIO_HDR_LEN + bytes));
                        size_t flen = HERA_HDR_LEN + HERA_AUDIO_HDR_LEN + bytes;

                        uint8_t odg[SEC_DATAGRAM_OVERHEAD + HERA_MAX_FRAME];
                        size_t total = sec_seal_datagram(&sec.audio_tx,
                                                         session_id, out, flen,
                                                         odg, sizeof odg);
                        if (total)
                            sendto(audio, odg, total, 0,
                                   (struct sockaddr *)&audio_addr,
                                   sizeof audio_addr);
                        ul_ts += batch * fmt_block_samples(fmt, frame_ms);
                        blocks_out += batch;
                        send_count -= batch;
                    }
                }
            }
        }

        /* ---- control channel ---- */
        if (!(fds[0].revents & (POLLIN | POLLHUP)))
            continue;
        ssize_t got = read(ctrl, rx + rx_len, sizeof rx - rx_len);
        if (got <= 0) {
            printf("control channel closed\n");
            break;
        }
        rx_len += (size_t)got;

        size_t off = 0;
        uint8_t plainbuf[SEC_MAX_PLAIN];
        for (;;) {
            const uint8_t *src;
            size_t srclen;
            size_t consumed;

            if (!sec.ready) {
                /* Plaintext phase: HELLO, or an ERROR sent before the keys
                 * exist (§11.2). */
                if (rx_len - off < HERA_HDR_LEN)
                    break;
                uint16_t plen = get_u16le(rx + off + 2);
                if (plen > HERA_MAX_PAYLOAD) {
                    printf("frame length %u out of range\n", plen);
                    return 1;
                }
                if (rx_len - off < (size_t)HERA_HDR_LEN + plen)
                    break;
                src = rx + off;
                srclen = (size_t)HERA_HDR_LEN + plen;
                consumed = srclen;
            } else {
                if (rx_len - off < 2)
                    break;
                size_t reclen = sec_record_len(rx + off);
                if (reclen == 0) {
                    printf("record length out of range\n");
                    return 1;
                }
                if (rx_len - off < reclen)
                    break;
                ssize_t got2 = sec_open_record(&sec.ctrl_rx, rx + off, reclen,
                                               plainbuf, sizeof plainbuf);
                if (got2 < 0) {
                    printf("record rejected: authentication failed\n");
                    return 1;
                }
                src = plainbuf;
                srclen = (size_t)got2;
                consumed = reclen;
            }
            off += consumed;

            const uint8_t *f = src;
            size_t plen = srclen - HERA_HDR_LEN;
            const uint8_t *pl = f + HERA_HDR_LEN;

            switch (f[0]) {
            case T_HELLO: {
                hera_hello_t hello;
                if (proto_parse_hello(pl, plen, &hello) == 0) {
                    session_id = hello.session_id;
                    printf("HELLO  session %08x, formats 0x%02x, durations 0x%02x,"
                           " \"%.*s\"\n", hello.session_id, hello.formats,
                           hello.durations, (int)hello.ident_len, hello.ident);
                    /* Verify the device's tag with the token (§11.4). */
                    uint8_t expect[SEC_TAG_LEN];
                    sec_hello_tag(token, HERA_TOKEN_LEN, hello.role,
                                  hello.formats, hello.durations, hello.flags,
                                  hello.session_id, hello.pubkey, expect);
                    if (!sec_tag_equal(expect, hello.tag)) {
                        printf("       device tag does not verify: "
                               "wrong token or on-path attacker\n");
                        return 1;
                    }
                    if (sec_derive(&sec, &kp, hello.pubkey, 0) != 0) {
                        printf("       unusable device public key\n");
                        return 1;
                    }
                    printf("       keys derived, encryption active\n");
                } else {
                    printf("invalid HELLO\n");
                }
                break;
            }
            case T_EVENT: {
                hera_tlv_t e;
                if (proto_parse_tlv(pl, plen, &e) != 0)
                    break;
                char text[128];
                proto_tlv_string(&e, text, sizeof text);
                printf("EVENT  %-16s a1=%u a2=%u%s%s\n", event_name(e.id),
                       e.a1, e.a2, *text ? " " : "", text);
                if (e.id == EV_READY) {
                    fmt = e.a2 & 0xFF;
                    printf("       selected format: %s\n", fmt_name(fmt));
                } else if (e.id == EV_AUDIO_FMT) {
                    fmt = e.a1;
                    frame_ms = e.a2 & 0xFF;
                    epoch = (uint8_t)((e.a2 >> 8) & 0x0F);
                    ul_ts = 0;
                    expected_ts = 0;
                    printf("       format %s, %u ms blocks, epoch %u\n",
                           fmt_name(fmt), frame_ms, epoch);
                } else if (e.id == EV_AUDIO_OPEN) {
                    audio_open = 1;
                } else if (e.id == EV_AUDIO_CLOSE) {
                    audio_open = 0;
                } else if (e.id == EV_RING && want_answer) {
                    uint8_t cmd[HERA_MAX_FRAME];
                    size_t cn = proto_build_event(cmd, CMD_ANSWER, 0, 0, NULL);
                    cmd[0] = T_CMD;
                    uint8_t rec[SEC_MAX_RECORD];
                    size_t rn = sec_seal_record(&sec.ctrl_tx, cmd, cn,
                                                rec, sizeof rec);
                    if (rn)
                        (void)!write(ctrl, rec, rn);
                    printf("       -> ANSWER\n");
                }
                break;
            }
            case T_ACK: {
                hera_tlv_t a;
                if (proto_parse_tlv(pl, plen, &a) == 0)
                    printf("ACK    command 0x%02x %s\n", a.id,
                           a.a1 == 0 ? "accepted" : "REJECTED");
                break;
            }
            case T_STATS:
                if (plen >= HERA_STATS_LEN)
                    print_stats(pl);
                break;
            case T_LOG:
                printf("LOG    %.*s\n", (int)plen, (const char *)pl);
                break;
            case T_BYE:
                printf("BYE    reason %u%s%.*s\n", plen ? pl[0] : 0,
                       plen > 1 ? " " : "", plen > 1 ? (int)plen - 1 : 0,
                       plen > 1 ? (const char *)pl + 1 : "");
                goto done;
            default:
                break;
            }
        }
        if (off) {
            memmove(rx, rx + off, rx_len - off);
            rx_len -= off;
        }
    }

done:
    printf("\nreceived %u blocks, sent %u, gaps filled %u%s\n",
           blocks_in, blocks_out, gaps,
           audio_open ? " (audio channel still open)" : "");
    close(ctrl);
    close(audio);
    return 0;
}
