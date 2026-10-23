#include "audio.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "config.h"
#include "g711.h"
#include "jitter.h"
#include "log.h"
#include "resample.h"
#include "stats.h"
#include "util.h"

#define TAG "audio"

/* Used when SCO_OPTIONS cannot be read. This is the MTU reported by the
 * controllers found on Raspberry Pi boards. */
#define SCO_MTU_FALLBACK 48

/* Jitter buffer slots. 200 ms of 5 ms blocks is 40 slots; 64 leaves headroom. */
#define JB_SLOTS 64

/* Playback ring size in SCO samples: several SCO packets plus one block. */
#define PLAY_RING_SAMPS (SCO_FRAME_SAMPS * 8)

/* One queued block, already converted to SCO PCM. `filler` is set for blocks
 * the client marked as filler and for blocks inserted to cover a gap. */
typedef struct {
    int16_t  pcm[SCO_FRAME_SAMPS];   /* sized for a 20 ms block at the SCO rate */
    unsigned samples;                /* samples actually used */
    int      filler;
} jb_slot_t;

struct audio {
    link_t  *link;
    void   (*on_closed)(void *ud);
    void    *ud;

    /* Under ctl_lock for open/close; `sco_fd`, `running` and `finished` are
     * also read atomically by other threads. */
    int      sco_fd;
    int      sco_mtu;
    int      running;
    int      finished;
    int      thread_started;
    pthread_t thread;
    pthread_mutex_t ctl_lock;

    /* ----- guarded by jb_lock ----- */
    pthread_mutex_t jb_lock;
    /* Negotiated wire format and block duration (§6.2). The audio thread
     * works on its own copy (ul_fmt, ul_frame_ms), taken when `fmt_changed`
     * is set, so that a change from another thread never meets a block
     * half accumulated at the old size. */
    uint8_t  fmt;
    uint8_t  frame_ms;
    int      fmt_changed;
    jitter_t  jitter;               /* adaptive depth policy (§6.5) */
    jb_slot_t jb[JB_SLOTS];
    size_t    jb_head, jb_tail, jb_count;
    resample_down_t down;           /* client -> SCO conversion */

    /* ----- audio thread only ----- */
    uint8_t  ul_fmt;
    uint8_t  ul_frame_ms;
    int16_t  mic_acc[SCO_FRAME_SAMPS];
    size_t   mic_len;
    uint32_t dl_ts;
    resample_up_t up;
    int16_t  play[PLAY_RING_SAMPS];
    size_t   play_len;
    int      playing;
    int      fill_run;

    /* ----- atomic ----- */
    int      mute_ul;
    unsigned play_ms;               /* playback ring depth, for statistics */
};

/* Samples of SCO-rate audio in one block of `frame_ms`. */
static unsigned block_samples(unsigned frame_ms)
{
    unsigned ms = frame_ms ? frame_ms : PROTO_MAX_FRAME_MS;
    if (ms > PROTO_MAX_FRAME_MS)
        ms = PROTO_MAX_FRAME_MS;
    return SCO_RATE * ms / 1000;
}

/* Number of slots allowed by the configured ceiling at the current duration.
 * Caller holds jb_lock. */
static size_t jb_capacity(const audio_t *a)
{
    unsigned ms = a->frame_ms ? a->frame_ms : PROTO_MAX_FRAME_MS;
    size_t cap = (size_t)g_config.jitter_max_ms / ms;
    if (cap < 2)
        cap = 2;
    return cap > JB_SLOTS ? JB_SLOTS : cap;
}

/* Number of consecutive underrun blocks played as silence before playback
 * stops and the buffer refills: 60 ms worth (§6.5). Audio thread. */
static int fill_max(const audio_t *a)
{
    unsigned ms = a->ul_frame_ms ? a->ul_frame_ms : PROTO_MAX_FRAME_MS;
    int n = (int)(60 / ms);
    return n < 1 ? 1 : n;
}

/* -------------------------------------------------------------- lifecycle */

