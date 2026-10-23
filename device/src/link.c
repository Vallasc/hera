#include "link.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "sec.h"
#include "stats.h"
#include "util.h"

#define TAG "link"

/* LOG lines waiting to be sent; the oldest are dropped when full. Lines are
 * limited to the LOG payload size (§5.2). */
#define LOG_QUEUE 32
#define LOG_LINE  257

/* One control connection.
 *
 * A link has two slots: the current connection, and a pending one for a
 * second client that connects while the first is still open. A pending
 * connection becomes current, or is refused, only after its HELLO has been
 * verified (§5.7). `rx` is only accessed by the link thread. */
typedef struct {
    int           fd;
    int           ready;        /* HELLO verified and keys derived */
    int64_t       opened_ms;
    sec_keypair_t kp;           /* ephemeral, per connection (§11.3) */
    sec_session_t sec;

    /* Reassembly buffer (§4.1.1), sized for records (§11.2). */
    uint8_t       rx[2 * SEC_MAX_RECORD];
    size_t        rx_len;
} conn_t;

struct link {
    link_cb_t cb;
    char     *ident;

    uint8_t   token[HERA_TOKEN_LEN];

    int       listen_fd;
    int       audio;
    int       wake;             /* eventfd, to kick the poll loop */

    pthread_t thread;
    int       running;

    /* Serialises control writes, so that records reach the socket in counter
     * order whichever thread sends them. Taken before `lock`, never while
     * holding it. */
    pthread_mutex_t tx_lock;

    /* ----- guarded by `lock` ----- */
    pthread_mutex_t lock;
    conn_t    conn[2];
    int       cur;              /* index of the current connection */

    /* Peer address of the audio flow, learned from the first valid datagram
     * (§4.2.1). No AUDIO_RX is sent while `audio_bound` is 0. */
    struct sockaddr_in peer;
    int       audio_bound;

    /* Session (§5). `up` means a session is established on conn[cur]. */
    uint32_t  session_id;
    int       up;
    int       suspended;        /* inside the resume window */
    int64_t   suspended_at_ms;
    int64_t   last_rx_ms;       /* for the peer timeout (§5.5) */
    uint8_t   fmt;
    unsigned  frame_ms;         /* negotiated block duration (§6.2) */
    uint8_t   offered_dur;      /* the client's duration bitmap */
    uint8_t   epoch;
    unsigned  sco_rate;
    uint8_t   offered;          /* the client's format bitmap */

    /* Uplink `ts` tracking for ul_lost (§6.4), reset with every session. */
    uint32_t  ul_expected;
    int       ul_epoch;         /* -1 until the first block */

    /* ----- guarded by `log_lock` -----
     * LOG lines for the client (§5.2). Queued and sent by the link thread:
     * sending from log_msg() directly would re-enter the link from code that
     * logs while holding `lock` or `tx_lock`, and deadlock. */
    pthread_mutex_t log_lock;
    char      logq[LOG_QUEUE][LOG_LINE];
    unsigned  log_head, log_count;

    /* ----- link thread only ----- */
    uint8_t   plain[SEC_MAX_PLAIN];
};

#define CUR(l)  (&(l)->conn[(l)->cur])
#define PEND(l) (&(l)->conn[1 - (l)->cur])

/* --------------------------------------------------------------- helpers --- */

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Wake the link thread. Under log_lock, which link_stop() also takes to
 * close the eventfd: the descriptor cannot be closed, and reused, between
 * reading it and writing to it. The write does not block. */
static void kick(link_t *l)
{
    uint64_t one = 1;
    pthread_mutex_lock(&l->log_lock);
    if (l->wake >= 0 && write(l->wake, &one, sizeof one) < 0) {
        /* Counter saturated; the loop is already due to wake up. */
    }
    pthread_mutex_unlock(&l->log_lock);
}

static uint32_t new_session_id(void)
{
    /* 0 is reserved for "no session" (§4.2). */
    uint32_t id = 0;
    while (id == 0)
        id = (uint32_t)rand_u32();
    return id;
}

/* Forget the session state. Caller holds `lock`. */
static void discard_session_locked(link_t *l)
{
    l->suspended = 0;
    l->session_id = 0;
    l->fmt = 0;
    l->audio_bound = 0;
}

/* Write `len` bytes, waiting briefly for socket space. A partial write would
 * desynchronise the stream; this only stalls if the peer has stopped
 * reading, in which case the peer timeout ends the session. */
static void write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, 200) > 0)
                continue;
        }
        log_debug(TAG, "control write failed: %s", strerror(errno));
        return;
    }
}

/* Write one frame on connection `c`, or on the current connection if `c` is
 * NULL. With `seal` the frame is sent in a record, and dropped if the
 * connection has no keys yet; without it, it is sent in the clear, which is
 * only allowed for HELLO and for an ERROR sent before the keys exist (§5.3).
 * Nothing is queued (§6.6). */
