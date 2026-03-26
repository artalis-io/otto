/*
 * simplex_phase1_zones.c - Phase 1 crisis zone handler implementations.
 *
 * Each function corresponds to one crisis zone in the Phase 1 main loop.
 * The main loop in simplex.c calls these sequentially per iteration.
 *
 * Zero behavior change — pure code motion from simplex.c.
 */

#include "simplex_phase1_zones.h"
#include "simplex_internal.h"
#include "simplex_pricing.h"
#include "simplex_ratio.h"
#include "simplex_perturb.h"
#include "lp_refactor_policy.h"
#include "lp_glpk_strict.h"
#include "lp_basis_governor.h"
#include "lp_log.h"

#include <math.h>
#include <limits.h>

/* ── Shared helpers ─────────────────────────────────────────────────── */

typedef enum {
    P1_DIR_DEFER_MODERATE,    /* sets moderate_defer_pending = 1 */
    P1_DIR_DEFER_COOLDOWN,    /* updates cooldown, clears moderate_defer */
    P1_DIR_DEFER_LADDER       /* updates cooldown only */
} P1DirDeferReason;

/* Core defer logic shared by 3 paths in p1_zone_direction_guard():
 * moderate-defer, cooldown-skip, and no-pivot ladder-defer.
 * Encapsulates: note_dir_skip → exclude_entering → bump streak →
 * use_bland → recompute_dir_skip_safe → activate_force_pivot →
 * progress_update (moderate/cooldown only). */
static void p1_dir_skip_defer_core(SimplexSolver *solver,
                                   SimplexTableau *tab,
                                   P1RecoveryState *rs,
                                   int entering,
                                   P1DirDeferReason reason,
                                   int dir_stabilize_cooldown_target)
{
    /* Per-reason state setup */
    if (reason == P1_DIR_DEFER_MODERATE) {
        rs->numerical.dir_stabilize_moderate_defer_pending = 1;
    } else if (reason == P1_DIR_DEFER_COOLDOWN) {
        rs->numerical.dir_stabilize_moderate_defer_pending = 0;
        if (dir_stabilize_cooldown_target > rs->numerical.dir_stabilize_cooldown) {
            rs->numerical.dir_stabilize_cooldown = dir_stabilize_cooldown_target;
        }
    } else { /* P1_DIR_DEFER_LADDER */
        if (dir_stabilize_cooldown_target > rs->numerical.dir_stabilize_cooldown) {
            rs->numerical.dir_stabilize_cooldown = dir_stabilize_cooldown_target;
        }
    }

    p1_numerical_note_dir_skip_entering(solver, entering, &rs->numerical);
    p1_basis_exclude_entering(solver, entering,
                              RALPH_PHASE1_ENTERING_EXCLUDE_ITERS, &rs->basis);
    if (rs->numerical.dir_skip_event_streak < INT_MAX) {
        rs->numerical.dir_skip_event_streak++;
    }
    rs->cycling.use_bland = 1;
    phase1_recompute_dir_skip_safe(solver,
                                   tab,
                                   rs->cycling.degenerate_count,
                                   rs->progress.no_pivot_streak + 1,
                                   &rs->numerical.rc_only_streak,
                                   &rs->numerical.dir_skip_no_recompute_streak);
    if (p1_progress_activate_force_pivot(
            solver,
            tab->m,
            rs->cycling.degenerate_count,
            1,
            &rs->numerical.dir_skip_event_streak,
            &rs->progress,
            &rs->progress.no_pivot_force_pending,
            &rs->progress.no_pivot_force_reason) &&
        solver->verbose >= 2) {
        LP_LOG_STDERR("[simplex_phase1] Force-pivot mode activated after repeated dir-skip/no-recompute (budget=%d)\n",
                rs->progress.force_pivot_attempt_budget);
    }
    /* Moderate-defer and cooldown-skip need a progress snapshot here;
     * ladder-defer already called p1_progress_update before the ladder
     * step check, so skip it to avoid double-counting window steps. */
    if (reason != P1_DIR_DEFER_LADDER) {
        p1_progress_update(solver, tab, &rs->progress);
    }
}

/* ── Zone 1: Cooldown tick ──────────────────────────────────────────── */

void p1_zone_tick_cooldowns(SimplexSolver *solver,
                            SimplexTableau *tab,
                            P1RecoveryState *rs)
{
    if (rs->basis.excluded_entering_ttl_a > 0) {
        rs->basis.excluded_entering_ttl_a--;
        if (rs->basis.excluded_entering_ttl_a == 0) {
            rs->basis.excluded_entering_a = -1;
        }
    }
    if (rs->basis.excluded_entering_ttl_b > 0) {
        rs->basis.excluded_entering_ttl_b--;
        if (rs->basis.excluded_entering_ttl_b == 0) {
            rs->basis.excluded_entering_b = -1;
        }
    }
    if (rs->numerical.dir_stabilize_cooldown > 0) {
        rs->numerical.dir_stabilize_cooldown--;
    }
    if (rs->progress.no_pivot_force_cooldown > 0) {
        rs->progress.no_pivot_force_cooldown--;
    }
    if (rs->progress.no_pivot_ladder_rescue_cooldown > 0) {
        rs->progress.no_pivot_ladder_rescue_cooldown--;
    }
    if (rs->progress.dir_escape_cooldown > 0) {
        rs->progress.dir_escape_cooldown--;
    }
    rs->shared.periodic_policy_cooldown =
        lp_refactor_policy_periodic_cooldown_tick(rs->shared.periodic_policy_cooldown);
#if PHASE1_STAGNATION_ESCAPE_RUNTIME
    if (tab->m >= PHASE1_STAGNATION_MIN_M &&
        solver->policy.phase1_stagnation.escape_cooldown > 0) {
        solver->policy.phase1_stagnation.escape_cooldown--;
    }
#else
    (void)solver;
    (void)tab;
#endif
    rs->shared.periodic_policy_pressure_decay =
        lp_refactor_policy_periodic_pressure_decay_recover(
            1, rs->shared.periodic_policy_pressure_decay);
}

/* ── Zone 2: Pre-iteration ──────────────────────────────────────────── */

P1ZoneResult p1_zone_pre_iter(SimplexSolver *solver,
                              SimplexTableau *tab,
                              P1RecoveryState *rs,
                              int iter,
                              P1IterContext *ctx)
{
    (void)ctx;

    /* Auto-Dantzig pricing switch for large degenerate Phase 1 */
    if (!rs->cycling.auto_dantzig_enabled &&
        solver->phase1_pricing < 0 &&
        tab->use_two_phase &&
        tab->m >= PHASE1_AUTO_DANTZIG_MIN_M &&
        tab->m <= PHASE1_AUTO_DANTZIG_MAX_M &&
        rs->cycling.degenerate_count >= PHASE1_AUTO_DANTZIG_DEGEN_TRIGGER) {
        rs->cycling.pricing_strategy = 0;  /* Dantzig */
        rs->cycling.auto_dantzig_enabled = 1;
        if (solver->verbose >= 2) {
            LP_LOG_STDERR("[simplex_phase1] Switching pricing to Dantzig under large degenerate Phase 1 workload (m=%d, degen=%d)\n",
                    tab->m, rs->cycling.degenerate_count);
        }
    }

    /* No-pivot force pending: refactorize and reset */
    if (rs->progress.no_pivot_force_pending) {
        rs->progress.no_pivot_force_pending = 0;
        if (solver->verbose >= 2) {
            LP_LOG_STDERR("[simplex_phase1] No-pivot streak force refactor (%s)\n",
                    lp_refactor_policy_phase1_no_pivot_force_reason_string((int)rs->progress.no_pivot_force_reason));
        }
        rs->progress.no_pivot_force_reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
        lp_telemetry_record_phase1_dir_stabilize_refactor_trigger(
            solver,
            PHASE1_DIR_REFACTOR_TELEM_NO_PIVOT_FORCE);
        if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) == 0) {
            rs->cycling.use_bland = 1;
            rs->basis.ratio_breakdown_count = 0;
            rs->basis.ratio_breakdown_last_entering = -1;
            rs->basis.ratio_breakdown_same_entering_streak = 0;
            rs->numerical.dir_skip_event_streak = 0;
            rs->numerical.dir_skip_no_recompute_streak = 0;
            rs->numerical.dir_force_refactor_streak = 0;
            rs->progress.dir_escape_cooldown = 0;
            p1_progress_reset(&rs->progress);
            rs->progress.no_pivot_ladder_rescue_cooldown = 0;
            rs->progress.no_pivot_ladder_rescue_fail_streak = 0;
            phase1_recompute_full_with_reason(
                solver,
                tab,
                &rs->numerical.rc_only_streak,
                LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR);
            return P1_ZONE_CONTINUE;
        }
    }

#if PHASE1_STAGNATION_ESCAPE_RUNTIME
    /* Stagnation escape: refactorize when Phase 1 stalls */
    if (tab->m >= PHASE1_STAGNATION_MIN_M &&
        phase1_stagnation_escape_should_trigger(solver, tab, iter)) {
        if (solver->verbose >= 2) {
            LP_LOG_STDERR("[simplex_phase1] Stagnation escape trigger: obj_delta=%g retry_ratio=%.3f update_ratio=%.3f recompute=[ratio=%d dir_skip=%d dir_ref=%d piv=%d pert=%d] window=%d\n",
                    solver->policy.phase1_stagnation.last_obj_delta,
                    solver->policy.phase1_stagnation.last_retry_defer_ratio,
                    solver->policy.phase1_stagnation.last_update_recovery_ratio,
                    solver->policy.phase1_stagnation.last_recompute_ratio,
                    solver->policy.phase1_stagnation.last_recompute_dir_skip,
                    solver->policy.phase1_stagnation.last_recompute_dir_refactor,
                    solver->policy.phase1_stagnation.last_recompute_pivot_fail,
                    solver->policy.phase1_stagnation.last_recompute_perturb,
                    solver->policy.phase1_stagnation.last_window_iters);
        }
        if (tableau_refactorize_with_reason(
                tab,
                RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) == 0) {
            solver->policy.phase1_stagnation.escape_successes++;
            solver->policy.phase1_stagnation.escape_cooldown =
                PHASE1_STAGNATION_ESCAPE_COOLDOWN_ITERS;
            rs->cycling.use_bland = 1;
            rs->progress.no_pivot_streak = 0;
            rs->basis.ratio_breakdown_count = 0;
            rs->basis.ratio_breakdown_last_entering = -1;
            rs->basis.ratio_breakdown_same_entering_streak = 0;
            rs->numerical.dir_skip_event_streak = 0;
            rs->numerical.dir_skip_no_recompute_streak = 0;
            rs->progress.dir_escape_cooldown = 0;
            p1_progress_reset(&rs->progress);
            rs->progress.no_pivot_ladder_rescue_cooldown = 0;
            rs->progress.no_pivot_ladder_rescue_fail_streak = 0;
            phase1_recompute_full_with_reason(
                solver,
                tab,
                &rs->numerical.rc_only_streak,
                LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR);
            phase1_stagnation_window_begin(solver, tab, iter);
            return P1_ZONE_CONTINUE;
        }
        solver->policy.phase1_stagnation.escape_failures++;
        if (solver->policy.phase1_stagnation.escape_cooldown <
            PHASE1_STAGNATION_ESCAPE_FAIL_COOLDOWN_ITERS) {
            solver->policy.phase1_stagnation.escape_cooldown =
                PHASE1_STAGNATION_ESCAPE_FAIL_COOLDOWN_ITERS;
        }
        phase1_stagnation_window_begin(solver, tab, iter);
    }
#else
    (void)iter;
#endif

    return P1_ZONE_PROCEED;
}

/* ── Zone 3: Pricing ────────────────────────────────────────────────── */

