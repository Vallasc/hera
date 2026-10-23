#include "netd.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "util.h"

#define TAG "netd"

#define RUN_DIR       "/run/hera"
#define HOSTAPD_CONF  RUN_DIR "/hostapd.conf"
#define DNSMASQ_CONF  RUN_DIR "/dnsmasq.conf"
#define WPA_CONF      RUN_DIR "/wpa_supplicant.conf"
#define STA_CRED      "/var/lib/hera/station"
#define MODE_FILE     "/var/lib/hera/net_mode"
#define STARTS_FILE   "/var/lib/hera/failed_starts"
#define HOSTAPD_LOG   RUN_DIR "/hostapd.log"

/* AP watchdog: interval between checks, and restart limit. Past the limit the
 * watchdog waits WATCH_BACKOFF_S before trying again, so that a radio that
 * cannot hold an AP is not restarted in a tight loop. */
#define WATCH_INTERVAL_S  5
#define WATCH_RESTARTS    3
#define WATCH_WINDOW_S    120
#define WATCH_BACKOFF_S   60

/* §2.5: three starts within five minutes without reaching `up` force AP. */
#define RECOVERY_WINDOW_S 300
#define RECOVERY_STARTS   3
#define RECOVERY_MAX      16

/* Serialises the operations that change the network. They are requested
 * both over GATT (NET_CFG) and over the control channel (CMD NET_MODE). */
static pthread_mutex_t op_lock = PTHREAD_MUTEX_INITIALIZER;

/* True in `none` mode, where the network configuration is left alone. */
static int hands_off(void)
{
    return config_net_mode() == NET_MODE_NONE;
}

/* IEEE 802.11 allows up to 63 characters. Generated passphrases have 16
 * characters from a 31-symbol alphabet (~79 bits). */
#define AP_PASS_MAX   64
#define AP_PASS_GEN   16

struct netd {
    netd_watch_fn watch;
    void         *ud;

    /* Written by the thread running a network operation (under op_lock),
     * read by the GATT, link and relay threads: every access goes through
     * `state_lock`, and readers get a copy. */
    pthread_mutex_t state_lock;
    net_state_t   state;
    char          ap_passphrase[AP_PASS_MAX];
    uint8_t       token[HERA_TOKEN_LEN];

    pid_t         hostapd;
    pid_t         dnsmasq;
    pid_t         wpa;

    pthread_t       watcher;
    int             watching;
    int             stopping;
    pthread_mutex_t watch_lock;
    pthread_cond_t  watch_cond;
};

/* ------------------------------------------------------------- utilities --- */

static void state_get(netd_t *n, net_state_t *out)
{
    pthread_mutex_lock(&n->state_lock);
    *out = n->state;
    pthread_mutex_unlock(&n->state_lock);
}

static void set_ssid(netd_t *n, const char *ssid)
{
    pthread_mutex_lock(&n->state_lock);
    snprintf(n->state.ssid, sizeof n->state.ssid, "%s", ssid);
    pthread_mutex_unlock(&n->state_lock);
}

static void set_ipv4(netd_t *n, const uint8_t ipv4[4])
{
    pthread_mutex_lock(&n->state_lock);
    memcpy(n->state.ipv4, ipv4, sizeof n->state.ipv4);
    pthread_mutex_unlock(&n->state_lock);
}

static void set_mode(netd_t *n, uint8_t mode)
{
    pthread_mutex_lock(&n->state_lock);
    n->state.mode = mode;
    pthread_mutex_unlock(&n->state_lock);
}

/* Report the state to the watcher, as a copy. */
static void publish(netd_t *n)
{
    if (!n->watch)
        return;
    net_state_t copy;
    state_get(n, &copy);
    n->watch(n->ud, &copy);
}

static void set_phase(netd_t *n, net_phase_t phase, uint8_t failure)
{
    pthread_mutex_lock(&n->state_lock);
    n->state.phase = phase;
    n->state.failure = failure;
    if (phase != NET_UP)
        memset(n->state.ipv4, 0, sizeof n->state.ipv4);
    pthread_mutex_unlock(&n->state_lock);
    if (phase == NET_UP)
        unlink(STARTS_FILE);    /* reached `up`: reset the §2.5 count */
    publish(n);
}

/* In a forked child: restore the default signal mask. The service blocks
 * SIGINT and SIGTERM in every thread (main()), and a child would inherit
 * that: hostapd, dnsmasq and wpa_supplicant would then ignore the SIGTERM
 * that reap() sends. Async-signal-safe. */
static void child_unblock_signals(void)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

/* Run a program and return its exit status.
 *
 * Uses execv(), never a shell. SSIDs and passphrases received over the protocol
 * are only ever written to mode-0600 configuration files (§11.6). */
static int run(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        child_unblock_signals();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            if (g_config.log_level < 3)
                dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO)
                close(null);
        }
        execv(path, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Spawn a daemon and keep its pid. */
static pid_t spawn(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        child_unblock_signals();
        setsid();
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            if (g_config.log_level < 3)
                dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO)
                close(null);
        }
        execv(path, argv);
        _exit(127);
    }
    return pid;
}

