#include "gatt.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <systemd/sd-event.h>

#include "config.h"
#include "log.h"
#include "netcfg.h"
#include "util.h"

#define TAG "gatt"

/* §3.1 -- fixed by the protocol, not configurable. */
static const char *SVC_UUID   = "48455241-0001-4c32-9a3e-8f1d5b7c2e01";
static const char *STATE_UUID = "48455241-0002-4c32-9a3e-8f1d5b7c2e01";
static const char *CFG_UUID   = "48455241-0003-4c32-9a3e-8f1d5b7c2e01";
static const char *SESS_UUID  = "48455241-0004-4c32-9a3e-8f1d5b7c2e01";

struct gatt {
    sd_bus      *bus;
    char        *adapter_path;
    gatt_cb_t    cb;

    char         app_path[128];
    char         svc_path[160];
    char         state_path[192];
    char         cfg_path[192];
    char         sess_path[192];

    /* Whether a client subscribed to NET_STATE notifications (§3.2). */
    int          notifying;

    /* NET_STATE is published from several threads (netd, its watchdog, the
     * NET_CFG worker), but sd-bus is not thread-safe: the value is stored
     * under `lock` and the event loop is woken through `wake_fd` to emit
     * the notification on its own thread. */
    pthread_mutex_t lock;
    uint8_t      last_state[NET_STATE_MAX];
    size_t       last_state_len;
    int          wake_fd;
    sd_event_source *wake_src;

    /* A NET_CFG operation is running on its worker thread (atomic). The
     * thread is joinable: gatt_destroy() waits for it. `op_started` is only
     * used on the event loop thread and in gatt_destroy(). */
    int          op_running;
    pthread_t    op_thread;
    int          op_started;

    sd_bus_slot *om_slot;
    sd_bus_slot *svc_slot;
    sd_bus_slot *state_slot;
    sd_bus_slot *cfg_slot;
    sd_bus_slot *sess_slot;
    sd_bus_slot *adv_slot;
    sd_bus_slot *call_slot_app;
    sd_bus_slot *call_slot_adv;
};

/* ------------------------------------------------------------- properties */

static int prop_uuid_state(sd_bus *bus, const char *path, const char *iface,
                           const char *prop, sd_bus_message *reply, void *ud,
                           sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", STATE_UUID);
}

static int prop_uuid_cfg(sd_bus *bus, const char *path, const char *iface,
                         const char *prop, sd_bus_message *reply, void *ud,
                         sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", CFG_UUID);
}

static int prop_uuid_sess(sd_bus *bus, const char *path, const char *iface,
                          const char *prop, sd_bus_message *reply, void *ud,
                          sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", SESS_UUID);
}

static int prop_svc_uuid(sd_bus *bus, const char *path, const char *iface,
                         const char *prop, sd_bus_message *reply, void *ud,
                         sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", SVC_UUID);
}

static int prop_true(sd_bus *bus, const char *path, const char *iface,
                     const char *prop, sd_bus_message *reply, void *ud,
                     sd_bus_error *err)
{
    return sd_bus_message_append(reply, "b", 1);
}

static int prop_service_path(sd_bus *bus, const char *path, const char *iface,
                             const char *prop, sd_bus_message *reply, void *ud,
                             sd_bus_error *err)
{
    gatt_t *g = ud;
    return sd_bus_message_append(reply, "o", g->svc_path);
}

/* `encrypt-read` / `encrypt-write`: BlueZ requires an encrypted link and
 * starts pairing on first access (§3.1). */
static int prop_flags_state(sd_bus *bus, const char *path, const char *iface,
                            const char *prop, sd_bus_message *reply, void *ud,
                            sd_bus_error *err)
{
    return sd_bus_message_append(reply, "as", 2, "encrypt-read", "notify");
}

static int prop_flags_cfg(sd_bus *bus, const char *path, const char *iface,
                          const char *prop, sd_bus_message *reply, void *ud,
                          sd_bus_error *err)
{
    return sd_bus_message_append(reply, "as", 1, "encrypt-write");
}

static int prop_flags_sess(sd_bus *bus, const char *path, const char *iface,
                           const char *prop, sd_bus_message *reply, void *ud,
                           sd_bus_error *err)
{
    return sd_bus_message_append(reply, "as", 1, "encrypt-read");
}

