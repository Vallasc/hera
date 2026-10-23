/* HERA device -- entry point.
 *
 * Runs on a Raspberry Pi as a Bluetooth HFP hands-free unit and relays call
 * audio and control to a client application over IP (see PROTOCOL.md).
 *
 *      phone  --HFP/SCO-->  Pi  --TCP control + UDP audio-->  app
 *                               --BLE GATT: discovery only-->
 *
 * Run as root:  sudo -E ./hera-device
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "bluez.h"
#include "config.h"
#include "gatt.h"
#include "hfp.h"
#include "link.h"
#include "log.h"
#include "netcfg.h"
#include "netd.h"
#include "relay.h"
#include "sco.h"

#define HERA_FIRMWARE "hera-rpi 0.3.0"

/* Exit status after CMD REBOOT or NET_CFG RESTART, distinct from a clean
 * stop; systemd restarts the service in both cases. */
#define EXIT_RESTART 75

/* Context for the GATT callbacks, which run on the D-Bus thread. */
typedef struct {
    netd_t *netd;
    gatt_t *gatt;
    link_t *link;
    int    *restart;
} provisioning_t;

/* ------------------------------------------------------- GATT callbacks --- */

/* Called by netd from its own threads (watchdog, network operations), also
 * before the GATT surface exists: `gatt` is published atomically. */
static void on_net_changed(void *ud, const net_state_t *state)
{
    provisioning_t *p = ud;
    gatt_t *gatt = __atomic_load_n(&p->gatt, __ATOMIC_ACQUIRE);
    if (gatt)
        gatt_notify_state(gatt, state);
}

static int prov_use_ap(void *ud)
{
    provisioning_t *p = ud;
    /* The reconfiguration drops the IP link, so send BYE first (§5.6). */
    link_say_bye(p->link, BYE_RECONFIGURING, "switching to hotspot");
    return netd_use_ap(p->netd);
}

static int prov_join(void *ud, const char *ssid, const char *passphrase)
{
    provisioning_t *p = ud;
    link_say_bye(p->link, BYE_RECONFIGURING, "joining a network");
    return netd_join(p->netd, ssid, passphrase);
}

static int prov_forget(void *ud)
{
    provisioning_t *p = ud;
    link_say_bye(p->link, BYE_RECONFIGURING, "forgetting the network");
    return netd_forget_station(p->netd);
}

static int prov_restart(void *ud)
{
    provisioning_t *p = ud;
    link_say_bye(p->link, BYE_RECONFIGURING, "restarting");
    __atomic_store_n(p->restart, 1, __ATOMIC_RELEASE);
    bluez_quit();
    return 0;
}

static void prov_state(void *ud, net_state_t *out)
{
    provisioning_t *p = ud;
    netd_state(p->netd, out);
}

static const uint8_t *prov_token(void *ud)
{
    provisioning_t *p = ud;
    return netd_token(p->netd);
}

static const char *prov_passphrase(void *ud)
{
    provisioning_t *p = ud;
    return netd_ap_passphrase(p->netd);
}

/* True while a session is established or inside its resume window. SESSION
 * is not readable over GATT while this holds (§3.4). */
static int prov_claimed(void *ud)
{
    provisioning_t *p = ud;
    if (!p->link)
        return 0;
    return link_is_up(p->link) || link_is_suspended(p->link);
}

/* ------------------------------------------------------------------ main --- */