static void reap(pid_t *pid)
{
    if (*pid <= 0)
        return;
    kill(*pid, SIGTERM);
    for (int i = 0; i < 30; i++) {
        if (waitpid(*pid, NULL, WNOHANG) == *pid) {
            *pid = 0;
            return;
        }
        sleep_ms(100);
    }
    kill(*pid, SIGKILL);
    waitpid(*pid, NULL, 0);
    *pid = 0;
}

/* Write a file with mode 0600, creating its directory. */
static int write_private(const char *path, const char *contents)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0700);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        log_error(TAG, "cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(contents);
    int ok = write(fd, contents, len) == (ssize_t)len;
    close(fd);
    return ok ? 0 : -1;
}

/* The address without its prefix length, and the prefix itself. */
static void split_cidr(const char *cidr, char *addr, size_t addrsz, int *prefix)
{
    snprintf(addr, addrsz, "%s", cidr);
    char *slash = strchr(addr, '/');
    *prefix = 24;
    if (slash) {
        *slash = '\0';
        *prefix = atoi(slash + 1);
    }
}

/* Last two bytes of the interface MAC, for the SSID suffix (§2.1). */
static void iface_suffix(const char *iface, char *out, size_t outsz)
{
    char path[128];
    snprintf(path, sizeof path, "/sys/class/net/%s/address", iface);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, outsz, "0000");
        return;
    }
    char mac[32] = { 0 };
    if (!fgets(mac, sizeof mac, f)) {
        fclose(f);
        snprintf(out, outsz, "0000");
        return;
    }
    fclose(f);
    /* "aa:bb:cc:dd:ee:ff" -> "eeff" */
    size_t len = strlen(mac);
    while (len && isspace((unsigned char)mac[len - 1]))
        mac[--len] = '\0';
    if (len >= 17)
        snprintf(out, outsz, "%c%c%c%c", mac[12], mac[13], mac[15], mac[16]);
    else
        snprintf(out, outsz, "0000");
    for (char *p = out; *p; p++)
        *p = (char)toupper((unsigned char)*p);
}

/* Generate an AP passphrase. The alphabet omits characters that are easily
 * confused when read or typed (0/o, 1/l/i). */
static void make_passphrase(char *out, size_t len)
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t raw[32];
    if (random_bytes(raw, sizeof raw) != 0) {
        for (size_t i = 0; i < sizeof raw; i++)
            raw[i] = (uint8_t)rand_u32();
    }
    size_t n = len - 1 < sizeof raw ? len - 1 : sizeof raw;
    for (size_t i = 0; i < n; i++)
        out[i] = alphabet[raw[i] % (sizeof alphabet - 1)];
    out[n] = '\0';
}

/* ------------------------------------------------------ mode and recovery --- */

/* Persist the mode selected by the client (§2.5). */
static void save_mode(int mode)
{
    write_private(MODE_FILE, mode == NET_MODE_STA ? "sta\n" : "ap\n");
}

/* The mode to start in: the one persisted by the last NET_CFG or NET_MODE
 * request if any, otherwise HERA_NET_MODE. */
static int startup_mode(void)
{
    int mode = config_net_mode();
    FILE *f = fopen(MODE_FILE, "r");
    if (!f)
        return mode;
    char buf[8] = "";
    if (fgets(buf, sizeof buf, f)) {
        if (strncmp(buf, "sta", 3) == 0)
            mode = NET_MODE_STA;
        else if (strncmp(buf, "ap", 2) == 0)
            mode = NET_MODE_AP;
    }
    fclose(f);
    log_info(TAG, "network mode: %s (persisted)",
             mode == NET_MODE_STA ? "sta" : "ap");
    return mode;
}

/* Record this start and report whether the §2.5 recovery rule applies.
 * Times are wall-clock seconds, so that restarts across reboots count. The
 * record is deleted once the network reaches `up` (set_phase). */
static int record_start(void)
{
    int64_t starts[RECOVERY_MAX];
    size_t count = 0;
    FILE *f = fopen(STARTS_FILE, "r");
    if (f) {
        long long v;
        while (count < RECOVERY_MAX && fscanf(f, "%lld", &v) == 1)
            starts[count++] = (int64_t)v;
        fclose(f);
    }

    int64_t now = (int64_t)time(NULL);
    int force = recovery_forces_ap(starts, count, now, RECOVERY_WINDOW_S,
                                   RECOVERY_STARTS);

    /* Keep the recent entries, oldest first, and append this start. */
    char buf[RECOVERY_MAX * 24];
    size_t off = 0;
    size_t first = count >= RECOVERY_MAX ? 1 : 0;
    for (size_t i = first; i < count; i++) {
        if (starts[i] <= now && now - starts[i] <= RECOVERY_WINDOW_S)
            off += (size_t)snprintf(buf + off, sizeof buf - off, "%lld\n",
                                    (long long)starts[i]);
    }
    snprintf(buf + off, sizeof buf - off, "%lld\n", (long long)now);
    write_private(STARTS_FILE, buf);
    return force;
}

