#ifndef HERA_NETD_H
#define HERA_NETD_H

#include <stdint.h>

#include "proto.h"

/* Network modes (PROTOCOL.md §2) and transitions between them.
 *
 * AP is the default mode: the device runs hostapd and dnsmasq. STA joins an
 * existing network with wpa_supplicant and is intended for bench use (§2.2).
 * `none` is a development mode that leaves the network untouched.
 *
 * A device given wrong credentials must remain reachable (§2.5): STA falls
 * back to AP after HERA_STA_TIMEOUT_S, and BLE stays up in every mode. */

typedef struct netd netd_t;

/* §3.2 -- what NET_STATE reports. */
typedef enum {
    NET_DOWN = 0,
    NET_BRINGING_UP = 1,
    NET_UP = 2,
    NET_FAILED = 3,
} net_phase_t;

/* §3.2.1 -- failure codes. */
enum {
    NETFAIL_NONE = 0,
    NETFAIL_NO_SSID = 1,
    NETFAIL_AUTH = 2,
    NETFAIL_NO_IP = 3,
    NETFAIL_IFACE = 4,
    NETFAIL_SERVICE = 5,
    NETFAIL_NO_CREDENTIALS = 6,
};

typedef struct {
    uint8_t     mode;           /* NET_MODE_* */
    net_phase_t phase;
    uint8_t     failure;
    uint8_t     ipv4[4];        /* dotted-quad order; all zero = none */
    char        ssid[33];
} net_state_t;

/* Called on every state change, so that GATT can notify (§3.2). Runs on the
 * thread that triggered the change. */
typedef void (*netd_watch_fn)(void *ud, const net_state_t *state);

netd_t *netd_create(netd_watch_fn watch, void *ud);
void    netd_destroy(netd_t *n);

/* Bring up the network: the mode persisted by the last NET_CFG or NET_MODE
 * request, or HERA_NET_MODE if none. After three starts within five minutes
 * that did not reach `up`, AP is used regardless (§2.5). Returns 0 if the
 * network is up, or -1; a failed STA start has already fallen back to AP. */
int  netd_start(netd_t *n);

/* Tear down whatever is running. */
void netd_stop(netd_t *n);

/* Stop the background watchdog; first step of shutdown, so that it does not
 * publish to objects being destroyed. */
void netd_halt(netd_t *n);

/* The current state, for NET_STATE and STATS. */
void netd_state(netd_t *n, net_state_t *out);

/* §3.3.1 -- NET_CFG operations, also used by CMD NET_MODE. The selected mode
 * is persisted. These block until the new mode is up or has failed, and are
 * serialised with each other. */
int  netd_use_ap(netd_t *n);
int  netd_join(netd_t *n, const char *ssid, const char *passphrase);
int  netd_forget_station(netd_t *n);

/* The AP passphrase, for the SESSION characteristic (§3.4). Empty outside AP. */
const char *netd_ap_passphrase(netd_t *n);

/* The session token, loaded or generated at startup (§11.4). */
const uint8_t *netd_token(netd_t *n);

/* RSSI of the associated client in dBm, or 0 if unknown (STATS, §10.1). */
int8_t netd_client_rssi(netd_t *n);

#endif /* HERA_NETD_H */
