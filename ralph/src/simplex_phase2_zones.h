/*
 * simplex_phase2_zones.h - Phase 2 zone handlers for the simplex method.
 *
 * The Phase 2 main loop in simplex.c processes 6 sequential zones per
 * iteration.  Each zone is extracted into its own handler function, reducing
 * the main loop body from ~770 lines to ~80 lines of orchestration.
 *
 * Zero behavior change — pure code motion.
 *
 * Zone flow per iteration:
 *   1. pre_iter     — cooldown tick, objective limit check, user callbacks
 *   2. pricing      — select entering variable, confirm optimality on no-entering
 *   3. ratio        — ratio test + ratio failure recovery (refactorize/retry)
 *   4. pre_pivot    — direction geometry, repeat tracking, degeneracy + perturbation/Bland
 *   5. pivot        — simplex_pivot + failure recovery
 *   6. post_pivot   — refactor policy, periodic recompute, stall detection
 */

#ifndef SIMPLEX_PHASE2_ZONES_H
#define SIMPLEX_PHASE2_ZONES_H

#include "lp.h"

/* ── Zone return codes ─────────────────────────────────────────────── */

typedef enum {
    P2_ZONE_PROCEED,       /* fall through to next zone */
    P2_ZONE_CONTINUE,      /* caller does `continue` (restart iteration) */
    P2_ZONE_RETURN_OPTIMAL,/* Phase 2 optimal — caller returns 0 */
    P2_ZONE_RETURN_FAIL    /* unbounded/error — caller returns -1 */
} P2ZoneResult;

/* ── Per-iteration mutable state (locals crossing zone boundaries) ── */

typedef struct {
    /* Cycling detection */
    int degenerate_count;
    int non_degen_streak;
    int use_bland;

    /* Stall detection */
    double last_obj;
    int stall_count;
    int perturb_attempts;

    /* Perturbation */
    int perturbation_active;

    /* Repeat entering/leaving tracking */
    int last_entering;
    int last_leaving;
    int repeat_entering_streak;
    int repeat_leaving_streak;

    /* Two-phase Bland start */
    int bland_start_iters;

    /* Refactor policy state */
    int periodic_policy_cooldown;
    double periodic_policy_pressure_decay;
    int lu_soft_health_streak;

    /* Timing */
    double phase2_hot_ms_prev;

    /* Per-iteration pricing/ratio/pivot results */
    int entering;
    int leaving;
    double theta;
    int ratio_status;
    int price_status;
    int adaptive_devex_partial;
} P2IterState;

/* ── Zone constants (matching simplex.c originals) ─────────────────── */

#define P2_DEGEN_THRESHOLD 50
#define P2_NON_DEGEN_THRESHOLD 100
#define P2_STALL_THRESHOLD 50
#define P2_MAX_PERTURB_ATTEMPTS 15
#define P2_NEAR_DEGEN_TOL 1e-3
#define P2_PERTURB_THRESHOLD 30

/* ── Zone 1: Pre-iteration ─────────────────────────────────────────── */

/* Cooldown tick, objective limit check, user callbacks.
 * Returns P2_ZONE_RETURN_OPTIMAL on objective limit reached,
 * P2_ZONE_RETURN_FAIL on time limit / user cancel,
 * P2_ZONE_PROCEED otherwise. */
P2ZoneResult p2_zone_pre_iter(SimplexSolver *solver,
                              SimplexTableau *tab,
                              P2IterState *st,
                              int iter);

/* ── Zone 2: Pricing ───────────────────────────────────────────────── */

/* Select entering variable via pricing dispatch.
 * On no-entering, confirms optimality (refactorize + Dantzig recheck).
 * Returns P2_ZONE_RETURN_OPTIMAL on confirmed optimal,
 * P2_ZONE_RETURN_FAIL on error,
 * P2_ZONE_PROCEED with st->entering set. */
P2ZoneResult p2_zone_pricing(SimplexSolver *solver,
                             SimplexTableau *tab,
                             P2IterState *st,
                             int iter);

/* ── Zone 3: Ratio test ────────────────────────────────────────────── */

/* Ratio test + ratio failure recovery (refactorize → re-price → retry).
 * On unrecoverable failure, declares UNBOUNDED.
 * Returns P2_ZONE_RETURN_OPTIMAL if re-pricing finds optimal,
 * P2_ZONE_RETURN_FAIL on unbounded/error,
 * P2_ZONE_PROCEED with st->leaving, st->theta set. */
P2ZoneResult p2_zone_ratio(SimplexSolver *solver,
                           SimplexTableau *tab,
                           P2IterState *st,
                           int iter);

/* ── Zone 4: Pre-pivot ─────────────────────────────────────────────── */

/* Direction geometry telemetry, repeat entering/leaving tracking,
 * degeneracy detection + perturbation/Bland switching.
 * Always returns P2_ZONE_PROCEED. */
P2ZoneResult p2_zone_pre_pivot(SimplexSolver *solver,
                               SimplexTableau *tab,
                               P2IterState *st,
                               int iter);

/* ── Zone 5: Pivot ─────────────────────────────────────────────────── */

/* Perform simplex_pivot + failure recovery (refactorize or basis repair).
 * Returns P2_ZONE_CONTINUE on recovery (restart iteration),
 * P2_ZONE_RETURN_FAIL on unrecoverable failure,
 * P2_ZONE_PROCEED on successful pivot. */
P2ZoneResult p2_zone_pivot(SimplexSolver *solver,
                           SimplexTableau *tab,
                           P2IterState *st,
                           int iter);

/* ── Zone 6: Post-pivot ────────────────────────────────────────────── */

/* Refactor policy (LU health + periodic + soft cost + reinvert controller +
 * basis governor), periodic recompute, stall detection.
 * Returns P2_ZONE_CONTINUE on stall-escape (restart iteration),
 * P2_ZONE_RETURN_FAIL on unrecoverable refactor failure,
 * P2_ZONE_PROCEED on normal continuation. */
P2ZoneResult p2_zone_post_pivot(SimplexSolver *solver,
                                SimplexTableau *tab,
                                P2IterState *st,
                                int iter);

#endif /* SIMPLEX_PHASE2_ZONES_H */
