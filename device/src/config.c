#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

config_t g_config;

/* --- minimal .env loader (KEY=VALUE, #comments, optional export/quotes) --- */

static void trim(char *s, char **start, char **end)
{
    char *b = s;
    while (*b && isspace((unsigned char)*b))
        b++;
    char *e = b + strlen(b);
    while (e > b && isspace((unsigned char)e[-1]))
        e--;
    *start = b;
    *end = e;
}

static void load_dotenv(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s, *e;
        trim(line, &s, &e);
        *e = '\0';
        if (*s == '\0' || *s == '#')
            continue;
        if (strncmp(s, "export ", 7) == 0) {
            s += 7;
            while (*s && isspace((unsigned char)*s))
                s++;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key_s, *key_e, *val_s, *val_e;
        trim(s, &key_s, &key_e);
        *key_e = '\0';
        trim(eq + 1, &val_s, &val_e);
        *val_e = '\0';
        size_t vlen = strlen(val_s);
        if (vlen >= 2 && (val_s[0] == '"' || val_s[0] == '\'') &&
            val_s[vlen - 1] == val_s[0]) {
            val_s[vlen - 1] = '\0';
            val_s++;
        }
        if (*key_s && !getenv(key_s))
            setenv(key_s, val_s, 0);
    }
    fclose(f);
}

/* --- typed environment accessors --- */

static char *env_str(const char *name, const char *def)
{
    const char *v = getenv(name);
    return xstrdup(v ? v : def);
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return def;
    char *end;
    long n = strtol(v, &end, 0);   /* base 0: also accepts 0x... */
    return (*end == '\0') ? (int)n : def;
}

