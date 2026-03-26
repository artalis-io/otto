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
