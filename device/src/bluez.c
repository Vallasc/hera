#include "bluez.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
/* For struct signalfd_siginfo in on_signal(); included explicitly rather than
 * relying on sd-event.h pulling it in. */
#include <sys/signalfd.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "config.h"
#include "gatt.h"
#include "log.h"
#include "proto.h"
#include "store.h"
#include "util.h"

static const char *HFP_HF_UUID = "0000111e-0000-1000-8000-00805f9b34fb";

/* SDP record for the Hands-Free role. */
static const char *HFP_SDP_RECORD =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
    "<record>\n"
    "    <attribute id=\"0x0001\">\n"
    "        <sequence>\n"
    "            <uuid value=\"0x111E\"/>\n"
    "            <uuid value=\"0x1203\"/>\n"
    "        </sequence>\n"
    "    </attribute>\n"
    "    <attribute id=\"0x0004\">\n"
    "        <sequence>\n"
    "            <sequence>\n"
    "                <uuid value=\"0x0100\"/>\n"
    "            </sequence>\n"
    "            <sequence>\n"
    "                <uuid value=\"0x0003\"/>\n"
    "                <uint8 value=\"0x01\" name=\"channel\"/>\n"
    "            </sequence>\n"
    "        </sequence>\n"
    "    </attribute>\n"
    "    <attribute id=\"0x0005\">\n"
    "        <sequence>\n"
    "            <uuid value=\"0x1002\"/>\n"
    "        </sequence>\n"
    "    </attribute>\n"
    "    <attribute id=\"0x0009\">\n"
    "        <sequence>\n"
    "            <sequence>\n"
    "                <uuid value=\"0x111E\"/>\n"
    "                <uint16 value=\"0x0107\"/>\n"
    "            </sequence>\n"
    "        </sequence>\n"
    "    </attribute>\n"
    "    <attribute id=\"0x0100\">\n"
    "        <text value=\"HERA Hands-Free\"/>\n"
    "    </attribute>\n"
    "    <attribute id=\"0x0311\">\n"
    "        <uint16 value=\"0x0000\"/>\n"
    "    </attribute>\n"
    "</record>";

struct bluez {
    sd_bus       *bus;
    sd_event     *event;
    sd_bus_slot  *agent_slot;
    sd_bus_slot  *profile_slot;
    gatt_t       *gatt;
    hfp_t        *hfp;

    /* Reconnect supervisor: joinable, stopped by bluez_stop_workers(). */
    pthread_t       reconnect;
    int             reconnect_started;
    pthread_mutex_t worker_lock;
    pthread_cond_t  worker_wake;
    int             stopping;
    char          adapter_path[256];
    char          adapter_addr[32];
    char          hci_name[16];
};

static void run_cmd(const char *cmd)
{
    int rc = system(cmd);
    (void)rc;
}

/* ============================ agent vtable ============================ */

