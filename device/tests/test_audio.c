/* audio.c end to end, without Bluetooth.
 *
 * The SCO link is a SOCK_SEQPACKET socketpair: a fake phone sends 24-sample
 * packets every 3 ms, as the Pi's controller does, and records what the
 * device sends back. The client side is link_send_audio(), replaced here, and
 * audio_push_block(). Each direction carries a sine; a clean stream follows
 * x[n] = 2 cos(w) x[n-1] - x[n-2], and every glitch (gap, repeated or
 * misaligned block, wrong format) shows up as a large residual.
 *
 *   make test-audio
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "config.h"
#include "g711.h"
#include "log.h"
#include "proto.h"

#define RATE        8000
#define SCO_SAMPLES 24          /* 3 ms packets, as on the Pi */
#define RUN_MS      3000
#define MAX_SAMPLES (RATE * 4)

static int failures;

static void check(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* ------------------------------------------------------- captured audio --- */

typedef struct {
    pthread_mutex_t lock;
    int16_t samples[MAX_SAMPLES];
    size_t  len;
} capture_t;

static capture_t g_up;      /* phone -> client, via link_send_audio() */
static capture_t g_down;    /* client -> phone, read from the SCO socket */
static uint8_t g_fmt;
static unsigned g_frame_ms;
static uint32_t g_last_ts;
static int g_ts_errors;
static int g_ts_seen;

static void capture_add(capture_t *c, const int16_t *s, size_t n)
{
    pthread_mutex_lock(&c->lock);
    for (size_t i = 0; i < n && c->len < MAX_SAMPLES; i++)
        c->samples[c->len++] = s[i];
    pthread_mutex_unlock(&c->lock);
}

/* Replaces link.c: what the device would send to the client. */
void link_send_audio(link_t *l, const uint8_t *samples, uint32_t ts)
{
    (void)l;
    unsigned n = fmt_block_samples(g_fmt, g_frame_ms);
    int16_t pcm[320];
    if (g_fmt == FMT_G711A)
        g711_alaw_decode_block(samples, n, pcm);
    else
        memcpy(pcm, samples, n * 2);
    if (g_ts_seen && ts != g_last_ts + n)
        g_ts_errors++;
    g_ts_seen = 1;
    g_last_ts = ts;
    capture_add(&g_up, pcm, n);
}

/* --------------------------------------------------------------- timing --- */

static void sleep_until(struct timespec *t, long step_ns)
{
    t->tv_nsec += step_ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL);
}

static int16_t sine(double freq, long n)
{
    return (int16_t)lrint(8000.0 * sin(2.0 * M_PI * freq * (double)n / RATE));
}

/* ------------------------------------------------------------ fake phone --- */

static int g_phone_fd;
static volatile int g_running;

static void *phone(void *arg)
{
    (void)arg;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long n = 0;
    while (g_running) {
        int16_t out[SCO_SAMPLES];
        for (int i = 0; i < SCO_SAMPLES; i++)
            out[i] = sine(440.0, n++);
        if (send(g_phone_fd, out, sizeof out, 0) < 0)
            break;
        int16_t in[256];
        ssize_t r = recv(g_phone_fd, in, sizeof in, 0);
        if (r > 0)
            capture_add(&g_down, in, (size_t)r / 2);
        sleep_until(&t, 3000000L);
    }
    return NULL;
}

/* ----------------------------------------------------------- fake client --- */

static audio_t *g_audio;

static void *client(void *arg)
{
    (void)arg;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long n = 0;
    uint32_t ts = 0;
    while (g_running) {
        unsigned samples = RATE * g_frame_ms / 1000;
        int16_t pcm[160];
        uint8_t wire[320];
        for (unsigned i = 0; i < samples; i++)
            pcm[i] = sine(300.0, n++);
        if (g_fmt == FMT_G711A)
            g711_alaw_encode_block(pcm, samples, wire);
        else
            memcpy(wire, pcm, samples * 2);
        hera_audio_t block = {
            .fmt = g_fmt, .frame_ms = (uint8_t)g_frame_ms, .count = 1,
            .ts = ts, .samples = wire,
            .bytes = fmt_block_bytes(g_fmt, g_frame_ms),
        };
        audio_push_block(g_audio, &block);
        ts += samples;
        sleep_until(&t, (long)g_frame_ms * 1000000L);
    }
    return NULL;
}

