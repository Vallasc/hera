#include "jitter.h"

static unsigned clampu(unsigned v, unsigned lo, unsigned hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void jitter_reset(jitter_t *j, unsigned frame_ms, unsigned floor_ms,
                  unsigned ceiling_ms)
{
    if (frame_ms == 0)
        frame_ms = 20;
    /* The floor cannot be less than one block. */
    if (floor_ms < frame_ms)
        floor_ms = frame_ms;
    if (ceiling_ms < floor_ms)
        ceiling_ms = floor_ms;

    j->frame_ms = frame_ms;
    j->floor_ms = floor_ms;
    j->ceiling_ms = ceiling_ms;
    /* Start at the floor; the estimate raises the target as needed. */
    j->target_ms = floor_ms;
    j->jitter_x16 = 0;
    j->jitter_ms = 0;
    j->last_arrival = 0;
    j->holddown_until = 0;
    j->underrun_floor_ms = 0;
}

/* target = 3 * J + frame_ms, clamped to [floor, ceiling] (§6.5). The extra
 * block covers quantisation against the SCO packet size (§7.1). */
static void recompute(jitter_t *j, uint64_t now)
{
    unsigned wanted = (3 * j->jitter_x16 + 8) / 16 + j->frame_ms;
    /* During the hold-down the target may rise but not fall below the value
     * set by the last underrun. Without this, the next arrival would undo the
     * increase. */
    if (now < j->holddown_until && wanted < j->underrun_floor_ms)
        wanted = j->underrun_floor_ms;
    j->target_ms = clampu(wanted, j->floor_ms, j->ceiling_ms);
}

void jitter_on_arrival(jitter_t *j, uint64_t now)
{
    if (j->last_arrival != 0) {
        uint64_t gap = now - j->last_arrival;
        /* Absolute deviation from the nominal interval. */
        unsigned deviation = gap > j->frame_ms
                           ? (unsigned)(gap - j->frame_ms)
                           : (unsigned)(j->frame_ms - gap);
        /* Clamp, so that a stream restart does not inflate the estimate. */
        if (deviation > 10u * j->frame_ms)
            deviation = 10u * j->frame_ms;
        int j16 = (int)j->jitter_x16;
        j16 += (int)deviation - ((j16 + 8) >> JB_JITTER_SHIFT);
        j->jitter_x16 = j16 > 0 ? (unsigned)j16 : 0;
        j->jitter_ms = (j->jitter_x16 + 8) >> JB_JITTER_SHIFT;
        recompute(j, now);
    }
    j->last_arrival = now;
}

void jitter_on_underrun(jitter_t *j, uint64_t now)
{
    /* Applied directly rather than through the estimate. */
    unsigned raised = clampu(j->target_ms + 2 * j->frame_ms, j->floor_ms,
                             j->ceiling_ms);
    j->target_ms = raised;
    j->underrun_floor_ms = raised;
    j->holddown_until = now + JB_HOLDDOWN_MS;
}

unsigned jitter_target_ms(const jitter_t *j)
{
    return j->target_ms;
}

jitter_action_t jitter_decide(const jitter_t *j, unsigned depth_ms,
                              uint64_t now)
{
    /* Grow when at least one block below target. */
    if (depth_ms + j->frame_ms <= j->target_ms)
        return JB_GROW;

    if (now < j->holddown_until)
        return JB_HOLD;

    /* Shrink only when more than two blocks above target. */
    if (depth_ms > j->target_ms + 2 * j->frame_ms)
        return JB_SHRINK;

    return JB_HOLD;
}
