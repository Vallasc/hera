/* Integration test for link.c (Linux only: eventfd). Run with `make test-link`.
 *
 * Starts the link on localhost and drives it with test clients that perform
 * the encrypted handshake: new session, busy refusal (BYE 2), idle
 * connection timeout, wrong token (ERROR 5), takeover (BYE 3), resume after a
 * drop, and concurrent control writes from several threads. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "link.h"
#include "log.h"
#include "proto.h"
#include "sec.h"
#include "util.h"

#define CTRL_PORT  17420
#define AUDIO_PORT 17421

static int failures;

static void check(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

/* ------------------------------------------------------------ callbacks --- */

static link_t *g_link;
/* Written by the callbacks on the link thread, read by main. */
static int ups, downs, suspends, last_resumed;
#define GET(v) __atomic_load_n(&(v), __ATOMIC_SEQ_CST)

static void cb_up(void *ud, int resumed)
{
    __atomic_add_fetch(&ups, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&last_resumed, resumed, __ATOMIC_SEQ_CST);
}
static void cb_down(void *ud, int expired)
{
    __atomic_add_fetch(&downs, 1, __ATOMIC_SEQ_CST);
}
static void cb_suspend(void *ud)
{
    __atomic_add_fetch(&suspends, 1, __ATOMIC_SEQ_CST);
}
static void cb_cmd(void *ud, const hera_tlv_t *cmd)
{
    link_send_ack(g_link, cmd->id, 0);
}

/* --------------------------------------------------------------- client --- */

static const uint8_t TOKEN[HERA_TOKEN_LEN] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

typedef struct {
    int           fd;
    sec_keypair_t kp;
    sec_session_t sec;
    uint32_t      sid;
    uint8_t       rx[8192];
    size_t        rx_len;
} cli_t;

static int cli_connect(cli_t *c)
{
    memset(c, 0, sizeof *c);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_port = htons(CTRL_PORT) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0)
        return -1;
    return sec_keypair(&c->kp);
}

static void cli_hello(cli_t *c, const uint8_t *token, int resume, uint32_t sid)
{
    uint8_t tag[SEC_TAG_LEN];
    uint8_t flags = resume ? 1 : 0;
    sec_hello_tag(token, HERA_TOKEN_LEN, 0, FMT_ALL, DUR_ALL, flags, sid,
                  c->kp.pub, tag);
    uint8_t frame[HERA_MAX_FRAME];
    size_t n = proto_build_hello(frame, FMT_ALL, DUR_ALL, flags, sid,
                                 c->kp.pub, tag, "test");
    frame[HERA_HDR_LEN + 5] = 0;        /* role: client */
    (void)!write(c->fd, frame, n);
}

static void cli_send(cli_t *c, const uint8_t *frame, size_t len)
{
    uint8_t rec[SEC_MAX_RECORD];
    size_t n = sec_seal_record(&c->sec.ctrl_tx, frame, len, rec, sizeof rec);
    (void)!write(c->fd, rec, n);
}

/* Read one frame. Plaintext until the device HELLO has been verified and the
 * keys derived, records afterwards. Returns 1 with a frame, 0 on timeout, -1
 * if the connection closed or a record was rejected. */
static int cli_read(cli_t *c, uint8_t *frame, size_t *len, int timeout_ms)
{
    int64_t deadline = (int64_t)now_ms() + timeout_ms;
    for (;;) {
        if (!c->sec.ready && c->rx_len >= HERA_HDR_LEN) {
            size_t total = HERA_HDR_LEN + get_u16le(c->rx + 2);
            if (c->rx_len >= total) {
                memcpy(frame, c->rx, total);
                *len = total;
                memmove(c->rx, c->rx + total, c->rx_len - total);
                c->rx_len -= total;
                if (frame[0] == T_HELLO) {
                    hera_hello_t h;
                    uint8_t want[SEC_TAG_LEN];
                    if (proto_parse_hello(frame + HERA_HDR_LEN,
                                          total - HERA_HDR_LEN, &h) != 0)
                        return -1;
                    sec_hello_tag(TOKEN, HERA_TOKEN_LEN, h.role, h.formats,
                                  h.durations, h.flags, h.session_id,
                                  h.pubkey, want);
                    if (!sec_tag_equal(want, h.tag) ||
                        sec_derive(&c->sec, &c->kp, h.pubkey, 0) != 0)
                        return -1;
                    c->sid = h.session_id;
                }
                return 1;
            }
        } else if (c->sec.ready && c->rx_len >= 2) {
            size_t reclen = sec_record_len(c->rx);
            if (reclen == 0)
                return -1;
            if (c->rx_len >= reclen) {
                ssize_t n = sec_open_record(&c->sec.ctrl_rx, c->rx, reclen,
                                            frame, HERA_MAX_FRAME);
                if (n < 0)
                    return -1;
                *len = (size_t)n;
                memmove(c->rx, c->rx + reclen, c->rx_len - reclen);
                c->rx_len -= reclen;
                return 1;
            }
        }
        int64_t left = deadline - (int64_t)now_ms();
        if (left <= 0)
            return 0;
        struct pollfd p = { .fd = c->fd, .events = POLLIN };
        if (poll(&p, 1, (int)left) <= 0)
            return 0;
        ssize_t got = read(c->fd, c->rx + c->rx_len, sizeof c->rx - c->rx_len);
        if (got <= 0)
            return -1;
        c->rx_len += (size_t)got;
    }
}