audio_t *audio_create(link_t *link, void (*on_closed)(void *ud), void *ud)
{
    audio_t *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->link = link;
    a->on_closed = on_closed;
    a->ud = ud;
    a->sco_fd = -1;
    a->fmt = a->ul_fmt = FMT_PCM16_8K;
    a->frame_ms = a->ul_frame_ms = PROTO_MAX_FRAME_MS;
    pthread_mutex_init(&a->ctl_lock, NULL);
    pthread_mutex_init(&a->jb_lock, NULL);
    return a;
}

void audio_destroy(audio_t *a)
{
    if (!a)
        return;
    audio_close(a);
    pthread_mutex_destroy(&a->jb_lock);
    pthread_mutex_destroy(&a->ctl_lock);
    free(a);
}

/* Any thread. The queue and the downlink converter are reset here, under
 * jb_lock; the uplink state belongs to the audio thread, which picks the
 * change up at its next packet (apply_format). */
void audio_set_format(audio_t *a, uint8_t fmt, unsigned frame_ms)
{
    if (!a || !fmt_block_bytes(fmt, frame_ms) || frame_ms > PROTO_MAX_FRAME_MS)
        return;
    pthread_mutex_lock(&a->jb_lock);
    if (a->fmt == fmt && a->frame_ms == frame_ms) {
        pthread_mutex_unlock(&a->jb_lock);
        return;
    }
    a->fmt = fmt;
    a->frame_ms = (uint8_t)frame_ms;
    a->fmt_changed = 1;
    /* Flush the queue; its contents were converted from the old format. */
    a->jb_head = a->jb_tail = a->jb_count = 0;
    /* Restart the adaptive policy with the new block duration. */
    jitter_reset(&a->jitter, frame_ms, (unsigned)g_config.jitter_prefill_ms,
                 (unsigned)g_config.jitter_max_ms);
    resample_down_reset(&a->down);
    pthread_mutex_unlock(&a->jb_lock);
    log_info(TAG, "wire format now 0x%02x, blocks of %u ms (%u Hz, %u B each)",
             fmt, frame_ms, fmt_rate(fmt), fmt_block_bytes(fmt, frame_ms));
}

/* Audio thread: adopt a format set by audio_set_format(). The partial uplink
 * block was accumulated at the old size and is dropped. */
static void apply_format(audio_t *a)
{
    pthread_mutex_lock(&a->jb_lock);
    int changed = a->fmt_changed;
    a->fmt_changed = 0;
    uint8_t fmt = a->fmt;
    uint8_t frame_ms = a->frame_ms;
    pthread_mutex_unlock(&a->jb_lock);
    if (!changed)
        return;
    a->ul_fmt = fmt;
    a->ul_frame_ms = frame_ms;
    a->mic_len = 0;
    resample_up_reset(&a->up);
}

void audio_set_mute_ul(audio_t *a, int mute)
{
    if (!a)
        return;
    __atomic_store_n(&a->mute_ul, mute ? 1 : 0, __ATOMIC_RELAXED);
    log_debug(TAG, "emulated microphone %s", mute ? "muted" : "live");
}

/* ----------------------------------------------------------- jitter buffer */

static void jb_reset(audio_t *a)
{
    pthread_mutex_lock(&a->jb_lock);
    a->jb_head = a->jb_tail = a->jb_count = 0;
    pthread_mutex_unlock(&a->jb_lock);
}

/* Append one slot of SCO-rate PCM, dropping the oldest at the ceiling.
 * Caller holds jb_lock. */
static void jb_push_pcm_locked(audio_t *a, const int16_t *pcm, unsigned samples,
                               int filler)
{
    if (a->jb_count >= jb_capacity(a)) {
        /* At the ceiling, drop the oldest block (§6.5). Counted in ul_late,
         * which is distinct from network loss (ul_lost, §10.1). */
        a->jb_head = (a->jb_head + 1) % JB_SLOTS;
        a->jb_count--;
        STAT_INC(ul_late);
    }
    jb_slot_t *s = &a->jb[a->jb_tail];
    if (samples > SCO_FRAME_SAMPS)
        samples = SCO_FRAME_SAMPS;
    if (pcm)
        memcpy(s->pcm, pcm, samples * sizeof(int16_t));
    else
        memset(s->pcm, 0, samples * sizeof(int16_t));
    s->samples = samples;
    s->filler = filler;
    a->jb_tail = (a->jb_tail + 1) % JB_SLOTS;
    a->jb_count++;
}

