#ifndef HERA_GATT_H
#define HERA_GATT_H

#include <stdint.h>
#include <systemd/sd-bus.h>

#include "netcfg.h"
#include "netd.h"
#include "proto.h"

/* BLE discovery and provisioning (PROTOCOL.md §3).
 *
 * Registers an LE advertisement with the HERA name and service UUID, and a GATT
 * service with three characteristics: NET_STATE (network state), NET_CFG
 * (network configuration) and SESSION (token and hotspot passphrase). All three
 * require an encrypted link, so the first access triggers pairing (§3.1).
 *
 * BLE carries no audio, call control or statistics. It stays up in every
 * network mode as the recovery path (§2.5).
 */

typedef struct gatt gatt_t;

/* Callbacks. The NET_CFG handlers run on the D-Bus thread and may take
 * seconds; the GATT write is acknowledged before they are called (§3.3). */
typedef struct {
    void *ud;
    int (*use_ap)(void *ud);
    int (*join)(void *ud, const char *ssid, const char *passphrase);
    int (*forget_station)(void *ud);
    int (*restart)(void *ud);
    /* Current network state, for NET_STATE. */
    void (*state)(void *ud, net_state_t *out);
    /* Session token and hotspot passphrase, for SESSION. */
    const uint8_t *(*token)(void *ud);
    const char *(*passphrase)(void *ud);
    /* True while a session is up or resumable (§5.4). SESSION reads are
     * refused while this holds (§3.4). */
    int (*claimed)(void *ud);
} gatt_cb_t;

gatt_t *gatt_create(sd_bus *bus, const char *adapter_path, const gatt_cb_t *cb);
int     gatt_register(gatt_t *g);

/* Push a NET_STATE notification (§3.2). Safe to call when nobody subscribed. */
void    gatt_notify_state(gatt_t *g, const net_state_t *state);

void    gatt_destroy(gatt_t *g);

#endif /* HERA_GATT_H */