static void write_frame(link_t *l, conn_t *c, const uint8_t *frame, size_t len,
                        int seal)
{
    uint8_t record[SEC_MAX_RECORD];

    pthread_mutex_lock(&l->tx_lock);
    pthread_mutex_lock(&l->lock);
    if (!c)
        c = CUR(l);
    int fd = c->fd;
    if (seal) {
        if (!c->sec.ready) {
            fd = -1;
        } else {
            size_t reclen = sec_seal_record(&c->sec.ctrl_tx, frame, len,
                                            record, sizeof record);
            if (reclen == 0) {
                log_warn(TAG, "cannot seal a %zu byte frame", len);
                fd = -1;
            }
            frame = record;
            len = reclen;
        }
    }
    pthread_mutex_unlock(&l->lock);

    if (fd >= 0)
        write_all(fd, frame, len);
    pthread_mutex_unlock(&l->tx_lock);
}

/* Sealed frame on the current connection. */
static void send_ctrl(link_t *l, const uint8_t *frame, size_t len)
{
    write_frame(l, NULL, frame, len, 1);
}

/* Prepare slot `c` for a newly accepted socket. Caller holds `lock`. */
static void conn_open_locked(conn_t *c, int fd)
{
    c->fd = fd;
    c->ready = 0;
    c->opened_ms = now_ms();
    c->rx_len = 0;
    sec_clear(&c->sec);
    /* New ephemeral key pair for every connection, including resumes
     * (§11.3). */
    if (sec_keypair(&c->kp) != 0)
        log_error(TAG, "cannot generate an ephemeral keypair");
}

/* Close a connection's socket and wipe its keys. Session state is not
 * touched. */
static void conn_close(link_t *l, conn_t *c)
{
    /* tx_lock first, so that no other thread is writing to this descriptor
     * when it is closed and possibly reused by the next accept(). */
    pthread_mutex_lock(&l->tx_lock);
    pthread_mutex_lock(&l->lock);
    int fd = c->fd;
    c->fd = -1;
    c->ready = 0;
    c->rx_len = 0;
    sec_clear(&c->sec);
    sec_keypair_clear(&c->kp);
    pthread_mutex_unlock(&l->lock);
    if (fd >= 0)
        close(fd);
    pthread_mutex_unlock(&l->tx_lock);
}

/* The current connection is closed. If a session was established on it, the
 * session becomes resumable (§5.4) or is discarded. */
static void cur_closed(link_t *l, int resumable)
{
    pthread_mutex_lock(&l->lock);
    conn_t *c = CUR(l);
    int was_up = l->up;
    l->up = 0;
    if (was_up) {
        if (resumable) {
            l->suspended = 1;
            l->suspended_at_ms = now_ms();
        } else {
            discard_session_locked(l);
        }
    }
    pthread_mutex_unlock(&l->lock);

    conn_close(l, c);

    if (!was_up)
        return;
    if (resumable) {
        log_info(TAG, "control channel dropped, session resumable for %d s",
                 g_config.resume_window_s);
        if (l->cb.on_suspend)
            l->cb.on_suspend(l->cb.ud);
    } else if (l->cb.on_down) {
        l->cb.on_down(l->cb.ud, 0);
    }
}

/* Close connection `c` because of a fatal error (§5.1). */
static void conn_fail(link_t *l, conn_t *c)
{
    pthread_mutex_lock(&l->lock);
    int is_cur = c == CUR(l);
    pthread_mutex_unlock(&l->lock);
    if (is_cur)
        cur_closed(l, 0);
    else
        conn_close(l, c);
}

void link_send_event(link_t *l, uint8_t id, uint8_t a1, uint16_t a2,
                     const char *str)
{
    uint8_t frame[HERA_MAX_FRAME];
    size_t len = proto_build_event(frame, id, a1, a2, str);
    send_ctrl(l, frame, len);
}

void link_send_ack(link_t *l, uint8_t id, uint8_t a1)
{
    uint8_t frame[HERA_MAX_FRAME];
    size_t len = proto_build_ack(frame, id, a1);
    send_ctrl(l, frame, len);
}

void link_send_stats(link_t *l, const uint8_t stats[HERA_STATS_LEN])
{
    uint8_t frame[HERA_MAX_FRAME];
    size_t len = proto_build_stats(frame, stats);
    send_ctrl(l, frame, len);
}

/* Safe from any thread, including with link locks held: the line is only
 * queued here. */
void link_send_log(link_t *l, const char *text)
{
    pthread_mutex_lock(&l->log_lock);
    unsigned slot = (l->log_head + l->log_count) % LOG_QUEUE;
    if (l->log_count == LOG_QUEUE) {
        l->log_head = (l->log_head + 1) % LOG_QUEUE;   /* drop the oldest */
        slot = (l->log_head + l->log_count - 1) % LOG_QUEUE;
    } else {
        l->log_count++;
    }
    snprintf(l->logq[slot], sizeof l->logq[slot], "%s", text ? text : "");
    pthread_mutex_unlock(&l->log_lock);
    kick(l);
}

/* Send the queued LOG lines. Link thread, no locks held. Only the lines
 * present on entry: sending can log in turn (e.g. a failed write), and those
 * lines wait for the next round instead of keeping this loop going. */
static void flush_logs(link_t *l)
{
    pthread_mutex_lock(&l->log_lock);
    unsigned pending = l->log_count;
    pthread_mutex_unlock(&l->log_lock);

    while (pending--) {
        char line[LOG_LINE];
        pthread_mutex_lock(&l->log_lock);
        if (!l->log_count) {
            pthread_mutex_unlock(&l->log_lock);
            return;
        }
        memcpy(line, l->logq[l->log_head], sizeof line);
        l->log_head = (l->log_head + 1) % LOG_QUEUE;
        l->log_count--;
        pthread_mutex_unlock(&l->log_lock);

        uint8_t frame[HERA_MAX_FRAME];
        size_t len = proto_build_log(frame, line);
        send_ctrl(l, frame, len);
    }
}