static int jb_pop(audio_t *a, jb_slot_t *out)
{
    int got = 0;
    pthread_mutex_lock(&a->jb_lock);
    if (a->jb_count > 0) {
        *out = a->jb[a->jb_head];
        a->jb_head = (a->jb_head + 1) % JB_SLOTS;
        a->jb_count--;
        got = 1;
    }
    pthread_mutex_unlock(&a->jb_lock);
    return got;
}

static size_t jb_level(audio_t *a)
{
    pthread_mutex_lock(&a->jb_lock);
    size_t n = a->jb_count;
    pthread_mutex_unlock(&a->jb_lock);
    return n;
}

/* ------------------------------------------------- client -> SCO conversion */

/* Convert one block from the wire format to SCO-rate PCM. Runs on the link
 * thread. */
static void convert_to_sco(audio_t *a, uint8_t fmt, const uint8_t *in,
                           unsigned samples, int16_t *out)
{
    switch (fmt) {
    case FMT_PCM16_8K:
        /* Same representation as the SCO link. */
        memcpy(out, in, samples * sizeof(int16_t));
        break;

    case FMT_PCM16_16K: {
        /* Only reached if the client requested 16 kHz on an 8 kHz link. */
        int16_t wide[2 * SCO_FRAME_SAMPS];
        memcpy(wide, in, 2 * samples * sizeof(int16_t));
        resample_down(&a->down, wide, 2 * samples, out);
        break;
    }

    case FMT_G711A:
        g711_alaw_decode_block(in, samples, out);
        break;

    case FMT_G711U:
        g711_ulaw_decode_block(in, samples, out);
        break;

    default:
        memset(out, 0, samples * sizeof(int16_t));
        break;
    }
}

void audio_push_block(audio_t *a, const hera_audio_t *block)
{
    if (!a || !block)
        return;

    uint64_t now = now_ms();

    unsigned block_bytes = fmt_block_bytes(block->fmt, block->frame_ms);
    if (!block_bytes || block->frame_ms > PROTO_MAX_FRAME_MS)
        return;
    unsigned samples = block_samples(block->frame_ms);

    /* Under jb_lock: the converter state (`down`) is reset by format changes
     * from other threads. */
    pthread_mutex_lock(&a->jb_lock);
    /* A block that does not match the current format was sent before a
     * change and is dropped. */
    if (block->fmt != a->fmt || block->frame_ms != a->frame_ms) {
        pthread_mutex_unlock(&a->jb_lock);
        return;
    }
    jitter_on_arrival(&a->jitter, now);
    for (unsigned i = 0; i < block->count; i++) {
        int16_t pcm[SCO_FRAME_SAMPS];
        convert_to_sco(a, block->fmt, block->samples + i * block_bytes,
                       samples, pcm);
        jb_push_pcm_locked(a, pcm, samples, block->filler);
    }
    pthread_mutex_unlock(&a->jb_lock);
}

void audio_push_gap(audio_t *a, unsigned blocks)
{
    if (!a)
        return;
    pthread_mutex_lock(&a->jb_lock);
    unsigned samples = block_samples(a->frame_ms);
    for (unsigned i = 0; i < blocks && i < JB_SLOTS; i++)
        jb_push_pcm_locked(a, NULL, samples, 1);
    pthread_mutex_unlock(&a->jb_lock);
}

unsigned audio_buffered_ms(audio_t *a)
{
    if (!a)
        return 0;
    pthread_mutex_lock(&a->jb_lock);
    unsigned ms = (unsigned)a->jb_count *
                  (a->frame_ms ? a->frame_ms : PROTO_MAX_FRAME_MS);
    pthread_mutex_unlock(&a->jb_lock);
    return ms + __atomic_load_n(&a->play_ms, __ATOMIC_RELAXED);
}

unsigned audio_target_ms(audio_t *a)
{
    if (!a)
        return 0;
    pthread_mutex_lock(&a->jb_lock);
    unsigned target = jitter_target_ms(&a->jitter);
    pthread_mutex_unlock(&a->jb_lock);
    return target;
}

