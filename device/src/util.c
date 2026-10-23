#include "util.h"

#include <arpa/inet.h>
#include <stdio.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

char *xstrdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

int thread_set_realtime(int prio)
{
    if (prio <= 0)
        return 0;
    struct sched_param sp = { .sched_priority = prio };
    int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    return r == 0 ? 0 : -1;
}

int cpu_percent_since_last(void)
{
    static unsigned long long last_ticks;
    static uint64_t last_ms;
    static int primed;

    FILE *f = fopen("/proc/self/stat", "r");
    if (!f)
        return 0;
    /* Field 14 (utime) and 15 (stime); comm may contain spaces, so resume
     * parsing after the closing ')'. */
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return 0;
    buf[n] = '\0';

    char *p = strrchr(buf, ')');
    if (!p)
        return 0;
    p++;
    unsigned long long utime = 0, stime = 0;
    int field = 2;                       /* ')' ends field 2 (comm) */
    for (char *tok = strtok(p, " "); tok; tok = strtok(NULL, " ")) {
        field++;
        if (field == 14)
            utime = strtoull(tok, NULL, 10);
        else if (field == 15) {
            stime = strtoull(tok, NULL, 10);
            break;
        }
    }

    unsigned long long ticks = utime + stime;
    uint64_t t = now_ms();
    int pct = 0;
    if (primed && t > last_ms) {
        long hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0)
            hz = 100;
        unsigned long long dticks = ticks - last_ticks;
        double busy_ms = (double)dticks * 1000.0 / (double)hz;
        pct = (int)(busy_ms * 100.0 / (double)(t - last_ms) + 0.5);
        if (pct < 0)
            pct = 0;
        if (pct > 100)
            pct = 100;
    }
    last_ticks = ticks;
    last_ms = t;
    primed = 1;
    return pct;
}

int recovery_forces_ap(const int64_t *starts, size_t n, int64_t now,
                       int window_s, int threshold)
{
    int recent = 0;
    for (size_t i = 0; i < n; i++) {
        if (starts[i] <= now && now - starts[i] <= window_s)
            recent++;
    }
    return recent >= threshold;
}

void secure_zero(void *p, size_t len)
{
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

uint32_t rand_u32(void)
{
    uint32_t v = 0;
    if (random_bytes((uint8_t *)&v, sizeof v) == 0 && v)
        return v;
    /* No /dev/urandom. Session ids only need to be unlikely to repeat (§4.2). */
    return (uint32_t)(now_us() ^ (now_us() << 16));
}

int random_bytes(uint8_t *out, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    size_t got = fread(out, 1, len, f);
    fclose(f);
    return got == len ? 0 : -1;
}

const char *inet_ntoa_safe(struct in_addr addr)
{
    static __thread char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &addr, buf, sizeof buf))
        snprintf(buf, sizeof buf, "?");
    return buf;
}

int credential_ok(const char *value, size_t min, size_t max)
{
    if (!value)
        return 0;
    size_t n = strlen(value);
    if (n < min || n > max)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7E)   /* non printable, newlines included */
            return 0;
        if (c == '"' || c == '\\')  /* would break the quoted directive */
            return 0;
    }
    return 1;
}

