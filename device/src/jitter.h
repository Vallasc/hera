#ifndef HERA_JITTER_H
#define HERA_JITTER_H

#include <stdint.h>

/* Adaptive target depth for the uplink jitter buffer (PROTOCOL.md §6.5).
 *
 * The target tracks measured interarrival jitter, bounded by a floor and a
 * ceiling. The policy is asymmetric:
 *
 * - grow as soon as the depth is one block below target; shrink only when it
 *   is more than two blocks above;
 * - on underrun, raise the target by two blocks immediately and hold it for
 *   JB_HOLDDOWN_MS, which prevents oscillation;
 * - the caller shrinks only by discarding a filler block (§6.3, flags bit 0),
 *   so shrinking never removes speech.
 *
 * This module holds no buffer and performs no I/O. */

typedef enum {
    JB_HOLD = 0,
    JB_GROW,        /* insert one filler block */
    JB_SHRINK,      /* discard one filler block */
} jitter_action_t;

typedef struct {
    unsigned frame_ms;
    unsigned floor_ms;
    unsigned ceiling_ms;

    unsigned target_ms;     /* current target depth */
    unsigned jitter_x16;    /* EWMA of the interarrival deviation, ms * 16 */
    unsigned jitter_ms;     /* the same, rounded to ms */
    uint64_t last_arrival;  /* 0 = none yet */
    uint64_t holddown_until;/* no shrinking before this time */
    unsigned underrun_floor_ms; /* minimum target until holddown_until */
} jitter_t;

/* Hold-down period after an underrun. */
#define JB_HOLDDOWN_MS 5000

/* EWMA gain as a shift: J += (|D| - J) / 16, as in RFC 3550 §6.4.1. J is
 * kept in units of 1/16 ms (RFC 3550 Appendix A.8); with whole
 * milliseconds the update would round to zero for deviations less than
 * 16 ms above J. */
#define JB_JITTER_SHIFT 4

void jitter_reset(jitter_t *j, unsigned frame_ms, unsigned floor_ms,
                  unsigned ceiling_ms);

/* Record a block arrival at `now` (ms) and update the estimate. */
void jitter_on_arrival(jitter_t *j, uint64_t now);

/* Record an underrun: raise the target and start the hold-down (§6.5). */
void jitter_on_underrun(jitter_t *j, uint64_t now);

/* Current target depth, in ms. */
unsigned jitter_target_ms(const jitter_t *j);

/* Decide whether to grow, shrink or hold, given the current depth. */
jitter_action_t jitter_decide(const jitter_t *j, unsigned depth_ms,
                              uint64_t now);

#endif /* HERA_JITTER_H */