/* ------------------------------------------------------------ the token --- */

/* Load the session token, generating and persisting it on first start
 * (§11.4). */
static int load_token(netd_t *n)
{
    int fd = open(g_config.token_file, O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, n->token, HERA_TOKEN_LEN);
        close(fd);
        if (got == HERA_TOKEN_LEN)
            return 0;
        log_warn(TAG, "%s is short, generating a new token",
                 g_config.token_file);
    }
    if (random_bytes(n->token, HERA_TOKEN_LEN) != 0) {
        log_error(TAG, "no randomness available for the session token");
        return -1;
    }
    char dir[256];
    snprintf(dir, sizeof dir, "%s", g_config.token_file);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0700);
    }
    fd = open(g_config.token_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        log_error(TAG, "cannot persist the token: %s", strerror(errno));
        return -1;
    }
    int ok = write(fd, n->token, HERA_TOKEN_LEN) == HERA_TOKEN_LEN;
    close(fd);
    log_info(TAG, "generated a new session token");
    return ok ? 0 : -1;
}

/* Load the AP passphrase, persisting a generated one across restarts.
 *
 * Phones store the network (on Android as a network suggestion), so the
 * passphrase must be stable. HERA_AP_PASSPHRASE, if set, takes precedence over
 * the stored file. To change a generated passphrase, delete the file and
 * restart the service (see §3.3.1 on the removed ROTATE operation). */
static void load_ap_passphrase(netd_t *n)
{
    if (*g_config.ap_passphrase) {
        /* Configured value. It is still delivered over BLE. */
        snprintf(n->ap_passphrase, sizeof n->ap_passphrase, "%s",
                 g_config.ap_passphrase);
        log_info(TAG, "hotspot passphrase taken from the configuration");
        return;
    }

    int fd = open(g_config.ap_pass_file, O_RDONLY);
    if (fd >= 0) {
        char buf[AP_PASS_MAX];
        ssize_t got = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (got > 0) {
            buf[got] = '\0';
            char *nl = strchr(buf, '\n');
            if (nl)
                *nl = '\0';
            if (credential_ok(buf, 8, 63)) {
                snprintf(n->ap_passphrase, sizeof n->ap_passphrase, "%s", buf);
                log_info(TAG, "hotspot passphrase loaded from %s",
                         g_config.ap_pass_file);
                return;
            }
            log_warn(TAG, "%s is not a usable passphrase, generating a new one",
                     g_config.ap_pass_file);
        }
    }

    make_passphrase(n->ap_passphrase, AP_PASS_GEN + 1);
    char dir[256];
    snprintf(dir, sizeof dir, "%s", g_config.ap_pass_file);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0700);
    }
    if (write_private(g_config.ap_pass_file, n->ap_passphrase) != 0) {
        /* Not fatal, but the passphrase will change on the next start. */
        log_warn(TAG, "cannot persist the hotspot passphrase in %s: it will "
                 "change at the next restart", g_config.ap_pass_file);
    } else {
        log_info(TAG, "hotspot passphrase generated and persisted: readable "
                 "only over BLE (set HERA_AP_PASSPHRASE to fix it)");
    }
}

/* ------------------------------------------------------------- AP mode --- */

static void stop_all(netd_t *n)
{
    reap(&n->hostapd);
    reap(&n->dnsmasq);
    reap(&n->wpa);

    char *flush[] = { "/sbin/ip", "addr", "flush", "dev",
                      g_config.wifi_iface, NULL };
    run("/sbin/ip", flush);
}

/* Release the interface from NetworkManager, or hand it back.
 *
 * On Raspberry Pi OS (trixie) NetworkManager manages wlan0 and reconfigures it
 * as a station whenever another process changes it, so hostapd and NM would
 * fight over the interface. Marking it unmanaged avoids that.
 *
 * Without NetworkManager (older images using dhcpcd) nmcli fails and nothing
 * is changed. */
static void set_managed(const char *iface, int managed)
{
    char *argv[] = { "/usr/bin/nmcli", "device", "set", (char *)iface,
                     "managed", managed ? "yes" : "no", NULL };
    if (run("/usr/bin/nmcli", argv) != 0)
        return;
    log_info(TAG, "NetworkManager: %s %s", iface,
             managed ? "handed back" : "released to us");
    if (!managed) {
        /* NM tears the interface down asynchronously; give it time before
         * hostapd opens it. */
        sleep_ms(500);
    }
}

/* Unblock the radio and set the regulatory domain.
 *
 * Without a country the kernel uses the world domain "00", in which the 2.4 GHz
 * channels are no-IR and an AP cannot send beacons. Raspberry Pi OS also keeps
 * WiFi soft-blocked by rfkill until a country is set. In both cases hostapd
 * runs but no network is visible.
 *
 * Failures here are not fatal; ap_on_air() detects the result afterwards. */