void link_say_bye(link_t *l, uint8_t reason, const char *text)
{
    uint8_t frame[HERA_MAX_FRAME];
    size_t len = proto_build_bye(frame, reason, text);
    send_ctrl(l, frame, len);
}

void link_send_audio(link_t *l, const uint8_t *samples, uint32_t ts)
{
    uint8_t frame[HERA_MAX_FRAME];
    uint8_t datagram[SEC_DATAGRAM_OVERHEAD + HERA_MAX_FRAME];

    /* One critical section: format, epoch, keys and session id must belong
     * to the same session, which may change at any time (§5.4), and the
     * socket must not be closed by link_stop() between reading it and
     * sending. sendto() does not block (MSG_DONTWAIT). */
    pthread_mutex_lock(&l->lock);
    /* Until the flow is bound there is nowhere to send (§4.2.1). */
    if (!l->up || !l->fmt || l->audio < 0 || !l->audio_bound ||
        !CUR(l)->sec.ready) {
        pthread_mutex_unlock(&l->lock);
        return;
    }
    size_t len = proto_build_audio(frame, l->fmt, l->frame_ms, 1, l->epoch, 0,
                                   ts, samples);
    size_t total = len ? sec_seal_datagram(&CUR(l)->sec.audio_tx,
                                           l->session_id, frame, len,
                                           datagram, sizeof datagram)
                       : 0;
    ssize_t n = total ? sendto(l->audio, datagram, total, MSG_DONTWAIT,
                               (struct sockaddr *)&l->peer, sizeof l->peer)
                      : -1;
    pthread_mutex_unlock(&l->lock);

    /* Audio is never queued (§6.6). */
    if (n < 0)
        STAT_INC(dl_drop);
    else
        STAT_INC(dl_blocks);
}

/* --------------------------------------------------------------- incoming --- */

/* Send our HELLO in the clear and derive the keys of `c` (§5.3, §11.3).
 * Returns 0 on success. */
static int handshake(link_t *l, conn_t *c, const hera_hello_t *hello,
                     uint32_t sid)
{
    uint8_t tag[SEC_TAG_LEN];
    sec_hello_tag(l->token, HERA_TOKEN_LEN, 1, FMT_ALL, DUR_ALL, 0, sid,
                  c->kp.pub, tag);

    uint8_t frame[HERA_MAX_FRAME];
    size_t flen = proto_build_hello(frame, FMT_ALL, DUR_ALL, 0, sid,
                                    c->kp.pub, tag, l->ident);
    write_frame(l, c, frame, flen, 0);

    /* Derivation fails only if the peer key is a low-order point. */
    pthread_mutex_lock(&l->lock);
    int ok = sec_derive(&c->sec, &c->kp, hello->pubkey, 1) == 0;
    if (ok)
        c->ready = 1;
    pthread_mutex_unlock(&l->lock);
    return ok ? 0 : -1;
}

/* A verified client that is neither resuming nor taking over the established
 * session: complete the handshake so that the refusal is sealed, then send
 * BYE reason 2 and close (§5.7). */
static void refuse_busy(link_t *l, conn_t *c, const hera_hello_t *hello)
{
    log_info(TAG, "second client refused: another session is in progress");
    if (handshake(l, c, hello, 0) == 0) {
        uint8_t frame[HERA_MAX_FRAME];
        size_t n = proto_build_bye(frame, BYE_BUSY,
                                   "another session is in progress");
        write_frame(l, c, frame, n, 1);
    }
    conn_close(l, c);
}

/* Handle a HELLO on connection `c`: verify it, decide between a new session,
 * a resume, a takeover and a refusal, then reply with our HELLO and READY
 * (§5.3, §5.4, §5.7). */