P1ZoneResult p1_zone_pricing(SimplexSolver *solver,
                             SimplexTableau *tab,
                             P1RecoveryState *rs,
                             int iter,
                             P1IterContext *ctx)
{
    int entering;
    int price_status;
    double t_pricing_ms = lp_telemetry_timer_start();

    price_status = pricing_dispatch(tab, rs->cycling.pricing_strategy, rs->cycling.use_bland,
                                    0, iter, &entering);

    if ((rs->basis.excluded_entering_ttl_a > 0 || rs->basis.excluded_entering_ttl_b > 0) &&
        entering >= 0 &&
        (entering == rs->basis.excluded_entering_a || entering == rs->basis.excluded_entering_b)) {
        int alt_entering = -1;
        int rerouted = 0;
        int exclude_a = (rs->basis.excluded_entering_ttl_a > 0) ? rs->basis.excluded_entering_a : -1;
        int exclude_b = (rs->basis.excluded_entering_ttl_b > 0) ? rs->basis.excluded_entering_b : -1;
        if (pricing_bland_excluding_two(tab, exclude_a, exclude_b, &alt_entering) == 0) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Excluding unstable entering (%d,%d), using %d instead\n",
                        exclude_a, exclude_b, alt_entering);
            }
            entering = alt_entering;
            rerouted = 1;
        }
        lp_telemetry_record_phase1_entering_exclusion_hit(solver, rerouted);
    }
    {
        lp_telemetry_record_pricing_timed(solver, 1, t_pricing_ms);
    }

    if (price_status != 0) {
        double art_sum;
        phase1_trace_record_no_entering(solver, iter, price_status);

        /* Optimal for Phase 1 - remove perturbation first, then check */
        primal_remove_perturbation(tab);

        /* Recompute solution without perturbation */
        tab->phase1_compute_solution_context =
            LP_PHASE1_COMPUTE_CTX_NO_ENTERING_CLEANUP;
        tableau_compute_solution(tab);

        /* Check if all artificial variables are zero */
        art_sum = 0.0;
        for (int k = 0; k < tab->num_artificial; k++) {
            int j = tab->artificial_vars[k];
            art_sum += fabs(tab->x[j]);
        }

        if (art_sum > RALPH_FEAS_TOL) {
            /* Small residual might be fixable with a few more iterations.
             * Use a relaxed tolerance (1e-4) to distinguish true infeasibility
             * from numerical noise. */
            if (art_sum > 1e-4) {
                rs->progress.no_entering_cleanup_streak = 0;
                /* Revalidate on a freshly factorized basis before certifying infeasible.
                 * This guards against RC/solution drift on numerically hard instances. */
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
                    tab->phase1_compute_solution_context =
                        LP_PHASE1_COMPUTE_CTX_INFEAS_CLEANUP;
                    tab->phase1_compute_rc_context =
                        LP_PHASE1_COMPUTE_CTX_INFEAS_CLEANUP;
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);

                    double refined_art_sum = 0.0;
                    for (int k = 0; k < tab->num_artificial; k++) {
                        int j = tab->artificial_vars[k];
                        refined_art_sum += fabs(tab->x[j]);
                    }

                    if (refined_art_sum <= 1e-4) {
                        if (solver->verbose) {
                            LP_LOG_STDERR("[simplex_phase1] Refactorized cleanup: art_sum %g -> %g\n",
                                    art_sum, refined_art_sum);
                        }
                        return P1_ZONE_CONTINUE;
                    }
                    art_sum = refined_art_sum;
                }

                /* Truly infeasible - extract Farkas ray from Phase 1 duals.
                 * The Phase 1 duals y = c_B^T * B^{-1} provide the certificate. */
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] INFEASIBLE: artificial sum = %g after %d iterations\n",
                            art_sum, iter);
                }
                extract_farkas_ray(solver);
                solver->current_phase = SIMPLEX_PHASE_INFEASIBLE;
                solver->status = RALPH_STATUS_INFEASIBLE;
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_INFEASIBLE);
                return P1_ZONE_RETURN_FAIL;
            }

            /* Small residual - try to clean up with a few more iterations */
            rs->progress.no_entering_cleanup_streak++;
            if (rs->progress.no_entering_cleanup_streak >= PHASE1_NO_ENTERING_CLEANUP_MAX_ITERS) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Accepting Phase 1 feasibility after %d no-entering cleanup iterations (art_sum=%g)\n",
                            rs->progress.no_entering_cleanup_streak, art_sum);
                }
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
                return P1_ZONE_RETURN_OK;
            }
            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] Cleanup phase: art_sum=%g, continuing...\n", art_sum);
            }
            phase1_recompute_rc_only_guarded(solver, tab, &rs->numerical.rc_only_streak);
            return P1_ZONE_CONTINUE;
        }

        /* Success */
        rs->progress.no_entering_cleanup_streak = 0;
        if (solver->verbose) {
            LP_LOG_STDERR("[simplex_phase1] Phase 1 complete: feasible in %d iterations\n", iter);
        }
        solver->iterations = iter;
        phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
        return P1_ZONE_RETURN_OK;
    }

    rs->progress.no_entering_cleanup_streak = 0;

    /* Ratio test: select leaving variable */
    {
        double t_ratio_ms = lp_telemetry_timer_start();
        ctx->ratio_status = primal_ratio_test_with_policy(solver,
                                                          tab,
                                                          0,
                                                          entering,
                                                          &ctx->leaving,
                                                          &ctx->theta);
        lp_telemetry_record_ratio_timed(solver, 1, t_ratio_ms);
    }

    ctx->entering = entering;
    return P1_ZONE_PROCEED;
}

/* ── Zone 4: Ratio breakdown ──────────────────────────────────────── */

P1ZoneResult p1_zone_ratio_breakdown(SimplexSolver *solver,
                                     SimplexTableau *tab,
                                     P1RecoveryState *rs,
                                     int iter,
                                     P1IterContext *ctx)
{
    int entering = ctx->entering;

    rs->numerical.shadow_guard_followup_direction_pending = 0;
    rs->numerical.force_extreme_followup_direction_pending = 0;
    rs->numerical.force_extreme_followup_bound_flip_streak = 0;
    rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
    p1_numerical_consume_followup(solver, P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN, &rs->numerical);
    phase1_trace_record_no_entering(solver, iter, ctx->ratio_status);
    if (p1_progress_note_no_pivot(
            solver,
            tab->m,
            rs->cycling.degenerate_count,
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            &rs->progress)) {
        rs->progress.no_pivot_force_pending = 1;
        rs->progress.no_pivot_force_reason =
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN;
    }

    p1_progress_update(solver, tab, &rs->progress);
    {
        int no_pivot_ladder_threshold = 0;
        int no_pivot_force_mode_active =
            (rs->progress.force_pivot_attempt_budget > 0);
        int no_pivot_ladder_step = phase1_no_pivot_ladder_step(
            solver,
            tab->m,
            rs->cycling.degenerate_count,
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            rs->progress.no_pivot_streak,
            rs->progress.no_pivot_no_progress_streak,
            no_pivot_force_mode_active,
            &no_pivot_ladder_threshold);
        no_pivot_ladder_step = phase1_no_pivot_ladder_apply_rescue_guard(
            solver,
            no_pivot_ladder_step,
            rs->progress.no_pivot_ladder_rescue_cooldown,
            rs->progress.no_pivot_ladder_rescue_fail_streak);
        if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_RETRY) {
            lp_telemetry_record_phase1_no_pivot_ladder_retry(
                solver,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN);
            lp_telemetry_record_phase1_ratio_breakdown_retry(solver);
            p1_basis_exclude_entering(solver, entering,
                                      RALPH_PHASE1_ENTERING_EXCLUDE_ITERS, &rs->basis);
            rs->cycling.use_bland = 1;
            phase1_recompute_rc_only_guarded(solver,
                                             tab,
                                             &rs->numerical.rc_only_streak);
            return P1_ZONE_CONTINUE;
        }
        if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
            int rescue_result = p1_progress_attempt_ladder_rescue(
                solver,
                tab,
                iter,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
                LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN,
                &rs->progress,
                &rs->numerical.rc_only_streak);
            if (rescue_result == 1) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] No-pivot ladder dual rescue succeeded at iter %d (streak=%d no_progress=%d threshold=%d)\n",
                            iter,
                            rs->progress.no_pivot_streak,
                            rs->progress.no_pivot_no_progress_streak,
                            no_pivot_ladder_threshold);
                }
                rs->basis.ratio_breakdown_count = 0;
                rs->basis.ratio_breakdown_last_entering = -1;
                rs->basis.ratio_breakdown_same_entering_streak = 0;
                return P1_ZONE_CONTINUE;
            }
            if (rescue_result < 0) {
                return P1_ZONE_RETURN_FAIL;
            }
            rs->cycling.use_bland = 1;
            lp_telemetry_record_phase1_ratio_breakdown_retry(solver);
            phase1_recompute_rc_only_guarded(solver,
                                             tab,
                                             &rs->numerical.rc_only_streak);
            return P1_ZONE_CONTINUE;
        }
        lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
            solver,
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN);
    }

    /* "Unbounded" in Phase 1 is typically numerical, not structural.
     * Try to recover via refactorization and conservative pricing first. */
    if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_RATIO_RECOVERY) == 0) {
        p1_progress_reset(&rs->progress);
        rs->progress.no_pivot_ladder_rescue_cooldown = 0;
        rs->progress.no_pivot_ladder_rescue_fail_streak = 0;
        phase1_recompute_full_with_reason(
            solver,
            tab,
            &rs->numerical.rc_only_streak,
            LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN);
        rs->cycling.use_bland = 1;
        return P1_ZONE_CONTINUE;
    }
    if (!rs->cycling.use_bland) {
        rs->cycling.use_bland = 1;
        phase1_recompute_rc_only_guarded(solver, tab, &rs->numerical.rc_only_streak);
        return P1_ZONE_CONTINUE;
    }

    /* Last-chance recovery before treating Phase-1 "unbounded" as
     * numerical breakdown. */
    int marked = mark_basic_artificial_rows_redundant(tab, 1);
    if (marked > 0) {
        if (solver->verbose >= 2) {
            LP_LOG_STDERR("[simplex_phase1] Marked %d infeasible artificial rows as redundant after ratio-test breakdown\n",
                    marked);
        }
        if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
            phase1_recompute_full_with_reason(
                solver,
                tab,
                &rs->numerical.rc_only_streak,
                LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN);
            return P1_ZONE_CONTINUE;
        }
    }

    int rescue_status = p1_progress_attempt_direct_rescue(
        solver, tab, iter, &rs->progress);
    if (rescue_status == 0) {
        if (solver->verbose) {
            LP_LOG_STDERR("[simplex_phase1] Dual rescue recovered after ratio-test breakdown at iter %d\n", iter);
        }
        phase1_recompute_full_with_reason(
            solver,
            tab,
            &rs->numerical.rc_only_streak,
            LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN);
        rs->basis.ratio_breakdown_count = 0;
        rs->basis.ratio_breakdown_last_entering = -1;
        rs->basis.ratio_breakdown_same_entering_streak = 0;
        return P1_ZONE_CONTINUE;
    }
    if (solver->status == RALPH_STATUS_TIME_LIMIT) {
        primal_remove_perturbation(tab);
        solver->iterations = iter;
        phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
        return P1_ZONE_RETURN_FAIL;
    }

    rs->basis.ratio_breakdown_count++;
    if (entering == rs->basis.ratio_breakdown_last_entering) {
        if (rs->basis.ratio_breakdown_same_entering_streak < 1000000) {
            rs->basis.ratio_breakdown_same_entering_streak++;
        }
    } else {
        rs->basis.ratio_breakdown_last_entering = entering;
        rs->basis.ratio_breakdown_same_entering_streak = 1;
    }
    p1_basis_exclude_entering(solver, entering,
                              RALPH_PHASE1_ENTERING_EXCLUDE_ITERS, &rs->basis);
    {
        int ratio_breakdown_limit =
            lp_refactor_policy_phase1_ratio_breakdown_limit(
                tab->m,
                rs->basis.ratio_breakdown_same_entering_streak);
        if (rs->basis.ratio_breakdown_count < ratio_breakdown_limit) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Continuing after ratio-test breakdown (count=%d, entering=%d streak=%d limit=%d), excluding entering for %d iterations\n",
                        rs->basis.ratio_breakdown_count,
                        entering,
                        rs->basis.ratio_breakdown_same_entering_streak,
                        ratio_breakdown_limit,
                        RALPH_PHASE1_ENTERING_EXCLUDE_ITERS);
            }
            rs->cycling.use_bland = 1;
            lp_telemetry_record_phase1_ratio_breakdown_retry(solver);
            phase1_recompute_rc_only_guarded(solver, tab, &rs->numerical.rc_only_streak);
            return P1_ZONE_CONTINUE;
        }
    }
    lp_telemetry_record_phase1_ratio_breakdown_escalation(solver);

    if (solver->verbose) {
        LP_LOG_STDERR("[simplex_phase1] ERROR: unbounded in Phase 1 at iter %d (after recovery)\n", iter);
    }
    primal_remove_perturbation(tab);
    /* Treat unrecoverable Phase 1 "unbounded" as numerical breakdown. */
    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    solver->iterations = iter;
    phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
    return P1_ZONE_RETURN_FAIL;
}

