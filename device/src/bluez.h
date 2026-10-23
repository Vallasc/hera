#ifndef HERA_BLUEZ_H
#define HERA_BLUEZ_H

#include "gatt.h"
#include "hfp.h"

/* BlueZ integration over sd-bus: adapter configuration, the Just Works pairing
 * agent (§3.1), HFP Hands-Free profile registration (the RFCOMM fd arrives via
 * Profile1.NewConnection), the BLE discovery and provisioning surface (see
 * gatt.c) and auto-reconnect to the last phone.
 *
 * The sd_event loop runs on the main thread; everything time-critical lives on
 * its own thread, so a slow D-Bus round trip can never stall the audio. */

typedef struct bluez bluez_t;

bluez_t *bluez_create(void);

/* Connect to the system bus, configure the adapter, register agent, profile,
 * GATT and advertising. `gcb` carries the provisioning handlers the three
 * characteristics of §3 call into. Returns 0 on success. */
int bluez_setup(bluez_t *bz, hfp_t *hfp, const gatt_cb_t *gcb);

/* The GATT surface, for pushing NET_STATE notifications (§3.2). */
gatt_t *bluez_gatt(bluez_t *bz);

/* Adapter Bluetooth address (valid after a successful bluez_setup). */
const char *bluez_adapter_address(bluez_t *bz);

/* Ask BlueZ to drop the ACL to `address` (CMD DISCONNECT, §9 0x41). */
int bluez_disconnect_device(bluez_t *bz, const char *address);

void bluez_run(bluez_t *bz);      /* blocks in the D-Bus event loop */

/* Leave the event loop. Safe from any thread (sends SIGTERM to the
 * process). */
void bluez_stop(bluez_t *bz);
void bluez_quit(void);

/* Stop and join the reconnect supervisor. */
void bluez_stop_workers(bluez_t *bz);
void bluez_destroy(bluez_t *bz);

#endif /* HERA_BLUEZ_H */
