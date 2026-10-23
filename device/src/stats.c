#include "stats.h"

#include <string.h>

#include "util.h"

stats_t g_stats;

void stats_serialise(uint8_t out[HERA_STATS_LEN], uint32_t uptime_ms,
                     const stats_live_t *live)
{
    memset(out, 0, HERA_STATS_LEN);
    put_u32le(out + 0,  uptime_ms);
    put_u32le(out + 4,  STAT_GET(dl_blocks));
    put_u32le(out + 8,  STAT_GET(ul_blocks));
    put_u32le(out + 12, STAT_GET(dl_drop));
    put_u32le(out + 16, STAT_GET(ul_lost));
    put_u32le(out + 20, STAT_GET(ul_late));
    put_u32le(out + 24, STAT_GET(ul_fill));
    put_u32le(out + 28, STAT_GET(ul_bad));
    put_u16le(out + 32, live->ul_buffered_ms);
    put_u16le(out + 34, live->ul_jitter_ms);
    out[36] = live->sco_codec;
    out[37] = live->flags;
    out[38] = live->cpu_pct;
    out[39] = (uint8_t)live->rssi;      /* i8 on the wire: dBm are negative */
    out[40] = live->net_mode;
    out[41] = live->fmt;
    out[42] = live->ul_target_ms;
    /* 43 reserved, already zero */
}