unsigned audio_jitter_ms(audio_t *a)
{
    if (!a)
        return 0;
    /* Report the same estimate the adaptive policy uses. */
    pthread_mutex_lock(&a->jb_lock);
    unsigned ms = a->jitter.jitter_ms;
    pthread_mutex_unlock(&a->jb_lock);
    return ms;
}

/* ------------------------------------------------------------- downlink    */

/* Move one block into the playback ring. Applies the adaptive policy and
 * plays silence on underrun (§6.5).
 *
 * This is the only place that knows both the buffer depth and whether the next
 * block is filler, which is required to shrink the buffer without an audible
 * discontinuity (§6.3). */
static void refill_play(audio_t *a)
{
    unsigned block = block_samples(a->ul_frame_ms);
    if (a->play_len + block > PLAY_RING_SAMPS)
        return;

    uint64_t now = now_ms();
    unsigned depth = audio_buffered_ms(a);

    if (!a->playing) {
        /* Refill up to the current adaptive target. */
        pthread_mutex_lock(&a->jb_lock);
        unsigned target = jitter_target_ms(&a->jitter);
        pthread_mutex_unlock(&a->jb_lock);
        if (depth < target) {
            memset(a->play + a->play_len, 0, block * sizeof(int16_t));
            a->play_len += block;
            return;
        }
        a->playing = 1;
        a->fill_run = 0;
    }

    pthread_mutex_lock(&a->jb_lock);
    jitter_action_t action = jitter_decide(&a->jitter, depth, now);
    /* Shrink only by discarding a filler block. If the head of the queue is
     * speech, the buffer stays deeper until a filler block comes up. */
    if (action == JB_SHRINK && a->jb_count > 0 && a->jb[a->jb_head].filler) {
        a->jb_head = (a->jb_head + 1) % JB_SLOTS;
        a->jb_count--;
        /* Not counted in ul_late, which only counts received audio that was
         * lost (§10.1). */
    }
    pthread_mutex_unlock(&a->jb_lock);

    /* Grow by inserting one block of silence, but only when the queue is
     * empty, so that queued speech is not interrupted. */
    if (action == JB_GROW && jb_level(a) == 0) {
        memset(a->play + a->play_len, 0, block * sizeof(int16_t));
        a->play_len += block;
        return;
    }

    jb_slot_t slot;
    if (!jb_pop(a, &slot)) {
        if (a->fill_run >= fill_max(a)) {
            /* Underrun lasted fill_max() blocks: stop and refill, rather than
             * alternate single blocks of audio and silence. */
            a->playing = 0;
            a->fill_run = 0;
            /* Raise the target immediately (§6.5). */
            pthread_mutex_lock(&a->jb_lock);
            jitter_on_underrun(&a->jitter, now);
            pthread_mutex_unlock(&a->jb_lock);
        }
        a->fill_run++;
        STAT_INC(ul_fill);
        memset(a->play + a->play_len, 0, block * sizeof(int16_t));
        a->play_len += block;
        return;
    }

    a->fill_run = 0;
    unsigned n = slot.samples ? slot.samples : block;
    if (n > block)
        n = block;
    memcpy(a->play + a->play_len, slot.pcm, n * sizeof(int16_t));
    if (n < block)
        memset(a->play + a->play_len + n, 0, (block - n) * sizeof(int16_t));
    a->play_len += block;
}

/* Hand `n` samples to the SCO socket, refilling the ring as needed. */
static void take_play(audio_t *a, int16_t *out, size_t n)
{
    while (a->play_len < n) {
        size_t before = a->play_len;
        refill_play(a);
        if (a->play_len == before) {
            /* Ring full; only possible if the SCO MTU exceeds the ring.
             * Pad with silence. */
            memset(a->play + a->play_len, 0,
                   (n - a->play_len) * sizeof(int16_t));
            a->play_len = n;
            break;
        }
    }

    if (__atomic_load_n(&a->mute_ul, __ATOMIC_RELAXED))
        memset(out, 0, n * sizeof(int16_t));
    else
        memcpy(out, a->play, n * sizeof(int16_t));

    a->play_len -= n;
    if (a->play_len)
        memmove(a->play, a->play + n, a->play_len * sizeof(int16_t));
    __atomic_store_n(&a->play_ms, (unsigned)(a->play_len / (SCO_RATE / 1000)),
                     __ATOMIC_RELAXED);
}