/* ── Zone 6: Pivot ─────────────────────────────────────────────────── */

P1ZoneResult p1_zone_pivot(SimplexSolver *solver,
                           SimplexTableau *tab,
                           P1RecoveryState *rs,
                           int iter,
                           P1IterContext *ctx)
{
    int entering = ctx->entering;
    int leaving = ctx->leaving;
    double theta = ctx->theta;

    /* If we are retrying the same failing entering/leaving pair, force an
     * alternate leaving choice from the current direction to escape loops. */
    if (rs->basis.fail_repeat_count > 0 &&
        entering == rs->basis.fail_entering &&
        leaving == rs->basis.fail_leaving_pos &&
        leaving >= 0) {
        int alt_leaving = -1;
        double alt_theta = RALPH_INFINITY;
        if (ratio_test_harris_excluding_current(tab, entering, leaving,
                                                &alt_leaving, &alt_theta) == 0 &&
            alt_leaving >= 0) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Using alternate leaving row %d instead of repeatedly failing row %d\n",
                        alt_leaving, leaving);
            }
            leaving = alt_leaving;
            theta = alt_theta;
        }
    }

    /* Track degeneracy and apply anti-cycling measures */
    if (theta < RALPH_FEAS_TOL) {
        rs->cycling.degenerate_count++;

        /* In Phase 1, avoid reactive bound perturbation because it can
         * destabilize the feasibility objective; switch directly to Bland. */
        if (rs->cycling.degenerate_count > rs->cycling.degen_threshold && !rs->cycling.use_bland) {
            rs->cycling.use_bland = 1;
            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] Switching to Bland's rule after %d degenerate pivots\n",
                        rs->cycling.degenerate_count);
            }
        }
    }

    /* Perform pivot */
    int pivot_status;
    if (rs->progress.force_pivot_attempt_budget > 0) {
        lp_telemetry_record_phase1_force_pivot_budget_pivot_spend(solver);
        rs->progress.force_pivot_attempt_budget--;
    }
    {
        double t_pivot_ms = lp_telemetry_timer_start();
        pivot_status = simplex_pivot(tab, entering, leaving, theta, rs->basis.fail_repeat_count);
        lp_telemetry_record_pivot_timed(solver, 1, t_pivot_ms);
    }
    if (pivot_status != 0) {
        rs->numerical.shadow_guard_followup_direction_pending = 0;
        rs->numerical.force_extreme_followup_direction_pending = 0;
        rs->numerical.force_extreme_followup_bound_flip_streak = 0;
        rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
        p1_numerical_consume_followup(solver, P1_FOLLOWUP_EVENT_PIVOT_FAIL, &rs->numerical);
        if (p1_progress_note_no_pivot(
                solver,
                tab->m,
                rs->cycling.degenerate_count,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
                &rs->progress)) {
            rs->progress.no_pivot_force_pending = 1;
            rs->progress.no_pivot_force_reason =
                LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL;
        }
        int pivot_fail_reason = tab->trace_last_fail_reason;
        if (entering == rs->basis.fail_entering &&
            leaving == rs->basis.fail_leaving_pos &&
            pivot_fail_reason == rs->basis.fail_reason) {
            rs->basis.fail_repeat_count++;
        } else {
            rs->basis.fail_entering = entering;
            rs->basis.fail_leaving_pos = leaving;
            rs->basis.fail_reason = pivot_fail_reason;
            rs->basis.fail_repeat_count = 1;
        }

        phase1_trace_record_pivot_failure(solver, tab, iter, rs->basis.fail_repeat_count);

        if (solver->verbose) {
            if (rs->basis.fail_repeat_count <= RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER ||
                rs->basis.fail_repeat_count % 10 == 0) {
                LP_LOG_STDERR("[simplex_phase1] Pivot failed at iter %d (repeat %d), attempting recovery\n",
                        iter, rs->basis.fail_repeat_count);
            }
        }

        /* First recovery attempt: choose a different leaving row for the
         * same entering column to avoid a numerically singular pivot pair. */
        if (leaving >= 0) {
            int alt_leaving = -1;
            double alt_theta = RALPH_INFINITY;
            if (ratio_test_harris_excluding_current(tab, entering, leaving,
                                                    &alt_leaving, &alt_theta) == 0 &&
                alt_leaving >= 0 &&
                alt_leaving != leaving) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Retrying with alternate leaving row %d (failed row %d)\n",
                            alt_leaving, leaving);
                }
                int alt_pivot_status;
                {
                    double t_pivot_ms = lp_telemetry_timer_start();
                    alt_pivot_status = simplex_pivot(tab, entering, alt_leaving, alt_theta, rs->basis.fail_repeat_count);
                    lp_telemetry_record_pivot_timed(solver, 1, t_pivot_ms);
                }
                if (alt_pivot_status == 0) {
                    rs->basis.fail_reason = PHASE1_PIVOT_FAIL_NONE;
                    rs->basis.fail_repeat_count = 0;
                    rs->progress.no_pivot_streak = 0;
                    return P1_ZONE_PROCEED;
                } else {
                    phase1_trace_record_pivot_failure(solver, tab, iter, rs->basis.fail_repeat_count);
                }
            }
        }

        if (rs->basis.fail_repeat_count >= RALPH_PHASE1_FAIL_REPEAT_LIMIT) {
            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] Repeated pivot failure (%d) for entering=%d leaving_pos=%d, terminating as ITERATION_LIMIT\n",
                        rs->basis.fail_repeat_count, entering, leaving);
            }
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_ITERATION_LIMIT;
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
            return P1_ZONE_RETURN_FAIL;
        }

        if (rs->basis.fail_repeat_count >= PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER) {
            int no_pivot_force_mode_active =
                (rs->progress.force_pivot_attempt_budget > 0);
            int no_pivot_ladder_step;
            p1_progress_update(solver, tab, &rs->progress);
            no_pivot_ladder_step = phase1_no_pivot_ladder_step(
                solver,
                tab->m,
                rs->cycling.degenerate_count,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
                rs->progress.no_pivot_streak,
                rs->progress.no_pivot_no_progress_streak,
                no_pivot_force_mode_active,
                NULL);
            no_pivot_ladder_step = phase1_no_pivot_ladder_apply_rescue_guard(
                solver,
                no_pivot_ladder_step,
                rs->progress.no_pivot_ladder_rescue_cooldown,
                rs->progress.no_pivot_ladder_rescue_fail_streak);
            if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                int rescue_result = p1_progress_attempt_ladder_rescue(
                    solver,
                    tab,
                    iter,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
                    LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY,
                    &rs->progress,
                    &rs->numerical.rc_only_streak);
                if (rescue_result == 1) {
                    rs->progress.no_pivot_streak = 0;
                    rs->basis.fail_reason = PHASE1_PIVOT_FAIL_NONE;
                    rs->basis.fail_repeat_count = 0;
                    rs->cycling.use_bland = 1;
                    return P1_ZONE_CONTINUE;
                }
                if (rescue_result < 0) return P1_ZONE_RETURN_FAIL;
            } else if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_RETRY) {
                lp_telemetry_record_phase1_no_pivot_ladder_retry(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL);
            } else {
                lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL);
            }
        }

        /* simplex_pivot can leave basis/LU partially updated on failure.
         * Try the same recovery ladder used in Phase 2. */
        if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
            p1_basis_pivot_fail_maybe_exclude(solver,
                rs->basis.fail_repeat_count, entering, &rs->basis);
            rs->cycling.use_bland = 1;
            phase1_recompute_full_with_reason(
                solver,
                tab,
                &rs->numerical.rc_only_streak,
                LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
            return P1_ZONE_CONTINUE;
        }
        if (repair_singular_basis(tab) == 0) {
            p1_basis_pivot_fail_maybe_exclude(solver,
                rs->basis.fail_repeat_count, entering, &rs->basis);
            rs->cycling.use_bland = 1;
            phase1_recompute_full_with_reason(
                solver,
                tab,
                &rs->numerical.rc_only_streak,
                LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
            return P1_ZONE_CONTINUE;
        }

        /* Last structural recovery in Phase 1: if the problematic leaving
         * row is driven by an artificial basic variable, treat the row as
         * redundant and allow LU regularization to proceed. */
        if (leaving >= 0 && leaving < tab->m) {
            int leave_var = tab->basis[leaving];
            if (is_artificial_var(tab, leave_var) &&
                tab->redundant_rows &&
                !tab->redundant_rows[leaving]) {
                tab->redundant_rows[leaving] = 1;
                tab->num_redundant++;
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Marking row %d as redundant due to stuck artificial %d\n",
                            leaving, leave_var);
                }
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                    p1_basis_pivot_fail_maybe_exclude(solver,
                        rs->basis.fail_repeat_count, entering, &rs->basis);
                    rs->cycling.use_bland = 1;
                    phase1_recompute_full_with_reason(
                        solver,
                        tab,
                        &rs->numerical.rc_only_streak,
                        LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
                    return P1_ZONE_CONTINUE;
                }
            }
        }

        /* Broader recovery for heavily degenerate Phase 1 states:
         * mark all currently-basic artificial rows as potentially redundant
         * and retry a full refactorization. */
        int marked = mark_basic_artificial_rows_redundant(tab, 0);
        if (marked > 0) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Marked %d additional artificial rows as redundant for recovery\n",
                        marked);
            }
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                p1_basis_pivot_fail_maybe_exclude(solver,
                    rs->basis.fail_repeat_count, entering, &rs->basis);
                rs->cycling.use_bland = 1;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &rs->numerical.rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
                return P1_ZONE_CONTINUE;
            }
        }

        /* Final fallback for stuck Phase 1 states: try a bounded dual-simplex
         * rescue on the current tableau (no recursion to primal simplex). */
        int rescue_status = p1_progress_attempt_direct_rescue(
            solver, tab, iter, &rs->progress);
        if (rescue_status == 0) {
            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] Dual rescue restored feasibility progress at iter %d\n", iter);
            }
            p1_basis_pivot_fail_maybe_exclude(solver,
                rs->basis.fail_repeat_count, entering, &rs->basis);
            rs->cycling.use_bland = 1;
            phase1_recompute_full_with_reason(
                solver,
                tab,
                &rs->numerical.rc_only_streak,
                LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
            rs->basis.fail_reason = PHASE1_PIVOT_FAIL_NONE;
            rs->basis.fail_repeat_count = 0;
            return P1_ZONE_CONTINUE;
        }
        if (solver->status == RALPH_STATUS_TIME_LIMIT) {
            primal_remove_perturbation(tab);
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
            return P1_ZONE_RETURN_FAIL;
        }

        if (solver->verbose) {
            LP_LOG_STDERR("[simplex_phase1] ERROR: all pivot recovery attempts failed at iter %d\n", iter);
        }
        primal_remove_perturbation(tab);
        /* Treat unrecoverable Phase 1 pivot breakdown as numerical breakdown. */
        solver->status = RALPH_STATUS_ITERATION_LIMIT;
        solver->iterations = iter;
        phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
        return P1_ZONE_RETURN_FAIL;
    }
    return P1_ZONE_PROCEED;
}

