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