static void prepare_radio(netd_t *n)
{
    set_managed(g_config.wifi_iface, 0);

    char *unblock[] = { "/usr/sbin/rfkill", "unblock", "wifi", NULL };
    if (run("/usr/sbin/rfkill", unblock) != 0) {
        char *alt[] = { "/sbin/rfkill", "unblock", "wifi", NULL };
        run("/sbin/rfkill", alt);
    }

    char *reg[] = { "/sbin/iw", "reg", "set", g_config.wifi_country, NULL };
    if (run("/sbin/iw", reg) != 0) {
        char *alt[] = { "/usr/sbin/iw", "reg", "set", g_config.wifi_country,
                        NULL };
        run("/usr/sbin/iw", alt);
    }
    log_info(TAG, "regulatory domain %s requested", g_config.wifi_country);
}

/* Return 1 if the interface is operating as an AP, 0 if not, -1 if unknown.
 * `iw dev X info` reports `type AP` only once the BSS exists, which is not
 * implied by hostapd still running. */
static int ap_on_air(netd_t *n)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "/sbin/iw dev %s info 2>/dev/null",
             g_config.wifi_iface);
    FILE *f = popen(cmd, "r");
    if (!f)
        return -1;    /* unknown */

    int is_ap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "type AP"))
            is_ap = 1;
    }
    pclose(f);
    return is_ap;
}

static int start_ap_inner(netd_t *n);

/* Start AP mode. On failure, hand the interface back to NetworkManager so that
 * the device stays reachable on its usual network (§2.5). */
static int start_ap(netd_t *n)
{
    int rc = start_ap_inner(n);
    if (rc != 0) {
        log_warn(TAG, "the access point did not come up: handing %s back to "
                 "NetworkManager so the device stays reachable",
                 g_config.wifi_iface);
        set_managed(g_config.wifi_iface, 1);
    }
    return rc;
}

static int start_ap_inner(netd_t *n)
{
    set_phase(n, NET_BRINGING_UP, NETFAIL_NONE);
    stop_all(n);
    prepare_radio(n);

    char suffix[8];
    iface_suffix(g_config.wifi_iface, suffix, sizeof suffix);
    char ssid[sizeof n->state.ssid];
    snprintf(ssid, sizeof ssid, "%s-%s", g_config.ap_ssid_prefix, suffix);
    set_ssid(n, ssid);

    char addr[32];
    int prefix;
    split_cidr(g_config.ap_addr, addr, sizeof addr, &prefix);

    /* DHCP pool: .10 to .60 in the device's /24. */
    char base[32];
    snprintf(base, sizeof base, "%s", addr);
    char *last = strrchr(base, '.');
    if (!last) {
        set_phase(n, NET_FAILED, NETFAIL_IFACE);
        return -1;
    }
    *last = '\0';

    char conf[1024];
    int written;
    /* WPA2-PSK with CCMP only. TKIP would limit the link to 802.11g rates. */
    written = snprintf(conf, sizeof conf,
             "interface=%s\n"
             "driver=nl80211\n"
             "ssid=%s\n"
             "hw_mode=g\n"
             "channel=%d\n"
             /* Required; see prepare_radio(). */
             "country_code=%s\n"
             "ieee80211d=1\n"
             "ieee80211n=1\n"
             "wmm_enabled=1\n"
             "auth_algs=1\n"
             "ignore_broadcast_ssid=0\n"
             "wpa=2\n"
             "wpa_passphrase=%s\n"
             "wpa_key_mgmt=WPA-PSK\n"
             "rsn_pairwise=CCMP\n",
             g_config.wifi_iface, ssid, g_config.ap_channel,
             g_config.wifi_country, n->ap_passphrase);
    if (written >= (int)sizeof conf) {
        log_error(TAG, "hostapd.conf truncated: SSID or passphrase too long");
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }
    if (write_private(HOSTAPD_CONF, conf) != 0) {
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }

    /* No DHCP option 3 (router) and no option 6 (DNS), so that phones keep the
     * cellular network as their default route (§2.4). */
    snprintf(conf, sizeof conf,
             "interface=%s\n"
             "bind-interfaces\n"
             "dhcp-range=%s.10,%s.60,255.255.255.0,1h\n"
             "dhcp-option=3\n"          /* no router */
             "dhcp-option=6\n"          /* no DNS */
             "no-resolv\n"
             "no-hosts\n"
             "port=0\n",                /* DNS server disabled */
             g_config.wifi_iface, base, base);
    if (write_private(DNSMASQ_CONF, conf) != 0) {
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }

    char cidr[40];
    snprintf(cidr, sizeof cidr, "%s/%d", addr, prefix);
    char *up[] = { "/sbin/ip", "addr", "add", cidr, "dev",
                   g_config.wifi_iface, NULL };
    char *link_up[] = { "/sbin/ip", "link", "set", g_config.wifi_iface, "up",
                        NULL };
    run("/sbin/ip", up);
    run("/sbin/ip", link_up);

    /* -f keeps hostapd's own messages: the watchdog copies the last lines to
     * the journal when the access point goes away. */
    unlink(HOSTAPD_LOG);
    char *hostapd_argv[] = { "/usr/sbin/hostapd", "-t", "-f", HOSTAPD_LOG,
                             HOSTAPD_CONF, NULL };
    char *dnsmasq_argv[] = { "/usr/sbin/dnsmasq", "-k", "-C", DNSMASQ_CONF,
                             NULL };
    n->hostapd = spawn("/usr/sbin/hostapd", hostapd_argv);
    n->dnsmasq = spawn("/usr/sbin/dnsmasq", dnsmasq_argv);
    if (n->hostapd <= 0 || n->dnsmasq <= 0) {
        log_error(TAG, "hostapd or dnsmasq did not start");
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }

    /* Give hostapd time to create the BSS. */
    sleep_ms(1500);
    if (waitpid(n->hostapd, NULL, WNOHANG) == n->hostapd) {
        n->hostapd = 0;
        log_error(TAG, "hostapd exited immediately: check the channel and the "
                  "regulatory domain");
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }

    /* hostapd may keep running without transmitting, e.g. when the
     * regulatory domain does not allow the channel. Do not report NET_UP in
     * that case. */
    if (ap_on_air(n) == 0) {
        log_error(TAG, "hostapd is running but %s is not an AP: the radio is "
                  "blocked or the regulatory domain (%s) does not allow "
                  "channel %d",
                  g_config.wifi_iface, g_config.wifi_country,
                  g_config.ap_channel);
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }

    uint8_t ipv4[4] = { 0 };
    sscanf(addr, "%hhu.%hhu.%hhu.%hhu", &ipv4[0], &ipv4[1], &ipv4[2],
           &ipv4[3]);
    set_mode(n, NET_MODE_AP);
    set_phase(n, NET_UP, NETFAIL_NONE);
    /* After set_phase(), which clears the address for any phase but `up`. */
    set_ipv4(n, ipv4);
    publish(n);
    log_info(TAG, "access point \"%s\" up on %s, device at %s",
             ssid, g_config.wifi_iface, addr);
    return 0;
}