static int agent_ok(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    return sd_bus_reply_method_return(m, "");
}
static int on_request_pincode(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    return sd_bus_reply_method_return(m, "s", "0000");
}
static int on_request_passkey(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    return sd_bus_reply_method_return(m, "u", (uint32_t)0);
}
static int on_request_confirmation(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    /* Just Works: the device has no display or keyboard (§3.1). */
    log_info("agent", "Pairing confirmed");
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", on_request_pincode, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", on_request_passkey, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", on_request_confirmation, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ============================ profile vtable ============================ */

static int on_new_connection(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    bluez_t *bz = ud;
    const char *device = NULL;
    int fd = -1;
    int r = sd_bus_message_read(m, "oh", &device, &fd);
    if (r < 0) {
        log_error("hfp", "NewConnection: cannot read arguments");
        return r;
    }
    /* The fd belongs to the message; dup it to keep it past this call. */
    int owned = dup(fd);
    sd_bus_message_skip(m, "a{sv}");

    if (owned < 0) {
        log_error("hfp", "NewConnection: dup failed");
        return sd_bus_reply_method_return(m, "");
    }
    hfp_new_connection(bz->hfp, owned, device);
    return sd_bus_reply_method_return(m, "");
}

/* BlueZ asks the profile to drop the connection, e.g. when the device is
 * being disconnected. Acknowledging without closing would leave the AT
 * thread on a dead link and the client told the phone is still there. */
static int on_request_disconnection(sd_bus_message *m, void *ud, sd_bus_error *e)
{
    bluez_t *bz = ud;
    hfp_request_disconnect(bz->hfp);
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable profile_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("NewConnection", "oha{sv}", "", on_new_connection, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestDisconnection", "o", "", on_request_disconnection, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Release", "", "", agent_ok, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ============================ adapter discovery ============================ */

/* Within an a{sv} at the current read position, find `wanted` string prop. */
static int read_str_prop(sd_bus_message *reply, const char *wanted,
                         char *out, size_t outsz)
{
    int found = 0;
    if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0)
        return 0;
    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key = NULL;
        sd_bus_message_read(reply, "s", &key);
        if (key && strcmp(key, wanted) == 0) {
            const char *val = NULL;
            if (sd_bus_message_enter_container(reply, 'v', "s") > 0) {
                sd_bus_message_read(reply, "s", &val);
                if (val) {
                    snprintf(out, outsz, "%s", val);
                    found = 1;
                }
                sd_bus_message_exit_container(reply);
            } else {
                sd_bus_message_skip(reply, "v");
            }
        } else {
            sd_bus_message_skip(reply, "v");
        }
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);
    return found;
}

static int get_adapter(bluez_t *bz)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(bz->bus, "org.bluez", "/",
                               "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &reply, "");
    if (r < 0) {
        log_error("adapter", "GetManagedObjects failed: %s", err.message);
        sd_bus_error_free(&err);
        return -1;
    }

    int found = 0;
    sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    while (!found &&
           sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        const char *path = NULL;
        sd_bus_message_read(reply, "o", &path);
        sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface = NULL;
            sd_bus_message_read(reply, "s", &iface);
            if (iface && strcmp(iface, "org.bluez.Adapter1") == 0) {
                if (read_str_prop(reply, "Address",
                                  bz->adapter_addr, sizeof(bz->adapter_addr))) {
                    snprintf(bz->adapter_path, sizeof(bz->adapter_path), "%s", path);
                    found = 1;
                }
            } else {
                sd_bus_message_skip(reply, "a{sv}");
            }
            sd_bus_message_exit_container(reply);
            if (found)
                break;
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_exit_container(reply);
    }
    sd_bus_message_unref(reply);

    if (!found) {
        log_error("adapter", "No Bluetooth adapter found");
        return -1;
    }
    const char *slash = strrchr(bz->adapter_path, '/');
    snprintf(bz->hci_name, sizeof(bz->hci_name), "%s", slash ? slash + 1 : "hci0");
    log_info("adapter", "Adapter %s (%s)", bz->adapter_path, bz->adapter_addr);
    return 0;
}

static void configure_adapter(bluez_t *bz)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    const char *p = bz->adapter_path;
    sd_bus_set_property(bz->bus, "org.bluez", p, "org.bluez.Adapter1",
                        "Alias", &err, "s", g_config.bt_alias);
    sd_bus_set_property(bz->bus, "org.bluez", p, "org.bluez.Adapter1",
                        "Powered", &err, "b", 1);
    sd_bus_set_property(bz->bus, "org.bluez", p, "org.bluez.Adapter1",
                        "Discoverable", &err, "b", 1);
    sd_bus_set_property(bz->bus, "org.bluez", p, "org.bluez.Adapter1",
                        "DiscoverableTimeout", &err, "u", (uint32_t)0);
    sd_bus_set_property(bz->bus, "org.bluez", p, "org.bluez.Adapter1",
                        "Pairable", &err, "b", 1);
    sd_bus_set_property(bz->bus, "org.bluez", p, "org.bluez.Adapter1",
                        "PairableTimeout", &err, "u", (uint32_t)0);
    sd_bus_error_free(&err);
    log_info("adapter", "Configured as '%s' (discoverable, pairable)",
             g_config.bt_alias);
}

static void enable_sco_over_hci(void)
{
    /* §12.2: Broadcom controllers on the Pi route SCO to the PCM pins by
     * default. This vendor command (Write_SCO_PCM_Int_Param, routing = HCI)
     * routes it over HCI so that the SCO socket receives samples. Best effort:
     * some firmware revisions ignore it. */
    run_cmd("hcitool cmd 0x3F 0x1C 0x01 0x00 0x00 0x00 0x00 >/dev/null 2>&1");
    log_info("adapter", "SCO routing set to HCI (best effort)");
}

/* ============================ agent / profile ============================ */

static int register_agent(bluez_t *bz)
{
    int r = sd_bus_add_object_vtable(bz->bus, &bz->agent_slot,
                                     g_config.agent_path, "org.bluez.Agent1",
                                     agent_vtable, bz);
    if (r < 0) {
        log_error("agent", "cannot export the agent object: %s", strerror(-r));
        return r;
    }
    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(bz->bus, "org.bluez", "/org/bluez",
                           "org.bluez.AgentManager1", "RegisterAgent", &err, NULL,
                           "os", g_config.agent_path, "NoInputNoOutput");
    if (r < 0) {
        log_error("agent", "RegisterAgent failed: %s", err.message);
        sd_bus_error_free(&err);
        return r;
    }
    sd_bus_call_method(bz->bus, "org.bluez", "/org/bluez",
                       "org.bluez.AgentManager1", "RequestDefaultAgent", &err, NULL,
                       "o", g_config.agent_path);
    sd_bus_error_free(&err);
    log_info("agent", "Just Works pairing agent registered");
    return 0;
}

