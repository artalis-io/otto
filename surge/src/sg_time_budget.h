#ifndef SURGE_SG_TIME_BUDGET_H
#define SURGE_SG_TIME_BUDGET_H

#include <float.h>
#include <stdint.h>
#include "sh_time.h"

/*
 * SGTimeBudget — lightweight global time envelope for multi-phase solves.
 *
 * All functions take an explicit `now` parameter (monotonic seconds) so the
 * module contains no clock calls and is fully deterministic for testing.
 * Callers use sh_monotonic_seconds() from shared to obtain `now`.
 */

/* Backward compat alias — callers should prefer sh_monotonic_seconds(). */
#define sg_monotonic_seconds sh_monotonic_seconds

typedef struct {
    double deadline;      /* absolute monotonic time when budget expires */
    double solve_start;   /* when solve began (for elapsed reporting) */
} SGTimeBudget;

/* Initialize budget: deadline = now + total_seconds.
   If total_seconds <= 0, budget is unlimited (never expires). */
void sg_time_budget_init(SGTimeBudget *tb, double now, double total_seconds);

/* Has the global deadline passed? */
int sg_time_budget_expired(const SGTimeBudget *tb, double now);

/* Seconds remaining until deadline (0.0 if expired, DBL_MAX if unlimited). */
double sg_time_budget_remaining(const SGTimeBudget *tb, double now);

/* Compute seconds to give a phase.  fraction in (0,1] of remaining time.
   Returns at least min_seconds (to avoid starving short phases),
   capped at remaining time.  Returns 0 if already expired. */
double sg_time_budget_phase(const SGTimeBudget *tb, double now,
                            double fraction, double min_seconds);

/* Convenience: convert remaining seconds to int for ARALNSParams.max_time_seconds.
   Returns 0 if expired, clamped to [1, INT_MAX] otherwise. */
int sg_time_budget_remaining_int(const SGTimeBudget *tb, double now);

/* Total elapsed since solve start. */
double sg_time_budget_elapsed(const SGTimeBudget *tb, double now);

/* ---------------------------------------------------------------------------
 * SGBudgetProbe — amortized time-budget checker for hot inner loops.
 *
 * clock_gettime is ~100-500ns per call.  Checking every iteration of an
 * O(R×V) inner loop adds measurable overhead.  SGBudgetProbe calls the
 * clock only every `interval` ticks, returning a cached result otherwise.
 * Once expired the flag is sticky — no further clock calls are made.
 * ---------------------------------------------------------------------------*/

#define SG_BUDGET_PROBE_INTERVAL 64

typedef struct {
    const SGTimeBudget *budget;
    uint32_t interval;
    uint32_t counter;
    int expired;
} SGBudgetProbe;

static inline void sg_budget_probe_init(SGBudgetProbe *p,
                                         const SGTimeBudget *tb,
                                         uint32_t interval) {
    p->budget   = tb;
    p->interval = interval > 0 ? interval : 1;
    p->counter  = 0;
    p->expired  = 0;
}

static inline int sg_budget_probe_expired(SGBudgetProbe *p) {
    if (p->expired) return 1;
    if (++p->counter >= p->interval) {
        p->counter = 0;
        p->expired = sg_time_budget_expired(p->budget, sg_monotonic_seconds());
    }
    return p->expired;
}

static inline int sg_budget_probe_expired_at(SGBudgetProbe *p, double now) {
    if (p->expired) return 1;
    if (++p->counter >= p->interval) {
        p->counter = 0;
        p->expired = sg_time_budget_expired(p->budget, now);
    }
    return p->expired;
}

#endif /* SURGE_SG_TIME_BUDGET_H */