/* Read frames until one of `type` (and event `id`, if not 0) arrives. */
static int cli_wait(cli_t *c, uint8_t type, uint8_t id, uint8_t *frame,
                    size_t *len, int timeout_ms)
{
    int64_t deadline = (int64_t)now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(deadline - (int64_t)now_ms());
        if (left <= 0)
            return 0;
        int r = cli_read(c, frame, len, left);
        if (r <= 0)
            return r;
        if (frame[0] == type && (!id || frame[HERA_HDR_LEN] == id))
            return 1;
    }
}

/* Complete a handshake: HELLO, then READY. Returns 1 on success. */
static int cli_session(cli_t *c, int resume, uint32_t sid)
{
    uint8_t f[HERA_MAX_FRAME];
    size_t n;
    if (cli_connect(c) != 0)
        return 0;
    cli_hello(c, TOKEN, resume, sid);
    return cli_wait(c, T_EVENT, EV_READY, f, &n, 2000) == 1;
}

/* A PING on the control channel answered with PONG. */
static int cli_alive(cli_t *c)
{
    uint8_t f[HERA_MAX_FRAME];
    size_t n;
    proto_put_header(f, T_PING, 0);
    cli_send(c, f, HERA_HDR_LEN);
    return cli_wait(c, T_PONG, 0, f, &n, 2000) == 1;
}

static int cli_closed(cli_t *c, int timeout_ms)
{
    uint8_t f[HERA_MAX_FRAME];
    size_t n;
    int64_t deadline = (int64_t)now_ms() + timeout_ms;
    while ((int64_t)now_ms() < deadline) {
        int r = cli_read(c, f, &n, (int)(deadline - (int64_t)now_ms()));
        if (r < 0)
            return 1;
    }
    return 0;
}

/* Wait up to `timeout_ms` for `idle` to be closed by the device, keeping
 * `live` within the peer timeout meanwhile. */