static void on_hello(link_t *l, conn_t *c, const uint8_t *payload, size_t len)
{
    hera_hello_t hello;
    uint8_t frame[HERA_MAX_FRAME];

    if (proto_parse_hello(payload, len, &hello) != 0) {
        log_warn(TAG, "invalid HELLO, closing");
        /* Unencrypted: no keys exist yet (§5.3). */
        size_t n = proto_build_event(frame, EV_ERROR, 0, ERR_BAD_HELLO,
                                     "invalid HELLO");
        write_frame(l, c, frame, n, 0);
        conn_close(l, c);
        return;
    }

    /* The tag authenticates the key exchange and is compared in constant time
     * (§11.4). The check is never skipped; there is no unencrypted fallback
     * (§11.1). */
    uint8_t expected[SEC_TAG_LEN];
    sec_hello_tag(l->token, HERA_TOKEN_LEN, hello.role, hello.formats,
                  hello.durations, hello.flags, hello.session_id,
                  hello.pubkey, expected);
    if (!sec_tag_equal(expected, hello.tag)) {
        log_warn(TAG, "HELLO tag does not verify, closing");
        size_t n = proto_build_event(frame, EV_ERROR, 0, ERR_BAD_TOKEN, NULL);
        write_frame(l, c, frame, n, 0);
        conn_close(l, c);
        return;
    }

    /* §5.7: a second connection is decided only now, once authenticated. */
    pthread_mutex_lock(&l->lock);
    int pending = c != CUR(l);
    int resumes_current = hello.resume && hello.session_id != 0 &&
                          hello.session_id == l->session_id;
    int busy = pending && l->up && !resumes_current;
    int takeover = pending && l->up && resumes_current;
    conn_t *old = CUR(l);
    pthread_mutex_unlock(&l->lock);

    if (busy) {
        refuse_busy(l, c, &hello);
        return;
    }

    if (pending) {
        if (takeover) {
            log_info(TAG, "session %08x taken over by a new connection",
                     hello.session_id);
            size_t n = proto_build_bye(frame, BYE_TAKEN_OVER, NULL);
            write_frame(l, old, frame, n, 1);
        } else if (old->fd >= 0) {
            /* The current connection has not authenticated; the verified
             * one replaces it. */
            log_info(TAG, "replacing an unauthenticated connection");
        }
        conn_close(l, old);
        pthread_mutex_lock(&l->lock);
        l->cur = (int)(c - l->conn);
        pthread_mutex_unlock(&l->lock);
    }

    pthread_mutex_lock(&l->lock);
    int resumed = 0;
    if (resumes_current && (l->suspended || takeover)) {
        /* Resume within the window, or takeover (§5.4, §5.7). */
        resumed = 1;
    } else {
        if (hello.resume)
            log_info(TAG, "resume of %08x refused", hello.session_id);
        l->session_id = new_session_id();
    }
    l->suspended = 0;
    l->up = 0;
    /* The flow is bound again by the first datagram of this connection
     * (§4.2.1), and uplink `ts` tracking restarts. */
    l->audio_bound = 0;
    l->ul_expected = 0;
    l->ul_epoch = -1;
    l->offered = hello.formats;
    l->offered_dur = hello.durations;
    l->fmt = proto_pick_format(hello.formats, l->sco_rate, config_fmt());
    l->frame_ms = proto_pick_duration(hello.durations, g_config.frame_ms);
    uint32_t sid = l->session_id;
    uint8_t fmt = l->fmt;
    unsigned frame_ms = l->frame_ms;
    pthread_mutex_unlock(&l->lock);

    char ident[64];
    size_t n = hello.ident_len < sizeof ident - 1 ? hello.ident_len
                                                  : sizeof ident - 1;
    memcpy(ident, hello.ident, n);
    ident[n] = '\0';

    if (handshake(l, c, &hello, sid) != 0) {
        log_warn(TAG, "unusable peer key, closing");
        pthread_mutex_lock(&l->lock);
        if (resumed) {
            /* Keep the session resumable for a better-behaved client. */
            l->suspended = 1;
            l->suspended_at_ms = now_ms();
        }
        pthread_mutex_unlock(&l->lock);
        conn_close(l, c);
        return;
    }

    pthread_mutex_lock(&l->lock);
    l->up = 1;
    l->last_rx_ms = now_ms();
    pthread_mutex_unlock(&l->lock);

    log_info(TAG, "client up: %s, session %08x, format 0x%02x, blocks of %u ms%s",
             n ? ident : "(anonymous)", sid, fmt, frame_ms,
             resumed ? " (resumed)" : "");

    if (hello.resume && !resumed)
        link_send_event(l, EV_ERROR, 0, ERR_RESUME, "session expired");

    /* READY carries the chosen format; the relay then sends the state
     * snapshot. */
    link_send_event(l, EV_READY, HERA_VERSION, fmt, l->ident);
    if (l->cb.on_format)
        l->cb.on_format(l->cb.ud, fmt, frame_ms);
    if (l->cb.on_up)
        l->cb.on_up(l->cb.ud, resumed);
}

/* Validate an audio block, count gaps and pass it on (§6.4). */
static void on_audio_frame(link_t *l, const uint8_t *payload, size_t len)
{
    pthread_mutex_lock(&l->lock);
    uint8_t fmt = l->fmt;
    unsigned frame_ms = l->frame_ms;
    pthread_mutex_unlock(&l->lock);

    hera_audio_t block;
    if (proto_parse_audio(payload, len, fmt, frame_ms, &block) != 0) {
        STAT_INC(ul_bad);
        return;
    }
    STAT_ADD(ul_blocks, block.count);

    /* A forward jump in `ts` counts as network loss (ul_lost), which is kept
     * separate from blocks dropped by the jitter buffer (§10.1). A new epoch
     * restarts `ts` (§6.3). */
    unsigned samples_per_block = fmt_block_samples(block.fmt, block.frame_ms);
    unsigned missing = 0;
    pthread_mutex_lock(&l->lock);
    if (l->ul_epoch != block.epoch) {
        l->ul_epoch = block.epoch;
        l->ul_expected = 0;
    }
    if (l->ul_expected && block.ts > l->ul_expected && samples_per_block)
        missing = (block.ts - l->ul_expected) / samples_per_block;
    l->ul_expected = block.ts + block.count * samples_per_block;
    pthread_mutex_unlock(&l->lock);

    if (missing) {
        STAT_ADD(ul_lost, missing);
        if (l->cb.on_audio_gap)
            l->cb.on_audio_gap(l->cb.ud, missing);
    }
    if (l->cb.on_audio)
        l->cb.on_audio(l->cb.ud, &block);
}

