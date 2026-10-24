/* Interoperability server: the device's link (link.c, with the real
 * handshake, encryption, resume and timeouts) behind a scripted relay, for
 * testing a client against it without Bluetooth.
 *
 *   make tests/link_server && ./tests/link_server
 *
 * Listens on HERA_CTRL_PORT / HERA_AUDIO_PORT (7420/7421). The token is
 * 00 01 02 ... 0f. Once a session is up it sends the state snapshot, STATS
 * every second and a 20 ms block of downlink audio every 20 ms; it answers
 * CMD ANSWER with CALL 1 and CMD HANGUP with CALL 0. Stops on SIGINT or
 * SIGTERM.
 *
 * With PCM16 at 8 kHz the downlink carries a 300 Hz sine and the uplink is
 * checked for a continuous 440 Hz sine; the result is logged as
 * "uplink check: N samples, M glitches".
 */

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "link.h"
#include "log.h"
#include "proto.h"
#include "stats.h"
#include "util.h"

static const uint8_t TOKEN[HERA_TOKEN_LEN] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

static link_t *g_link;

/* Continuity of the uplink sine: x[n] = 2 cos(w) x[n-1] - x[n-2]. Link
 * thread only. */
static int16_t g_prev[2];
static unsigned long g_checked, g_glitches, g_seen;

static int g_stop;
static uint64_t g_start;
static uint32_t g_uplink;

static void on_up(void *ud, int resumed)
{
    /* A new stream: the next uplink samples need not continue the old sine. */
    g_seen = 0;
    log_info("server", "session up%s", resumed ? " (resumed)" : "");
    link_send_event(g_link, EV_NET_MODE, 1, 0, "HERA-TEST");
    link_send_event(g_link, EV_BT_CONNECTED, 0, 0, "00:11:22:33:44:55");
    link_send_event(g_link, EV_SLC_UP, 0, 0, "00:11:22:33:44:55");
    link_send_event(g_link, EV_SERVICE, 1, 0, NULL);
    link_send_event(g_link, EV_CALL, 0, 0, NULL);
    link_new_audio_epoch(g_link);
    link_set_sco_rate(g_link, SCO_RATE);
    link_send_event(g_link, EV_AUDIO_OPEN, SCO_CODEC_CVSD, SCO_RATE / 100,
                    NULL);
    link_send_event(g_link, EV_AUDIO_FMT, link_format(g_link),
                    (uint16_t)(link_frame_ms(g_link) |
                               (link_epoch(g_link) << 8)), NULL);
}

static void on_down(void *ud, int expired)
{
    log_info("server", "session down%s", expired ? " (expired)" : "");
}

static void on_suspend(void *ud)
{
    log_info("server", "session suspended");
}

static void on_cmd(void *ud, const hera_tlv_t *cmd)
{
    log_info("server", "command 0x%02x", cmd->id);
    link_send_ack(g_link, cmd->id, 0);
    if (cmd->id == CMD_ANSWER)
        link_send_event(g_link, EV_CALL, 1, 0, NULL);
    else if (cmd->id == CMD_HANGUP)
        link_send_event(g_link, EV_CALL, 0, 0, NULL);
}


static void check_uplink(const int16_t *s, size_t n)
{
    const double k = 2.0 * cos(2.0 * M_PI * 440.0 / 8000.0);
    for (size_t i = 0; i < n; i++) {
        if (g_seen >= 2 && g_seen > 800) {
            double predicted = k * g_prev[1] - g_prev[0];
            if (fabs(predicted - s[i]) > 4)
                g_glitches++;
            g_checked++;
        }
        g_prev[0] = g_prev[1];
        g_prev[1] = s[i];
        g_seen++;
    }
}

static void on_audio(void *ud, const hera_audio_t *block)
{
    uint32_t n = __atomic_add_fetch(&g_uplink, block->count, __ATOMIC_RELAXED);
    if (block->fmt == FMT_PCM16_8K) {
        int16_t pcm[4 * 160];
        size_t samples = block->bytes / 2;
        if (samples > sizeof pcm / sizeof pcm[0])
            samples = sizeof pcm / sizeof pcm[0];
        memcpy(pcm, block->samples, samples * 2);
        check_uplink(pcm, samples);
    }
    if (n % 250 < block->count)
        log_info("server", "uplink check: %lu samples, %lu glitches "
                 "(%u blocks)", g_checked, g_glitches, n);
}

static void on_tick(void *ud)
{
    stats_live_t live;
    memset(&live, 0, sizeof live);
    live.fmt = link_format(g_link);
    live.flags = STF_PROTECTED;
    uint8_t payload[HERA_STATS_LEN];
    stats_serialise(payload, (uint32_t)(now_ms() - g_start), &live);
    link_send_stats(g_link, payload);
}

/* Downlink audio paced at 20 ms, like the SCO thread. */
static void *audio_thread(void *arg)
{
    uint8_t samples[2 * SCO_FRAME_BYTES];
    memset(samples, 0, sizeof samples);
    uint32_t ts = 0;
    long phase = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        sleep_ms(20);
        if (!link_is_up(g_link))
            continue;
        uint8_t fmt = link_format(g_link);
        unsigned ms = link_frame_ms(g_link);
        if (!fmt)
            continue;
        unsigned n = fmt_block_samples(fmt, ms);
        for (unsigned i = 0; i < 20 / ms; i++) {
            if (fmt == FMT_PCM16_8K) {
                /* 300 Hz sine, little endian, continuous across blocks. */
                for (unsigned j = 0; j < n; j++) {
                    int16_t v = (int16_t)lrint(8000.0 * sin(2.0 * M_PI * 300.0 *
                                                            (double)phase++ / 8000.0));
                    samples[2 * j] = (uint8_t)(v & 0xFF);
                    samples[2 * j + 1] = (uint8_t)((uint16_t)v >> 8);
                }
            } else {
                memset(samples, fmt_silence(fmt), fmt_block_bytes(fmt, ms));
            }
            link_send_audio(g_link, samples, ts);
            ts += n;
        }
    }
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    config_load();
    g_start = now_ms();

    link_cb_t cb = {
        .on_up = on_up, .on_down = on_down, .on_suspend = on_suspend,
        .on_cmd = on_cmd, .on_audio = on_audio, .on_tick = on_tick,
    };
    g_link = link_create(&cb, "hera-link-server");
    if (!g_link || link_start(g_link, g_config.ctrl_port, g_config.audio_port,
                              TOKEN) != 0)
        return 1;
    pthread_t t;
    pthread_create(&t, NULL, audio_thread, NULL);
    log_info("server", "ready on tcp/%d udp/%d", g_config.ctrl_port,
             g_config.audio_port);

    int sig = 0;
    sigwait(&sigs, &sig);
    log_info("server", "signal %d: stopping", sig);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    pthread_join(t, NULL);
    link_destroy(g_link);
    return 0;
}
