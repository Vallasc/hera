#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem() */
#endif

#include "hfp.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include "log.h"
#include "proto.h"
#include "store.h"
#include "util.h"

/* HF feature bits we claim in AT+BRSF (HFP 1.7 §4.34.1):
 *   bit1 call waiting / 3-way   -> +CCWA, needed for CLIP a1=1 (§8)
 *   bit2 CLI presentation       -> +CLIP
 *   bit3 voice recognition      -> AT+BVRA (CMD VR_START/VR_STOP)
 *   bit4 remote volume control  -> +VGS / +VGM (EVENT VOLUME, CMD VOL_*)
 *   bit5 enhanced call status   -> AT+CLCC
 *   bit7 codec negotiation      -> AT+BAC / +BCS
 * We do NOT claim EC/NR (bit0): the phone's own processing is better placed. */
#define HF_FEATURES 190

/* AG feature bit 9 = codec negotiation (HFP 1.7 §4.34.2). */
#define AG_FEAT_CODEC_NEG (1u << 9)
/* AG feature bit 5 = 3-way calling, which gates the AT+CHLD=? exchange. */
#define AG_FEAT_3WAY      (1u << 5)

#define AT_TIMEOUT_MS 3000

/* Positions of the indicators we care about inside +CIEV, learnt from the
 * AT+CIND=? test response. 0 means "the phone does not report it". */
typedef struct {
    int service, call, callsetup, callheld, signal, battchg, roam;
} cind_map_t;

struct hfp {
    /* The RFCOMM socket. Read, written to and closed only under `write_lock`:
     * otherwise a writer on another thread could write to a descriptor that
     * the AT thread has just closed and the kernel has reused. */
    int             fd;
    int             running;        /* atomic */
    pthread_t       thread;
    int             thread_started; /* under conn_lock */
    pthread_mutex_t write_lock;
    pthread_mutex_t state_lock;

    /* Serialises installing a link (incoming or outgoing), joining the AT
     * thread and destroying. */
    pthread_mutex_t conn_lock;

    cind_map_t      cind;
    hfp_state_t     st;

    char            buf[2048];      /* line reassembly, AT thread only */
    size_t          buf_len;

    unsigned        ag_features;

    hfp_event_fn    on_event;
    void           *ud;
};

static void emit(hfp_t *h, uint8_t id, uint8_t a1, uint16_t a2, const char *str)
{
    if (h->on_event)
        h->on_event(h->ud, id, a1, a2, str);
}

hfp_t *hfp_create(hfp_event_fn on_event, void *ud)
{
    hfp_t *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->fd = -1;
    h->on_event = on_event;
    h->ud = ud;
    h->st.service = h->st.signal = h->st.battery = -1;
    h->st.vol_spk = h->st.vol_mic = -1;
    pthread_mutex_init(&h->write_lock, NULL);
    pthread_mutex_init(&h->state_lock, NULL);
    pthread_mutex_init(&h->conn_lock, NULL);
    return h;
}

static int get_fd(hfp_t *h)
{
    pthread_mutex_lock(&h->write_lock);
    int fd = h->fd;
    pthread_mutex_unlock(&h->write_lock);
    return fd;
}

int hfp_is_connected(hfp_t *h)
{
    return h && get_fd(h) >= 0;
}


void hfp_get_state(hfp_t *h, hfp_state_t *out)
{
    pthread_mutex_lock(&h->state_lock);
    *out = h->st;
    pthread_mutex_unlock(&h->state_lock);
}

/* --------------------------------------------------------- AT plumbing */