/* --------------------------------------------------------------- uplink    */

/* Convert one accumulated block from the phone and send it (§6.3). */
static void convert_and_send(audio_t *a)
{
    uint8_t fmt = a->ul_fmt;
    unsigned n = block_samples(a->ul_frame_ms);
    uint8_t wire[2 * SCO_FRAME_BYTES];

    switch (fmt) {
    case FMT_PCM16_8K:
        memcpy(wire, a->mic_acc, n * sizeof(int16_t));
        break;
    case FMT_PCM16_16K: {
        int16_t wide[2 * SCO_FRAME_SAMPS];
        resample_up(&a->up, a->mic_acc, n, wide);
        memcpy(wire, wide, 2 * n * sizeof(int16_t));
        break;
    }
    case FMT_G711A:
        g711_alaw_encode_block(a->mic_acc, n, wire);
        break;
    case FMT_G711U:
        g711_ulaw_encode_block(a->mic_acc, n, wire);
        break;
    default:
        return;
    }

    link_send_audio(a->link, wire, a->dl_ts);
    a->dl_ts += fmt_block_samples(fmt, a->ul_frame_ms);
}

/* Accumulate SCO samples into blocks of the negotiated duration and send each
 * complete block.
 *
 * Block and SCO packet sizes need not divide each other (e.g. 5 ms blocks and
 * 3.75 ms packets); the remainder carries over to the next packet. This adds
 * at most one SCO packet interval of latency (§7.1). */
static void feed_mic(audio_t *a, const int16_t *pcm, size_t n)
{
    size_t off = 0;
    size_t target = block_samples(a->ul_frame_ms);
    /* mic_len < target always holds: apply_format() empties the accumulator
     * whenever the block size changes. */
    while (off < n) {
        size_t take = target - a->mic_len;
        if (take > n - off)
            take = n - off;
        memcpy(a->mic_acc + a->mic_len, pcm + off, take * sizeof(int16_t));
        a->mic_len += take;
        off += take;
        if (a->mic_len == target) {
            a->mic_len = 0;
            convert_and_send(a);
        }
    }
}

/* ---------------------------------------------------------- the SCO thread */

/* Each received SCO packet is answered with a packet of the same size, which
 * keeps both directions locked to the controller clock (§12.1). */
static void *audio_thread(void *arg)
{
    audio_t *a = arg;
    if (thread_set_realtime(g_config.audio_rt_prio) != 0)
        log_warn(TAG, "cannot raise the audio thread to SCHED_FIFO "
                 "(run as root for stable audio)");

    int16_t in[512];
    int16_t out[512];

    while (__atomic_load_n(&a->running, __ATOMIC_ACQUIRE)) {
        apply_format(a);
        ssize_t r = recv(a->sco_fd, in, sizeof in, 0);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            log_info(TAG, "SCO receive ended: %s", strerror(errno));
            break;
        }
        if (r == 0)
            break;

        size_t samples = (size_t)r / 2;
        if (samples == 0 || samples > sizeof out / sizeof out[0])
            continue;

        /* Send to the phone before processing the received samples. */
        take_play(a, out, samples);
        if (send(a->sco_fd, out, samples * 2, 0) < 0 &&
            errno != EAGAIN && errno != EINTR) {
            log_info(TAG, "SCO send ended: %s", strerror(errno));
            break;
        }

        feed_mic(a, in, samples);
    }

    /* Notify the relay. The socket is closed by the next audio_open() or by
     * audio_close(), never from this thread. */
    __atomic_store_n(&a->finished, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&a->running, __ATOMIC_ACQUIRE) && a->on_closed)
        a->on_closed(a->ud);
    return NULL;
}

/* -------------------------------------------------------------- open/close */

