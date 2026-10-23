#ifndef HERA_STATS_H
#define HERA_STATS_H

#include <stdint.h>

#include "proto.h"

/* Counters reported in STATS (PROTOCOL.md §10.1).
 *
 * Written by the audio and link threads and read once per second, using
 * relaxed atomics; no lock is taken on the audio path. Counters are reset at
 * service start, not per session. */

typedef struct {
    uint32_t dl_blocks;      /* audio blocks sent to the client             */
    uint32_t ul_blocks;      /* audio blocks received from the client       */
    uint32_t dl_drop;        /* not sent: the socket refused them (§6.6)    */
    uint32_t ul_lost;        /* missing on arrival: gaps in `ts`            */
    uint32_t ul_late;        /* received, dropped at the buffer ceiling     */
    uint32_t ul_fill;        /* silence generated on underrun               */
    uint32_t ul_bad;         /* malformed blocks dropped (§6.3)             */
} stats_t;

extern stats_t g_stats;

#define STAT_INC(field) __atomic_add_fetch(&g_stats.field, 1, __ATOMIC_RELAXED)
#define STAT_ADD(field, n) \
    __atomic_add_fetch(&g_stats.field, (uint32_t)(n), __ATOMIC_RELAXED)
#define STAT_GET(field) __atomic_load_n(&g_stats.field, __ATOMIC_RELAXED)

/* Instantaneous values, filled in by the relay on each tick. */
typedef struct {
    uint16_t ul_buffered_ms;
    uint16_t ul_jitter_ms;
    uint8_t  sco_codec;      /* SCO_CODEC_* */
    uint8_t  flags;          /* STF_*       */
    uint8_t  cpu_pct;
    int8_t   rssi;           /* dBm, 0 when unknown */
    uint8_t  net_mode;       /* NET_MODE_*  */
    uint8_t  fmt;            /* negotiated audio format */
    uint8_t  ul_target_ms;   /* adaptive buffer target (§6.5)            */
} stats_live_t;

/* Serialise the 44-byte STATS payload. `uptime_ms` is milliseconds since the
 * service started. */
void stats_serialise(uint8_t out[HERA_STATS_LEN], uint32_t uptime_ms,
                     const stats_live_t *live);

#endif /* HERA_STATS_H */