static bool env_flag(const char *name, bool def)
{
    const char *v = getenv(name);
    if (!v)
        return def;
    return !(strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
             strcasecmp(v, "no") == 0 || strcasecmp(v, "off") == 0);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Map a format name to its id. "auto" and unknown names map to 0 (automatic
 * selection, §6.2). */
static uint8_t parse_fmt(const char *name)
{
    if (!name || strcasecmp(name, "auto") == 0)  return 0;
    if (strcasecmp(name, "pcm16_8k") == 0)       return FMT_PCM16_8K;
    if (strcasecmp(name, "pcm16_16k") == 0)      return FMT_PCM16_16K;
    if (strcasecmp(name, "g711a") == 0)          return FMT_G711A;
    if (strcasecmp(name, "g711u") == 0)          return FMT_G711U;
    log_warn("config", "HERA_FMT=\"%s\" unknown, falling back to auto", name);
    return 0;
}

int config_net_mode(void)
{
    const char *m = g_config.net_mode;
    if (m && strcasecmp(m, "sta") == 0) return NET_MODE_STA;
    if (m && strcasecmp(m, "none") == 0) return NET_MODE_NONE;
    return NET_MODE_AP;        /* default (§2) */
}

uint8_t config_fmt(void)
{
    return parse_fmt(g_config.fmt);
}

void config_load(void)
{
    const char *env_file = getenv("HERA_ENV_FILE");
    load_dotenv(env_file ? env_file : ".env");

    /* ----- network (§2) ----- */
    g_config.net_mode        = env_str("HERA_NET_MODE", "ap");
    g_config.ctrl_port       = env_int("HERA_CTRL_PORT", HERA_CTRL_PORT);
    g_config.audio_port      = env_int("HERA_AUDIO_PORT", HERA_AUDIO_PORT);
    g_config.ap_ssid_prefix  = env_str("HERA_AP_SSID_PREFIX", "HERA");
    g_config.ap_channel      = env_int("HERA_AP_CHANNEL", 6);
    g_config.wifi_country    = env_str("HERA_WIFI_COUNTRY", "IT");
    g_config.ap_pass_file    = env_str("HERA_AP_PASS_FILE",
                                       "/var/lib/hera/ap_passphrase");
    g_config.ap_addr         = env_str("HERA_AP_ADDR", "192.168.73.1/24");
    g_config.ap_passphrase   = env_str("HERA_AP_PASSPHRASE", "");
    g_config.sta_ssid        = env_str("HERA_STA_SSID", "");
    g_config.sta_psk         = env_str("HERA_STA_PSK", "");
    g_config.sta_timeout_s   = env_int("HERA_STA_TIMEOUT_S", 30);
    g_config.wifi_iface      = env_str("HERA_WIFI_IFACE", "wlan0");

    /* ----- session (§5) ----- */
    g_config.resume_window_s = env_int("HERA_RESUME_WINDOW_S", 10);
    g_config.peer_timeout_s  = env_int("HERA_PEER_TIMEOUT_S", 5);
    g_config.on_orphan       = env_str("HERA_ON_ORPHAN", "silence");

    /* ----- audio (§6) ----- */
    g_config.fmt             = env_str("HERA_FMT", "auto");
    g_config.frame_ms        = env_int("HERA_FRAME_MS", 0);
    g_config.jitter_prefill_ms = env_int("HERA_JITTER_PREFILL_MS", 20);
    g_config.jitter_max_ms   = env_int("HERA_JITTER_MAX_MS", 100);

    /* ----- security (§11) ----- */
    g_config.token_file      = env_str("HERA_TOKEN_FILE", "/var/lib/hera/token");

    /* ----- audio engine ----- */
    g_config.audio_rt_prio   = env_int("HERA_AUDIO_RT_PRIO", 20);
    /* Default 0: each primed packet adds ~3.75 ms of permanent latency
     * towards the phone (§7.1). */
    g_config.sco_prime_pkts  = env_int("HERA_SCO_PRIME_PKTS", 0);

    /* ----- HFP ----- */
    g_config.auto_reconnect  = env_flag("HERA_AUTO_RECONNECT", true);
    g_config.reconnect_attempts = env_int("HERA_RECONNECT_ATTEMPTS", 12);
    g_config.reconnect_interval = env_int("HERA_RECONNECT_INTERVAL", 5);
    g_config.auto_answer     = env_flag("HERA_AUTO_ANSWER", false);
    g_config.auto_answer_delay_ms = env_int("HERA_AUTO_ANSWER_DELAY_MS", 0);
    g_config.last_device_file = env_str("HERA_LAST_DEVICE_FILE", "last_device");

    /* ----- BLE (§3) ----- */
    g_config.bt_alias        = env_str("HERA_BT_ALIAS", "HERA");

    g_config.agent_path      = env_str("HERA_AGENT_PATH", "/org/hera/agent");
    g_config.profile_path    = env_str("HERA_PROFILE_PATH", "/org/hera/hfp");
    g_config.gatt_path       = env_str("HERA_GATT_PATH", "/org/hera/gatt");
    g_config.adv_path        = env_str("HERA_ADV_PATH", "/org/hera/adv");

    g_config.log_level       = env_int("HERA_LOG_LEVEL", 2);

    /* Keep ports out of the privileged and ephemeral ranges. */
    g_config.ctrl_port  = clampi(g_config.ctrl_port, 1024, 60000);
    g_config.audio_port = clampi(g_config.audio_port, 1024, 60000);
    if (g_config.ctrl_port == g_config.audio_port) {
        log_warn("config", "control and audio ports are the same, moving audio "
                 "to %d", g_config.ctrl_port + 1);
        g_config.audio_port = g_config.ctrl_port + 1;
    }
    /* Invalid credentials would make hostapd or wpa_supplicant fail later;
     * reject them here with an explicit message. */
    if (*g_config.ap_passphrase && !credential_ok(g_config.ap_passphrase, 8, 63)) {
        log_warn("config", "HERA_AP_PASSPHRASE is invalid (8-63 printable "
                 "ASCII characters, no \" or \\): using a generated "
                 "passphrase");
        g_config.ap_passphrase = xstrdup("");
    }
    if (*g_config.sta_ssid && !credential_ok(g_config.sta_ssid, 1, 32)) {
        log_warn("config", "HERA_STA_SSID is invalid: ignored");
        g_config.sta_ssid = xstrdup("");
    }
    if (*g_config.sta_psk && !credential_ok(g_config.sta_psk, 8, 63)) {
        log_warn("config", "HERA_STA_PSK is invalid: ignored");
        g_config.sta_psk = xstrdup("");
    }

    /* Only the durations of §6.2 are valid; 0 means automatic. */
    if (g_config.frame_ms != 0 && g_config.frame_ms != 20 &&
        g_config.frame_ms != 10 && g_config.frame_ms != 5) {
        log_warn("config", "HERA_FRAME_MS=%d is invalid (0, 20, 10 or 5): "
                 "using automatic selection", g_config.frame_ms);
        g_config.frame_ms = 0;
    }

    g_config.ap_channel        = clampi(g_config.ap_channel, 1, 13);
    g_config.sta_timeout_s     = clampi(g_config.sta_timeout_s, 5, 300);
    g_config.resume_window_s   = clampi(g_config.resume_window_s, 0, 120);
    g_config.peer_timeout_s    = clampi(g_config.peer_timeout_s, 2, 60);
    /* Keep the floor at or below half the ceiling (§6.5). */
    g_config.jitter_max_ms     = clampi(g_config.jitter_max_ms, 40, 400);
    g_config.jitter_prefill_ms = clampi(g_config.jitter_prefill_ms, 0,
                                        g_config.jitter_max_ms / 2);
    g_config.audio_rt_prio     = clampi(g_config.audio_rt_prio, 0, 99);
    g_config.sco_prime_pkts    = clampi(g_config.sco_prime_pkts, 0, 8);
    g_config.log_level         = clampi(g_config.log_level, 0, 3);

    log_set_level(g_config.log_level);
}
