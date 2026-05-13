/*
 * simplex_phase1_zones.h - Phase 1 crisis zone handlers for the simplex method.
 *
 * The Phase 1 main loop in simplex.c processes 6 sequential crisis zones per
 * iteration.  Each zone is extracted into its own handler function, reducing
 * the main loop body from ~2400 lines to ~100 lines of orchestration.
 *
 * Zero behavior change — pure code motion.
 *
 * Zone flow per iteration:
 *   1. tick_cooldowns  — decrement 8 TTL counters
 *   2. pre_iter        — stagnation escape, no-pivot force, time limit
 *   3. pricing         — select entering variable, handle no-entering
 *   4. ratio_breakdown — handle ratio test failure escalation
 *   5. direction_guard — direction stabilization, defer, retry
 *   6. pivot_failure   — handle pivot failure escalation
 *   (post-pivot: reset, stall detect, periodic refactor)
 */

#ifndef SIMPLEX_PHASE1_ZONES_H
#define SIMPLEX_PHASE1_ZONES_H

#include "lp.h"
#include "simplex_phase1_recovery.h"

/* ── Zone return codes ─────────────────────────────────────────────── */

typedef enum {
    P1_ZONE_CONTINUE,     /* caller does `continue` */
    P1_ZONE_RETURN_OK,    /* Phase 1 feasible — caller returns 0 */
    P1_ZONE_RETURN_FAIL,  /* infeasible/breakdown — caller returns -1 */
    P1_ZONE_PROCEED       /* fall through to next zone */
} P1ZoneResult;

/* ── Per-iteration context (locals crossing zone boundaries) ──────── */

typedef struct {
    /* Pricing / ratio / pivot results */
    int entering;
    int leaving;
    double theta;
    int ratio_status;
    int pivot_status;

    /* Direction guard locals */
    double dir_inf;
    int force_dir_refactor_extreme;
    int force_dir_refactor_lu_health;
    int force_pivot_mode_active;
    int dir_refactor_ladder_forced;
    int force_extreme_followup_tracked;
    int cooldown_active;
    int dir_stabilize_cooldown_target;

    /* Failed-stabilize retry locals */
    int original_entering;
    int stabilized;
    int retry_penalize_last_failed;
    int retry_consumed_alternate;
    int retry_used_local_memory_alt;
    int retry_direction_guard_exclude_original;
    int arm_shadow_guard_followup_pending;
    int arm_force_extreme_followup_pending;

    /* Timing */
    double phase1_hot_ms_prev;
} P1IterContext;

/* ── Zone 1: Cooldown tick ──────────────────────────────────────────── */

/* Decrement all TTL-based cooldown counters at the start of each iteration. */
void p1_zone_tick_cooldowns(SimplexSolver *solver,
                            SimplexTableau *tab,
                            P1RecoveryState *rs);

/* ── Zone 2: Pre-iteration ──────────────────────────────────────────── */

/* Handle stagnation escape, no-pivot force pending, and time limit check.
 * Returns P1_ZONE_CONTINUE if a refactor+recompute was triggered,
 * P1_ZONE_RETURN_FAIL on time limit, P1_ZONE_PROCEED otherwise. */
P1ZoneResult p1_zone_pre_iter(SimplexSolver *solver,
                              SimplexTableau *tab,
                              P1RecoveryState *rs,
                              int iter,
                              P1IterContext *ctx);

/* ── Zone 3: Pricing ────────────────────────────────────────────────── */

/* Select entering variable via pricing dispatch, handle exclusion rerouting,
 * and process no-entering (feasibility check / infeasibility detection).
 * On P1_ZONE_PROCEED, ctx->entering is set and ratio test is already done
 * (ctx->leaving, ctx->theta, ctx->ratio_status are set). */
P1ZoneResult p1_zone_pricing(SimplexSolver *solver,
                             SimplexTableau *tab,
                             P1RecoveryState *rs,
                             int iter,
                             P1IterContext *ctx);

/* ── Zone 4: Ratio breakdown ────────────────────────────────────────── */

/* Handle ratio test failure: escalation ladder from retry → rescue →
 * refactor → redundant → dual → breakdown.
 * Only called when ctx->ratio_status != 0.
 * Returns P1_ZONE_CONTINUE on recovery, P1_ZONE_RETURN_FAIL on breakdown,
 * P1_ZONE_PROCEED if ratio test passed (shouldn't normally be called). */
P1ZoneResult p1_zone_ratio_breakdown(SimplexSolver *solver,
                                     SimplexTableau *tab,
                                     P1RecoveryState *rs,
                                     int iter,
                                     P1IterContext *ctx);

/* ── Zone 5: Direction guard ────────────────────────────────────────── */

/* Guard against numerically explosive search directions.  Contains the
 * force-decision computation, defer paths (moderate/cooldown/ladder), and
 * failed-stabilize retry with inner loop.
 * Returns P1_ZONE_CONTINUE on skip/defer, P1_ZONE_PROCEED on stable direction. */
P1ZoneResult p1_zone_direction_guard(SimplexSolver *solver,
                                     SimplexTableau *tab,
                                     P1RecoveryState *rs,
                                     int iter,
                                     P1IterContext *ctx);

/* ── Zone 6: Pivot ──────────────────────────────────────────────────── */

/* Handle the alt-leaving retry for repeated failures, degeneracy tracking,
 * and the actual pivot call.  On pivot failure, runs the escalation ladder.
 * Returns P1_ZONE_CONTINUE on recovery, P1_ZONE_RETURN_FAIL on breakdown,
 * P1_ZONE_PROCEED on successful pivot. */
P1ZoneResult p1_zone_pivot(SimplexSolver *solver,
                           SimplexTableau *tab,
                           P1RecoveryState *rs,
                           int iter,
                           P1IterContext *ctx);

/* ── Zone 7: Post-pivot ─────────────────────────────────────────────── */

/* Reset success-dependent state after a successful pivot. */
void p1_zone_post_pivot_reset(SimplexSolver *solver,
                              SimplexTableau *tab,
                              P1RecoveryState *rs);

/* Artificial-feasibility progress window for large Phase 1 stalls. */
P1ZoneResult p1_zone_stall_detect(SimplexSolver *solver,
                                  SimplexTableau *tab,
                                  P1RecoveryState *rs,
                                  int iter);

/* Periodic refactorization: LU health check, periodic policy, reinvert
 * controller, cost gates, and refactorization with recovery.
 * Returns P1_ZONE_CONTINUE on recovery-refactor that needs to restart
 * the iteration, P1_ZONE_RETURN_FAIL on unrecoverable failure,
 * P1_ZONE_PROCEED on normal continuation. */
P1ZoneResult p1_zone_periodic_refactor(SimplexSolver *solver,
                                       SimplexTableau *tab,
                                       P1RecoveryState *rs,
                                       int iter,
                                       P1IterContext *ctx);

#endif /* SIMPLEX_PHASE1_ZONES_H */