/* ------------------------------------------------------------ STA mode --- */

/* Read the station credentials.
 *
 * Credentials persisted by NET_CFG JOIN_STA take precedence over
 * HERA_STA_SSID/HERA_STA_PSK. FORGET_STA deletes the file. The source used is
 * logged. */
static int read_credentials(char *ssid, size_t ssidsz, char *psk, size_t psksz)
{
    ssid[0] = psk[0] = '\0';

    FILE *f = fopen(STA_CRED, "r");
    if (f) {
        int ok = fgets(ssid, (int)ssidsz, f) && fgets(psk, (int)psksz, f);
        fclose(f);
        for (char *p = ssid; *p; p++)
            if (*p == '\n') *p = '\0';
        for (char *p = psk; *p; p++)
            if (*p == '\n') *p = '\0';
        if (ok && ssid[0]) {
            log_info(TAG, "station credentials: persisted (\"%s\")", ssid);
            return 0;
        }
        log_warn(TAG, "%s is unreadable, falling back to the configuration",
                 STA_CRED);
        ssid[0] = psk[0] = '\0';
    }

    if (*g_config.sta_ssid) {
        snprintf(ssid, ssidsz, "%s", g_config.sta_ssid);
        snprintf(psk, psksz, "%s", g_config.sta_psk);
        log_info(TAG, "station credentials: configuration (\"%s\")", ssid);
        return 0;
    }
    return -1;
}

/* Read the IPv4 address of `iface`. Returns 0, or -1 if there is none. */
static int iface_address(const char *iface, uint8_t out[4])
{
    char cmd[160];
    snprintf(cmd, sizeof cmd,
             "/sbin/ip -4 -o addr show dev %s 2>/dev/null", iface);
    FILE *f = popen(cmd, "r");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        const char *inet = strstr(line, "inet ");
        if (!inet)
            continue;
        unsigned a, b, c, d;
        if (sscanf(inet + 5, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            out[0] = (uint8_t)a; out[1] = (uint8_t)b;
            out[2] = (uint8_t)c; out[3] = (uint8_t)d;
            found = 0;
            break;
        }
    }
    pclose(f);
    return found;
}