/* Cached NET_STATE value, carried by notifications. */
static int prop_state_value(sd_bus *bus, const char *path, const char *iface,
                            const char *prop, sd_bus_message *reply, void *ud,
                            sd_bus_error *err)
{
    gatt_t *g = ud;
    uint8_t value[NET_STATE_MAX];
    pthread_mutex_lock(&g->lock);
    size_t n = g->last_state_len;
    memcpy(value, g->last_state, n);
    pthread_mutex_unlock(&g->lock);
    return sd_bus_message_append_array(reply, 'y', value, n);
}

static int prop_notifying(sd_bus *bus, const char *path, const char *iface,
                          const char *prop, sd_bus_message *reply, void *ud,
                          sd_bus_error *err)
{
    gatt_t *g = ud;
    return sd_bus_message_append(reply, "b", g->notifying);
}

/* ---------------------------------------------------------------- methods */

static int reply_bytes(sd_bus_message *m, const void *data, size_t len)
{
    sd_bus_message *reply = NULL;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0)
        return r;
    r = sd_bus_message_append_array(reply, 'y', data, len);
    if (r >= 0)
        r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r;
}

static int on_read_state(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    gatt_t *g = ud;
    net_state_t state;
    memset(&state, 0, sizeof state);
    if (g->cb.state)
        g->cb.state(g->cb.ud, &state);

    uint8_t out[NET_STATE_MAX];
    size_t n = netcfg_encode_state(out, sizeof out, &state,
                                   g_config.ctrl_port, g_config.audio_port);
    pthread_mutex_lock(&g->lock);
    memcpy(g->last_state, out, n);
    g->last_state_len = n;
    pthread_mutex_unlock(&g->lock);
    log_info(TAG, "NET_STATE read: mode %u, phase %u", state.mode, state.phase);
    return reply_bytes(m, out, n);
}

static int on_read_session(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    gatt_t *g = ud;

    /* SESSION is refused while a session is up or resumable (§3.4), so that
     * a second phone cannot obtain the token during a call. It does not
     * protect the first pairing, which is Just Works (§11.6). */
    if (g->cb.claimed && g->cb.claimed(g->cb.ud)) {
        log_warn(TAG, "SESSION read refused: another client has the device");
        return sd_bus_error_setf(err, "org.bluez.Error.NotPermitted",
                                 "device already in use");
    }

    const uint8_t *token = g->cb.token ? g->cb.token(g->cb.ud) : NULL;
    const char *psk = g->cb.passphrase ? g->cb.passphrase(g->cb.ud) : "";
    if (!token)
        return sd_bus_error_setf(err, "org.bluez.Error.Failed", "no token");

    uint8_t out[SESSION_MAX];
    size_t n = netcfg_encode_session(out, sizeof out, token, psk);
    log_info(TAG, "SESSION read: token and %s", *psk ? "passphrase" : "no passphrase");
    return reply_bytes(m, out, n);
}

struct cfg_job {
    gatt_t *g;
    int     op;
    char    ssid[64];
    char    psk[128];
};

static void *cfg_worker(void *arg)
{
    struct cfg_job *job = arg;
    gatt_t *g = job->g;

    log_info(TAG, "NET_CFG op %d", job->op);
    int rc = -1;
    switch (job->op) {
    case NETOP_START_AP:
        rc = g->cb.use_ap ? g->cb.use_ap(g->cb.ud) : -1;
        break;
    case NETOP_JOIN_STA:
        rc = g->cb.join ? g->cb.join(g->cb.ud, job->ssid, job->psk) : -1;
        break;
    case NETOP_FORGET_STA:
        rc = g->cb.forget_station ? g->cb.forget_station(g->cb.ud) : -1;
        break;
    case NETOP_RESTART:
        rc = g->cb.restart ? g->cb.restart(g->cb.ud) : -1;
        break;
    }
    secure_zero(job->psk, sizeof job->psk);
    free(job);
    if (rc != 0)
        log_warn(TAG, "NET_CFG did not succeed; see NET_STATE.fail");

    /* Report the outcome, also when nothing changed (e.g. HERA_NET_MODE=none
     * refused the operation), so that the client does not wait for its own
     * timeout. */
    net_state_t state;
    memset(&state, 0, sizeof state);
    if (g->cb.state) {
        g->cb.state(g->cb.ud, &state);
        gatt_notify_state(g, &state);
    }
    __atomic_store_n(&g->op_running, 0, __ATOMIC_RELEASE);
    return NULL;
}