int main(void)
{
    if (geteuid() != 0) {
        fprintf(stderr, "Error: run as root (sudo -E ./hera-device)\n");
        return 1;
    }
    /* Ignore SIGPIPE; write errors are handled where they occur. */
    signal(SIGPIPE, SIG_IGN);

    /* Block SIGINT and SIGTERM before any thread is created: threads inherit
     * the mask, and the event loop receives both through a signalfd
     * (bluez_setup). A thread created earlier would leave them unblocked
     * there, and a SIGTERM delivered to it would kill the process without
     * the clean shutdown below. */
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    config_load();

    static int restart_requested;
    provisioning_t prov = { .restart = &restart_requested };

    /* Bring up the network first. BLE is started regardless of the outcome,
     * since it is the recovery path (§2.5). */
    netd_t *netd = netd_create(on_net_changed, &prov);
    if (!netd) {
        log_error("main", "cannot initialise the network manager");
        return 1;
    }
    prov.netd = netd;
    if (netd_start(netd) != 0)
        log_warn("main", "the network did not come up cleanly; see NET_STATE");

    relay_t *relay = relay_create();
    hfp_t *hfp = hfp_create(relay_on_hfp_event, relay);
    if (!relay || !hfp) {
        log_error("main", "allocation failed");
        return 1;
    }

    link_cb_t cb = {
        .ud           = relay,
        .on_up        = relay_on_client_up,
        .on_down      = relay_on_client_down,
        .on_suspend   = relay_on_client_suspend,
        .on_cmd       = relay_on_cmd,
        .on_audio     = relay_on_audio,
        .on_audio_gap = relay_on_audio_gap,
        .on_format    = relay_on_format,
        .on_cstats    = relay_on_cstats,
        .on_tick      = relay_on_tick,
    };
    link_t *link = link_create(&cb, HERA_FIRMWARE);
    audio_t *audio = audio_create(link, relay_on_sco_closed, relay);
    if (!link || !audio) {
        log_error("main", "cannot create the transport or the audio engine");
        return 1;
    }
    prov.link = link;

    bluez_t *bz = bluez_create();
    if (!bz) {
        log_error("main", "Bluetooth setup failed");
        return 1;
    }

    gatt_cb_t gcb = {
        .ud             = &prov,
        .use_ap         = prov_use_ap,
        .join           = prov_join,
        .forget_station = prov_forget,
        .restart        = prov_restart,
        .state          = prov_state,
        .token          = prov_token,
        .passphrase     = prov_passphrase,
        .claimed        = prov_claimed,
    };
    /* Before bluez_setup(): from there on the reconnect supervisor can bring
     * up the phone link, whose events go through the relay. */
    relay_attach(relay, link, hfp, audio, bz, netd);
    if (bluez_setup(bz, hfp, &gcb) != 0) {
        log_error("main", "Bluetooth setup failed");
        return 1;
    }
    __atomic_store_n(&prov.gatt, bluez_gatt(bz), __ATOMIC_RELEASE);

    sco_server_t *sco = sco_server_create(bluez_adapter_address(bz),
                                          relay_on_sco, relay);
    if (!sco || sco_server_start(sco) != 0) {
        log_error("main", "SCO server failed to start");
        return 1;
    }

    net_state_t net;
    netd_state(netd, &net);
    if (link_start(link, g_config.ctrl_port, g_config.audio_port,
                   netd_token(netd)) != 0) {
        log_error("main", "the transport failed to start");
        return 1;
    }

    /* In `none` mode NET_STATE reports STA; make it clear in the log that the
     * network configuration is not managed by this service. */
    const char *mode = config_net_mode() == NET_MODE_NONE
                     ? "existing network, untouched (HERA_NET_MODE=none)"
                     : net.mode == NET_MODE_STA ? "shared WiFi" : "hotspot";
    log_info("main", "====================================================");
    log_info("main", "  HERA device started (%s, protocol v%d)",
             HERA_FIRMWARE, HERA_VERSION);
    log_info("main", "  Adapter : %s (%s)", g_config.bt_alias,
             bluez_adapter_address(bz));
    log_info("main", "  Network : %s%s%s", mode,
             net.ssid[0] ? ", SSID " : "", net.ssid[0] ? net.ssid : "");
    log_info("main", "  Link    : %u.%u.%u.%u, control tcp/%d, audio udp/%d",
             net.ipv4[0], net.ipv4[1], net.ipv4[2], net.ipv4[3],
             g_config.ctrl_port, g_config.audio_port);
    log_info("main", "  Audio   : no codec, format chosen per §6.2");
    log_info("main", "  Ctrl+C to quit");
    log_info("main", "====================================================");

    /* Blocks in the D-Bus event loop until SIGINT/SIGTERM or CMD REBOOT. */
    bluez_run(bz);

    log_info("main", "Stopping...");
    int restart = __atomic_load_n(&restart_requested, __ATOMIC_ACQUIRE) ||
                  relay_reboot_requested(relay);

    /* Order: first stop everything that produces work, then free. Each
     * object is freed only once no thread can reach it any more. */
    link_stop(link);            /* no more client commands; sends no-op */
    netd_halt(netd);            /* no more AP watchdog notifications */
    bluez_stop_workers(bz);     /* no more reconnect attempts */
    relay_stop_workers(relay);  /* auto-answer and NET_MODE workers */
    sco_server_stop(sco);       /* no new audio channels */
    audio_destroy(audio);       /* joins the audio thread (uses link) */
    hfp_destroy(hfp);           /* joins the AT thread (uses relay, link) */
    bluez_destroy(bz);          /* waits for a NET_CFG worker; frees GATT */
    relay_destroy(relay);
    netd_destroy(netd);
    link_destroy(link);
    log_info("main", "HERA device stopped");
    return restart ? EXIT_RESTART : 0;
}