static int dict_open(sd_bus_message *m, const char *key)
{
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0)
        return r;
    return sd_bus_message_append(m, "s", key);
}

static int register_hfp(bluez_t *bz)
{
    int r = sd_bus_add_object_vtable(bz->bus, &bz->profile_slot,
                                     g_config.profile_path, "org.bluez.Profile1",
                                     profile_vtable, bz);
    if (r < 0) {
        log_error("hfp", "cannot export the profile object: %s", strerror(-r));
        return r;
    }

    sd_bus_message *m = NULL;
    r = sd_bus_message_new_method_call(bz->bus, &m, "org.bluez", "/org/bluez",
                                       "org.bluez.ProfileManager1", "RegisterProfile");
    if (r < 0)
        return r;
    sd_bus_message_append(m, "os", g_config.profile_path, HFP_HF_UUID);
    sd_bus_message_open_container(m, 'a', "{sv}");
    dict_open(m, "Name");                  sd_bus_message_append(m, "v", "s", "HERA HFP");       sd_bus_message_close_container(m);
    dict_open(m, "Role");                  sd_bus_message_append(m, "v", "s", "server");         sd_bus_message_close_container(m);
    dict_open(m, "RequireAuthentication"); sd_bus_message_append(m, "v", "b", 0);                 sd_bus_message_close_container(m);
    dict_open(m, "RequireAuthorization");  sd_bus_message_append(m, "v", "b", 0);                 sd_bus_message_close_container(m);
    dict_open(m, "AutoConnect");           sd_bus_message_append(m, "v", "b", 1);                 sd_bus_message_close_container(m);
    dict_open(m, "ServiceRecord");         sd_bus_message_append(m, "v", "s", HFP_SDP_RECORD);    sd_bus_message_close_container(m);
    dict_open(m, "Channel");               sd_bus_message_append(m, "v", "q", (uint16_t)1);       sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);

    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call(bz->bus, m, 0, &err, NULL);
    sd_bus_message_unref(m);
    if (r < 0) {
        log_error("hfp", "RegisterProfile failed: %s", err.message);
        sd_bus_error_free(&err);
        return r;
    }
    sd_bus_error_free(&err);
    log_info("hfp", "Hands-Free profile registered");
    return 0;
}

/* ============================ auto-reconnect ============================ */

/* Slow phase: one attempt per minute once the fast attempts are used up. */
#define RECONNECT_SLOW_S 60

/* Keeps the phone link up for the life of the service.
 *
 * Runs at start-up and after every drop: HERA_RECONNECT_ATTEMPTS attempts
 * every HERA_RECONNECT_INTERVAL seconds, then one attempt per minute until
 * the phone is back. The phone may reconnect on its own at the same time;
 * hfp_connect() then finds the link up and does nothing. */
/* Sleep up to `ms`; returns 1 if the supervisor must stop. */
static int reconnect_wait(bluez_t *bz, int ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&bz->worker_lock);
    while (!bz->stopping &&
           pthread_cond_timedwait(&bz->worker_wake, &bz->worker_lock,
                                  &until) == 0) { }
    int stop = bz->stopping;
    pthread_mutex_unlock(&bz->worker_lock);
    return stop;
}

static void *reconnect_worker(void *arg)
{
    bluez_t *bz = arg;
    int attempt = 0;
    int was_connected = 0;
    int trusted = 0;

    for (;;) {
        if (hfp_is_connected(bz->hfp)) {
            was_connected = 1;
            attempt = 0;
            if (reconnect_wait(bz, 1000))
                break;
            continue;
        }
        if (was_connected) {
            was_connected = 0;
            attempt = 0;
            log_info("adapter", "Phone link lost: reconnecting");
            /* Leave the phone a moment to reconnect by itself. */
            if (reconnect_wait(bz, 2000))
                break;
            continue;
        }

        char *address = store_load_last_device();
        if (!address) {
            if (attempt == 0)
                log_info("adapter", "Auto-reconnect: no phone seen yet");
            attempt = 1;
            if (reconnect_wait(bz, 5000))
                break;
            continue;
        }
        if (!trusted && store_is_safe_token(address)) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "bluetoothctl trust %s >/dev/null 2>&1",
                     address);
            run_cmd(cmd);
            trusted = 1;
        }

        attempt++;
        int fast = attempt <= g_config.reconnect_attempts;
        /* Log every fast attempt, then one slow attempt in ten. */
        if (fast || attempt % 10 == 0)
            log_info("adapter", "Reconnecting to %s (attempt %d%s)", address,
                     attempt, fast ? "" : ", once a minute");
        int ok = hfp_connect(bz->hfp, address);
        free(address);
        if (ok)
            continue;
        int wait_s = fast ? g_config.reconnect_interval : RECONNECT_SLOW_S;
        if (reconnect_wait(bz, wait_s * 1000))
            break;
    }
    return NULL;
}