static int start_station(netd_t *n)
{
    /* wpa_supplicant is started by this service; see set_managed(). */
    set_managed(g_config.wifi_iface, 0);

    char ssid[64], psk[128];
    if (read_credentials(ssid, sizeof ssid, psk, sizeof psk) != 0) {
        log_warn(TAG, "no saved credentials: falling back to the access point");
        set_phase(n, NET_FAILED, NETFAIL_NO_CREDENTIALS);
        return -1;
    }

    /* Validate again: the values may come from NET_CFG rather than from the
     * configuration (§11.6). */
    if (!credential_ok(ssid, 1, 32) ||
        (*psk && !credential_ok(psk, 8, 63))) {
        log_warn(TAG, "station credentials rejected: SSID or passphrase out of "
                 "range, or containing characters the configuration cannot "
                 "carry");
        set_phase(n, NET_FAILED, NETFAIL_AUTH);
        return -1;
    }

    set_phase(n, NET_BRINGING_UP, NETFAIL_NONE);
    stop_all(n);
    set_ssid(n, ssid);

    /* SSID and passphrase are written to a file, never to a command line
     * (§11.6). */
    char conf[512];
    int written = snprintf(conf, sizeof conf,
             "ctrl_interface=/run/wpa_supplicant\n"
             "network={\n"
             "\tssid=\"%s\"\n"
             "\tpsk=\"%s\"\n"
             "\tscan_ssid=1\n"
             "}\n", ssid, psk);
    if (written >= (int)sizeof conf) {
        log_error(TAG, "wpa_supplicant.conf truncated");
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }
    if (write_private(WPA_CONF, conf) != 0) {
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }

    char *link_up[] = { "/sbin/ip", "link", "set", g_config.wifi_iface, "up",
                        NULL };
    run("/sbin/ip", link_up);

    char *wpa_argv[] = { "/sbin/wpa_supplicant", "-i", g_config.wifi_iface,
                         "-c", WPA_CONF, NULL };
    n->wpa = spawn("/sbin/wpa_supplicant", wpa_argv);
    if (n->wpa <= 0) {
        set_phase(n, NET_FAILED, NETFAIL_SERVICE);
        return -1;
    }

    char *dhcp[] = { "/sbin/dhclient", "-1", g_config.wifi_iface, NULL };
    run("/sbin/dhclient", dhcp);

    /* Fall back to AP if no address is obtained in time (§2.5). */
    for (int waited = 0; waited < g_config.sta_timeout_s; waited++) {
        uint8_t ipv4[4];
        if (iface_address(g_config.wifi_iface, ipv4) == 0) {
            set_mode(n, NET_MODE_STA);
            set_phase(n, NET_UP, NETFAIL_NONE);
            set_ipv4(n, ipv4);
            publish(n);
            log_info(TAG, "joined \"%s\", device at %u.%u.%u.%u", ssid,
                     ipv4[0], ipv4[1], ipv4[2], ipv4[3]);
            log_warn(TAG, "shared-network mode: the control port is reachable "
                     "from the whole network (PROTOCOL.md §2.2)");
            return 0;
        }
        sleep_ms(1000);
    }

    log_warn(TAG, "no address within %d s: back to the access point",
             g_config.sta_timeout_s);
    set_phase(n, NET_FAILED, NETFAIL_NO_IP);
    return -1;
}

/* ---------------------------------------------------------------- public --- */

/* ------------------------------------------------------------ watchdog --- */

/* Copy the last lines of the hostapd log to the journal. */
static void log_hostapd_tail(void)
{
    FILE *f = fopen(HOSTAPD_LOG, "r");
    if (!f)
        return;
    enum { LINES = 15 };
    static char ring[LINES][256];
    int count = 0;
    while (count < 1000000 && fgets(ring[count % LINES], sizeof ring[0], f))
        count++;
    fclose(f);
    int first = count > LINES ? count - LINES : 0;
    for (int i = first; i < count; i++) {
        char *line = ring[i % LINES];
        line[strcspn(line, "\n")] = '\0';
        log_warn(TAG, "hostapd: %s", line);
    }
}

/* Return a description of what is wrong with the access point, or NULL if it
 * is on the air. Called with op_lock held. */
static const char *ap_problem(netd_t *n)
{
    if (n->hostapd > 0) {
        int status = 0;
        if (waitpid(n->hostapd, &status, WNOHANG) == n->hostapd) {
            n->hostapd = 0;
            return WIFSIGNALED(status) ? "hostapd killed by a signal"
                                       : "hostapd exited";
        }
    } else {
        return "hostapd not running";
    }
    if (ap_on_air(n) == 0)
        return "the interface is no longer an access point";
    return NULL;
}

/* Restart the access point when it stops transmitting.
 *
 * The AP is checked when it is started (start_ap_inner) but can go away
 * later: hostapd can exit, or the driver can drop the BSS. NET_STATE would
 * then still report `up`, and clients would try to join a network that does
 * not exist. */