/* Stop the audio thread and close the socket. Caller holds ctl_lock. */
static void close_locked(audio_t *a)
{
    if (a->sco_fd < 0)
        return;
    __atomic_store_n(&a->running, 0, __ATOMIC_RELEASE);
    if (a->thread_started) {
        shutdown(a->sco_fd, SHUT_RDWR);
        pthread_join(a->thread, NULL);
        a->thread_started = 0;
    }
    close(a->sco_fd);
    __atomic_store_n(&a->sco_fd, -1, __ATOMIC_RELEASE);
    a->play_len = 0;
    __atomic_store_n(&a->play_ms, 0, __ATOMIC_RELAXED);
    log_info(TAG, "audio bridge closed");
}

int audio_open(audio_t *a, int sco_fd)
{
    if (!a)
        return -1;

    /* Serialised: both the SCO accept thread and the link thread (CMD
     * AUDIO_DISC) get here. */
    pthread_mutex_lock(&a->ctl_lock);
    close_locked(a);
    a->sco_mtu = SCO_MTU_FALLBACK;
    {
        struct sco_options opts;
        socklen_t olen = sizeof opts;
        if (getsockopt(sco_fd, SOL_SCO, SCO_OPTIONS, &opts, &olen) == 0 &&
            opts.mtu >= 16 && opts.mtu <= 512)
            a->sco_mtu = opts.mtu;
    }

    /* All per-stream state restarts at AUDIO_OPEN (§6.3). The audio thread
     * is not running here (close_locked joined it), so its state can be
     * reset directly; the shared part under jb_lock. */
    resample_up_reset(&a->up);
    pthread_mutex_lock(&a->jb_lock);
    resample_down_reset(&a->down);
    a->jb_head = a->jb_tail = a->jb_count = 0;
    jitter_reset(&a->jitter, a->frame_ms ? a->frame_ms : PROTO_MAX_FRAME_MS,
                 (unsigned)g_config.jitter_prefill_ms,
                 (unsigned)g_config.jitter_max_ms);
    a->ul_fmt = a->fmt;
    a->ul_frame_ms = a->frame_ms;
    a->fmt_changed = 0;
    pthread_mutex_unlock(&a->jb_lock);
    a->mic_len = 0;
    a->dl_ts = 0;
    a->play_len = 0;
    __atomic_store_n(&a->play_ms, 0, __ATOMIC_RELAXED);
    a->playing = 0;
    a->fill_run = 0;
    __atomic_store_n(&a->finished, 0, __ATOMIC_RELEASE);

    /* Receive timeout, so the thread notices `running` being cleared. */
    struct timeval tv = { 1, 0 };
    setsockopt(sco_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* Optional priming with silent packets (HERA_SCO_PRIME_PKTS). Each one
     * adds one SCO packet interval of permanent latency (§7.1). */
    if (g_config.sco_prime_pkts > 0) {
        uint8_t silence[512];
        memset(silence, 0, sizeof silence);
        for (int i = 0; i < g_config.sco_prime_pkts; i++) {
            if (send(sco_fd, silence, (size_t)a->sco_mtu, 0) < 0)
                break;
        }
    }

    __atomic_store_n(&a->sco_fd, sco_fd, __ATOMIC_RELEASE);
    __atomic_store_n(&a->running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&a->thread, NULL, audio_thread, a) != 0) {
        log_error(TAG, "cannot start the audio thread");
        __atomic_store_n(&a->running, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&a->sco_fd, -1, __ATOMIC_RELEASE);
        close(sco_fd);
        pthread_mutex_unlock(&a->ctl_lock);
        return -1;
    }
    a->thread_started = 1;
    int mtu = a->sco_mtu;
    uint8_t fmt = a->ul_fmt;
    pthread_mutex_unlock(&a->ctl_lock);

    log_info(TAG, "audio bridge open: SCO %d Hz CVSD (MTU %d) <-> format 0x%02x, "
             "no codec", SCO_RATE, mtu, fmt);
    return 0;
}

void audio_close(audio_t *a)
{
    if (!a)
        return;
    pthread_mutex_lock(&a->ctl_lock);
    close_locked(a);
    pthread_mutex_unlock(&a->ctl_lock);
    jb_reset(a);
}

int audio_is_open(audio_t *a)
{
    return a && __atomic_load_n(&a->sco_fd, __ATOMIC_ACQUIRE) >= 0 &&
           !__atomic_load_n(&a->finished, __ATOMIC_ACQUIRE);
}
