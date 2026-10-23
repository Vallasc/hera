#include "relay.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "proto.h"
#include "stats.h"
#include "store.h"
#include "util.h"

struct relay {
    link_t  *link;
    hfp_t   *hfp;
    audio_t *audio;
    bluez_t *bz;
    netd_t  *netd;

    pthread_mutex_t lock;
    int      sco_open;
    int      auto_answer;
    int      auto_answer_delay_ms;
    int      reboot;

    /* The phone hears silence while either holds: the client asked for it
     * (CMD MUTE_UL), or the session is suspended (§5.4). */
    int      client_mute;
    int      suspended;

    /* CMD NET_MODE accepted and waiting to be applied after the ACK (§9). */
    int      net_pending;
    int      net_mode;
    int      net_delay_ms;
    char     net_ssid[33];
    char     net_psk[64];

    /* Worker threads. Joinable, so that relay_stop_workers() can wait for
     * them; each waits on `wake` instead of sleeping, to stop promptly.
     * Guarded by `lock`. */
    pthread_cond_t wake;
    int       stopping;
    pthread_t answer_thread;
    int       answer_started;
    int       answer_armed;         /* an auto-answer is waiting */
    pthread_t net_thread;
    int       net_started;
    int       net_busy;             /* a NET_MODE change is running */

    uint64_t start_ms;
};

/* Wait up to `ms`, or until relay_stop_workers(). Caller holds `lock`.
 * Returns 1 if stopping. */
static int wait_locked(relay_t *r, int ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    while (!r->stopping &&
           pthread_cond_timedwait(&r->wake, &r->lock, &until) == 0) { }
    return r->stopping;
}

relay_t *relay_create(void)
{
    relay_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->wake, NULL);
    r->auto_answer = g_config.auto_answer;
    r->auto_answer_delay_ms = g_config.auto_answer_delay_ms;
    r->start_ms = now_ms();
    return r;
}

void relay_attach(relay_t *r, link_t *link, hfp_t *hfp, audio_t *audio,
                  bluez_t *bz, netd_t *netd)
{
    r->link = link;
    r->hfp = hfp;
    r->audio = audio;
    r->bz = bz;
    r->netd = netd;
}

int relay_reboot_requested(relay_t *r)
{
    return r ? __atomic_load_n(&r->reboot, __ATOMIC_ACQUIRE) : 0;
}

/* Apply the effective uplink mute. */
static void apply_mute(relay_t *r)
{
    pthread_mutex_lock(&r->lock);
    int mute = r->client_mute || r->suspended;
    pthread_mutex_unlock(&r->lock);
    audio_set_mute_ul(r->audio, mute);
}

/* Send an event if a session is up. A client that connects later receives the
 * state snapshot instead (§8.1). */
static void emit(relay_t *r, uint8_t id, uint8_t a1, uint16_t a2, const char *s)
{
    if (!link_is_up(r->link)) {
        /* No client: the event is lost. Logged, since a dropped RING means
         * the app never learns about the incoming call. */
        log_warn("relay", "event 0x%02x dropped: no client", id);
        return;
    }
    log_debug("relay", "-> event 0x%02x a1=%u a2=%u%s%s", id, a1, a2,
              s ? " " : "", s ? s : "");
    link_send_event(r->link, id, a1, a2, s);
}

/* ============================ HFP -> client ============================ */

/* Auto-answer, after the configured delay. RING repeats every few seconds
 * while the phone rings: one answer is armed per call, and it is sent only
 * if the call is still ringing (not answered on the phone, not hung up). */
