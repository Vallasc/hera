#ifndef HERA_UTIL_H
#define HERA_UTIL_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

/* Monotonic clock in milliseconds (wraps well past any realistic uptime). */
uint64_t now_ms(void);

/* Monotonic clock in microseconds, for timing the audio loop. */
uint64_t now_us(void);

/* Sleep for `ms` milliseconds, restarting on EINTR. */
void sleep_ms(int ms);

/* strdup that maps NULL to NULL instead of crashing. */
char *xstrdup(const char *s);

/* Raise the calling thread to SCHED_FIFO at `prio` (1..99). No-op when prio
 * is 0. Returns 0 on success; failure is non-fatal (logged by the caller). */
int thread_set_realtime(int prio);

/* CPU used by this process since the previous call, as a percentage of one
 * core (0..100). The first call primes the counters and returns 0. */
int cpu_percent_since_last(void);

/* True if `value` is acceptable as a WiFi SSID or passphrase: length within
 * [`min`, `max`], printable ASCII only, no `"` and no `\\`.
 *
 * The values are written into quoted hostapd / wpa_supplicant directives, which
 * a quote or a backslash would break (PROTOCOL.md §11.6). IEEE 802.11 limits
 * are 1..32 for an SSID and 8..63 for a passphrase. */
int credential_ok(const char *value, size_t min, size_t max);

/* 32 random bits from /dev/urandom, falling back to the clock. Used for
 * session ids (PROTOCOL.md §4.2), which only need to be unlikely to repeat. */
uint32_t rand_u32(void);

/* §2.5 recovery rule. `starts` are the start times, in seconds, of earlier
 * service starts that did not reach state `up`. Returns 1 if at least
 * `threshold` of them lie within `window_s` before `now`. */
int recovery_forces_ap(const int64_t *starts, size_t n, int64_t now,
                       int window_s, int threshold);

/* Overwrite `len` bytes at `p` with zeros, in a way the compiler does not
 * remove. For credentials held in memory. */
void secure_zero(void *p, size_t len);

/* Fill `out` with `len` random bytes. Returns 0 on success. */
int random_bytes(uint8_t *out, size_t len);

/* Dotted form of an IPv4 address into a caller-owned static buffer. Unlike
 * inet_ntoa it is per-thread, so two log lines cannot clobber each other. */
const char *inet_ntoa_safe(struct in_addr addr);

/* Little-endian stores/loads used by the wire format (PROTOCOL.md §5.1). */
static inline void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif /* HERA_UTIL_H */