/* ── Zone 7a: Post-pivot reset ─────────────────────────────────────── */

void p1_zone_post_pivot_reset(SimplexSolver *solver,
                              SimplexTableau *tab,
                              P1RecoveryState *rs)
{
    (void)tab;
    rs->progress.no_pivot_streak = 0;
    p1_progress_reset(&rs->progress);
    p1_window_pressure_reset(solver, &rs->progress);
    rs->progress.window_pressure_force_pivot_armed = 0;
    rs->progress.no_pivot_ladder_rescue_cooldown = 0;
    rs->progress.no_pivot_ladder_rescue_fail_streak = 0;
    rs->basis.fail_reason = PHASE1_PIVOT_FAIL_NONE;
    rs->basis.fail_repeat_count = 0;
    rs->basis.ratio_breakdown_count = 0;
    rs->basis.ratio_breakdown_last_entering = -1;
    rs->basis.ratio_breakdown_same_entering_streak = 0;
    rs->numerical.dir_skip_event_streak = 0;
    rs->progress.dir_escape_cooldown = 0;
    rs->numerical.dir_stabilize_moderate_defer_pending = 0;
    rs->numerical.rc_only_streak = 0;
    rs->numerical.dir_skip_no_recompute_streak = 0;
    rs->numerical.dir_force_refactor_streak = 0;
    rs->numerical.last_dir_skip_entering = -1;
    rs->numerical.dir_skip_same_entering_streak = 0;
    rs->basis.last_failed_stabilize_entering = -1;
    rs->basis.failed_stabilize_same_entering_streak = 0;
    rs->basis.last_failed_stabilize_retry_alt = -1;
    rs->basis.failed_stabilize_retry_alt_streak = 0;
    rs->basis.failed_stabilize_retry_alt_ratio_fail_streak = 0;
    rs->basis.excluded_entering_a = -1;
    rs->basis.excluded_entering_ttl_a = 0;
    rs->basis.excluded_entering_b = -1;
    rs->basis.excluded_entering_ttl_b = 0;
}

/* ── Zone 7b: Stall detection ──────────────────────────────────────── */

void p1_zone_stall_detect(SimplexSolver *solver,
                          SimplexTableau *tab,
                          P1RecoveryState *rs)
{
    /* Phase 1 stall detection: re-perturb when objective stalls.
     * This is critical for problems like recipe (80 artificials) where
     * Bland's rule grinds forever without making progress. */
    double obj_tol_p1 =
        lp_refactor_policy_phase1_stall_obj_tol(rs->cycling.last_obj);
    double obj_change_p1 = fabs(tab->obj_value - rs->cycling.last_obj);
    if (obj_change_p1 < obj_tol_p1) {
        rs->cycling.stall_count++;
        if (rs->cycling.stall_count >= rs->cycling.stall_threshold) {
            rs->cycling.perturb_attempts++;
            if (rs->cycling.perturb_attempts <= rs->cycling.max_perturb_attempts) {
                double scale = 1.0 + 2.0 * rs->cycling.perturb_attempts;
                primal_apply_perturbation_scaled(tab, scale);
                /* Reset Bland's to allow faster pricing */
                rs->cycling.use_bland = 0;
                rs->cycling.degenerate_count = 0;
                rs->cycling.stall_count = 0;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &rs->numerical.rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_PERTURB);
                if (rs->cycling.pricing_strategy == 4) heap_build(tab);
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Stall detected, re-perturbing (attempt %d, scale %.1f)\n",
                            rs->cycling.perturb_attempts, scale);
                }
            }
        }
    } else {
        rs->cycling.stall_count = 0;
        rs->cycling.last_obj = tab->obj_value;
    }
}

/* ── Zone 7c: Periodic refactorization ─────────────────────────────── */

