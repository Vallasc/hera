#ifndef HERA_CONFIG_H
#define HERA_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

/* SCO side of the bridge: CVSD, 8 kHz PCM16 mono. With FMT_PCM16_8K on the
 * wire no conversion is needed (§6.2). */
#define SCO_RATE          8000
/* Sized for the longest block duration (20 ms, §6.2). */
#define SCO_FRAME_SAMPS   (SCO_RATE * PROTO_MAX_FRAME_MS / 1000)  /* 160       */
#define SCO_FRAME_BYTES   (SCO_FRAME_SAMPS * 2)                   /* 320 bytes */

typedef struct {
    /* ----- network (§2) ----- */
    char    *net_mode;        /* "ap" | "sta" | "none" (development)         */
    int      ctrl_port;       /* TCP, control channel (§4.1)                 */
    int      audio_port;      /* UDP, audio channel (§4.2)                   */
    char    *ap_ssid_prefix;  /* SSID becomes <prefix>-xxxx                  */
    int      ap_channel;
    char    *wifi_country;    /* regulatory domain, ISO 3166-1 alpha-2       */
    char    *ap_pass_file;    /* where the generated passphrase is stored    */
    char    *ap_addr;         /* CIDR, e.g. 192.168.73.1/24                  */

    /* Hotspot passphrase. If empty (the default), a passphrase is generated
     * on first start, stored in ap_pass_file and delivered over BLE (§3.4).
     * A fixed value is useful during bring-up, to join the hotspot without
     * BLE. */
    char    *ap_passphrase;

    /* Station-mode credentials. Normally provisioned over BLE (NET_CFG, §3.3)
     * and persisted; persisted credentials take precedence (see netd.c). */
    char    *sta_ssid;
    char    *sta_psk;

    int      sta_timeout_s;   /* beyond which STA falls back to AP (§2.5)    */
    char    *wifi_iface;

    /* ----- session (§5) ----- */
    int      resume_window_s; /* resume window (§5.4)                        */
    int      peer_timeout_s;  /* silence after which the peer is dead (§5.5) */
    char    *on_orphan;       /* "silence" | "hangup" (§5.4)                 */

    /* ----- audio (§6) ----- */
    char    *fmt;             /* "auto" or a format name (§6.2)              */

    /* Block duration in ms (§6.2): 0 selects the shortest one offered by the
     * client; 20, 10 or 5 forces that value if the client offered it. */
    int      frame_ms;

    int      jitter_prefill_ms;
    int      jitter_max_ms;

    /* ----- security (§11) ----- */
    char    *token_file;      /* where the 16-byte session token lives       */

    /* ----- audio engine ----- */
    int      audio_rt_prio;   /* SCHED_FIFO priority, 0 = leave scheduling   */
    int      sco_prime_pkts;  /* silent SCO packets queued at open (§7.1)    */

    /* ----- HFP ----- */
    bool     auto_reconnect;
    int      reconnect_attempts;
    int      reconnect_interval;
    bool     auto_answer;
    int      auto_answer_delay_ms;
    char    *last_device_file;

    /* ----- BLE advertising and GATT (§3) ----- */
    char    *bt_alias;        /* GAP name, also the advertised local name    */

    /* ----- D-Bus object paths (rarely changed) ----- */
    char    *agent_path;
    char    *profile_path;
    char    *gatt_path;
    char    *adv_path;

    /* ----- misc ----- */
    int      log_level;       /* 0 off, 1 error, 2 info, 3 debug (§9 0x71)   */
} config_t;

extern config_t g_config;

/* Load .env (if present) then resolve every setting from the environment. */
void config_load(void);

/* The network mode as a NET_MODE_* value. */
int config_net_mode(void);

/* The configured audio format id, or 0 for "auto" (§6.2). */
uint8_t config_fmt(void);

#endif /* HERA_CONFIG_H */
