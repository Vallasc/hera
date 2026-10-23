#ifndef HERA_RELAY_H
#define HERA_RELAY_H

#include "audio.h"
#include "bluez.h"
#include "hfp.h"
#include "link.h"
#include "netd.h"

/* Glue between the HFP stack, the audio engine and the link. Performs no I/O
 * of its own:
 *
 *   HFP AT thread   -> EVENT frames                          (§8)
 *   link thread     -> CMD frames -> HFP / audio + ACK       (§9)
 *   SCO accept      -> audio_open + AUDIO_OPEN / AUDIO_FMT   (§8, §6.2)
 *   link 1 Hz tick  -> STATS                                 (§10.1)
 *
 * Entry points run on different threads; shared state is protected by a
 * mutex. */

typedef struct relay relay_t;

relay_t *relay_create(void);
void     relay_destroy(relay_t *r);
/* Stop and join the worker threads (auto-answer, NET_MODE). */
void     relay_stop_workers(relay_t *r);

/* Wire up the collaborators. `bz` may be NULL before bluez_setup has run. */
void relay_attach(relay_t *r, link_t *link, hfp_t *hfp, audio_t *audio,
                  bluez_t *bz, netd_t *netd);

/* HFP event callback (AT thread). */
void relay_on_hfp_event(void *ud, uint8_t id, uint8_t a1, uint16_t a2,
                        const char *str);

/* SCO accept callback (accept thread); takes ownership of `fd`. */
void relay_on_sco(void *ud, int fd);

/* Audio-engine callback: the phone dropped the voice channel (audio thread). */
void relay_on_sco_closed(void *ud);

/* Link callbacks (link thread). */
void relay_on_client_up(void *ud, int resumed);
void relay_on_client_down(void *ud, int expired);
void relay_on_client_suspend(void *ud);
void relay_on_cmd(void *ud, const hera_tlv_t *cmd);
void relay_on_audio(void *ud, const hera_audio_t *block);
void relay_on_audio_gap(void *ud, unsigned missing);
void relay_on_format(void *ud, uint8_t fmt, unsigned frame_ms);
void relay_on_cstats(void *ud, const hera_cstats_t *cs);
void relay_on_tick(void *ud);

/* The network state, for the NET_MODE event and for STATS. */
void relay_net_state(relay_t *r, net_state_t *out);

/* RSSI of the connected client in dBm, 0 when unknown (§10.1). */
int8_t relay_client_rssi(relay_t *r);

/* True once a client asked for a restart with CMD REBOOT (§9 0x7F). */
int  relay_reboot_requested(relay_t *r);

#endif /* HERA_RELAY_H */