/* -------------------------------------------------------------- analysis --- */

/* Samples, after the first `skip` non-silent ones, that break the sine
 * recurrence by more than `tolerance`. */
static int glitches(capture_t *c, double freq, int tolerance, size_t *checked)
{
    double k = 2.0 * cos(2.0 * M_PI * freq / RATE);
    size_t start = 0;
    while (start < c->len && abs(c->samples[start]) < 100)
        start++;
    start += RATE / 10;    /* let the buffer settle: 100 ms */
    int bad = 0;
    *checked = 0;
    for (size_t i = start + 2; i < c->len; i++) {
        double predicted = k * c->samples[i - 1] - c->samples[i - 2];
        if (fabs(predicted - c->samples[i]) > tolerance)
            bad++;
        (*checked)++;
    }
    return bad;
}

static void run(uint8_t fmt, unsigned frame_ms, int change_midway,
                const char *name)
{
    memset(&g_up.samples, 0, sizeof g_up.samples);
    memset(&g_down.samples, 0, sizeof g_down.samples);
    g_up.len = g_down.len = 0;
    g_ts_errors = g_ts_seen = 0;
    g_fmt = fmt;
    g_frame_ms = frame_ms;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        check(0, "socketpair");
        return;
    }
    g_phone_fd = sv[1];
    g_audio = audio_create((link_t *)0x1, NULL, NULL);
    audio_set_format(g_audio, fmt, frame_ms);
    check(audio_open(g_audio, sv[0]) == 0, "audio open");

    g_running = 1;
    pthread_t tp, tc;
    pthread_create(&tp, NULL, phone, NULL);
    pthread_create(&tc, NULL, client, NULL);
    if (change_midway) {
        usleep(RUN_MS * 1000 / 2);
        /* Renegotiation, as when the client reconnects during a call. Both
         * sides switch: the client thread reads g_frame_ms. */
        g_frame_ms = change_midway;
        audio_set_format(g_audio, fmt, (unsigned)change_midway);
        usleep(RUN_MS * 1000 / 2);
    } else {
        usleep(RUN_MS * 1000);
    }
    g_running = 0;
    pthread_join(tc, NULL);
    pthread_join(tp, NULL);
    audio_destroy(g_audio);
    close(sv[1]);

    /* G.711 quantisation leaves a larger residual than PCM. */
    int tol = fmt == FMT_G711A ? 400 : 4;
    size_t n_up, n_down;
    int bad_up = glitches(&g_up, 440.0, tol, &n_up);
    int bad_down = glitches(&g_down, 300.0, tol, &n_down);
    char line[128];
    printf("  %s: uplink %zu samples, %d glitches; downlink %zu samples, "
           "%d glitches; ts errors %d\n", name, n_up, bad_up, n_down,
           bad_down, g_ts_errors);
    /* A format change resets the stream once in each direction. */
    int allowed = change_midway ? 40 : 0;
    snprintf(line, sizeof line, "%s: phone -> client clean", name);
    check(n_up > RATE && bad_up <= allowed, line);
    snprintf(line, sizeof line, "%s: client -> phone clean", name);
    check(n_down > RATE && bad_down <= allowed, line);
    snprintf(line, sizeof line, "%s: uplink ts contiguous", name);
    check(change_midway || g_ts_errors == 0, line);
}

int main(void)
{
    config_load();
    log_set_level(1);
    pthread_mutex_init(&g_up.lock, NULL);
    pthread_mutex_init(&g_down.lock, NULL);

    run(FMT_PCM16_8K, 20, 0, "PCM16, 20 ms");
    run(FMT_PCM16_8K, 5, 0, "PCM16, 5 ms");
    run(FMT_G711A, 20, 0, "A-law, 20 ms");
    run(FMT_G711A, 5, 0, "A-law, 5 ms");
    run(FMT_PCM16_8K, 20, 5, "PCM16, 20 -> 5 ms during the call");

    printf("\n%s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