P1ZoneResult p1_zone_periodic_refactor(SimplexSolver *solver,
                                       SimplexTableau *tab,
                                       P1RecoveryState *rs,
                                       int iter,
                                       P1IterContext *ctx)
{
    /* Record iteration timing */
    {
        double phase1_hot_ms_now = phase_hotpath_ms(solver, 1);
        double iter_hot_ms = phase1_hot_ms_now - ctx->phase1_hot_ms_prev;
        soft_lu_record_iter_cost(solver, 1, iter_hot_ms);
        lp_reinvert_controller_state_record_iter_cost(
            reinvert_state_for_phase(solver, 1), iter_hot_ms);
        ctx->phase1_hot_ms_prev = phase1_hot_ms_now;
    }
    LPLUHealthRefactorDecision lu_health_decision =
        lp_refactor_policy_lu_health_refactor_decision(tab->m,
                                                       lu_get_use_ft_updates(tab->lu),
                                                       lu_get_num_updates(tab->lu),
                                                       lu_get_max_updates(tab->lu),
                                                       lu_get_spike_pool_used(tab->lu),
                                                       lu_get_spike_pool_capacity(tab->lu),
                                                       lu_get_cond_estimate(tab->lu),
                                                       lu_get_growth_factor(tab->lu),
                                                       rs->shared.lu_soft_health_streak);
    int lu_refactor_nominal = lu_health_decision.refactor_now;
    int lu_refactor_needed = lu_health_decision.refactor_now;
    int lu_soft_cost_deferred = 0;
    int cooldown_eligible = 0;
    double effective_policy_pressure = 0.0;
    double periodic_feedback_bias = periodic_feedback_bias_for_phase(solver, 1);
    LPPeriodicRefactorPolicy periodic_policy =
        lp_refactor_policy_build_from_metrics(1,
                                              tab->m,
                                              lu_get_max_updates(tab->lu),
                                              lu_get_num_updates(tab->lu),
                                              lu_get_spike_pool_used(tab->lu),
                                              lu_get_spike_pool_capacity(tab->lu),
                                              lu_get_cond_estimate(tab->lu),
                                              lu_get_growth_factor(tab->lu),
                                              rs->cycling.use_bland,
                                              rs->cycling.degenerate_count,
                                              periodic_feedback_bias);
    int periodic_refactor = 0;
    int periodic_refactor_nominal = 0;
    int needs_refactor = lu_refactor_needed;
    int reinvert_periodic_candidate = 0;
    int reinvert_control_periodic = 0;
    LPReinvertShadowEval reinvert_shadow_eval;
    reinvert_shadow_eval_reset(&reinvert_shadow_eval);
    rs->shared.lu_soft_health_streak = lu_health_decision.soft_breach_streak_next;
    reinvert_phase1_pressure_safety_update(solver,
                                           iter,
                                           rs->progress.no_pivot_streak,
                                           rs->progress.no_pivot_no_progress_streak,
                                           rs->basis.ratio_breakdown_count,
                                           rs->numerical.dir_skip_no_recompute_streak,
                                           lu_health_decision.hard_trigger);
    reinvert_control_periodic = reinvert_controller_controls_periodic_phase(solver, 1);
    if (lu_health_decision.hard_trigger) {
        rs->shared.periodic_policy_cooldown = 0;
        rs->shared.periodic_policy_pressure_decay = 0.0;
        soft_lu_reset_defer_streak(solver, 1);
        periodic_cost_reset_defer_streak(solver, 1);
    } else if (lu_refactor_needed) {
        periodic_cost_reset_defer_streak(solver, 1);
    } else if (!lu_health_decision.soft_trigger || !solver->policy.soft_lu_cost_gate_enabled) {
        soft_lu_reset_defer_streak(solver, 1);
    }
    if (lu_refactor_needed &&
        solver->policy.soft_lu_cost_gate_enabled &&
        lu_health_decision.soft_trigger &&
        !lu_health_decision.hard_trigger) {
        int cap_blocked = 0;
        int next_consecutive = 0;
        int should_defer = simplex_soft_lu_defer_plan_for_test(
            1,
            tab->m,
            rs->cycling.use_bland,
            rs->cycling.degenerate_count,
            lu_get_num_updates(tab->lu),
            lu_get_max_updates(tab->lu),
            lu_get_spike_pool_used(tab->lu),
            lu_get_spike_pool_capacity(tab->lu),
            lu_get_cond_estimate(tab->lu),
            lu_get_growth_factor(tab->lu),
            soft_lu_refactor_cost_ewma(solver, 1),
            soft_lu_iter_cost_ewma(solver, 1),
            soft_lu_consecutive_defers(solver, 1),
            NULL,
            &cap_blocked,
            &next_consecutive);
        if (should_defer) {
            lu_refactor_needed = 0;
            lu_soft_cost_deferred = 1;
            soft_lu_record_defer(solver, 1);
            soft_lu_set_consecutive_defers(solver, 1, next_consecutive);
        } else {
            soft_lu_reset_defer_streak(solver, 1);
            if (cap_blocked) {
                soft_lu_record_cap_forced(solver, 1);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Soft LU defer cap reached; forcing periodic LU-health refactor (updates=%d/%d, degen=%d)\n",
                            lu_get_num_updates(tab->lu),
                            lu_get_max_updates(tab->lu),
                            rs->cycling.degenerate_count);
                }
            }
        }
    } else {
        rs->numerical.shadow_guard_followup_direction_pending = 0;
        rs->numerical.force_extreme_followup_direction_pending = 0;
        rs->numerical.force_extreme_followup_bound_flip_streak = 0;
        rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
        p1_numerical_consume_followup(solver, P1_FOLLOWUP_EVENT_PIVOT_SUCCESS, &rs->numerical);
    }
    if (lu_soft_cost_deferred) {
        int soft_policy_cooldown = phase1_soft_lu_policy_cooldown_updates(
            tab->m,
            rs->cycling.degenerate_count,
            periodic_policy.interval);
        int next_policy_cooldown =
            lp_refactor_policy_periodic_cooldown_extend(
                rs->shared.periodic_policy_cooldown,
                soft_policy_cooldown);
        if (next_policy_cooldown > rs->shared.periodic_policy_cooldown) {
            rs->shared.periodic_policy_cooldown = next_policy_cooldown;
            lp_telemetry_record_phase1_soft_lu_policy_cooldown_defer(solver);
        }
    }
    if (!lu_refactor_needed) {
        LPPeriodicRefactorPlan periodic_plan = lp_refactor_policy_periodic_plan(
            1,
            iter,
            tab->m,
            lu_get_max_updates(tab->lu),
            lu_get_num_updates(tab->lu),
            lu_get_spike_pool_used(tab->lu),
            lu_get_spike_pool_capacity(tab->lu),
            lu_get_cond_estimate(tab->lu),
            lu_get_growth_factor(tab->lu),
            rs->cycling.use_bland,
            rs->cycling.degenerate_count,
            periodic_feedback_bias,
            periodic_policy_refactor_count(solver, 1),
            rs->shared.periodic_policy_cooldown,
            rs->shared.periodic_policy_pressure_decay);
        periodic_policy = periodic_plan.policy;
        cooldown_eligible = periodic_plan.cooldown_eligible;
        effective_policy_pressure = periodic_plan.effective_run_pressure;
        periodic_refactor = periodic_plan.should_run;
        periodic_refactor_nominal = periodic_refactor;
        reinvert_periodic_candidate = periodic_refactor_nominal;
        reinvert_shadow_prepare_phase(solver,
                                      tab,
                                      1,
                                      iter,
                                      &lu_health_decision,
                                      periodic_refactor_nominal,
                                      periodic_policy.min_update_age,
                                      rs->shared.periodic_policy_cooldown,
                                      reinvert_control_periodic,
                                      &reinvert_periodic_candidate,
                                      &reinvert_shadow_eval);
        if (reinvert_control_periodic) {
            periodic_refactor = reinvert_periodic_candidate;
            periodic_refactor_nominal = periodic_refactor;
            periodic_cost_reset_defer_streak(solver, 1);
        } else {
            if (periodic_refactor &&
                cooldown_eligible &&
                rs->shared.periodic_policy_cooldown > 0) {
                periodic_refactor = 0;
            }
            if (periodic_refactor) {
                if (solver->policy.periodic_cost_gate_enabled) {
                    int cap_blocked = 0;
                    int next_consecutive = 0;
                    int gate_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE;
                    int should_defer = simplex_periodic_cost_defer_plan_for_test(
                        1,
                        tab->m,
                        rs->cycling.use_bland,
                        rs->cycling.degenerate_count,
                        lu_get_num_updates(tab->lu),
                        lu_get_max_updates(tab->lu),
                        lu_get_spike_pool_used(tab->lu),
                        lu_get_spike_pool_capacity(tab->lu),
                        lu_get_cond_estimate(tab->lu),
                        lu_get_growth_factor(tab->lu),
                        soft_lu_refactor_cost_ewma(solver, 1),
                        soft_lu_iter_cost_ewma(solver, 1),
                        periodic_cost_refactor_samples(solver, 1),
                        periodic_cost_iter_samples(solver, 1),
                        periodic_cost_consecutive_defers(solver, 1),
                        &gate_reason,
                        NULL,
                        &cap_blocked,
                        &next_consecutive);
                    periodic_cost_record_gate_reason(
                        solver, 1, (LPPeriodicCostDampenReason)gate_reason);
                    if (should_defer) {
                        periodic_refactor = 0;
                        periodic_cost_record_defer(solver, 1);
                        periodic_cost_set_consecutive_defers(solver, 1, next_consecutive);
                        if (solver->verbose >= 2) {
                            LP_LOG_STDERR("[simplex_phase1] Deferred policy periodic refactor by cost gate (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                                    lu_get_num_updates(tab->lu),
                                    lu_get_max_updates(tab->lu),
                                    rs->cycling.degenerate_count,
                                    soft_lu_iter_cost_ewma(solver, 1),
                                    soft_lu_refactor_cost_ewma(solver, 1));
                        }
                    } else {
                        periodic_cost_reset_defer_streak(solver, 1);
                        if (cap_blocked) {
                            periodic_cost_record_cap_forced(solver, 1);
                            if (solver->verbose >= 2) {
                                LP_LOG_STDERR("[simplex_phase1] Policy periodic defer cap reached; forcing periodic policy refactor (updates=%d/%d, degen=%d)\n",
                                        lu_get_num_updates(tab->lu),
                                        lu_get_max_updates(tab->lu),
                                        rs->cycling.degenerate_count);
                            }
                        } else if (solver->verbose >= 3) {
                            LP_LOG_STDERR("[simplex_phase1] Policy periodic cost gate blocked defer: %s\n",
                                    lp_refactor_policy_periodic_cost_dampen_reason_string(
                                        (LPPeriodicCostDampenReason)gate_reason));
                        }
                    }
                } else {
                    periodic_cost_reset_defer_streak(solver, 1);
                }
            }
        }
        needs_refactor = periodic_refactor;
    } else {
        reinvert_shadow_prepare_phase(solver,
                                      tab,
                                      1,
                                      iter,
                                      &lu_health_decision,
                                      0,
                                      periodic_policy.min_update_age,
                                      rs->shared.periodic_policy_cooldown,
                                      0,
                                      NULL,
                                      &reinvert_shadow_eval);
    }
    if (periodic_refactor) {
        periodic_feedback_set_hint(solver, 1, periodic_policy.interval, effective_policy_pressure);
    }
    {
        int shadow_refactor = lp_basis_governor_shadow_decide(
            LP_BASIS_GOV_PHASE1,
            lu_refactor_nominal,
            periodic_refactor_nominal);
        int governed_refactor = lp_basis_governor_decide_refactor(
            &solver->policy.basis_governor,
            LP_BASIS_GOV_PHASE1,
            lu_refactor_nominal,
            periodic_refactor_nominal,
            needs_refactor);
        if (solver->telemetry_enabled) {
            lp_basis_governor_observe_refactor(
                &solver->policy.basis_governor,
                LP_BASIS_GOV_PHASE1,
                shadow_refactor,
                governed_refactor);
        }
        needs_refactor = governed_refactor;
    }
    reinvert_shadow_finalize_phase(solver, 1, &reinvert_shadow_eval, needs_refactor);

    if (needs_refactor) {
        rs->numerical.dir_skip_no_recompute_streak = 0;
        soft_lu_reset_defer_streak(solver, 1);
        periodic_cost_reset_defer_streak(solver, 1);
        runtime_record_periodic_refactor_trigger(solver, 1, lu_refactor_needed);
        double t_refactor_ms = lp_telemetry_timer_start();
        int rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
        double refactor_elapsed_ms = lp_telemetry_timer_elapsed_ms(t_refactor_ms);
        if (rc_refactor == 0) {
            soft_lu_record_refactor_cost(solver, 1, refactor_elapsed_ms);
            lp_reinvert_controller_state_record_refactor_cost(
                reinvert_state_for_phase(solver, 1), refactor_elapsed_ms);
        }
        if (lu_refactor_needed && rc_refactor == 0) {
            rs->shared.lu_soft_health_streak = 0;
        }
        if (!lu_refactor_needed && periodic_refactor) {
            lp_refactor_policy_periodic_post_refactor_update(
                1,
                cooldown_eligible,
                periodic_policy.interval,
                rc_refactor,
                &rs->shared.periodic_policy_cooldown,
                &rs->shared.periodic_policy_pressure_decay);
        }
        if (rc_refactor != 0) {
            if (periodic_refactor) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Periodic refactorization failed at iter %d, continuing with existing LU\n",
                            iter);
                }
                tab->phase1_compute_solution_context =
                    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAIL_CONTINUE;
                tab->phase1_compute_rc_context =
                    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAIL_CONTINUE;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                return P1_ZONE_CONTINUE;
            }

            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] Refactorization failed at iter %d, trying dual rescue\n", iter);
            }

            int marked = mark_basic_artificial_rows_redundant(tab, 1);
            if (marked > 0) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Marked %d infeasible artificial rows as redundant after refactorization failure\n",
                            marked);
                }
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
                    tab->phase1_compute_solution_context =
                        LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY;
                    tab->phase1_compute_rc_context =
                        LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY;
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    return P1_ZONE_CONTINUE;
                }
            }

            int rescue_status = p1_progress_attempt_direct_rescue(
                solver, tab, iter, &rs->progress);
            if (rescue_status == 0) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Dual rescue recovered after refactorization failure at iter %d\n", iter);
                }
                tab->phase1_compute_solution_context =
                    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY;
                tab->phase1_compute_rc_context =
                    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                rs->basis.fail_reason = PHASE1_PIVOT_FAIL_NONE;
                rs->basis.fail_repeat_count = 0;
                return P1_ZONE_CONTINUE;
            }
            if (solver->status == RALPH_STATUS_TIME_LIMIT) {
                primal_remove_perturbation(tab);
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
                return P1_ZONE_RETURN_FAIL;
            }

            if (repair_singular_basis(tab) == 0) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Basis repair recovered after refactorization failure at iter %d\n", iter);
                }
                tab->phase1_compute_solution_context =
                    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY;
                tab->phase1_compute_rc_context =
                    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                return P1_ZONE_CONTINUE;
            }

            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] ERROR: all refactorization recoveries failed at iter %d\n", iter);
            }
            primal_remove_perturbation(tab);
            /* Treat unrecoverable Phase 1 refactorization failure as numerical breakdown. */
            solver->status = RALPH_STATUS_ITERATION_LIMIT;
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
            return P1_ZONE_RETURN_FAIL;
        }
        /* Recompute primal solution and reduced costs after refactorization. */
        tab->phase1_compute_solution_context =
            LP_PHASE1_COMPUTE_CTX_REFACTOR_SUCCESS;
        tab->phase1_compute_rc_context =
            LP_PHASE1_COMPUTE_CTX_REFACTOR_SUCCESS;
        tableau_compute_solution(tab);
        tableau_compute_reduced_costs(tab);
        if (solver->pricing_strategy == 4) heap_build(tab);
    } else if (lu_soft_cost_deferred && solver->verbose >= 2) {
        LP_LOG_STDERR("[simplex_phase1] Deferred soft LU-health periodic refactor (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                lu_get_num_updates(tab->lu),
                lu_get_max_updates(tab->lu),
                rs->cycling.degenerate_count,
                soft_lu_iter_cost_ewma(solver, 1),
                soft_lu_refactor_cost_ewma(solver, 1));
    } else if (iter > 0 && iter % rs->shared.recompute_interval == 0) {
        /* Drift control even when LU updates are still accepted. */
        rs->numerical.dir_skip_no_recompute_streak = 0;
        tab->phase1_compute_solution_context =
            LP_PHASE1_COMPUTE_CTX_DRIFT_REFRESH;
        tab->phase1_compute_rc_context =
            LP_PHASE1_COMPUTE_CTX_DRIFT_REFRESH;
        tableau_compute_solution(tab);
        tableau_compute_reduced_costs(tab);
        if (solver->pricing_strategy == 4) heap_build(tab);
    }
    return P1_ZONE_PROCEED;
}

/* ── Zone 5: Direction guard ──────────────────────────────────────── */