static void *answer_worker(void *arg)
{
    relay_t *r = arg;
    pthread_mutex_lock(&r->lock);
    int stop = wait_locked(r, r->auto_answer_delay_ms);
    int on = r->auto_answer;
    pthread_mutex_unlock(&r->lock);

    if (!stop && on) {
        hfp_state_t st;
        hfp_get_state(r->hfp, &st);
        if (st.call_setup == 1 && !st.call)
            hfp_answer(r->hfp);
        else
            log_info("relay", "auto-answer skipped: the call is no longer "
                     "ringing");
    }
    pthread_mutex_lock(&r->lock);
    r->answer_armed = 0;
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

static void maybe_auto_answer(relay_t *r)
{
    pthread_mutex_lock(&r->lock);
    if (!r->auto_answer || r->answer_armed || r->stopping) {
        pthread_mutex_unlock(&r->lock);
        return;
    }
    if (r->auto_answer_delay_ms <= 0) {
        pthread_mutex_unlock(&r->lock);
        hfp_answer(r->hfp);
        return;
    }
    /* The previous worker has finished (answer_armed is 0): reclaim it. */
    if (r->answer_started) {
        pthread_join(r->answer_thread, NULL);
        r->answer_started = 0;
    }
    r->answer_armed = 1;
    if (pthread_create(&r->answer_thread, NULL, answer_worker, r) == 0) {
        r->answer_started = 1;
    } else {
        r->answer_armed = 0;
        log_error("relay", "cannot start the auto-answer worker");
    }
    pthread_mutex_unlock(&r->lock);
}

void relay_on_hfp_event(void *ud, uint8_t id, uint8_t a1, uint16_t a2,
                        const char *str)
{
    relay_t *r = ud;
    emit(r, id, a1, a2, str);

    if (id == EV_RING)
        maybe_auto_answer(r);
}

/* ============================ SCO -> client ============================ */

/* Announce the audio channel: AUDIO_OPEN describes the SCO link (codec and
 * rate), AUDIO_FMT the format used on the wire (§8). */
static void announce_audio_open(relay_t *r)
{
    /* New audio stream: `ts` restarts at 0 and the epoch advances (§6.3). */
    link_new_audio_epoch(r->link);
    link_set_sco_rate(r->link, SCO_RATE);

    uint8_t fmt = link_format(r->link);
    unsigned frame_ms = link_frame_ms(r->link);
    audio_set_format(r->audio, fmt, frame_ms);
    emit(r, EV_AUDIO_OPEN, SCO_CODEC_CVSD, SCO_RATE / 100, NULL);
    emit(r, EV_AUDIO_FMT, fmt,
         (uint16_t)(frame_ms | (link_epoch(r->link) << 8)), NULL);
}

void relay_on_sco(void *ud, int fd)
{
    relay_t *r = ud;

    if (audio_open(r->audio, fd) != 0) {
        emit(r, EV_ERROR, 0, ERR_SCO, "cannot start the audio bridge");
        return;
    }
    pthread_mutex_lock(&r->lock);
    r->sco_open = 1;
    pthread_mutex_unlock(&r->lock);

    announce_audio_open(r);
}

void relay_on_sco_closed(void *ud)
{
    relay_t *r = ud;
    pthread_mutex_lock(&r->lock);
    int was_open = r->sco_open;
    r->sco_open = 0;
    pthread_mutex_unlock(&r->lock);
    if (was_open)
        emit(r, EV_AUDIO_CLOSE, 0, 0, NULL);
}

/* ============================ client -> device ============================ */

void relay_on_audio(void *ud, const hera_audio_t *block)
{
    relay_t *r = ud;
    audio_push_block(r->audio, block);
}

void relay_on_audio_gap(void *ud, unsigned missing)
{
    relay_t *r = ud;
    audio_push_gap(r->audio, missing);
}

/* The negotiated format changed (§6.2). */
void relay_on_format(void *ud, uint8_t fmt, unsigned frame_ms)
{
    relay_t *r = ud;
    audio_set_format(r->audio, fmt, frame_ms);
}

/* CSTATS from the client (§10.2). Logged so that both sides appear in the
 * device journal (§13.2). */
void relay_on_cstats(void *ud, const hera_cstats_t *cs)
{
    (void)ud;
    log_debug("relay", "client: %u down (%u lost), %u up, %u ms buffered, "
              "rtt %u ms%s", cs->dl_blocks, cs->dl_lost, cs->ul_blocks,
              cs->dl_buffered_ms, cs->rtt_ms,
              (cs->flags & 0x04) ? ", free-running" : "");
}

/* Send the state snapshot of §8.1 to a newly established session. */
static void send_snapshot(relay_t *r)
{
    hfp_state_t st;
    hfp_get_state(r->hfp, &st);

    /* READY has already been sent by the link (§5.4). The snapshot starts
     * with NET_MODE. */
    net_state_t net;
    relay_net_state(r, &net);
    link_send_event(r->link, EV_NET_MODE, net.mode, 0,
                    net.ssid[0] ? net.ssid : NULL);

    if (st.connected) {
        link_send_event(r->link, EV_BT_CONNECTED, 0, 0,
                        st.address[0] ? st.address : NULL);
        if (st.slc_up)
            link_send_event(r->link, EV_SLC_UP, 0, 0,
                            st.address[0] ? st.address : NULL);
    }
    if (st.service >= 0)
        link_send_event(r->link, EV_SERVICE, (uint8_t)st.service, 0, NULL);
    if (st.signal >= 0)
        link_send_event(r->link, EV_SIGNAL, (uint8_t)st.signal, 0, NULL);
    if (st.battery >= 0)
        link_send_event(r->link, EV_BATTERY, (uint8_t)st.battery, 0, NULL);
    if (st.operator_name[0])
        link_send_event(r->link, EV_OPER, 0, 0, st.operator_name);

    if (st.call_setup)
        link_send_event(r->link, EV_CALL_SETUP, (uint8_t)st.call_setup, 0, NULL);
    link_send_event(r->link, EV_CALL, (uint8_t)st.call, 0, NULL);
    if (st.call_held)
        link_send_event(r->link, EV_CALL_HELD, (uint8_t)st.call_held, 0, NULL);

    if (audio_is_open(r->audio))
        announce_audio_open(r);
}

/* Forward log lines to the client as LOG frames (§5.2). */
static void log_to_client(void *ud, const char *line)
{
    relay_t *r = ud;
    if (link_is_up(r->link))
        link_send_log(r->link, line);
}

void relay_on_client_up(void *ud, int resumed)
{
    relay_t *r = ud;
    /* Restore the uplink (§5.4). A resumed session keeps the client's own
     * MUTE_UL setting; a new session starts unmuted. */
    pthread_mutex_lock(&r->lock);
    r->suspended = 0;
    if (!resumed)
        r->client_mute = 0;
    pthread_mutex_unlock(&r->lock);
    apply_mute(r);

    send_snapshot(r);
    log_set_sink(log_to_client, r);
}

void relay_on_client_down(void *ud, int expired)
{
    relay_t *r = ud;
    log_set_sink(NULL, NULL);

    pthread_mutex_lock(&r->lock);
    r->suspended = 0;
    r->client_mute = 0;
    pthread_mutex_unlock(&r->lock);
    apply_mute(r);

    /* Orphan policy, applied when the resume window expires (§5.4). With
     * `silence` the call stays up and the phone hears silence, since no
     * uplink audio arrives. */
    if (!expired || strcasecmp(g_config.on_orphan, "hangup") != 0)
        return;
    hfp_state_t st;
    hfp_get_state(r->hfp, &st);
    if (st.call || st.call_setup) {
        log_info("relay", "resume window expired: hanging up (HERA_ON_ORPHAN)");
        hfp_hangup(r->hfp);
    }
}

/* ============================ commands (§9) ============================ */

static int cmd_connect(relay_t *r, const char *address)
{
    if (!store_is_safe_token(address))
        return -1;
    return hfp_connect(r->hfp, address) ? 0 : -1;
}

static int cmd_disconnect(relay_t *r)
{
    hfp_state_t st;
    hfp_get_state(r->hfp, &st);
    hfp_disconnect(r->hfp);
    if (r->bz && st.address[0])
        bluez_disconnect_device(r->bz, st.address);
    return 0;
}

/* Validate CMD NET_MODE and record it; relay_on_cmd() applies it after the
 * ACK (§9). Text for STA is "ssid\tpsk". */
static int prepare_net_mode(relay_t *r, const hera_tlv_t *c, const char *str)
{
    if (!r->netd || config_net_mode() == NET_MODE_NONE) {
        log_warn("relay", "NET_MODE refused: the network is not managed "
                 "(HERA_NET_MODE=none)");
        return -1;
    }
    pthread_mutex_lock(&r->lock);
    int busy = r->net_busy;
    pthread_mutex_unlock(&r->lock);
    if (busy) {
        log_warn("relay", "NET_MODE refused: a change is already running");
        return -1;
    }
    char ssid[33] = "", psk[64] = "";
    if (c->a1 == NET_MODE_STA) {
        const char *tab = str ? strchr(str, '\t') : NULL;
        if (!tab || (size_t)(tab - str) >= sizeof ssid ||
            strlen(tab + 1) >= sizeof psk)
            return -1;
        memcpy(ssid, str, (size_t)(tab - str));
        ssid[tab - str] = '\0';
        snprintf(psk, sizeof psk, "%s", tab + 1);
        /* Same validation as NET_CFG (§11.6). */
        int valid = credential_ok(ssid, 1, 32) &&
                    (!*psk || credential_ok(psk, 8, 63));
        if (!valid) {
            secure_zero(psk, sizeof psk);
            return -1;
        }
    } else if (c->a1 != NET_MODE_AP) {
        return -1;
    }

    pthread_mutex_lock(&r->lock);
    r->net_pending = 1;
    r->net_mode = c->a1;
    r->net_delay_ms = c->a2 ? c->a2 : 500;
    snprintf(r->net_ssid, sizeof r->net_ssid, "%s", ssid);
    snprintf(r->net_psk, sizeof r->net_psk, "%s", psk);
    pthread_mutex_unlock(&r->lock);
    secure_zero(psk, sizeof psk);
    return 0;
}

typedef struct {
    relay_t *r;
    int      mode;
    int      delay_ms;
    char     ssid[33];
    char     psk[64];
} net_change_t;

/* Apply a NET_MODE change after its delay. Runs on its own thread, since
 * bringing up a network can take tens of seconds. */
static void *net_change_worker(void *arg)
{
    net_change_t *n = arg;
    relay_t *r = n->r;
    pthread_mutex_lock(&r->lock);
    int stop = wait_locked(r, n->delay_ms);
    pthread_mutex_unlock(&r->lock);
    if (!stop) {
        int rc = n->mode == NET_MODE_AP
               ? netd_use_ap(r->netd)
               : netd_join(r->netd, n->ssid, n->psk);
        if (rc != 0) {
            log_warn("relay", "NET_MODE change failed; see NET_STATE.fail");
            /* Only reaches a client that has reconnected in the meantime. */
            emit(r, EV_ERROR, 0, ERR_NETWORK, "network mode change failed");
        }
    }
    secure_zero(n, sizeof *n);
    free(n);
    pthread_mutex_lock(&r->lock);
    r->net_busy = 0;
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

/* Returns 0 if the device accepted the command, -1 otherwise. This is the
 * local outcome reported in ACK, not the effect on the phone (§9.1). */
static int execute(relay_t *r, const hera_tlv_t *c, const char *str)
{
    switch (c->id) {
    case CMD_ANSWER:        return hfp_answer(r->hfp);
    case CMD_HANGUP:        return hfp_hangup(r->hfp);
    case CMD_DIAL:          return hfp_dial(r->hfp, str);
    case CMD_REDIAL:        return hfp_redial(r->hfp);

    case CMD_AUDIO_CONNECT: return hfp_audio_connect(r->hfp);
    case CMD_AUDIO_DISC:
        /* HFP has no AT command for this; the HF closes the SCO link. */
        audio_close(r->audio);
        relay_on_sco_closed(r);
        return 0;

    case CMD_VOL_SPK:       return hfp_set_volume(r->hfp, 1, c->a2);
    case CMD_VOL_MIC:       return hfp_set_volume(r->hfp, 0, c->a2);

    case CMD_AUTO_ANSWER: {
        pthread_mutex_lock(&r->lock);
        r->auto_answer = c->a1 ? 1 : 0;
        if (c->a2 > 0)
            r->auto_answer_delay_ms = c->a2;   /* 0 = leave it unchanged */
        int on = r->auto_answer, delay = r->auto_answer_delay_ms;
        pthread_mutex_unlock(&r->lock);
        log_info("relay", "Auto-answer %s (%d ms)", on ? "on" : "off", delay);
        return 0;
    }

    case CMD_CONNECT:       return cmd_connect(r, str);
    case CMD_DISCONNECT:    return cmd_disconnect(r);

    case CMD_VR_START:      return hfp_voice_recognition(r->hfp, 1);
    case CMD_VR_STOP:       return hfp_voice_recognition(r->hfp, 0);

    case CMD_MUTE_UL:
        pthread_mutex_lock(&r->lock);
        r->client_mute = c->a1 ? 1 : 0;
        pthread_mutex_unlock(&r->lock);
        apply_mute(r);
        return 0;

    case CMD_NET_MODE:
        return prepare_net_mode(r, c, str);

    case CMD_SET_FORMAT:
        if (link_set_format(r->link, c->a1) != 0)
            return -1;
        /* Clients detect the change from the `fmt` field of the next block
         * (§6.2); the event is informational. */
        emit(r, EV_AUDIO_FMT, c->a1,
             (uint16_t)(link_frame_ms(r->link) | (link_epoch(r->link) << 8)),
             NULL);
        return 0;

    case CMD_STATS:
        relay_on_tick(r);
        return 0;

    case CMD_LOG_LEVEL:
        log_set_sink_level(c->a1);
        log_info("relay", "Client log level set to %u", c->a1);
        return 0;

    case CMD_REBOOT:
        log_info("relay", "Restart requested by the client");
        __atomic_store_n(&r->reboot, 1, __ATOMIC_RELEASE);
        bluez_quit();           /* safe from this thread */
        return 0;

    default:
        log_info("relay", "Unknown command 0x%02x", c->id);
        return -1;
    }
}

void relay_on_cmd(void *ud, const hera_tlv_t *cmd)
{
    relay_t *r = ud;
    char str[128];
    proto_tlv_string(cmd, str, sizeof(str));

    int rc = execute(r, cmd, str[0] ? str : NULL);
    /* Logged at info level: commands are infrequent, and the log should show
     * whether a command arrived and whether it succeeded. */
    if (rc == 0)
        log_info("relay", "<- cmd 0x%02x a1=%u a2=%u%s%s: ok", cmd->id, cmd->a1,
                 cmd->a2, str[0] ? " " : "", str[0] ? str : "");
    else
        log_warn("relay", "<- cmd 0x%02x a1=%u a2=%u%s%s: failed", cmd->id, cmd->a1,
                 cmd->a2, str[0] ? " " : "", str[0] ? str : "");
    link_send_ack(r->link, cmd->id, rc == 0 ? 0 : 1);

    /* NET_MODE takes down the link the command arrived on, so the order is
     * ACK, BYE reason 4, then the change after the requested delay (§9). */
    pthread_mutex_lock(&r->lock);
    int pending = r->net_pending && !r->stopping;
    r->net_pending = 0;
    net_change_t *n = pending ? calloc(1, sizeof *n) : NULL;
    if (n) {
        n->r = r;
        n->mode = r->net_mode;
        n->delay_ms = r->net_delay_ms;
        snprintf(n->ssid, sizeof n->ssid, "%s", r->net_ssid);
        snprintf(n->psk, sizeof n->psk, "%s", r->net_psk);
    }
    secure_zero(r->net_psk, sizeof r->net_psk);
    pthread_mutex_unlock(&r->lock);
    if (!n)
        return;

    link_say_bye(r->link, BYE_RECONFIGURING,
                 n->mode == NET_MODE_AP ? "switching to hotspot"
                                        : "joining a network");
    pthread_mutex_lock(&r->lock);
    if (r->net_started) {
        /* net_busy was 0 when the command was accepted: finished. */
        pthread_join(r->net_thread, NULL);
        r->net_started = 0;
    }
    r->net_busy = 1;
    if (pthread_create(&r->net_thread, NULL, net_change_worker, n) == 0) {
        r->net_started = 1;
    } else {
        r->net_busy = 0;
        log_error("relay", "cannot start the NET_MODE worker");
        secure_zero(n, sizeof *n);
        free(n);
    }
    pthread_mutex_unlock(&r->lock);
}

/* ============================ network (§2, §3) ============================ */

void relay_net_state(relay_t *r, net_state_t *out)
{
    memset(out, 0, sizeof *out);
    if (r->netd)
        netd_state(r->netd, out);
}

int8_t relay_client_rssi(relay_t *r)
{
    return r->netd ? netd_client_rssi(r->netd) : 0;
}

/* The control channel dropped and the session is resumable (§5.4). The call
 * is kept up and the phone receives silence until the session is resumed or
 * discarded. */
void relay_on_client_suspend(void *ud)
{
    relay_t *r = ud;
    log_info("relay", "client gone, holding the call for the resume window");
    pthread_mutex_lock(&r->lock);
    r->suspended = 1;
    pthread_mutex_unlock(&r->lock);
    apply_mute(r);
}

/* ============================ statistics (§10.1) ============================ */

void relay_on_tick(void *ud)
{
    relay_t *r = ud;

    hfp_state_t hs;
    hfp_get_state(r->hfp, &hs);
    int sco = audio_is_open(r->audio);

    stats_live_t live;
    memset(&live, 0, sizeof(live));
    net_state_t net;
    relay_net_state(r, &net);

    live.ul_buffered_ms = (uint16_t)audio_buffered_ms(r->audio);
    live.ul_jitter_ms   = (uint16_t)audio_jitter_ms(r->audio);
    live.ul_target_ms   = (uint8_t)(audio_target_ms(r->audio) > 255
                                    ? 255 : audio_target_ms(r->audio));
    live.sco_codec      = sco ? SCO_CODEC_CVSD : SCO_CODEC_NONE;
    live.cpu_pct        = (uint8_t)cpu_percent_since_last();
    live.rssi           = relay_client_rssi(r);
    live.net_mode       = net.mode;
    live.fmt            = link_format(r->link);
    /* STF_PROTECTED reports whether the underlying link is protected (WPA2 in
     * AP mode). Diagnostic only; IP traffic is encrypted in every mode
     * (§10.1, §11.1). */
    live.flags = (uint8_t)((hs.slc_up ? STF_SLC : 0) |
                           (sco ? STF_SCO_OPEN : 0) |
                           (hs.call ? STF_IN_CALL : 0) |
                           (net.mode != NET_MODE_STA ? STF_PROTECTED : 0));

    uint8_t payload[HERA_STATS_LEN];
    stats_serialise(payload, (uint32_t)(now_ms() - r->start_ms), &live);
    link_send_stats(r->link, payload);
}

/* Stop and join the worker threads. A NET_MODE change in progress is
 * waited for: it cannot be interrupted half way. */
void relay_stop_workers(relay_t *r)
{
    if (!r)
        return;
    pthread_mutex_lock(&r->lock);
    r->stopping = 1;
    pthread_cond_broadcast(&r->wake);
    int answer = r->answer_started, net = r->net_started;
    r->answer_started = r->net_started = 0;
    pthread_mutex_unlock(&r->lock);
    if (answer)
        pthread_join(r->answer_thread, NULL);
    if (net)
        pthread_join(r->net_thread, NULL);
}

void relay_destroy(relay_t *r)
{
    if (!r)
        return;
    log_set_sink(NULL, NULL);
    relay_stop_workers(r);
    pthread_cond_destroy(&r->wake);
    pthread_mutex_destroy(&r->lock);
    free(r);
}