static int closed_while_alive(cli_t *idle, cli_t *live, int timeout_ms)
{
    int64_t deadline = (int64_t)now_ms() + timeout_ms;
    while ((int64_t)now_ms() < deadline) {
        if (cli_closed(idle, 300))
            return 1;
        if (!cli_alive(live))
            return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- tests --- */

static void *sender(void *arg)
{
    for (int i = 0; i < 300; i++)
        link_send_event(g_link, EV_SIGNAL, 3, 0, NULL);
    return NULL;
}

static void *logger(void *arg)
{
    for (int i = 0; i < 100; i++)
        link_send_log(g_link, "burst log line");
    return NULL;
}

int main(void)
{
    log_set_level(1);
    /* The device sends with MSG_NOSIGNAL; the test client must not die on a
     * socket the device has closed either. */
    signal(SIGPIPE, SIG_IGN);
    /* HERA_TEST_PEER_TIMEOUT lengthens the timeouts for slow runs, e.g.
     * under ThreadSanitizer. */
    const char *slow = getenv("HERA_TEST_PEER_TIMEOUT");
    g_config.peer_timeout_s = slow && atoi(slow) > 0 ? atoi(slow) : 2;
    g_config.resume_window_s = 10;
    g_config.frame_ms = 0;
    g_config.fmt = "auto";
    if (sec_init() != 0)
        return 1;

    link_cb_t cb = { .on_up = cb_up, .on_down = cb_down,
                     .on_suspend = cb_suspend, .on_cmd = cb_cmd };
    g_link = link_create(&cb, "test-device");
    check(link_start(g_link, CTRL_PORT, AUDIO_PORT, TOKEN) == 0, "link starts");

    uint8_t f[HERA_MAX_FRAME];
    size_t n;

    /* New session. */
    cli_t a;
    check(cli_session(&a, 0, 0), "client A: READY");
    check(link_is_up(g_link) && GET(ups) == 1 && !GET(last_resumed), "A: new session up");
    check(cli_alive(&a), "A: PING answered");

    /* Concurrent control writes keep records in counter order. */
    pthread_t t[3];
    for (int i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, sender, NULL);
    int events = 0, r;
    while ((r = cli_read(&a, f, &n, 1000)) == 1)
        if (f[0] == T_EVENT && f[HERA_HDR_LEN] == EV_SIGNAL)
            events++;
    for (int i = 0; i < 3; i++)
        pthread_join(t[i], NULL);
    check(r == 0 && events == 900, "A: 900 concurrent records, in order");

    /* LOG lines are queued and sent by the link thread; a burst larger than
     * the queue loses the oldest lines, never the session. A PING first:
     * the reads above sent nothing, and the peer timeout must not expire. */
    check(cli_alive(&a), "A: alive after the concurrent records");
    link_send_log(g_link, "single log line");
    int logs = 0;
    while ((r = cli_read(&a, f, &n, 500)) == 1)
        if (f[0] == T_LOG)
            logs++;
    check(r == 0 && logs == 1, "A: queued LOG line delivered");
    check(cli_alive(&a), "A: alive before the LOG burst");
    for (int i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, logger, NULL);
    for (int i = 0; i < 3; i++)
        pthread_join(t[i], NULL);
    logs = 0;
    while ((r = cli_read(&a, f, &n, 500)) == 1)
        if (f[0] == T_LOG)
            logs++;
    check(r == 0 && logs >= 1 && logs <= 300, "A: LOG burst bounded, session intact");
    check(cli_alive(&a), "A: alive after the LOG burst");

    /* Second client, new session: refused with a sealed BYE 2. */
    cli_t b;
    cli_connect(&b);
    cli_hello(&b, TOKEN, 0, 0);
    int got = cli_wait(&b, T_BYE, 0, f, &n, 2000);
    check(got == 1 && f[HERA_HDR_LEN] == BYE_BUSY, "B: refused with BYE 2");
    check(cli_closed(&b, 1000), "B: connection closed");
    check(cli_alive(&a), "A: unaffected by B");

    /* Idle connection: closed after the handshake timeout. */
    cli_t c;
    cli_connect(&c);
    check(closed_while_alive(&c, &a, g_config.peer_timeout_s * 2500),
          "C: idle connection closed");
    check(cli_alive(&a), "A: unaffected by C");

    /* Wrong token: unencrypted ERROR 5, then close. */
    cli_t e;
    uint8_t bad[HERA_TOKEN_LEN] = { 0 };
    cli_connect(&e);
    cli_hello(&e, bad, 0, 0);
    got = cli_read(&e, f, &n, 2000);
    check(got == 1 && f[0] == T_EVENT && f[HERA_HDR_LEN] == EV_ERROR &&
          get_u16le(f + HERA_HDR_LEN + 2) == ERR_BAD_TOKEN,
          "E: wrong token gets ERROR 5");
    check(cli_closed(&e, 1000), "E: connection closed");
    check(cli_alive(&a), "A: unaffected by E");

    /* Takeover: D resumes A's session while A is still connected. */
    uint32_t sid = a.sid;
    cli_t d;
    check(cli_session(&d, 1, sid), "D: READY on takeover");
    check(d.sid == sid, "D: same session id");
    check(GET(last_resumed) == 1 && GET(suspends) == 0, "D: session continued");
    got = cli_wait(&a, T_BYE, 0, f, &n, 2000);
    check(got == 1 && f[HERA_HDR_LEN] == BYE_TAKEN_OVER, "A: sealed BYE 3");
    check(cli_alive(&d), "D: PING answered");

    /* Drop without BYE, then resume. */
    close(d.fd);
    for (int i = 0; i < 100 && !GET(suspends); i++)
        sleep_ms(10);
    check(GET(suspends) == 1 && link_is_suspended(g_link), "D dropped: suspended");
    cli_t g;
    check(cli_session(&g, 1, sid), "G: READY on resume");
    check(g.sid == sid && GET(last_resumed) == 1, "G: session resumed");
    check(cli_alive(&g), "G: PING answered");

    /* A command is acknowledged on the current connection. */
    size_t cn = proto_build_event(f, CMD_STATS, 0, 0, NULL);
    f[0] = T_CMD;
    cli_send(&g, f, cn);
    check(cli_wait(&g, T_ACK, 0, f, &n, 2000) == 1, "G: CMD acknowledged");

    /* BYE ends the session. */
    proto_build_bye(f, BYE_NORMAL, NULL);
    cli_send(&g, f, HERA_HDR_LEN + 1);
    for (int i = 0; i < 100 && GET(downs) == 0; i++)
        sleep_ms(10);
    check(GET(downs) == 1 && !link_is_up(g_link) && !link_is_suspended(g_link),
          "G: BYE ends the session");

    link_destroy(g_link);
    printf("\n%s\n", failures ? "FAILED" : "all tests passed");
    return failures ? 1 : 0;
}