P1ZoneResult p1_zone_direction_guard(SimplexSolver *solver,
                                     SimplexTableau *tab,
                                     P1RecoveryState *rs,
                                     int iter,
                                     P1IterContext *ctx)
{
    int entering = ctx->entering;
    int leaving = ctx->leaving;
    double theta = ctx->theta;
    int ratio_status;

    /* Guard against numerically explosive search directions before pivoting.
     * Re-factorize and recompute ratio test from the same entering column. */
    double dir_inf = vec_abs_max(tab->work2, tab->m);
    if (rs->numerical.shadow_guard_followup_direction_pending) {
        p1_numerical_record_shadow_direction(solver, tab, leaving, theta);
        rs->numerical.shadow_guard_followup_direction_pending = 0;
    }
    if (rs->numerical.force_extreme_followup_direction_pending) {
        p1_numerical_record_extreme_direction(solver, tab, leaving, theta);
        if (leaving == -2) {
            if (rs->numerical.force_extreme_followup_bound_flip_streak < INT_MAX) {
                rs->numerical.force_extreme_followup_bound_flip_streak++;
            }
            rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
        } else if (theta <= RALPH_FEAS_TOL) {
            if (rs->numerical.force_extreme_followup_tiny_theta_streak < INT_MAX) {
                rs->numerical.force_extreme_followup_tiny_theta_streak++;
            }
            rs->numerical.force_extreme_followup_bound_flip_streak = 0;
        } else {
            rs->numerical.force_extreme_followup_bound_flip_streak = 0;
            rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
        }
        rs->numerical.force_extreme_followup_direction_pending = 0;
    }
    if (dir_inf > RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
        double dir_inf_ratio =
            dir_inf / RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER;
        double current_pivot_ratio = 0.0;
        double current_pivot_abs = 0.0;
        int cooldown_active = (rs->numerical.dir_stabilize_cooldown > 0);
        int dir_stabilize_cooldown_target;
        int force_dir_refactor_extreme = 0;
        int force_dir_refactor_lu_health = lu_needs_refactorization(tab->lu);
        int force_pivot_mode_active = 0;
        int force_dir_refactor_guard_trigger =
            (force_dir_refactor_lu_health || force_pivot_mode_active);
        int dir_refactor_ladder_forced = 0;
        int lu_hard_trigger = lu_refactor_hard_trigger(tab->lu);
        int escape_triggered = 0;
        int escape_hard_bypass = 0;
        int suppress_lu_health = 0;
        phase1_failed_stabilize_retry_direction_shape(
            tab,
            leaving,
            NULL,
            NULL,
            &current_pivot_abs);
        if (current_pivot_abs > 0.0 && dir_inf > 0.0) {
            current_pivot_ratio = current_pivot_abs / dir_inf;
        }
        if (lp_glpk_strict_allow_phase1_dir_stabilize_force(
                solver->glpk_strict_mode)) {
            force_dir_refactor_extreme =
                lp_refactor_policy_phase1_dir_stabilize_force_extreme_ratio(
                    dir_inf_ratio,
                    cooldown_active);
            force_pivot_mode_active =
                (rs->progress.force_pivot_attempt_budget > 0);
            if (force_pivot_mode_active) {
                lp_telemetry_record_phase1_force_pivot_budget_dir_event_seen(
                    solver);
            }
            force_dir_refactor_guard_trigger =
                (force_dir_refactor_lu_health || force_pivot_mode_active);
            suppress_lu_health = phase1_dir_stabilize_escape_gate_plan(
                tab->m,
                rs->cycling.degenerate_count,
                rs->numerical.dir_skip_event_streak,
                rs->progress.no_pivot_no_progress_streak,
                rs->progress.dir_escape_cooldown,
                force_dir_refactor_extreme,
                force_dir_refactor_guard_trigger,
                lu_hard_trigger,
                &rs->progress.dir_escape_cooldown,
                &escape_triggered,
                &escape_hard_bypass);
            if (escape_triggered) {
                lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                    solver,
                    PHASE1_DIR_ESCAPE_TELEM_TRIGGER);
            }
            if (escape_hard_bypass) {
                lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                    solver,
                    PHASE1_DIR_ESCAPE_TELEM_HARD_BYPASS);
            }
            if (suppress_lu_health) {
                if (force_dir_refactor_lu_health) {
                    force_dir_refactor_lu_health = 0;
                    lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                        solver,
                        PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_LU_HEALTH);
                }
                if (force_pivot_mode_active) {
                    force_pivot_mode_active = 0;
                    lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                        solver,
                        PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_FORCE_PIVOT_MODE);
                }
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Dir-stabilize escape gate suppressed forced direction-refactor path (iter=%d entering=%d dir_skip_streak=%d no_progress=%d cooldown=%d)\n",
                            iter,
                            entering,
                            rs->numerical.dir_skip_event_streak,
                            rs->progress.no_pivot_no_progress_streak,
                            rs->progress.dir_escape_cooldown);
                }
            }
            if (phase1_force_pivot_refactor_relax_plan(
                    tab->m,
                    rs->cycling.degenerate_count,
                    rs->progress.no_pivot_no_progress_streak,
                    force_pivot_mode_active,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health,
                    lu_hard_trigger,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_successes,
                    rs->progress.no_pivot_ladder_rescue_fail_streak)) {
                force_pivot_mode_active = 0;
                lp_telemetry_record_phase1_force_pivot_relax(solver);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Relaxed force-pivot refactor under stable LU + high dual-rescue success (iter=%d entering=%d no_progress=%d)\n",
                            iter,
                            entering,
                            rs->progress.no_pivot_no_progress_streak);
                }
            }
            if (phase1_force_extreme_refactor_relax_plan(
                    tab->m,
                    rs->cycling.degenerate_count,
                    rs->progress.no_pivot_no_progress_streak,
                    dir_inf_ratio,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health,
                    lu_hard_trigger,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_successes,
                    rs->progress.no_pivot_ladder_rescue_fail_streak)) {
                force_dir_refactor_extreme = 0;
                lp_telemetry_record_phase1_force_extreme_relax(solver);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Relaxed extreme-direction refactor under stable LU + high dual-rescue success (iter=%d entering=%d ratio=%.2f no_progress=%d)\n",
                            iter,
                            entering,
                            dir_inf_ratio,
                            rs->progress.no_pivot_no_progress_streak);
                }
            }
            if (phase1_force_extreme_bound_flip_relax_plan(
                    tab->m,
                    rs->cycling.degenerate_count,
                    rs->progress.no_pivot_no_progress_streak,
                    dir_inf_ratio,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health,
                    lu_hard_trigger,
                    rs->numerical.force_extreme_followup_bound_flip_streak)) {
                force_dir_refactor_extreme = 0;
                lp_telemetry_record_phase1_force_extreme_bound_flip_relax(
                    solver);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Relaxed extreme-direction refactor on repeated bound-flip follow-up treadmill (iter=%d entering=%d ratio=%.2f streak=%d)\n",
                            iter,
                            entering,
                            dir_inf_ratio,
                            rs->numerical.force_extreme_followup_bound_flip_streak);
                }
            }
            if (phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
                    tab->m,
                    rs->cycling.degenerate_count,
                    rs->progress.no_pivot_no_progress_streak,
                    dir_inf_ratio,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health,
                    lu_hard_trigger,
                    rs->numerical.force_extreme_followup_tiny_theta_streak,
                    current_pivot_ratio)) {
                force_dir_refactor_extreme = 0;
                lp_telemetry_record_phase1_force_extreme_catastrophic_tiny_theta_relax(
                    solver);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Relaxed extreme-direction refactor on catastrophic tiny-theta follow-up treadmill (iter=%d entering=%d ratio=%.2f streak=%d pivot_ratio=%.3e)\n",
                            iter,
                            entering,
                            dir_inf_ratio,
                            rs->numerical.force_extreme_followup_tiny_theta_streak,
                            current_pivot_ratio);
                }
            }
            if (phase1_force_extreme_tiny_theta_relax_plan(
                    tab->m,
                    rs->cycling.degenerate_count,
                    rs->progress.no_pivot_no_progress_streak,
                    dir_inf_ratio,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health,
                    lu_hard_trigger,
                    rs->numerical.force_extreme_followup_tiny_theta_streak)) {
                force_dir_refactor_extreme = 0;
                rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 1;
                lp_telemetry_record_phase1_force_extreme_tiny_theta_relax(
                    solver);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Relaxed extreme-direction refactor on repeated tiny-theta follow-up treadmill (iter=%d entering=%d ratio=%.2f streak=%d)\n",
                            iter,
                            entering,
                            dir_inf_ratio,
                            rs->numerical.force_extreme_followup_tiny_theta_streak);
                }
            }
        }
        int force_dir_refactor = force_dir_refactor_extreme ||
                                 force_dir_refactor_lu_health;
        int moderate_defer =
            lp_refactor_policy_phase1_dir_stabilize_should_defer_moderate(
                dir_inf_ratio,
                cooldown_active,
                force_dir_refactor_lu_health,
                rs->numerical.dir_stabilize_moderate_defer_pending);
        if (rs->numerical.dir_stabilize_repeat_count < 1000000) {
            rs->numerical.dir_stabilize_repeat_count++;
        }
        dir_stabilize_cooldown_target =
            lp_refactor_policy_phase1_dir_stabilize_cooldown_updates(
                tab->m, rs->cycling.degenerate_count, rs->numerical.dir_stabilize_repeat_count);

        if (cooldown_active) {
            lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(
                solver,
                dir_inf_ratio);
            if (force_dir_refactor) {
                lp_telemetry_record_phase1_dir_stabilize_force(
                    solver,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health);
            }
        }

        if (moderate_defer && !force_dir_refactor && !force_pivot_mode_active) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Moderate direction norm %.2e at iter %d (entering=%d), deferring one refactor and retrying pricing\n",
                        dir_inf, iter, entering);
            }
            p1_dir_skip_defer_core(solver, tab, rs, entering,
                                   P1_DIR_DEFER_MODERATE,
                                   dir_stabilize_cooldown_target);
            {
                int tiny_theta_relax_immediate_classified = 0;

            lp_telemetry_record_phase1_no_pivot_ladder_retry(
                solver,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
            if (p1_progress_note_no_pivot(
                    solver,
                    tab->m,
                    rs->cycling.degenerate_count,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                    &rs->progress)) {
                rs->progress.no_pivot_force_pending = 1;
                rs->progress.no_pivot_force_reason =
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                    lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor(
                        solver);
                    rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                    rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                    tiny_theta_relax_immediate_classified = 1;
                }
            }
            if (!rs->progress.no_pivot_force_pending &&
                lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(rs->numerical.dir_skip_event_streak)) {
                int rescue_step = phase1_no_pivot_ladder_apply_rescue_guard(
                    solver,
                    PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE,
                    rs->progress.no_pivot_ladder_rescue_cooldown,
                    rs->progress.no_pivot_ladder_rescue_fail_streak);
                if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                    int rescue_result = p1_progress_attempt_ladder_rescue(
                        solver,
                        tab,
                        iter,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                        &rs->progress,
                        &rs->numerical.rc_only_streak);
                    if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                        lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue(
                            solver);
                        rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                        rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                        tiny_theta_relax_immediate_classified = 1;
                    }
                    if (rescue_result == 1) return P1_ZONE_CONTINUE;
                    if (rescue_result < 0) return P1_ZONE_RETURN_FAIL;
                } else if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                    lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                        solver,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                    rs->progress.no_pivot_force_pending = 1;
                    rs->progress.no_pivot_force_reason =
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                        lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor(
                            solver);
                        rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                        rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                        tiny_theta_relax_immediate_classified = 1;
                    }
                }
            }
                if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending &&
                    !tiny_theta_relax_immediate_classified) {
                    lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry(
                        solver);
                    rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                    rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                }
            }
            return P1_ZONE_CONTINUE;
        }

        if (cooldown_active && !force_dir_refactor && !force_pivot_mode_active) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Large direction norm %.2e at iter %d (entering=%d), skipping direction-stabilize refactor (cooldown=%d)\n",
                        dir_inf, iter, entering, rs->numerical.dir_stabilize_cooldown);
            }
            p1_dir_skip_defer_core(solver, tab, rs, entering,
                                   P1_DIR_DEFER_COOLDOWN,
                                   dir_stabilize_cooldown_target);
            {
                int tiny_theta_relax_immediate_classified = 0;

            lp_telemetry_record_phase1_no_pivot_ladder_retry(
                solver,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
            if (p1_progress_note_no_pivot(
                    solver,
                    tab->m,
                    rs->cycling.degenerate_count,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                    &rs->progress)) {
                rs->progress.no_pivot_force_pending = 1;
                rs->progress.no_pivot_force_reason =
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                    lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor(
                        solver);
                    rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                    rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                    tiny_theta_relax_immediate_classified = 1;
                }
            }
            if (!rs->progress.no_pivot_force_pending &&
                lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(rs->numerical.dir_skip_event_streak)) {
                int rescue_step = phase1_no_pivot_ladder_apply_rescue_guard(
                    solver,
                    PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE,
                    rs->progress.no_pivot_ladder_rescue_cooldown,
                    rs->progress.no_pivot_ladder_rescue_fail_streak);
                if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                    int rescue_result = p1_progress_attempt_ladder_rescue(
                        solver,
                        tab,
                        iter,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                        &rs->progress,
                        &rs->numerical.rc_only_streak);
                    if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                        lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue(
                            solver);
                        rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                        rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                        tiny_theta_relax_immediate_classified = 1;
                    }
                    if (rescue_result == 1) return P1_ZONE_CONTINUE;
                    if (rescue_result < 0) return P1_ZONE_RETURN_FAIL;
                } else if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                    lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                        solver,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                    rs->progress.no_pivot_force_pending = 1;
                    rs->progress.no_pivot_force_reason =
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                        lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor(
                            solver);
                        rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                        rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                        tiny_theta_relax_immediate_classified = 1;
                    }
                }
            }
                if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending &&
                    !tiny_theta_relax_immediate_classified) {
                    lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry(
                        solver);
                    rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                    rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                }
            }
            return P1_ZONE_CONTINUE;
        }
        p1_progress_update(solver, tab, &rs->progress);
        if (!force_dir_refactor && !force_pivot_mode_active) {
            int ladder_threshold = 0;
            int ladder_step = phase1_no_pivot_ladder_step(
                solver,
                tab->m,
                rs->cycling.degenerate_count,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                rs->progress.no_pivot_streak + 1,
                rs->progress.no_pivot_no_progress_streak,
                force_pivot_mode_active,
                &ladder_threshold);
            ladder_step = phase1_no_pivot_ladder_apply_rescue_guard(
                solver,
                ladder_step,
                rs->progress.no_pivot_ladder_rescue_cooldown,
                rs->progress.no_pivot_ladder_rescue_fail_streak);
            if (ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                int rescue_result = p1_progress_attempt_ladder_rescue(
                    solver,
                    tab,
                    iter,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                    LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                    &rs->progress,
                    &rs->numerical.rc_only_streak);
                if (rescue_result == 1) return P1_ZONE_CONTINUE;
                if (rescue_result < 0) return P1_ZONE_RETURN_FAIL;
            }
            if (ladder_step != PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                lp_telemetry_record_phase1_no_pivot_ladder_retry(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] No-pivot ladder defers dir-stabilize refactor (iter=%d entering=%d no_progress=%d threshold=%d)\n",
                            iter, entering, rs->progress.no_pivot_no_progress_streak, ladder_threshold);
                }
                p1_dir_skip_defer_core(solver, tab, rs, entering,
                                       P1_DIR_DEFER_LADDER,
                                       dir_stabilize_cooldown_target);
                if (p1_progress_note_no_pivot(
                        solver,
                        tab->m,
                        rs->cycling.degenerate_count,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        &rs->progress)) {
                    rs->progress.no_pivot_force_pending = 1;
                    rs->progress.no_pivot_force_reason =
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                        lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor(
                            solver);
                        rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                        rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                    }
                } else if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
                    lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry(
                        solver);
                    rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                    rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
                }
                return P1_ZONE_CONTINUE;
            }
            lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                solver,
                LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
            dir_refactor_ladder_forced = 1;
        }
        if (rs->numerical.force_extreme_tiny_theta_relax_branch_pending) {
            if (force_dir_refactor_lu_health) {
                lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_refactor(
                    solver,
                    PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_REFACTOR_LU_HEALTH);
                rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
            } else if (force_pivot_mode_active) {
                lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_refactor(
                    solver,
                    PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_REFACTOR_FORCE_PIVOT);
                rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
            } else if (dir_refactor_ladder_forced) {
                lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_refactor(
                    solver,
                    PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_REFACTOR_LADDER);
                rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
                rs->numerical.force_extreme_tiny_theta_relax_next_pending = 1;
            }
        }
        if (force_dir_refactor_extreme && !force_pivot_mode_active) {
            if (rs->numerical.dir_force_refactor_streak < INT_MAX) {
                rs->numerical.dir_force_refactor_streak++;
            }
            if (p1_progress_activate_force_pivot(
                    solver,
                    tab->m,
                    rs->cycling.degenerate_count,
                    0,
                    &rs->numerical.dir_force_refactor_streak,
                    &rs->progress,
                    NULL,
                    NULL) &&
                solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Force-pivot mode armed after repeated extreme-direction refactors (budget=%d streak=%d)\n",
                        rs->progress.force_pivot_attempt_budget,
                        rs->numerical.dir_force_refactor_streak);
            }
            force_pivot_mode_active =
                (rs->progress.force_pivot_attempt_budget > 0);
        } else if (!force_dir_refactor_extreme) {
            rs->numerical.dir_force_refactor_streak = 0;
        }
        rs->numerical.dir_skip_event_streak = 0;
        rs->progress.dir_escape_cooldown = 0;
        rs->numerical.dir_stabilize_moderate_defer_pending = 0;

        if (solver->verbose >= 2) {
            LP_LOG_STDERR("[simplex_phase1] Large direction norm %.2e at iter %d (entering=%d), re-factorizing before pivot\n",
                    dir_inf, iter, entering);
        }
        int stabilized = 0;
        int original_entering = entering;
        int retry_penalize_last_failed =
            phase1_failed_stabilize_retry_penalty_plan(
                original_entering,
                rs->basis.last_failed_stabilize_entering,
                rs->basis.failed_stabilize_same_entering_streak);
        int retry_consumed_alternate = 0;
        int retry_used_local_memory_alt = 0;
        int retry_local_memory_repeat_streak = 0;
        int retry_local_memory_selector_tracked = 0;
        int retry_local_memory_used_guarded_selector = 0;
        int retry_local_memory_bland_alt = -1;
        int retry_shadow_best_alt = -1;
        int retry_direction_guard_exclude_original = 0;
        int retry_shadow_guard_exclude_original = 0;
        int arm_shadow_guard_followup_pending = 0;
        int arm_force_extreme_followup_pending = 0;
        int force_extreme_followup_tracked = 0;
        for (int stab_try = 0; stab_try < 1; stab_try++) {
            int dir_refactor_trigger = 0;
            if (force_dir_refactor_extreme) {
                dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_FORCE_EXTREME_DIR;
            } else if (force_dir_refactor_lu_health) {
                dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_FORCE_LU_HEALTH;
            } else if (force_pivot_mode_active) {
                dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_FORCE_PIVOT_MODE;
            } else if (dir_refactor_ladder_forced) {
                dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_LADDER_FORCE;
            }
            lp_telemetry_record_phase1_dir_stabilize_refactor_trigger(
                solver,
                dir_refactor_trigger);
            force_extreme_followup_tracked =
                (dir_refactor_trigger ==
                 PHASE1_DIR_REFACTOR_TELEM_FORCE_EXTREME_DIR);
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) != 0) {
                break;
            }
            rs->numerical.dir_stabilize_cooldown = dir_stabilize_cooldown_target;
            ratio_status = primal_ratio_test_with_policy(solver,
                                                         tab,
                                                         0,
                                                         entering,
                                                         &leaving,
                                                         &theta);
            if (ratio_status != 0) {
                if (force_extreme_followup_tracked) {
                    lp_telemetry_record_phase1_force_extreme_followup_ratio_breakdown(
                        solver);
                }
                p1_numerical_consume_followup(solver, P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN, &rs->numerical);
                if (retry_consumed_alternate) {
                    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(
                        solver, 0);
                }
                phase1_trace_record_no_entering(solver, iter, ratio_status);
                rs->cycling.use_bland = 1;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &rs->numerical.rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR);
                continue;
            }

            dir_inf = vec_abs_max(tab->work2, tab->m);
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Direction norm after re-factorization: %.2e\n",
                        dir_inf);
            }
            if (dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
                if (force_extreme_followup_tracked) {
                    lp_telemetry_record_phase1_force_extreme_followup_stabilized(
                        solver);
                    rs->numerical.force_extreme_followup_bound_flip_streak = 0;
                    rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
                }
                if (retry_used_local_memory_alt) {
                    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(
                        solver, 1);
                }
                stabilized = 1;
                break;
            }

            if (retry_penalize_last_failed) {
                lp_telemetry_record_phase1_failed_stabilize_retry_penalty_arm(
                    solver);
                phase1_failed_stabilize_retry_sample_pool(
                    solver,
                    tab,
                    original_entering,
                    rs->basis.last_failed_stabilize_entering,
                    &rs->basis.failed_stabilize_retry_pool_sample_counter);
                if (pricing_bland_excluding_two(tab,
                                                original_entering,
                                                rs->basis.last_failed_stabilize_entering,
                                                &entering) != 0) {
                    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_no_alt(
                        solver);
                    break;
                }
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Penalizing repeated failed-stabilize retry candidate %d, trying alternate %d inside dir-stabilize retry\n",
                            rs->basis.last_failed_stabilize_entering, entering);
                }
            } else {
                int retry_use_local_memory =
                    phase1_failed_stabilize_retry_local_memory_plan(
                        original_entering,
                        rs->basis.last_failed_stabilize_retry_alt,
                        rs->basis.failed_stabilize_retry_alt_streak,
                        retry_penalize_last_failed);
                if (retry_use_local_memory) {
                    retry_local_memory_repeat_streak =
                        rs->basis.failed_stabilize_retry_alt_streak;
                    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_arm(
                        solver);
                    phase1_failed_stabilize_retry_sample_pool(
                        solver,
                        tab,
                        original_entering,
                        rs->basis.last_failed_stabilize_retry_alt,
                        &rs->basis.failed_stabilize_retry_pool_sample_counter);
                    if (rs->basis.failed_stabilize_retry_alt_streak >=
                            PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_TRIGGER &&
                        phase1_failed_stabilize_retry_select_local_memory(
                            solver,
                            tab,
                            original_entering,
                            rs->basis.last_failed_stabilize_retry_alt,
                            rs->basis.failed_stabilize_retry_alt_ratio_fail_streak,
                            rs->basis.failed_stabilize_retry_alt_streak,
                            &entering,
                            &retry_local_memory_bland_alt,
                            &retry_shadow_best_alt,
                            &retry_local_memory_used_guarded_selector) == 0) {
                        if (retry_shadow_best_alt == retry_local_memory_bland_alt) {
                            retry_shadow_best_alt = -1;
                        }
                        retry_used_local_memory_alt = 1;
                        retry_local_memory_selector_tracked = 1;
                        lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_alternate(
                            solver);
                        if (solver->verbose >= 2) {
                            LP_LOG_STDERR("[simplex_phase1] Avoiding repeated retry alternate %d, trying local-memory %s alternate %d inside dir-stabilize retry\n",
                                    rs->basis.last_failed_stabilize_retry_alt,
                                    retry_local_memory_used_guarded_selector ? "guarded" : "bland",
                                    entering);
                        }
                    } else if (pricing_bland_excluding_two(tab,
                                                           original_entering,
                                                           rs->basis.last_failed_stabilize_retry_alt,
                                                           &entering) == 0) {
                        if (phase1_failed_stabilize_retry_eval_candidates(
                            solver,
                            tab,
                            original_entering,
                            rs->basis.last_failed_stabilize_retry_alt,
                            NULL,
                            NULL,
                            &retry_shadow_best_alt,
                            NULL,
                            NULL) != 0) {
                            retry_shadow_best_alt = -1;
                        }
                        if (retry_shadow_best_alt == entering) {
                            retry_shadow_best_alt = -1;
                        }
                        retry_used_local_memory_alt = 1;
                        retry_local_memory_selector_tracked = 1;
                        retry_local_memory_used_guarded_selector = 0;
                        retry_local_memory_bland_alt = entering;
                        lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(
                            solver, 0, 0);
                        lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_alternate(
                            solver);
                        if (solver->verbose >= 2) {
                            LP_LOG_STDERR("[simplex_phase1] Avoiding repeated retry alternate %d, trying local-memory bland alternate %d inside dir-stabilize retry\n",
                                    rs->basis.last_failed_stabilize_retry_alt, entering);
                        }
                    } else {
                        retry_local_memory_selector_tracked = 0;
                        retry_local_memory_used_guarded_selector = 0;
                        lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_no_alt(
                            solver);
                        if (pricing_bland_excluding(tab, original_entering, &entering) != 0) {
                            break;
                        }
                        if (entering == rs->basis.last_failed_stabilize_retry_alt) {
                            lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_fallback_same_alt(
                                solver);
                        }
                    }
                } else {
                    phase1_failed_stabilize_retry_sample_pool(
                        solver,
                        tab,
                        original_entering,
                        -1,
                        &rs->basis.failed_stabilize_retry_pool_sample_counter);
                    if (pricing_bland_excluding(tab, original_entering, &entering) != 0) {
                        break;
                    }
                }
            }
            retry_consumed_alternate = 1;
            for (;;) {
                double retry_dir_fail_inf = 0.0;
                int retry_dir_fail_nnz = 0;
                double retry_dir_fail_pivot_abs = 0.0;
                p1_basis_note_failed_stabilize_retry_alt(solver, entering, &rs->basis);
                if (rs->basis.failed_stabilize_retry_alt_streak <= 1) {
                    rs->basis.failed_stabilize_retry_alt_ratio_fail_streak = 0;
                }
                ratio_status = primal_ratio_test_with_policy(solver,
                                                             tab,
                                                             0,
                                                             entering,
                                                             &leaving,
                                                             &theta);
                if (ratio_status != 0) {
                    if (rs->basis.failed_stabilize_retry_alt_ratio_fail_streak < INT_MAX) {
                        rs->basis.failed_stabilize_retry_alt_ratio_fail_streak++;
                    }
                    if (retry_local_memory_selector_tracked) {
                        lp_telemetry_record_phase1_failed_stabilize_retry_selector_ratio_failure(
                            solver, retry_local_memory_used_guarded_selector);
                        lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(
                            solver, retry_local_memory_used_guarded_selector, 0);
                        if (retry_local_memory_used_guarded_selector &&
                            retry_local_memory_bland_alt >= 0 &&
                            retry_local_memory_bland_alt != entering) {
                            lp_telemetry_record_phase1_failed_stabilize_retry_selector_guarded_fallback(
                                solver);
                            entering = retry_local_memory_bland_alt;
                            retry_local_memory_used_guarded_selector = 0;
                            lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(
                                solver, 0, 0);
                            continue;
                        }
                    }
                    break;
                }
                dir_inf = vec_abs_max(tab->work2, tab->m);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Alternate entering %d direction norm: %.2e\n",
                            entering, dir_inf);
                }
                if (dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
                    if (force_extreme_followup_tracked) {
                        lp_telemetry_record_phase1_force_extreme_followup_stabilized(
                            solver);
                        rs->numerical.force_extreme_followup_bound_flip_streak = 0;
                        rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
                    }
                    rs->basis.failed_stabilize_retry_alt_ratio_fail_streak = 0;
                    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(
                        solver, 1);
                    if (retry_used_local_memory_alt) {
                        lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(
                            solver, 1);
                    }
                    if (retry_local_memory_selector_tracked) {
                        lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(
                            solver, retry_local_memory_used_guarded_selector, 1);
                    }
                    stabilized = 1;
                    break;
                }
                if (retry_used_local_memory_alt) {
                    int retry_shadow_ratio_success = 0;
                    int retry_shadow_dir_stable = 0;
                    int retry_shadow_dir_nnz = 0;
                    double retry_shadow_dir_inf = 0.0;
                    double retry_shadow_pivot_abs = 0.0;
                    phase1_failed_stabilize_retry_direction_shape(
                        tab,
                        leaving,
                        &retry_dir_fail_inf,
                        &retry_dir_fail_nnz,
                        &retry_dir_fail_pivot_abs);
                    lp_telemetry_record_phase1_failed_stabilize_retry_dir_fail_shape(
                        solver,
                        retry_dir_fail_inf,
                        retry_dir_fail_nnz,
                        retry_dir_fail_pivot_abs);
                    if (phase1_failed_stabilize_retry_direction_guard_plan(
                            retry_dir_fail_inf,
                            retry_dir_fail_nnz,
                            retry_dir_fail_pivot_abs,
                            retry_local_memory_repeat_streak)) {
                        retry_direction_guard_exclude_original = 1;
                    }
                    if (!retry_local_memory_used_guarded_selector &&
                        retry_shadow_best_alt >= 0 &&
                        retry_shadow_best_alt != entering) {
                        phase1_failed_stabilize_retry_shadow_direction_proxy(
                            solver,
                            tab,
                            retry_shadow_best_alt,
                            &retry_shadow_ratio_success,
                            &retry_shadow_dir_stable,
                            &retry_shadow_dir_inf,
                            &retry_shadow_dir_nnz,
                            &retry_shadow_pivot_abs);
                        if (phase1_failed_stabilize_retry_shadow_guard_plan(
                                retry_dir_fail_inf,
                                retry_dir_fail_nnz,
                                retry_dir_fail_pivot_abs,
                                retry_shadow_ratio_success,
                                retry_shadow_dir_inf,
                                retry_shadow_dir_nnz,
                                retry_shadow_pivot_abs,
                                retry_local_memory_repeat_streak)) {
                            retry_shadow_guard_exclude_original = 1;
                        }
                    }
                }
                if (retry_local_memory_selector_tracked) {
                    rs->basis.failed_stabilize_retry_alt_ratio_fail_streak = 0;
                    lp_telemetry_record_phase1_failed_stabilize_retry_selector_dir_failure(
                        solver, retry_local_memory_used_guarded_selector);
                    lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(
                        solver, retry_local_memory_used_guarded_selector, 0);
                    if (retry_local_memory_used_guarded_selector &&
                        retry_local_memory_bland_alt >= 0 &&
                        retry_local_memory_bland_alt != entering) {
                        lp_telemetry_record_phase1_failed_stabilize_retry_selector_guarded_fallback(
                            solver);
                        entering = retry_local_memory_bland_alt;
                        retry_local_memory_used_guarded_selector = 0;
                        lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(
                            solver, 0, 0);
                        continue;
                    }
                }
                break;
            }
            if (stabilized) {
                break;
            }
            lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(
                solver, 0);
            if (retry_used_local_memory_alt) {
                lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(
                    solver, 0);
            }
        }

        if (!stabilized) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Skipping unstable entering column after stabilization attempts (iter=%d, entering=%d, dir_inf=%.2e)\n",
                        iter, entering, dir_inf);
            }
            if ((retry_direction_guard_exclude_original ||
                 retry_shadow_guard_exclude_original) &&
                original_entering >= 0 &&
                original_entering != entering) {
                int exclude_iters = PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_EXCLUDE_ITERS;
                if (retry_direction_guard_exclude_original) {
                    lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_arm(
                        solver);
                } else {
                    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_arm(
                        solver);
                    exclude_iters =
                        PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_EXCLUDE_ITERS;
                }
                p1_basis_exclude_entering(solver, original_entering,
                                          exclude_iters, &rs->basis);
                if (retry_direction_guard_exclude_original) {
                    lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_original_exclusion(
                        solver);
                } else {
                    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_original_exclusion(
                        solver);
                    arm_shadow_guard_followup_pending = 1;
                }
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR(
                        "[simplex_phase1] Guard-excluding original entering %d after %s retry direction for alternate %d\n",
                        original_entering,
                        retry_direction_guard_exclude_original ? "catastrophic" : "shadow-toxic",
                        entering);
                }
            }
            rs->numerical.shadow_guard_followup_direction_pending = 0;
            rs->numerical.force_extreme_followup_direction_pending = 0;
            p1_numerical_consume_followup(solver, P1_FOLLOWUP_EVENT_FAILED_STABILIZE, &rs->numerical);
            if (force_extreme_followup_tracked) {
                lp_telemetry_record_phase1_force_extreme_followup_failed_stabilize(
                    solver);
                arm_force_extreme_followup_pending = 1;
            }
            lp_telemetry_record_phase1_failed_stabilize_site(
                solver, retry_consumed_alternate);
            p1_window_pressure_note(
                solver,
                PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE,
                retry_used_local_memory_alt,
                &rs->progress);
            p1_basis_note_failed_stabilize_entering(solver, entering, &rs->basis);
            p1_basis_exclude_entering(solver, entering,
                                      RALPH_PHASE1_ENTERING_EXCLUDE_ITERS, &rs->basis);
            if (arm_shadow_guard_followup_pending) {
                rs->numerical.shadow_guard_followup_pending = 1;
                rs->numerical.shadow_guard_followup_direction_pending = 1;
            }
            if (arm_force_extreme_followup_pending) {
                rs->numerical.force_extreme_followup_pending = 1;
                rs->numerical.force_extreme_followup_direction_pending = 1;
            }
            rs->numerical.dir_stabilize_cooldown = dir_stabilize_cooldown_target;
            rs->cycling.use_bland = 1;
            phase1_recompute_full_with_reason(
                solver,
                tab,
                &rs->numerical.rc_only_streak,
                LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
            p1_window_pressure_note(
                solver,
                PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP,
                0,
                &rs->progress);
            if (rs->progress.window_pressure_force_pivot_armed) {
                /* Already armed in this pressure window. */
            } else if (rs->progress.no_pivot_force_pending) {
                lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_pending(
                    solver);
            } else if (rs->progress.force_pivot_attempt_budget > 0) {
                lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_budget(
                    solver);
            } else {
                int window_force_pivot_reject_reason =
                    LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_NONE;
                int window_force_pivot_budget =
                    lp_refactor_policy_phase1_window_pressure_force_pivot_budget(
                        tab->m,
                        rs->cycling.degenerate_count,
                        rs->progress.window_pressure_events,
                        rs->progress.window_pressure_failed_stabilize,
                        rs->progress.window_pressure_dir_skip,
                        rs->progress.window_pressure_local_memory_fail,
                        rs->progress.window_pressure_alternations,
                        rs->progress.force_pivot_attempt_budget,
                        &window_force_pivot_reject_reason);
                if (window_force_pivot_budget > 0) {
                    rs->progress.force_pivot_attempt_budget =
                        window_force_pivot_budget;
                    rs->progress.window_pressure_force_pivot_armed = 1;
                    lp_telemetry_record_phase1_window_pressure_force_pivot_arm(
                        solver);
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR(
                            "[simplex_phase1] Windowed phase1 pressure armed force-pivot budget=%d (events=%d failed=%d dir_skip=%d local_fail=%d alt=%d)\n",
                            rs->progress.force_pivot_attempt_budget,
                            rs->progress.window_pressure_events,
                            rs->progress.window_pressure_failed_stabilize,
                            rs->progress.window_pressure_dir_skip,
                            rs->progress.window_pressure_local_memory_fail,
                            rs->progress.window_pressure_alternations);
                    }
                } else {
                    lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
                        solver,
                        window_force_pivot_reject_reason);
                }
            }
            p1_progress_update(solver, tab, &rs->progress);
            {
                int force_extreme_followup_immediate_classified = 0;
                int shadow_followup_immediate_classified = 0;

                lp_telemetry_record_phase1_no_pivot_ladder_retry(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                if (p1_progress_note_no_pivot(
                        solver,
                        tab->m,
                        rs->cycling.degenerate_count,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        &rs->progress)) {
                    rs->progress.no_pivot_force_pending = 1;
                    rs->progress.no_pivot_force_reason =
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    if (arm_force_extreme_followup_pending) {
                        lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_forced_refactor(
                            solver);
                        force_extreme_followup_immediate_classified = 1;
                    }
                    if (arm_shadow_guard_followup_pending) {
                        lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor(
                            solver);
                        shadow_followup_immediate_classified = 1;
                    }
                }
                if (!rs->progress.no_pivot_force_pending &&
                    lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(rs->numerical.dir_skip_event_streak)) {
                    int rescue_step = phase1_no_pivot_ladder_apply_rescue_guard(
                        solver,
                        PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE,
                        rs->progress.no_pivot_ladder_rescue_cooldown,
                        rs->progress.no_pivot_ladder_rescue_fail_streak);
                    if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                        int rescue_result = p1_progress_attempt_ladder_rescue(
                            solver,
                            tab,
                            iter,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                            LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                            &rs->progress,
                            &rs->numerical.rc_only_streak);
                        if (arm_force_extreme_followup_pending) {
                            lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_dual_rescue(
                                solver);
                            force_extreme_followup_immediate_classified = 1;
                        }
                        if (arm_shadow_guard_followup_pending) {
                            lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue(
                                solver);
                            shadow_followup_immediate_classified = 1;
                        }
                        if (rescue_result == 1) return P1_ZONE_CONTINUE;
                        if (rescue_result < 0) return P1_ZONE_RETURN_FAIL;
                    } else if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                        lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                            solver,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                        rs->progress.no_pivot_force_pending = 1;
                        rs->progress.no_pivot_force_reason =
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                        if (arm_force_extreme_followup_pending) {
                            lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_forced_refactor(
                                solver);
                            force_extreme_followup_immediate_classified = 1;
                        }
                        if (arm_shadow_guard_followup_pending) {
                            lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor(
                                solver);
                            shadow_followup_immediate_classified = 1;
                        }
                    }
                }
                if (arm_force_extreme_followup_pending &&
                    !force_extreme_followup_immediate_classified) {
                    lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_retry(
                        solver);
                }
                if (arm_shadow_guard_followup_pending &&
                    !shadow_followup_immediate_classified) {
                    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry(
                        solver);
                }
            }
            return P1_ZONE_CONTINUE;
        }
        rs->numerical.dir_stabilize_cooldown = dir_stabilize_cooldown_target;
        rs->cycling.use_bland = 1;
    }

    /* Write back locals that may have been modified. */
    ctx->entering = entering;
    ctx->leaving = leaving;
    ctx->theta = theta;
    return P1_ZONE_PROCEED;
}