static int on_write_cfg(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    gatt_t *g = ud;
    const void *data = NULL;
    size_t len = 0;
    int r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0)
        return r;

    char ssid[64], psk[128];
    int op = netcfg_decode(data, len, ssid, sizeof ssid, psk, sizeof psk);
    if (op == 0) {
        log_warn(TAG, "malformed NET_CFG write (%zu bytes)", len);
        return sd_bus_error_setf(err, "org.bluez.Error.InvalidValueLength",
                                 "malformed NET_CFG");
    }

    if (op < NETOP_START_AP || op > NETOP_RESTART || op == 4) {
        /* Reserved (op 4, formerly ROTATE) or unknown: ignored (§3.3.1). */
        sd_bus_reply_method_return(m, "");
        log_info(TAG, "NET_CFG op %d ignored", op);
        return 1;
    }
    if (__atomic_exchange_n(&g->op_running, 1, __ATOMIC_ACQ_REL)) {
        log_warn(TAG, "NET_CFG op %d refused: another one is running", op);
        return sd_bus_error_setf(err, "org.bluez.Error.InProgress",
                                 "a network change is in progress");
    }

    struct cfg_job *job = calloc(1, sizeof *job);
    if (!job) {
        __atomic_store_n(&g->op_running, 0, __ATOMIC_RELEASE);
        return sd_bus_error_setf(err, "org.bluez.Error.Failed", "out of memory");
    }
    job->g = g;
    job->op = op;
    snprintf(job->ssid, sizeof job->ssid, "%s", ssid);
    snprintf(job->psk, sizeof job->psk, "%s", psk);
    secure_zero(psk, sizeof psk);

    /* Acknowledge the write before applying the operation (§3.3). */
    r = sd_bus_reply_method_return(m, "");
    if (r < 0) {
        secure_zero(job->psk, sizeof job->psk);
        free(job);
        __atomic_store_n(&g->op_running, 0, __ATOMIC_RELEASE);
        return r;
    }

    /* The operation takes the network down and up again, which can last
     * tens of seconds. Run here, it would block the event loop: no NET_STATE
     * notification would leave before the end, and BlueZ calls (pairing,
     * HFP) would wait as well. */
    if (g->op_started) {
        /* op_running was 0: the previous worker has finished. */
        pthread_join(g->op_thread, NULL);
        g->op_started = 0;
    }
    if (pthread_create(&g->op_thread, NULL, cfg_worker, job) != 0) {
        log_error(TAG, "cannot start the NET_CFG worker");
        secure_zero(job->psk, sizeof job->psk);
        free(job);
        __atomic_store_n(&g->op_running, 0, __ATOMIC_RELEASE);
        return 1;
    }
    g->op_started = 1;
    return 1;
}

static int on_start_notify(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    gatt_t *g = ud;
    g->notifying = 1;
    log_info(TAG, "client subscribed to NET_STATE");
    return sd_bus_reply_method_return(m, "");
}

static int on_stop_notify(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    gatt_t *g = ud;
    g->notifying = 0;
    return sd_bus_reply_method_return(m, "");
}

/* ----------------------------------------------------------------- vtables */

