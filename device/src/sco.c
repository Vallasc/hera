#include "sco.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

#include "log.h"
#include "util.h"

struct sco_server {
    int   fd;
    char *adapter_addr;
    void (*on_conn)(void *ud, int fd);
    void *ud;
    volatile int running;
    pthread_t thread;
};

static void *accept_loop(void *arg)
{
    sco_server_t *s = arg;
    while (s->running) {
        struct sockaddr_sco addr;
        socklen_t alen = sizeof(addr);
        int cfd = accept(s->fd, (struct sockaddr *)&addr, &alen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            if (s->running)
                log_warn("sco", "accept error: %s", strerror(errno));
            continue;
        }
        char ba[18] = {0};
        ba2str(&addr.sco_bdaddr, ba);
        log_info("sco", "Voice channel from %s", ba);
        s->on_conn(s->ud, cfd);            /* hands off ownership */
    }
    return NULL;
}

sco_server_t *sco_server_create(const char *adapter_addr,
                                void (*on_conn)(void *ud, int fd), void *ud)
{
    sco_server_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->fd = -1;
    s->adapter_addr = xstrdup(adapter_addr);
    s->on_conn = on_conn;
    s->ud = ud;
    return s;
}

int sco_server_start(sco_server_t *s)
{
    s->fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
    if (s->fd < 0) {
        log_error("sco", "cannot create SCO socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_sco addr;
    memset(&addr, 0, sizeof(addr));
    addr.sco_family = AF_BLUETOOTH;
    if (s->adapter_addr && *s->adapter_addr)
        str2ba(s->adapter_addr, &addr.sco_bdaddr);   /* else BDADDR_ANY */

    if (bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("sco", "SCO bind failed: %s", strerror(errno));
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    if (listen(s->fd, 1) < 0) {
        log_error("sco", "SCO listen failed: %s", strerror(errno));
        close(s->fd);
        s->fd = -1;
        return -1;
    }

    /* A one-second accept timeout lets the loop observe `running`. */
    struct timeval tv = { 1, 0 };
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s->running = 1;
    if (pthread_create(&s->thread, NULL, accept_loop, s) != 0) {
        log_error("sco", "cannot start the accept thread");
        s->running = 0;
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    log_info("sco", "Listening for voice channels on %s",
             s->adapter_addr ? s->adapter_addr : "any adapter");
    return 0;
}

void sco_server_stop(sco_server_t *s)
{
    if (!s)
        return;
    if (s->running) {
        s->running = 0;
        if (s->fd >= 0)
            shutdown(s->fd, SHUT_RDWR);
        pthread_join(s->thread, NULL);
    }
    if (s->fd >= 0)
        close(s->fd);
    free(s->adapter_addr);
    free(s);
}
