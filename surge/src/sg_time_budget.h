#ifndef SURGE_SG_TIME_BUDGET_H
#define SURGE_SG_TIME_BUDGET_H

#include <float.h>

/*
 * SGTimeBudget — lightweight global time envelope for multi-phase solves.
 *
 * All functions take an explicit `now` parameter (monotonic seconds) so the
 * module contains no clock calls and is fully deterministic for testing.
 */

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

#endif /* SURGE_SG_TIME_BUDGET_H */