/* Dispatch one complete control frame received on `c`. */
static void on_frame(link_t *l, conn_t *c, const uint8_t *frame, size_t len)
{
    uint8_t type = frame[0];
    const uint8_t *payload = frame + HERA_HDR_LEN;
    size_t plen = len - HERA_HDR_LEN;

    pthread_mutex_lock(&l->lock);
    int ready = c->ready;
    if (ready)
        l->last_rx_ms = now_ms();
    pthread_mutex_unlock(&l->lock);

    /* The first frame on a new control channel must be HELLO (§5.1). */
    if (!ready) {
        if (type != T_HELLO) {
            log_warn(TAG, "first frame is 0x%02x, not HELLO: closing", type);
            conn_fail(l, c);
            return;
        }
        on_hello(l, c, payload, plen);
        return;
    }

    /* A ready connection is always the current one: pending connections are
     * promoted or closed while their HELLO is handled. */
    switch (type) {
    case T_HELLO:
        log_warn(TAG, "HELLO on an established session: closing");
        cur_closed(l, 0);
        break;

    case T_PING: {
        uint8_t out[HERA_MAX_FRAME];
        size_t n = proto_build_pong(out, payload, plen);
        send_ctrl(l, out, n);
        break;
    }

    case T_BYE: {
        uint8_t reason = plen ? payload[0] : BYE_NORMAL;
        log_info(TAG, "client said BYE (reason %u)", reason);
        /* BYE closes the session without a resume window (§5.6). */
        cur_closed(l, 0);
        break;
    }

    case T_CMD: {
        hera_tlv_t cmd;
        if (proto_parse_tlv(payload, plen, &cmd) != 0) {
            link_send_event(l, EV_ERROR, 0, ERR_SHORT_PAYLOAD, "short CMD");
            cur_closed(l, 0);
            break;
        }
        if (l->cb.on_cmd)
            l->cb.on_cmd(l->cb.ud, &cmd);
        break;
    }

    case T_CSTATS: {
        hera_cstats_t cs;
        if (proto_parse_cstats(payload, plen, &cs) != 0)
            break;              /* diagnostic only, never fatal (§10.2) */
        if (l->cb.on_cstats)
            l->cb.on_cstats(l->cb.ud, &cs);
        break;
    }

    default:
        /* Unknown types, and known types that do not belong on the control
         * channel, are ignored (§5.1). */
        break;
    }
}

/* Parse and dispatch the complete frames in a plaintext buffer (§4.1.1).
 *
 * Returns the number of bytes consumed, or -1 if a frame header is invalid;
 * the caller then closes the connection (§5.1). Stops early if a frame
 * closed the connection. */
static ssize_t dispatch_frames(link_t *l, conn_t *c, const uint8_t *buf,
                               size_t len)
{
    size_t off = 0;
    while (len - off >= HERA_HDR_LEN) {
        uint16_t plen = (uint16_t)(buf[off + 2] | (buf[off + 3] << 8));
        if (plen > HERA_MAX_PAYLOAD) {
            log_warn(TAG, "declared length %u, closing", plen);
            return -1;
        }
        size_t total = HERA_HDR_LEN + plen;
        if (len - off < total)
            break;
        on_frame(l, c, buf + off, total);
        off += total;
        if (c->fd < 0)
            break;
    }
    return (ssize_t)off;
}

/* Append received control bytes for `c` and process them.
 *
 * Before key derivation the stream carries plaintext frames (HELLO). After it,
 * the stream carries records, each containing one or more frames (§11.2). */