static void *watch_main(void *arg)
{
    netd_t *n = arg;
    time_t restarts[WATCH_RESTARTS] = { 0 };
    int next = 0;
    time_t hold_until = 0;

    pthread_mutex_lock(&n->watch_lock);
    while (!n->stopping) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += WATCH_INTERVAL_S;
        pthread_cond_timedwait(&n->watch_cond, &n->watch_lock, &until);
        if (n->stopping)
            break;
        pthread_mutex_unlock(&n->watch_lock);

        /* Do not wait behind a network change in progress. */
        if (pthread_mutex_trylock(&op_lock) == 0) {
            time_t now = time(NULL);
            int restart = 0;
            net_state_t st;
            state_get(n, &st);
            if (st.mode == NET_MODE_AP && st.phase == NET_UP) {
                const char *problem = ap_problem(n);
                if (problem) {
                    log_error(TAG, "access point down: %s", problem);
                    log_hostapd_tail();
                    restart = 1;
                }
            } else if (st.mode == NET_MODE_AP && st.phase == NET_FAILED &&
                       hold_until && now >= hold_until) {
                log_info(TAG, "retrying the access point");
                hold_until = 0;
                restart = 1;
            }

            if (restart) {
                if (restarts[next] && now - restarts[next] < WATCH_WINDOW_S) {
                    log_warn(TAG, "%d restarts in %d s: next attempt in %d s",
                             WATCH_RESTARTS, WATCH_WINDOW_S, WATCH_BACKOFF_S);
                    stop_all(n);
                    set_phase(n, NET_FAILED, NETFAIL_SERVICE);
                    hold_until = now + WATCH_BACKOFF_S;
                    memset(restarts, 0, sizeof restarts);
                } else {
                    restarts[next] = now;
                    next = (next + 1) % WATCH_RESTARTS;
                    if (start_ap(n) == 0)
                        log_info(TAG, "access point restarted");
                    else
                        hold_until = now + WATCH_BACKOFF_S;
                }
            }
            pthread_mutex_unlock(&op_lock);
        }
        pthread_mutex_lock(&n->watch_lock);
    }
    pthread_mutex_unlock(&n->watch_lock);
    return NULL;
}

static void watch_start(netd_t *n)
{
    if (n->watching || hands_off())
        return;
    n->stopping = 0;
    if (pthread_create(&n->watcher, NULL, watch_main, n) == 0)
        n->watching = 1;
    else
        log_warn(TAG, "cannot start the access point watchdog");
}

static void watch_stop(netd_t *n)
{
    if (!n->watching)
        return;
    pthread_mutex_lock(&n->watch_lock);
    n->stopping = 1;
    pthread_cond_signal(&n->watch_cond);
    pthread_mutex_unlock(&n->watch_lock);
    pthread_join(n->watcher, NULL);
    n->watching = 0;
}

netd_t *netd_create(netd_watch_fn watch, void *ud)
{
    netd_t *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->watch = watch;
    n->ud = ud;
    n->state.phase = NET_DOWN;
    pthread_mutex_init(&n->state_lock, NULL);
    pthread_mutex_init(&n->watch_lock, NULL);
    pthread_cond_init(&n->watch_cond, NULL);
    mkdir(RUN_DIR, 0700);
    if (load_token(n) != 0) {
        free(n);
        return NULL;
    }
    /* Loaded once, here: SESSION reads it from the GATT thread, so it must
     * not change afterwards. */
    if (!hands_off())
        load_ap_passphrase(n);
    return n;
}

void netd_destroy(netd_t *n)
{
    if (!n)
        return;
    watch_stop(n);
    if (!hands_off()) {
        stop_all(n);
        /* Hand the interface back to NetworkManager on exit. After SIGKILL
         * this does not run; the systemd unit's ExecStopPost covers that
         * case. */
        set_managed(g_config.wifi_iface, 1);
    }
    secure_zero(n->ap_passphrase, sizeof n->ap_passphrase);
    pthread_mutex_destroy(&n->watch_lock);
    pthread_cond_destroy(&n->watch_cond);
    pthread_mutex_destroy(&n->state_lock);
    free(n);
}

void netd_halt(netd_t *n)
{
    if (n)
        watch_stop(n);
}

/* Read the interface IPv4 address with getifaddrs(), without running any
 * external program. */
static int observe_address(netd_t *n)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0)
        return -1;

    int found = 0;
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, g_config.wifi_iface) != 0)
            continue;
        uint32_t addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
        uint8_t ipv4[4];
        memcpy(ipv4, &addr, 4);
        set_ipv4(n, ipv4);
        found = 1;
        break;
    }
    freeifaddrs(list);
    return found ? 0 : -1;
}

/* Read the SSID the interface is associated with, for NET_STATE. */
static void observe_ssid(netd_t *n)
{
    set_ssid(n, "");
    char cmd[256];
    snprintf(cmd, sizeof cmd, "/sbin/iw dev %s link 2>/dev/null",
             g_config.wifi_iface);
    FILE *f = popen(cmd, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *at = strstr(line, "SSID: ");
        if (!at)
            continue;
        at += 6;
        char *nl = strpbrk(at, "\r\n");
        if (nl)
            *nl = '\0';
        set_ssid(n, at);
        break;
    }
    pclose(f);
}

/* `none` mode: no hostapd, no wpa_supplicant, no nmcli and no address
 * changes. The current network state is observed and reported as STA.
 *
 * With a single radio, AP mode drops an SSH session over the home WiFi; this
 * mode keeps it, for development. */