void bluez_stop_workers(bluez_t *bz)
{
    if (!bz || !bz->reconnect_started)
        return;
    pthread_mutex_lock(&bz->worker_lock);
    bz->stopping = 1;
    pthread_cond_broadcast(&bz->worker_wake);
    pthread_mutex_unlock(&bz->worker_lock);
    /* An attempt in progress (SDP query, RFCOMM connect) finishes first. */
    pthread_join(bz->reconnect, NULL);
    bz->reconnect_started = 0;
}

/* ============================ lifecycle ============================ */

bluez_t *bluez_create(void)
{
    bluez_t *bz = calloc(1, sizeof(struct bluez));
    if (!bz)
        return NULL;
    pthread_mutex_init(&bz->worker_lock, NULL);
    pthread_cond_init(&bz->worker_wake, NULL);
    return bz;
}

static int on_signal(sd_event_source *s, const struct signalfd_siginfo *si, void *ud)
{
    bluez_t *bz = ud;
    log_info("bt", "Shutting down...");
    sd_event_exit(bz->event, 0);
    return 0;
}

int bluez_setup(bluez_t *bz, hfp_t *hfp, const gatt_cb_t *gcb)
{
    bz->hfp = hfp;

    int r = sd_bus_open_system(&bz->bus);
    if (r < 0) {
        log_error("bt", "cannot connect to the system bus: %s", strerror(-r));
        return -1;
    }
    r = sd_event_default(&bz->event);
    if (r < 0) {
        log_error("bt", "cannot create the event loop: %s", strerror(-r));
        return -1;
    }
    sd_bus_attach_event(bz->bus, bz->event, SD_EVENT_PRIORITY_NORMAL);

    /* Route SIGINT/SIGTERM through the event loop for a clean exit. */
    sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGTERM);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    sd_event_add_signal(bz->event, NULL, SIGINT, on_signal, bz);
    sd_event_add_signal(bz->event, NULL, SIGTERM, on_signal, bz);

    if (get_adapter(bz) != 0)
        return -1;
    configure_adapter(bz);
    enable_sco_over_hci();
    if (register_agent(bz) != 0)
        return -1;
    if (register_hfp(bz) != 0)
        return -1;

    bz->gatt = gatt_create(bz->bus, bz->adapter_path, gcb);
    if (!bz->gatt || gatt_register(bz->gatt) != 0)
        return -1;

    /* Auto-reconnect uses raw sockets and sdptool, so it stays off the bus. */
    if (g_config.auto_reconnect) {
        if (pthread_create(&bz->reconnect, NULL, reconnect_worker, bz) == 0)
            bz->reconnect_started = 1;
        else
            log_warn("adapter", "cannot start the reconnect supervisor");
    }
    return 0;
}

const char *bluez_adapter_address(bluez_t *bz)
{
    return bz->adapter_addr;
}

/* Called from the link thread (CMD DISCONNECT). sd-bus is not thread-safe
 * and the bus belongs to the event loop thread, so the request goes through
 * bluetoothctl instead. */
int bluez_disconnect_device(bluez_t *bz, const char *address)
{
    (void)bz;
    if (!store_is_safe_token(address))
        return -1;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s >/dev/null 2>&1",
             address);
    return system(cmd) == 0 ? 0 : -1;
}

gatt_t *bluez_gatt(bluez_t *bz)
{
    return bz ? bz->gatt : NULL;
}

void bluez_run(bluez_t *bz)
{
    sd_event_loop(bz->event);
}

/* Ask the event loop to exit. Safe from any thread: sd_event_exit() is not,
 * so the request is a SIGTERM to the process, which the loop receives
 * through its signalfd (all threads block the signal, see main()). */
void bluez_quit(void)
{
    kill(getpid(), SIGTERM);
}

void bluez_stop(bluez_t *bz)
{
    (void)bz;
    bluez_quit();
}

void bluez_destroy(bluez_t *bz)
{
    if (!bz)
        return;
    bluez_stop_workers(bz);
    gatt_destroy(bz->gatt);
    if (bz->profile_slot)
        sd_bus_slot_unref(bz->profile_slot);
    if (bz->agent_slot)
        sd_bus_slot_unref(bz->agent_slot);
    if (bz->bus)
        sd_bus_unref(bz->bus);
    if (bz->event)
        sd_event_unref(bz->event);
    pthread_cond_destroy(&bz->worker_wake);
    pthread_mutex_destroy(&bz->worker_lock);
    free(bz);
}