static void ingest_ctrl(link_t *l, conn_t *c, const uint8_t *data, size_t len)
{
    if (c->rx_len + len > sizeof c->rx) {
        /* Not reachable: the caller never reads more than the free space. */
        log_warn(TAG, "control buffer overflow, closing");
        conn_fail(l, c);
        return;
    }
    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;

    size_t off = 0;
    for (;;) {
        pthread_mutex_lock(&l->lock);
        int sealed = c->sec.ready;
        pthread_mutex_unlock(&l->lock);

        if (!sealed) {
            /* Plaintext phase. Once HELLO is handled the keys exist and the
             * next iteration switches to records. */
            ssize_t used = dispatch_frames(l, c, c->rx + off, c->rx_len - off);
            if (used < 0) {
                conn_fail(l, c);
                return;
            }
            if (c->fd < 0)
                return;
            if (used == 0)
                break;
            off += (size_t)used;
            continue;
        }

        if (c->rx_len - off < 2)
            break;
        size_t reclen = sec_record_len(c->rx + off);
        if (reclen == 0) {
            log_warn(TAG, "record length out of range, closing");
            conn_fail(l, c);
            return;
        }
        if (c->rx_len - off < reclen)
            break;

        pthread_mutex_lock(&l->lock);
        ssize_t plain = sec_open_record(&c->sec.ctrl_rx, c->rx + off, reclen,
                                        l->plain, sizeof l->plain);
        pthread_mutex_unlock(&l->lock);
        if (plain < 0) {
            /* Authentication failure or non-increasing counter: fatal on the
             * control channel (§11.2). */
            log_warn(TAG, "control record rejected, closing");
            conn_fail(l, c);
            return;
        }
        off += reclen;

        if (dispatch_frames(l, c, l->plain, (size_t)plain) < 0) {
            conn_fail(l, c);
            return;
        }
        if (c->fd < 0)
            return;
    }

    if (off) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/* Process the UDP datagrams waiting on the audio socket (§4.2). */
static void ingest_audio(link_t *l)
{
    uint8_t buf[SEC_DATAGRAM_OVERHEAD + HERA_MAX_FRAME];
    struct sockaddr_in from;
    socklen_t fromlen;

    for (;;) {
        fromlen = sizeof from;
        ssize_t n = recvfrom(l->audio, buf, sizeof buf, MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0)
            return;

        /* The session id is part of the AAD, so a datagram that opens is
         * authentic and may rebind the flow (§11.2). */
        uint8_t plain[HERA_MAX_FRAME];
        ssize_t flen_signed = -1;
        pthread_mutex_lock(&l->lock);
        uint32_t sid = l->session_id;
        if (l->up && CUR(l)->sec.ready)
            flen_signed = sec_open_datagram(&CUR(l)->sec.audio_rx, sid, buf,
                                            (size_t)n, plain, sizeof plain);
        pthread_mutex_unlock(&l->lock);
        if (flen_signed <= 0) {
            STAT_INC(ul_bad);
            continue;           /* silently discarded (§4.2) */
        }
        const uint8_t *frame = plain;
        size_t flen = (size_t)flen_signed;
        if (flen < HERA_HDR_LEN ||
            flen != (size_t)HERA_HDR_LEN + get_u16le(frame + 2)) {
            STAT_INC(ul_bad);
            continue;
        }

        /* Bind the flow to the source of the first valid datagram, and rebind
         * if the source changes (§4.2.1). */
        pthread_mutex_lock(&l->lock);
        if (!l->audio_bound || memcmp(&l->peer, &from, sizeof from) != 0) {
            l->peer = from;
            l->audio_bound = 1;
            log_info(TAG, "audio flow bound to %s:%u",
                     inet_ntoa_safe(from.sin_addr), ntohs(from.sin_port));
        }
        l->last_rx_ms = now_ms();
        pthread_mutex_unlock(&l->lock);

        switch (frame[0]) {
        case T_AUDIO_TX:
            on_audio_frame(l, frame + HERA_HDR_LEN, flen - HERA_HDR_LEN);
            break;
        case T_PING: {
            /* The PONG confirms to the client that the path works both ways. */
            uint8_t out[HERA_MAX_FRAME];
            size_t olen = proto_build_pong(out, frame + HERA_HDR_LEN,
                                           flen - HERA_HDR_LEN);
            uint8_t dg[SEC_DATAGRAM_OVERHEAD + HERA_MAX_FRAME];
            size_t total = 0;
            pthread_mutex_lock(&l->lock);
            if (l->up && CUR(l)->sec.ready)
                total = sec_seal_datagram(&CUR(l)->sec.audio_tx, sid, out,
                                          olen, dg, sizeof dg);
            pthread_mutex_unlock(&l->lock);
            if (total)
                sendto(l->audio, dg, total, MSG_DONTWAIT,
                       (struct sockaddr *)&from, sizeof from);
            break;
        }
        default:
            /* Other types are ignored on the audio channel (§4.2). */
            break;
        }
    }
}

/* Accept a control connection. If a connection is already open, the new one
 * is kept pending until its HELLO decides between takeover and refusal
 * (§5.7). An older pending connection, which has not authenticated, is
 * replaced. */
static void accept_ctrl(link_t *l)
{
    int fd = accept(l->listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    set_nonblocking(fd);
    int one = 1;
    /* Disable Nagle; control frames are small and latency-sensitive (§4.1). */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    pthread_mutex_lock(&l->lock);
    conn_t *cur = CUR(l);
    conn_t *slot = cur->fd < 0 ? cur : PEND(l);
    int evicted = slot->fd;
    int pending = slot != cur;
    pthread_mutex_unlock(&l->lock);

    if (evicted >= 0) {
        log_info(TAG, "replacing a pending connection that sent no HELLO");
        conn_close(l, slot);
    }

    pthread_mutex_lock(&l->lock);
    conn_open_locked(slot, fd);
    pthread_mutex_unlock(&l->lock);
    if (pending)
        log_info(TAG, "second connection pending until its HELLO (§5.7)");
}

/* --------------------------------------------------------------- the loop --- */

static void check_timeouts(link_t *l, int64_t now);

static void *link_thread(void *arg)
{
    link_t *l = arg;
    int64_t next_tick = now_ms() + 1000;

    while (__atomic_load_n(&l->running, __ATOMIC_RELAXED)) {
        struct pollfd fds[5];
        conn_t *owner[5] = { NULL };
        int n = 0;
        int i_listen = -1, i_audio = -1;

        if (l->listen_fd >= 0) {
            fds[n] = (struct pollfd){ .fd = l->listen_fd, .events = POLLIN };
            i_listen = n++;
        }
        pthread_mutex_lock(&l->lock);
        for (int i = 0; i < 2; i++) {
            if (l->conn[i].fd < 0)
                continue;
            /* POLLIN only: nothing is queued for output (§6.6). */
            fds[n] = (struct pollfd){ .fd = l->conn[i].fd, .events = POLLIN };
            owner[n++] = &l->conn[i];
        }
        int audio_fd = l->audio;
        pthread_mutex_unlock(&l->lock);
        if (audio_fd >= 0) {
            fds[n] = (struct pollfd){ .fd = audio_fd, .events = POLLIN };
            i_audio = n++;
        }
        fds[n] = (struct pollfd){ .fd = l->wake, .events = POLLIN };
        int i_wake = n++;

        int64_t now = now_ms();
        int timeout = (int)(next_tick > now ? next_tick - now : 0);
        if (poll(fds, (nfds_t)n, timeout) < 0 && errno != EINTR)
            break;

        if (fds[i_wake].revents & POLLIN) {
            uint64_t drain;
            while (read(l->wake, &drain, sizeof drain) > 0) { }
        }
        flush_logs(l);
        if (i_audio >= 0 && (fds[i_audio].revents & POLLIN))
            ingest_audio(l);

        for (int k = 0; k < n; k++) {
            conn_t *c = owner[k];
            if (!c || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            /* Skip descriptors closed or replaced since poll() returned. */
            if (c->fd != fds[k].fd)
                continue;
            /* Read no more than the reassembly buffer can hold, so that a
             * peer batching several records is not disconnected. */
            size_t room = sizeof c->rx - c->rx_len;
            uint8_t buf[sizeof c->rx];
            ssize_t got = room ? read(c->fd, buf, room) : -1;
            if (got > 0) {
                ingest_ctrl(l, c, buf, (size_t)got);
            } else if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                pthread_mutex_lock(&l->lock);
                int is_cur = c == CUR(l);
                pthread_mutex_unlock(&l->lock);
                /* Closed without BYE: keep the session resumable (§5.6). */
                if (is_cur)
                    cur_closed(l, 1);
                else
                    conn_close(l, c);
            }
        }

        /* Accept after reading, so that a new connection cannot be placed in
         * a slot whose events from this poll() are still pending. */
        if (i_listen >= 0 && (fds[i_listen].revents & POLLIN))
            accept_ctrl(l);

        now = now_ms();
        if (now >= next_tick) {
            next_tick = now + 1000;
            check_timeouts(l, now);
            if (link_is_up(l) && l->cb.on_tick)
                l->cb.on_tick(l->cb.ud);
        }
    }
    return NULL;
}

/* -------------------------------------------------------------- timeouts --- */

/* Handshake timeout, peer timeout and resume window (§5.4, §5.5).
 *
 * The periodic STATS/CSTATS frames act as keepalives; a peer that has sent
 * nothing for peer_timeout_s is considered dead. A connection that has not
 * completed its HELLO within the same time is closed, so that an idle
 * connection cannot hold a slot.
 */
static void check_timeouts(link_t *l, int64_t now)
{
    int64_t limit = (int64_t)g_config.peer_timeout_s * 1000;

    pthread_mutex_lock(&l->lock);
    int stale[2];
    for (int i = 0; i < 2; i++)
        stale[i] = l->conn[i].fd >= 0 && !l->conn[i].ready &&
                   now - l->conn[i].opened_ms > limit;
    int up = l->up;
    int suspended = l->suspended;
    int64_t last_rx = l->last_rx_ms;
    int64_t since_suspend = now - l->suspended_at_ms;
    pthread_mutex_unlock(&l->lock);

    for (int i = 0; i < 2; i++) {
        if (stale[i]) {
            log_warn(TAG, "no HELLO within %d s: closing the connection",
                     g_config.peer_timeout_s);
            conn_close(l, &l->conn[i]);
        }
    }

    if (up && now - last_rx > limit) {
        log_warn(TAG, "nothing from the client for %d s: peer is dead",
                 g_config.peer_timeout_s);
        cur_closed(l, 1);
        return;
    }

    if (suspended &&
        since_suspend > (int64_t)g_config.resume_window_s * 1000) {
        log_info(TAG, "resume window expired, session discarded");
        pthread_mutex_lock(&l->lock);
        discard_session_locked(l);
        pthread_mutex_unlock(&l->lock);
        if (l->cb.on_down)
            l->cb.on_down(l->cb.ud, 1);
    }
}

/* ----------------------------------------------------------- lifecycle --- */

link_t *link_create(const link_cb_t *cb, const char *ident)
{
    link_t *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->cb = *cb;
    l->ident = xstrdup(ident);
    l->listen_fd = l->audio = l->wake = -1;
    l->conn[0].fd = l->conn[1].fd = -1;
    l->cur = 0;
    l->ul_epoch = -1;
    l->frame_ms = PROTO_MAX_FRAME_MS;
    pthread_mutex_init(&l->lock, NULL);
    pthread_mutex_init(&l->tx_lock, NULL);
    pthread_mutex_init(&l->log_lock, NULL);
    if (sec_init() != 0) {
        log_error(TAG, "cannot initialise libsodium");
        free(l->ident);
        free(l);
        return NULL;
    }
    return l;
}

int link_start(link_t *l, int ctrl_port, int audio_port,
               const uint8_t token[HERA_TOKEN_LEN])
{
    if (token)
        memcpy(l->token, token, HERA_TOKEN_LEN);

    l->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (l->wake < 0) {
        log_error(TAG, "eventfd: %s", strerror(errno));
        return -1;
    }

    l->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    l->audio = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (l->listen_fd < 0 || l->audio < 0) {
        log_error(TAG, "socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(l->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in ctrl_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)ctrl_port),
    };
    struct sockaddr_in audio_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)audio_port),
    };
    if (bind(l->listen_fd, (struct sockaddr *)&ctrl_addr,
             sizeof ctrl_addr) != 0 ||
        bind(l->audio, (struct sockaddr *)&audio_addr,
             sizeof audio_addr) != 0 ||
        listen(l->listen_fd, 2) != 0) {
        log_error(TAG, "bind/listen: %s", strerror(errno));
        return -1;
    }
    log_info(TAG, "control on tcp/%d, audio on udp/%d", ctrl_port,
             audio_port);

    __atomic_store_n(&l->running, 1, __ATOMIC_RELAXED);
    if (pthread_create(&l->thread, NULL, link_thread, l) != 0) {
        log_error(TAG, "cannot start the link thread");
        __atomic_store_n(&l->running, 0, __ATOMIC_RELAXED);
        return -1;
    }
    return 0;
}

