#ifndef HERA_HFP_H
#define HERA_HFP_H

#include <stdint.h>

/* HFP Hands-Free role: the device is the HF, the phone is the Audio Gateway.
 * This module runs the AT dialogue over RFCOMM and reports state changes as
 * protocol events (PROTOCOL.md §8).
 *
 * Calls are answered only on CMD ANSWER or by the relay's auto-answer. All
 * command functions are thread-safe. */

typedef struct hfp hfp_t;

/* State needed for the snapshot of §8.1. */
typedef struct {
    int  connected;             /* RFCOMM link to the phone is up      */
    int  slc_up;                /* SLC handshake completed             */
    char address[20];           /* phone address, "AA:BB:CC:DD:EE:FF"  */

    int  service;               /* network availability, -1 = unknown  */
    int  signal;                /* 0..5, -1 = unknown                  */
    int  battery;               /* 0..5, -1 = unknown                  */
    char operator_name[32];

    int  call;                  /* 0/1                                 */
    int  call_setup;            /* 0..3                                */
    int  call_held;
    int  vol_spk;               /* 0..15, -1 = unknown                 */
    int  vol_mic;
} hfp_state_t;

/* Emitted from the AT thread. `id`/`a1`/`a2`/`str` map straight onto §8. */
typedef void (*hfp_event_fn)(void *ud, uint8_t id, uint8_t a1, uint16_t a2,
                             const char *str);

hfp_t *hfp_create(hfp_event_fn on_event, void *ud);
void   hfp_destroy(hfp_t *h);

/* Adopt a connected RFCOMM fd handed over by BlueZ (Profile1.NewConnection). */
void hfp_new_connection(hfp_t *h, int fd, const char *device_path);

/* Open the HFP link towards the phone's AG ourselves (auto-reconnect, and
 * CMD CONNECT). Returns 1 on success. */
int  hfp_connect(hfp_t *h, const char *address);

/* Close the RFCOMM link to the phone (CMD DISCONNECT). */
void hfp_disconnect(hfp_t *h);

/* True while the RFCOMM link to the phone is open. Used by auto-reconnect. */
int  hfp_is_connected(hfp_t *h);

/* Close the phone link, e.g. on BlueZ RequestDisconnection. The AT thread
 * notices, cleans up and emits BT_DISCONNECTED. */
void hfp_request_disconnect(hfp_t *h);

/* Copy the current state under the internal lock. */
void hfp_get_state(hfp_t *h, hfp_state_t *out);

/* ----- commands (§9). Each returns 0 when the AT command was written. ----- */
int hfp_answer(hfp_t *h);
int hfp_hangup(hfp_t *h);
int hfp_dial(hfp_t *h, const char *number);
int hfp_redial(hfp_t *h);
int hfp_audio_connect(hfp_t *h);        /* AT+BCC: ask the AG for the SCO   */
int hfp_set_volume(hfp_t *h, int speaker, int level);   /* AT+VGS / AT+VGM  */
int hfp_voice_recognition(hfp_t *h, int on);            /* AT+BVRA          */

#endif /* HERA_HFP_H */