static int start_none(netd_t *n)
{
    set_phase(n, NET_BRINGING_UP, NETFAIL_NONE);
    set_mode(n, NET_MODE_STA);
    observe_ssid(n);

    if (observe_address(n) != 0) {
        log_error(TAG, "HERA_NET_MODE=none but %s has no IPv4 address: "
                  "nothing to listen on", g_config.wifi_iface);
        set_phase(n, NET_FAILED, NETFAIL_IFACE);
        return -1;
    }
    set_phase(n, NET_UP, NETFAIL_NONE);
    log_warn(TAG, "HERA_NET_MODE=none: the network is left exactly as it is "
             "(no hotspot, no join, no NetworkManager handover)");
    net_state_t st;
    state_get(n, &st);
    log_info(TAG, "observed %s at %u.%u.%u.%u%s%s", g_config.wifi_iface,
             st.ipv4[0], st.ipv4[1], st.ipv4[2], st.ipv4[3],
             st.ssid[0] ? " on " : "", st.ssid[0] ? st.ssid : "");
    return 0;
}

int netd_start(netd_t *n)
{
    if (config_net_mode() == NET_MODE_NONE)
        return start_none(n);

    watch_start(n);
    int mode = startup_mode();
    if (record_start() && mode == NET_MODE_STA) {
        log_warn(TAG, "%d starts in %d s without reaching `up`: starting the "
                 "access point (§2.5)", RECOVERY_STARTS, RECOVERY_WINDOW_S);
        mode = NET_MODE_AP;
    }
    pthread_mutex_lock(&op_lock);
    int rc = mode == NET_MODE_STA && start_station(n) == 0 ? 0 : start_ap(n);
    pthread_mutex_unlock(&op_lock);
    return rc;
}

void netd_stop(netd_t *n)
{
    watch_stop(n);
    if (hands_off()) {
        set_phase(n, NET_DOWN, NETFAIL_NONE);
        return;
    }
    stop_all(n);
    /* Hand the interface back so the device rejoins its usual network. */
    set_managed(g_config.wifi_iface, 1);
    set_phase(n, NET_DOWN, NETFAIL_NONE);
}

void netd_state(netd_t *n, net_state_t *out)
{
    state_get(n, out);
}

/* In `none` mode, operations that change the network are refused. The
 * unchanged state is published again so that a client waiting for a NET_STATE
 * notification does not have to wait for its own timeout. */
static int refuse_hands_off(netd_t *n, const char *op)
{
    log_warn(TAG, "%s refused: HERA_NET_MODE=none, the network is not ours "
             "to change", op);
    publish(n);
    return -1;
}

int netd_use_ap(netd_t *n)
{
    if (hands_off())
        return refuse_hands_off(n, "START_AP");
    pthread_mutex_lock(&op_lock);
    save_mode(NET_MODE_AP);
    int rc = start_ap(n);
    pthread_mutex_unlock(&op_lock);
    return rc;
}

int netd_join(netd_t *n, const char *ssid, const char *passphrase)
{
    if (hands_off())
        return refuse_hands_off(n, "JOIN_STA");
    if (!ssid || !*ssid)
        return -1;
    char contents[256];
    snprintf(contents, sizeof contents, "%s\n%s\n", ssid,
             passphrase ? passphrase : "");
    pthread_mutex_lock(&op_lock);
    int rc = write_private(STA_CRED, contents);
    secure_zero(contents, sizeof contents);
    if (rc == 0) {
        save_mode(NET_MODE_STA);
        rc = start_station(n);
        if (rc != 0)
            start_ap(n);        /* fall back to AP (§2.5) */
    }
    pthread_mutex_unlock(&op_lock);
    return rc;
}

int netd_forget_station(netd_t *n)
{
    if (hands_off())
        return refuse_hands_off(n, "FORGET_STA");
    pthread_mutex_lock(&op_lock);
    unlink(STA_CRED);
    save_mode(NET_MODE_AP);
    int rc = start_ap(n);
    pthread_mutex_unlock(&op_lock);
    return rc;
}

const char *netd_ap_passphrase(netd_t *n)
{
    net_state_t st;
    state_get(n, &st);
    return st.mode == NET_MODE_AP ? n->ap_passphrase : "";
}

const uint8_t *netd_token(netd_t *n)
{
    return n->token;
}

int8_t netd_client_rssi(netd_t *n)
{
    net_state_t st;
    state_get(n, &st);
    if (st.mode != NET_MODE_AP)
        return 0;
    /* Signal of the first associated station; 0 means unknown (§10.1). */
    char cmd[160];
    snprintf(cmd, sizeof cmd, "/sbin/iw dev %s station dump 2>/dev/null",
             g_config.wifi_iface);
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    char line[256];
    int rssi = 0;
    while (fgets(line, sizeof line, f)) {
        const char *at = strstr(line, "signal:");
        if (at && sscanf(at + 7, "%d", &rssi) == 1)
            break;
    }
    pclose(f);
    if (rssi < -127 || rssi > 0)
        return 0;
    return (int8_t)rssi;
}