/* Stop the link thread and close every socket. The object stays valid, so
 * that the threads still running at shutdown (audio, HFP, workers) can call
 * link_* safely until link_destroy(): sends become no-ops. */
void link_stop(link_t *l)
{
    if (!l)
        return;
    if (__atomic_exchange_n(&l->running, 0, __ATOMIC_RELAXED)) {
        kick(l);
        pthread_join(l->thread, NULL);
    }
    for (int i = 0; i < 2; i++)
        conn_close(l, &l->conn[i]);

    pthread_mutex_lock(&l->lock);
    l->up = 0;
    l->suspended = 0;
    int audio = l->audio;
    l->audio = -1;
    pthread_mutex_unlock(&l->lock);
    if (audio >= 0)
        close(audio);
    if (l->listen_fd >= 0)
        close(l->listen_fd);
    l->listen_fd = -1;
    pthread_mutex_lock(&l->log_lock);
    if (l->wake >= 0)
        close(l->wake);
    l->wake = -1;
    pthread_mutex_unlock(&l->log_lock);
}

/* Free the link. No other thread may use it any more. */
void link_destroy(link_t *l)
{
    if (!l)
        return;
    link_stop(l);
    pthread_mutex_destroy(&l->log_lock);
    pthread_mutex_destroy(&l->tx_lock);
    pthread_mutex_destroy(&l->lock);
    free(l->ident);
    free(l);
}