static int at_send(hfp_t *h, const char *cmd)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    if (n < 0 || n >= (int)sizeof line)
        return -1;
    log_debug("hfp", "[AT] >> %s", cmd);
    pthread_mutex_lock(&h->write_lock);
    if (h->fd < 0) {
        pthread_mutex_unlock(&h->write_lock);
        return -1;
    }
    /* Bounded by SO_SNDTIMEO, set when the link is installed. */
    ssize_t w = write(h->fd, line, (size_t)n);
    pthread_mutex_unlock(&h->write_lock);
    if (w < 0) {
        log_info("hfp", "AT write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Read one CRLF-terminated line. 1 = line in `out`, 0 = timeout, -1 = closed. */
static int at_read_line(hfp_t *h, int timeout_ms, char *out, size_t outsz)
{
    for (;;) {
        char *nl = memmem(h->buf, h->buf_len, "\r\n", 2);
        if (nl) {
            size_t linelen = (size_t)(nl - h->buf);
            size_t copy = linelen < outsz - 1 ? linelen : outsz - 1;
            memcpy(out, h->buf, copy);
            out[copy] = '\0';
            size_t consumed = linelen + 2;
            memmove(h->buf, h->buf + consumed, h->buf_len - consumed);
            h->buf_len -= consumed;

            char *p = out;
            while (*p == ' ' || *p == '\t')
                p++;
            size_t e = strlen(p);
            while (e > 0 && (p[e - 1] == ' ' || p[e - 1] == '\t' || p[e - 1] == '\r'))
                p[--e] = '\0';
            if (*p == '\0')
                continue;                   /* blank line: keep reading */
            if (p != out)
                memmove(out, p, e + 1);
            return 1;
        }

        int fd = get_fd(h);
        if (fd < 0)
            return -1;
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0)
            return 0;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (h->buf_len >= sizeof(h->buf) - 1)
            h->buf_len = 0;                 /* overflow guard: drop garbage */
        ssize_t r = read(fd, h->buf + h->buf_len, sizeof(h->buf) - h->buf_len - 1);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;
        h->buf_len += (size_t)r;
    }
}

static void handle_line(hfp_t *h, const char *line);

/* Pump lines through the normal handler until a final result code arrives.
 * Using the same handler during the handshake means unsolicited events that
 * arrive mid-handshake are not lost. */
static void at_wait_result(hfp_t *h, int timeout_ms)
{
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    char line[512];
    for (;;) {
        uint64_t now = now_ms();
        if (now >= deadline)
            return;
        int r = at_read_line(h, (int)(deadline - now), line, sizeof(line));
        if (r < 0)
            return;
        if (r == 0)
            continue;
        int final = (strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0 ||
                     strncmp(line, "+CME ERROR", 10) == 0);
        handle_line(h, line);
        if (final)
            return;
    }
}

static int at_command(hfp_t *h, const char *cmd)
{
    if (at_send(h, cmd) != 0)
        return -1;
    return 0;
}

/* Send and wait for the result code -- handshake use only (AT thread). */
static void at_exchange(hfp_t *h, const char *cmd)
{
    if (at_send(h, cmd) == 0)
        at_wait_result(h, AT_TIMEOUT_MS);
}

/* ------------------------------------------------------------- parsing */

/* Skip the "+XXX:" prefix and any leading blanks. */
static const char *after_colon(const char *line)
{
    const char *p = strchr(line, ':');
    if (!p)
        return NULL;
    p++;
    while (*p == ' ')
        p++;
    return p;
}

/* Copy the first comma-separated field, stripping surrounding quotes. */
static void first_field(const char *p, char *out, size_t outsz)
{
    size_t i = 0;
    if (*p == '"')
        p++;
    while (*p && *p != ',' && *p != '"' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* +CIND: ("service",(0,1)),("call",(0,1)),("callsetup",(0-3)),...
 * Learn which 1-based slot each indicator occupies, so +CIEV can be read. */
static void parse_cind_test(hfp_t *h, const char *line)
{
    const char *p = after_colon(line);
    if (!p)
        return;
    memset(&h->cind, 0, sizeof(h->cind));

    int idx = 0;
    while (*p) {
        const char *q = strchr(p, '(');
        if (!q)
            break;
        q++;
        if (*q != '"') {
            p = q;                          /* a numeric range: skip */
            continue;
        }
        idx++;
        const char *name = ++q;
        const char *endq = strchr(q, '"');
        if (!endq)
            break;
        size_t n = (size_t)(endq - name);
        if (n == 7 && !strncmp(name, "service", 7))          h->cind.service = idx;
        else if (n == 4 && !strncmp(name, "call", 4))        h->cind.call = idx;
        else if (n == 9 && !strncmp(name, "callsetup", 9))   h->cind.callsetup = idx;
        else if (n == 10 && !strncmp(name, "call_setup", 10))h->cind.callsetup = idx;
        else if (n == 8 && !strncmp(name, "callheld", 8))    h->cind.callheld = idx;
        else if (n == 6 && !strncmp(name, "signal", 6))      h->cind.signal = idx;
        else if (n == 7 && !strncmp(name, "battchg", 7))     h->cind.battchg = idx;
        else if (n == 4 && !strncmp(name, "roam", 4))        h->cind.roam = idx;
        p = endq + 1;
    }
    log_debug("hfp", "Indicators: service=%d call=%d callsetup=%d callheld=%d "
              "signal=%d battchg=%d", h->cind.service, h->cind.call,
              h->cind.callsetup, h->cind.callheld, h->cind.signal,
              h->cind.battchg);
}

/* Apply one indicator value and emit the matching event (§8). */
static void apply_indicator(hfp_t *h, int idx, int val)
{
    if (idx <= 0)
        return;
    pthread_mutex_lock(&h->state_lock);
    if (idx == h->cind.service)        h->st.service = val;
    else if (idx == h->cind.call)      h->st.call = val;
    else if (idx == h->cind.callsetup) h->st.call_setup = val;
    else if (idx == h->cind.callheld)  h->st.call_held = val;
    else if (idx == h->cind.signal)    h->st.signal = val;
    else if (idx == h->cind.battchg)   h->st.battery = val;
    else idx = 0;
    pthread_mutex_unlock(&h->state_lock);
    if (idx == 0)
        return;

    if (idx == h->cind.service)        emit(h, EV_SERVICE, (uint8_t)val, 0, NULL);
    else if (idx == h->cind.call)      emit(h, EV_CALL, (uint8_t)val, 0, NULL);
    else if (idx == h->cind.callsetup) emit(h, EV_CALL_SETUP, (uint8_t)val, 0, NULL);
    else if (idx == h->cind.callheld)  emit(h, EV_CALL_HELD, (uint8_t)val, 0, NULL);
    else if (idx == h->cind.signal)    emit(h, EV_SIGNAL, (uint8_t)val, 0, NULL);
    else if (idx == h->cind.battchg)   emit(h, EV_BATTERY, (uint8_t)val, 0, NULL);
}

/* +CIND: 1,0,0,0,4,4,0  -- the current value of every indicator, in order. */
static void parse_cind_read(hfp_t *h, const char *line)
{
    const char *p = after_colon(line);
    if (!p)
        return;
    int idx = 0;
    while (*p) {
        while (*p == ' ')
            p++;
        if (*p < '0' || *p > '9')
            break;
        int val = 0;
        while (*p >= '0' && *p <= '9')
            val = val * 10 + (*p++ - '0');
        apply_indicator(h, ++idx, val);
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
}

/* +CLIP: "+390512345678",145  /  +CCWA: "+39...",145 (call waiting) */
static void parse_clip(hfp_t *h, const char *line, int waiting)
{
    const char *p = after_colon(line);
    if (!p)
        return;
    char num[64];
    first_field(p, num, sizeof(num));
    if (!num[0])
        return;
    log_info("hfp", "Caller%s: %s", waiting ? " (waiting)" : "", num);
    emit(h, EV_CLIP, waiting ? 1 : 0, 0, num);
}

/* +COPS: 0,0,"Operator" */
static void parse_cops(hfp_t *h, const char *line)
{
    const char *p = after_colon(line);
    if (!p)
        return;
    const char *q = strchr(p, '"');
    if (!q)
        return;
    q++;
    const char *end = strchr(q, '"');
    if (!end || end == q)
        return;
    char name[32];
    size_t n = (size_t)(end - q);
    if (n > sizeof(name) - 1)
        n = sizeof(name) - 1;
    memcpy(name, q, n);
    name[n] = '\0';

    pthread_mutex_lock(&h->state_lock);
    snprintf(h->st.operator_name, sizeof(h->st.operator_name), "%s", name);
    pthread_mutex_unlock(&h->state_lock);
    emit(h, EV_OPER, 0, 0, name);
}

/* ----------------------------------------------------- unsolicited input */

static void handle_line(hfp_t *h, const char *line)
{
    log_debug("hfp", "[AT] << %s", line);

    if (!strncmp(line, "+BRSF:", 6)) {
        const char *p = after_colon(line);
        if (p)
            h->ag_features = (unsigned)strtoul(p, NULL, 10);
        return;
    }
    if (!strncmp(line, "+CIND:", 6)) {
        /* The test response contains '(' ; the read response is bare numbers. */
        if (strchr(line, '('))
            parse_cind_test(h, line);
        else
            parse_cind_read(h, line);
        return;
    }
    if (!strncmp(line, "+CIEV:", 6)) {
        int idx = 0, val = 0;
        if (sscanf(line + 6, " %d , %d", &idx, &val) == 2)
            apply_indicator(h, idx, val);
        return;
    }
    if (!strncmp(line, "+BCS:", 5)) {
        /* Codec negotiation. Only CVSD is supported: the SCO socket is read
         * as raw PCM, while mSBC would arrive as a transparent encoded stream
         * (§12.2). */
        int codec = atoi(after_colon(line) ? after_colon(line) : "1");
        if (codec != SCO_CODEC_CVSD)
            log_info("hfp", "AG proposed codec %d; selecting CVSD", codec);
        at_command(h, "AT+BCS=1");
        return;
    }
    if (!strcmp(line, "RING")) {
        emit(h, EV_RING, 0, 0, NULL);
        return;
    }
    if (!strncmp(line, "+CLIP:", 6)) {
        parse_clip(h, line, 0);
        return;
    }
    if (!strncmp(line, "+CCWA:", 6)) {
        parse_clip(h, line, 1);
        return;
    }
    if (!strncmp(line, "+VGS:", 5) || !strncmp(line, "+VGM:", 5)) {
        int spk = line[3] == 'S';
        const char *p = after_colon(line);
        int level = p ? atoi(p) : 0;
        pthread_mutex_lock(&h->state_lock);
        if (spk)
            h->st.vol_spk = level;
        else
            h->st.vol_mic = level;
        pthread_mutex_unlock(&h->state_lock);
        emit(h, EV_VOLUME, spk ? 0 : 1, (uint16_t)level, NULL);
        return;
    }
    if (!strncmp(line, "+BVRA:", 6)) {
        const char *p = after_colon(line);
        emit(h, EV_VR, p && atoi(p) ? 1 : 0, 0, NULL);
        return;
    }
    if (!strncmp(line, "+COPS:", 6)) {
        parse_cops(h, line);
        return;
    }
    if (!strncmp(line, "NO CARRIER", 10)) {
        /* Some phones report a hang-up only this way, without a call
         * indicator change; update the indicators accordingly. */
        pthread_mutex_lock(&h->state_lock);
        int had = h->st.call || h->st.call_setup;
        h->st.call = 0;
        h->st.call_setup = 0;
        pthread_mutex_unlock(&h->state_lock);
        if (had) {
            emit(h, EV_CALL_SETUP, 0, 0, NULL);
            emit(h, EV_CALL, 0, 0, NULL);
        }
        return;
    }
}

/* ------------------------------------------------------- SLC handshake */

/* Run the service level connection handshake. Returns 0 once the SLC is
 * established, or -1 if the link dropped or the phone did not report the
 * call indicators. */
static int slc_handshake(hfp_t *h)
{
    char cmd[64];

    memset(&h->cind, 0, sizeof(h->cind));
    snprintf(cmd, sizeof(cmd), "AT+BRSF=%d", HF_FEATURES);
    at_exchange(h, cmd);

    /* Announce CVSD only (§12.2). */
    if (h->ag_features & AG_FEAT_CODEC_NEG)
        at_exchange(h, "AT+BAC=1");

    at_exchange(h, "AT+CIND=?");
    at_exchange(h, "AT+CIND?");
    at_exchange(h, "AT+CMER=3,0,0,1");

    if (h->ag_features & AG_FEAT_3WAY)
        at_exchange(h, "AT+CHLD=?");

    if (get_fd(h) < 0 || !h->cind.call || !h->cind.callsetup)
        return -1;

    /* SLC is established from here on; the rest is optional configuration. */
    pthread_mutex_lock(&h->state_lock);
    h->st.slc_up = 1;
    char addr[20];
    snprintf(addr, sizeof(addr), "%s", h->st.address);
    pthread_mutex_unlock(&h->state_lock);

    at_exchange(h, "AT+CMEE=1");
    at_exchange(h, "AT+CLIP=1");
    at_exchange(h, "AT+CCWA=1");
    at_exchange(h, "AT+COPS=3,0");
    at_exchange(h, "AT+COPS?");

    log_info("hfp", "SLC established with %s (AG features 0x%x)",
             addr[0] ? addr : "phone", h->ag_features);
    emit(h, EV_SLC_UP, 0, 0, addr[0] ? addr : NULL);
    return 0;
}

static void close_fd(hfp_t *h)
{
    pthread_mutex_lock(&h->write_lock);
    int fd = h->fd;
    h->fd = -1;
    pthread_mutex_unlock(&h->write_lock);
    if (fd >= 0)
        close(fd);
}

/* Shut the link down; the AT thread notices and cleans up. */
static void shutdown_fd(hfp_t *h)
{
    pthread_mutex_lock(&h->write_lock);
    if (h->fd >= 0)
        shutdown(h->fd, SHUT_RDWR);
    pthread_mutex_unlock(&h->write_lock);
}

static void *at_loop(void *arg)
{
    hfp_t *h = arg;
    h->buf_len = 0;

    char addr[20];
    pthread_mutex_lock(&h->state_lock);
    h->st.connected = 1;
    snprintf(addr, sizeof(addr), "%s", h->st.address);
    pthread_mutex_unlock(&h->state_lock);
    emit(h, EV_BT_CONNECTED, 0, 0, addr[0] ? addr : NULL);

    if (slc_handshake(h) != 0) {
        log_warn("hfp", "service level connection with %s failed",
                 addr[0] ? addr : "phone");
        emit(h, EV_ERROR, 0, ERR_HFP, "HFP service level connection failed");
        /* Close the link, so that the next connection or auto-reconnect
         * starts a clean handshake. */
        close_fd(h);
    }

    char line[512];
    while (__atomic_load_n(&h->running, __ATOMIC_ACQUIRE) && get_fd(h) >= 0) {
        int r = at_read_line(h, 1000, line, sizeof(line));
        if (r < 0)
            break;
        if (r == 0)
            continue;
        handle_line(h, line);
    }
    close_fd(h);

    pthread_mutex_lock(&h->state_lock);
    h->st.connected = 0;
    h->st.slc_up = 0;
    h->st.call = h->st.call_setup = h->st.call_held = 0;
    pthread_mutex_unlock(&h->state_lock);

    log_info("hfp", "Phone link closed");
    emit(h, EV_BT_DISCONNECTED, 0, 0, addr[0] ? addr : NULL);
    return NULL;
}

void hfp_request_disconnect(hfp_t *h)
{
    if (!hfp_is_connected(h))
        return;
    log_info("hfp", "Disconnection requested: closing the phone link");
    /* shutdown(), not close(): the AT thread owns the descriptor and closes
     * it once its read fails. */
    shutdown_fd(h);
}

/* Join a finished AT thread so its stack is released and `thread` can be
 * reused. Caller holds conn_lock. The AT thread clears `fd` just before it
 * exits, so a thread whose link is closed is finished or about to be. */
static void reap_locked(hfp_t *h)
{
    if (h->thread_started && get_fd(h) < 0) {
        pthread_join(h->thread, NULL);
        h->thread_started = 0;
    }
}

/* Take ownership of a connected RFCOMM socket and start the AT thread.
 * Returns 1 if installed, 0 if a link is already up (the socket is then
 * closed). Incoming (BlueZ) and outgoing (reconnect, CMD CONNECT) links
 * race for the same slot; conn_lock decides. */
static int install(hfp_t *h, int fd, const char *address)
{
    pthread_mutex_lock(&h->conn_lock);
    reap_locked(h);
    if (h->thread_started || get_fd(h) >= 0) {
        pthread_mutex_unlock(&h->conn_lock);
        close(fd);
        return 0;
    }

    /* A write on a link that died silently must not hold write_lock for
     * ever. */
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    pthread_mutex_lock(&h->state_lock);
    snprintf(h->st.address, sizeof(h->st.address), "%s", address ? address : "");
    pthread_mutex_unlock(&h->state_lock);

    pthread_mutex_lock(&h->write_lock);
    h->fd = fd;
    pthread_mutex_unlock(&h->write_lock);

    __atomic_store_n(&h->running, 1, __ATOMIC_RELEASE);
    if (pthread_create(&h->thread, NULL, at_loop, h) == 0) {
        h->thread_started = 1;
    } else {
        log_error("hfp", "cannot start the AT thread");
        close_fd(h);
    }
    int ok = h->thread_started;
    pthread_mutex_unlock(&h->conn_lock);
    return ok;
}

/* -------------------------------------------------------- connection set-up */

void hfp_new_connection(hfp_t *h, int fd, const char *device_path)
{
    char *addr = store_address_from_path(device_path);
    const char *who = addr ? addr : (device_path ? device_path : "");
    if (install(h, fd, who)) {
        log_info("hfp", "Phone connected: %s", addr ? addr : "?");
        if (addr)
            store_save_last_device(addr);
    } else {
        log_info("hfp", "Phone link already up: second connection from %s "
                 "refused", addr ? addr : "?");
    }
    free(addr);
}

/* Find the RFCOMM channel the phone publishes for the HFP Audio Gateway. */
static int find_ag_channel(const char *address)
{
    if (!store_is_safe_token(address))
        return -1;
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "sdptool browse --uuid 0x111F %s 2>/dev/null", address);
    FILE *p = popen(cmd, "r");
    if (!p) {
        log_warn("hfp", "sdptool not available");
        return -1;
    }
    int channel = -1;
    char line[256];
    while (fgets(line, sizeof(line), p)) {
        char *c = strstr(line, "Channel:");
        if (c && sscanf(c + 8, "%d", &channel) == 1)
            break;
        channel = -1;
    }
    pclose(p);
    return channel;
}

int hfp_connect(hfp_t *h, const char *address)
{
    if (hfp_is_connected(h))
        return 1;
    if (!address || !*address)
        return 0;

    int channel = find_ag_channel(address);
    if (channel <= 0) {
        log_info("hfp", "No HFP AG service found on %s", address);
        return 0;
    }

    int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (fd < 0) {
        log_info("hfp", "cannot create RFCOMM socket: %s", strerror(errno));
        return 0;
    }
    struct sockaddr_rc addr;
    memset(&addr, 0, sizeof(addr));
    addr.rc_family = AF_BLUETOOTH;
    str2ba(address, &addr.rc_bdaddr);
    addr.rc_channel = (uint8_t)channel;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_info("hfp", "RFCOMM to %s channel %d failed: %s",
                 address, channel, strerror(errno));
        close(fd);
        return 0;
    }
    if (!install(h, fd, address)) {
        /* The phone connected by itself meanwhile: that link is kept. */
        return 1;
    }
    store_save_last_device(address);
    log_info("hfp", "Connected to %s (RFCOMM channel %d)", address, channel);
    return 1;
}

void hfp_disconnect(hfp_t *h)
{
    if (hfp_is_connected(h)) {
        log_info("hfp", "Disconnecting the phone");
        shutdown_fd(h);
    }
}

/* ------------------------------------------------------------- commands */

int hfp_answer(hfp_t *h)
{
    log_info("hfp", "Answering (ATA)");
    return at_command(h, "ATA");
}

int hfp_hangup(hfp_t *h)
{
    log_info("hfp", "Hanging up (AT+CHUP)");
    return at_command(h, "AT+CHUP");
}

int hfp_dial(hfp_t *h, const char *number)
{
    if (!number || !*number)
        return -1;
    /* Keep only what a dial string may contain, so the AT line cannot be
     * broken by a stray CR from the client. */
    char clean[40];
    size_t j = 0;
    for (const char *p = number; *p && j < sizeof(clean) - 1; p++) {
        if ((*p >= '0' && *p <= '9') || strchr("+*#ABCDpw,", *p))
            clean[j++] = *p;
    }
    clean[j] = '\0';
    if (!j)
        return -1;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "ATD%s;", clean);
    log_info("hfp", "Dialling %s", clean);
    return at_command(h, cmd);
}

int hfp_redial(hfp_t *h)
{
    log_info("hfp", "Redialling (AT+BLDN)");
    return at_command(h, "AT+BLDN");
}

int hfp_audio_connect(hfp_t *h)
{
    log_info("hfp", "Requesting the audio channel (AT+BCC)");
    return at_command(h, "AT+BCC");
}

int hfp_set_volume(hfp_t *h, int speaker, int level)
{
    if (level < 0 || level > 15)
        return -1;
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+VG%c=%d", speaker ? 'S' : 'M', level);
    return at_command(h, cmd);
}

int hfp_voice_recognition(hfp_t *h, int on)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+BVRA=%d", on ? 1 : 0);
    return at_command(h, cmd);
}

void hfp_destroy(hfp_t *h)
{
    if (!h)
        return;
    __atomic_store_n(&h->running, 0, __ATOMIC_RELEASE);
    shutdown_fd(h);
    pthread_mutex_lock(&h->conn_lock);
    if (h->thread_started) {
        pthread_join(h->thread, NULL);
        h->thread_started = 0;
    }
    pthread_mutex_unlock(&h->conn_lock);
    close_fd(h);
    pthread_mutex_destroy(&h->conn_lock);
    pthread_mutex_destroy(&h->write_lock);
    pthread_mutex_destroy(&h->state_lock);
    free(h);
}