static const sd_bus_vtable service_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", prop_svc_uuid, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Primary", "b", prop_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable state_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", prop_uuid_state, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", prop_service_path, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", prop_flags_state, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Value", "ay", prop_state_value, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Notifying", "b", prop_notifying, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("ReadValue", "a{sv}", "ay", on_read_state, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StartNotify", "", "", on_start_notify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("StopNotify", "", "", on_stop_notify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable cfg_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", prop_uuid_cfg, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", prop_service_path, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", prop_flags_cfg, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("WriteValue", "aya{sv}", "", on_write_cfg, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static const sd_bus_vtable sess_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UUID", "s", prop_uuid_sess, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service", "o", prop_service_path, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Flags", "as", prop_flags_sess, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("ReadValue", "a{sv}", "ay", on_read_session, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ---------------------------------------------------------- advertisement */

static int prop_adv_type(sd_bus *bus, const char *path, const char *iface,
                         const char *prop, sd_bus_message *reply, void *ud,
                         sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", "peripheral");
}

/* §3.1: the name and the 128-bit UUID do not both fit in a 31-byte
 * advertising PDU; BlueZ places the overflow in the scan response. */
static int prop_adv_uuids(sd_bus *bus, const char *path, const char *iface,
                          const char *prop, sd_bus_message *reply, void *ud,
                          sd_bus_error *err)
{
    return sd_bus_message_append(reply, "as", 1, SVC_UUID);
}

static int prop_adv_name(sd_bus *bus, const char *path, const char *iface,
                         const char *prop, sd_bus_message *reply, void *ud,
                         sd_bus_error *err)
{
    return sd_bus_message_append(reply, "s", g_config.bt_alias);
}

static int on_adv_release(sd_bus_message *m, void *ud, sd_bus_error *err)
{
    log_info(TAG, "advertisement released by BlueZ");
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable adv_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Type", "s", prop_adv_type, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ServiceUUIDs", "as", prop_adv_uuids, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("LocalName", "s", prop_adv_name, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Discoverable", "b", prop_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("Release", "", "", on_adv_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------- lifecycle */

static int on_wake(sd_event_source *src, int fd, uint32_t revents, void *ud);

gatt_t *gatt_create(sd_bus *bus, const char *adapter_path, const gatt_cb_t *cb)
{
    gatt_t *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->bus = bus;
    g->adapter_path = xstrdup(adapter_path);
    g->cb = *cb;

    snprintf(g->app_path, sizeof g->app_path, "%s", g_config.gatt_path);
    snprintf(g->svc_path, sizeof g->svc_path, "%s/service0", g->app_path);
    snprintf(g->state_path, sizeof g->state_path, "%s/netstate", g->svc_path);
    snprintf(g->cfg_path, sizeof g->cfg_path, "%s/netcfg", g->svc_path);
    snprintf(g->sess_path, sizeof g->sess_path, "%s/session", g->svc_path);

    pthread_mutex_init(&g->lock, NULL);
    g->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    sd_event *event = sd_bus_get_event(bus);
    if (g->wake_fd < 0 || !event ||
        sd_event_add_io(event, &g->wake_src, g->wake_fd, EPOLLIN, on_wake,
                        g) < 0)
        log_error(TAG, "cannot set up NET_STATE notifications");
    return g;
}

/* Runs on the event loop thread, woken by gatt_notify_state(). */
static int on_wake(sd_event_source *src, int fd, uint32_t revents, void *ud)
{
    gatt_t *g = ud;
    uint64_t count;
    while (read(fd, &count, sizeof count) == (ssize_t)sizeof count) { }
    if (!g->notifying)
        return 0;
    /* BlueZ turns a PropertiesChanged on Value into a GATT notification. */
    int r = sd_bus_emit_properties_changed(g->bus, g->state_path,
                                          "org.bluez.GattCharacteristic1",
                                          "Value", NULL);
    if (r < 0)
        log_debug(TAG, "NET_STATE notification failed: %s", strerror(-r));
    return 0;
}

/* Safe from any thread. */
void gatt_notify_state(gatt_t *g, const net_state_t *state)
{
    if (!g)
        return;
    uint8_t out[NET_STATE_MAX];
    size_t n = netcfg_encode_state(out, sizeof out, state,
                                   g_config.ctrl_port, g_config.audio_port);
    pthread_mutex_lock(&g->lock);
    memcpy(g->last_state, out, n);
    g->last_state_len = n;
    pthread_mutex_unlock(&g->lock);

    uint64_t one = 1;
    if (g->wake_fd >= 0 && write(g->wake_fd, &one, sizeof one) < 0 &&
        errno != EAGAIN)
        log_debug(TAG, "cannot wake the event loop: %s", strerror(errno));
}

static int on_register_reply(sd_bus_message *reply, void *ud, sd_bus_error *err)
{
    const char *what = ud;
    const sd_bus_error *e = sd_bus_message_get_error(reply);
    if (e)
        log_error(TAG, "%s failed: %s", what, e->message ? e->message : e->name);
    else
        log_info(TAG, "%s registered", what);
    return 0;
}

static int export_characteristic(gatt_t *g, sd_bus_slot **slot,
                                 const char *path,
                                 const sd_bus_vtable *vtable, const char *what)
{
    int r = sd_bus_add_object_vtable(g->bus, slot, path,
                                     "org.bluez.GattCharacteristic1", vtable, g);
    if (r < 0)
        log_error(TAG, "cannot export %s: %s", what, strerror(-r));
    return r;
}

int gatt_register(gatt_t *g)
{
    int r;

    /* BlueZ discovers the application's objects through its ObjectManager. */
    r = sd_bus_add_object_manager(g->bus, &g->om_slot, g->app_path);
    if (r < 0) {
        log_error(TAG, "cannot export the object manager: %s", strerror(-r));
        return -1;
    }
    r = sd_bus_add_object_vtable(g->bus, &g->svc_slot, g->svc_path,
                                 "org.bluez.GattService1", service_vtable, g);
    if (r < 0) {
        log_error(TAG, "cannot export the service: %s", strerror(-r));
        return -1;
    }
    if (export_characteristic(g, &g->state_slot, g->state_path, state_vtable,
                              "NET_STATE") < 0 ||
        export_characteristic(g, &g->cfg_slot, g->cfg_path, cfg_vtable,
                              "NET_CFG") < 0 ||
        export_characteristic(g, &g->sess_slot, g->sess_path, sess_vtable,
                              "SESSION") < 0)
        return -1;

    r = sd_bus_add_object_vtable(g->bus, &g->adv_slot, g_config.adv_path,
                                 "org.bluez.LEAdvertisement1", adv_vtable, g);
    if (r < 0) {
        log_error(TAG, "cannot export the advertisement: %s", strerror(-r));
        return -1;
    }

    /* Asynchronous: BlueZ calls back into our objects while handling
     * RegisterApplication, and this thread must be free to answer. */
    sd_bus_message *m = NULL;
    r = sd_bus_message_new_method_call(g->bus, &m, "org.bluez", g->adapter_path,
                                       "org.bluez.GattManager1",
                                       "RegisterApplication");
    if (r < 0)
        return -1;
    sd_bus_message_append(m, "o", g->app_path);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_close_container(m);
    r = sd_bus_call_async(g->bus, &g->call_slot_app, m, on_register_reply,
                          (void *)"GATT application", 0);
    sd_bus_message_unref(m);
    if (r < 0) {
        log_error(TAG, "RegisterApplication failed: %s", strerror(-r));
        return -1;
    }

    m = NULL;
    r = sd_bus_message_new_method_call(g->bus, &m, "org.bluez", g->adapter_path,
                                       "org.bluez.LEAdvertisingManager1",
                                       "RegisterAdvertisement");
    if (r < 0)
        return -1;
    sd_bus_message_append(m, "o", g_config.adv_path);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_close_container(m);
    r = sd_bus_call_async(g->bus, &g->call_slot_adv, m, on_register_reply,
                          (void *)"Advertisement", 0);
    sd_bus_message_unref(m);
    if (r < 0) {
        log_error(TAG, "RegisterAdvertisement failed: %s", strerror(-r));
        return -1;
    }

    log_info(TAG, "advertising as '%s' on %s", g_config.bt_alias, SVC_UUID);
    log_info(TAG, "SESSION readable while no client has the device (§3.4)");
    return 0;
}

void gatt_destroy(gatt_t *g)
{
    if (!g)
        return;
    /* A network change in progress is waited for: it uses `g` to report its
     * outcome. */
    if (g->op_started)
        pthread_join(g->op_thread, NULL);
    sd_bus_slot *slots[] = {
        g->call_slot_adv, g->call_slot_app, g->adv_slot, g->sess_slot,
        g->cfg_slot, g->state_slot, g->svc_slot, g->om_slot,
    };
    for (size_t i = 0; i < sizeof slots / sizeof *slots; i++)
        if (slots[i])
            sd_bus_slot_unref(slots[i]);
    if (g->wake_src)
        sd_event_source_unref(g->wake_src);
    if (g->wake_fd >= 0)
        close(g->wake_fd);
    pthread_mutex_destroy(&g->lock);
    free(g->adapter_path);
    free(g);
}