/* ------------------------------------------------------------- accessors --- */

int link_is_up(link_t *l)
{
    pthread_mutex_lock(&l->lock);
    int up = l->up;
    pthread_mutex_unlock(&l->lock);
    return up;
}

int link_is_suspended(link_t *l)
{
    pthread_mutex_lock(&l->lock);
    int s = l->suspended;
    pthread_mutex_unlock(&l->lock);
    return s;
}

uint8_t link_format(link_t *l)
{
    pthread_mutex_lock(&l->lock);
    uint8_t fmt = l->fmt;
    pthread_mutex_unlock(&l->lock);
    return fmt;
}

uint8_t link_epoch(link_t *l)
{
    pthread_mutex_lock(&l->lock);
    uint8_t e = l->epoch;
    pthread_mutex_unlock(&l->lock);
    return e;
}

unsigned link_frame_ms(link_t *l)
{
    pthread_mutex_lock(&l->lock);
    unsigned ms = l->frame_ms ? l->frame_ms : PROTO_MAX_FRAME_MS;
    pthread_mutex_unlock(&l->lock);
    return ms;
}

void link_set_sco_rate(link_t *l, unsigned rate)
{
    pthread_mutex_lock(&l->lock);
    l->sco_rate = rate;
    /* With a session up, select the format again: the SCO rate determines
     * which format needs no conversion (§6.2). */
    uint8_t fmt = l->fmt;
    if (l->up && l->offered) {
        l->fmt = proto_pick_format(l->offered, rate, config_fmt());
        fmt = l->fmt;
    }
    pthread_mutex_unlock(&l->lock);
    if (fmt && l->cb.on_format)
        l->cb.on_format(l->cb.ud, fmt, link_frame_ms(l));
}

void link_new_audio_epoch(link_t *l)
{
    pthread_mutex_lock(&l->lock);
    l->epoch = (uint8_t)((l->epoch + 1) & 0x0F);
    pthread_mutex_unlock(&l->lock);
}

int link_set_format(link_t *l, uint8_t fmt)
{
    if (!fmt_block_bytes(fmt, link_frame_ms(l)))
        return -1;
    pthread_mutex_lock(&l->lock);
    /* Refuse formats the client did not offer; the current format is kept
     * (§9). */
    int ok = l->up && (l->offered & FMT_BIT(fmt));
    if (ok)
        l->fmt = fmt;
    pthread_mutex_unlock(&l->lock);
    if (!ok)
        return -1;
    if (l->cb.on_format)
        l->cb.on_format(l->cb.ud, fmt, link_frame_ms(l));
    return 0;
}
