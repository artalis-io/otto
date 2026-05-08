/*
 * simplex_phase2_zones.c - Phase 2 zone handler implementations.
 *
 * Each function corresponds to one zone in the Phase 2 main loop.
 * The main loop in simplex.c calls these sequentially per iteration.
 *
 * Zero behavior change — pure code motion from simplex.c.
 */

#include "simplex_phase2_zones.h"
#include "simplex_internal.h"
#include "simplex_pricing.h"
#include "simplex_ratio.h"
#include "simplex_perturb.h"
#include "lp_refactor_policy.h"
#include "lp_glpk_strict.h"
#include "lp_basis_governor.h"
#include "lp_log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define P2_RATIO_ALT_ENTERING_LIMIT 16

static void p2_reset_devex_reference(SimplexTableau *tab) {
    if (!tab || tab->pricing_strategy != 2 || !tab->use_steepest_edge ||
        !tab->se_weights || !tab->A_ext) {
        return;
    }

    for (int j = 0; j < tab->n; j++) {
        double col_norm_sq = 0.0;
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            col_norm_sq += tab->A_ext->values[p] * tab->A_ext->values[p];
        }
        tab->se_weights[j] = (col_norm_sq > 1.0) ? col_norm_sq : 1.0;
    }
    tab->devex_refcount = 0;
    tab->partial_price_pos = 0;
}

static int p2_bound_infeasibility_exceeds_scaled_tolerance(
    const SimplexTableau *tab,
    int basic_only) {
    if (!tab) return 0;
    int limit = basic_only ? tab->m : tab->n;
    for (int k = 0; k < limit; k++) {
        int j = basic_only ? tab->basis[k] : k;
        double infeas = 0.0;
        double bound_scale;
        double allowed;
        if (j < 0 || j >= tab->n) return 1;
        bound_scale = fmax(1.0, fabs(tab->x[j]));
        if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL) {
            infeas = tab->lb_ext[j] - tab->x[j];
            bound_scale = fmax(bound_scale, fabs(tab->lb_ext[j]));
        } else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
            infeas = tab->x[j] - tab->ub_ext[j];
            bound_scale = fmax(bound_scale, fabs(tab->ub_ext[j]));
        }
        allowed = 2000.0 * RALPH_FEAS_TOL * bound_scale;
        if (infeas > allowed) return 1;
    }
    return 0;
}

static P2ZoneResult p2_finalize_optimal(SimplexSolver *solver,
                                        SimplexTableau *tab,
                                        P2IterState *st,
                                        int iter) {
    primal_remove_perturbation(tab);
    tableau_compute_solution(tab);

    if (p2_bound_infeasibility_exceeds_scaled_tolerance(tab, 0)) {
        /* Removing primal bound shifts can leave a dual-feasible basis that is
         * slightly primal-infeasible under the original bounds.  Finish with
         * dual simplex, the standard unshift cleanup for a perturbed primal
         * optimum, before accepting OPTIMAL. */
        int m = tab->m;
        int n = tab->n;
        int *saved_basis = (int*)malloc((size_t)m * sizeof(int));
        int *saved_basis_pos = (int*)malloc((size_t)n * sizeof(int));
        VarStatus *saved_status = (VarStatus*)malloc((size_t)n * sizeof(VarStatus));
        double *saved_x = (double*)malloc((size_t)n * sizeof(double));
        int saved_solver_status = solver->status;
        if (!saved_basis || !saved_basis_pos || !saved_status || !saved_x) {
            free(saved_basis);
            free(saved_basis_pos);
            free(saved_status);
            free(saved_x);
            return P2_ZONE_RETURN_FAIL;
        }
        memcpy(saved_basis, tab->basis, (size_t)m * sizeof(int));
        memcpy(saved_basis_pos, tab->basis_pos, (size_t)n * sizeof(int));
        memcpy(saved_status, tab->var_status, (size_t)n * sizeof(VarStatus));
        memcpy(saved_x, tab->x, (size_t)n * sizeof(double));

        tableau_compute_reduced_costs(tab);
        if (dual_simplex_solve_v2(solver) == 0 &&
            solver->status == RALPH_STATUS_OPTIMAL) {
            free(solver->solution);
            free(solver->dual_solution);
            free(solver->reduced_costs);
            solver->solution = NULL;
            solver->dual_solution = NULL;
            solver->reduced_costs = NULL;
            tab->phase = 2;
        } else {
            tableau_compute_solution(tab);
            if (!p2_bound_infeasibility_exceeds_scaled_tolerance(tab, 0)) {
                tableau_compute_reduced_costs(tab);
                tab->phase = 2;
                solver->status = saved_solver_status;
                if (st) {
                    st->last_obj = tab->obj_value;
                    st->stall_count = 0;
                    st->perturb_attempts = 0;
                    st->perturbation_active = 0;
                    st->last_entering = -1;
                    st->last_leaving = -1;
                    st->repeat_entering_streak = 0;
                    st->repeat_leaving_streak = 0;
                }
                free(saved_basis);
                free(saved_basis_pos);
                free(saved_status);
                free(saved_x);
                return P2_ZONE_CONTINUE;
            }
            memcpy(tab->basis, saved_basis, (size_t)m * sizeof(int));
            memcpy(tab->basis_pos, saved_basis_pos, (size_t)n * sizeof(int));
            memcpy(tab->var_status, saved_status, (size_t)n * sizeof(VarStatus));
            memcpy(tab->x, saved_x, (size_t)n * sizeof(double));
            tab->phase = 2;
            solver->status = saved_solver_status;
            tab->basis_cache_valid = 0;
            tab->basis_cache_total_nnz = 0;
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PHASE_TRANSITION) != 0) {
                free(saved_basis);
                free(saved_basis_pos);
                free(saved_status);
                free(saved_x);
                return P2_ZONE_RETURN_FAIL;
            }
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
            if (p2_bound_infeasibility_exceeds_scaled_tolerance(tab, 0)) {
                solver->status = RALPH_STATUS_ERROR;
                free(saved_basis);
                free(saved_basis_pos);
                free(saved_status);
                free(saved_x);
                return P2_ZONE_RETURN_FAIL;
            }
        }
        free(saved_basis);
        free(saved_basis_pos);
        free(saved_status);
        free(saved_x);
    }

    solver->current_phase = SIMPLEX_PHASE_OPTIMAL;
    solver->status = RALPH_STATUS_OPTIMAL;
    solver->iterations = iter;
    solver->degenerate_pivots = st ? st->degenerate_count : 0;
    tableau_compute_solution(tab);
    solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
    return P2_ZONE_RETURN_OPTIMAL;
}

/* ── Zone 1: Pre-iteration ─────────────────────────────────────────── */

P2ZoneResult p2_zone_pre_iter(SimplexSolver *solver,
                              SimplexTableau *tab,
                              P2IterState *st,
                              int iter) {
    (void)iter;

    st->periodic_policy_cooldown =
        lp_refactor_policy_periodic_cooldown_tick(st->periodic_policy_cooldown);
    st->periodic_policy_pressure_decay =
        lp_refactor_policy_periodic_pressure_decay_recover(
            2, st->periodic_policy_pressure_decay);

    /* T3.1: Objective limit early-exit (internal minimization space) */
    if (solver->objective_limit < RALPH_INFINITY &&
        tab->obj_value >= solver->objective_limit) {
        primal_remove_perturbation(tab);
        tableau_compute_solution(tab);
        solver->status = RALPH_STATUS_OBJ_LIMIT;
        solver->iterations = iter;
        solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
        return P2_ZONE_RETURN_OPTIMAL;
    }

    return P2_ZONE_PROCEED;
}

/* ── Zone 2: Pricing ───────────────────────────────────────────────── */

P2ZoneResult p2_zone_pricing(SimplexSolver *solver,
                             SimplexTableau *tab,
                             P2IterState *st,
                             int iter) {
    double t_pricing_ms = lp_telemetry_timer_start();
    st->adaptive_devex_partial =
        phase2_use_adaptive_devex_partial(tab,
                                          iter,
                                          st->degenerate_count,
                                          (st->use_bland || iter < st->bland_start_iters),
                                          solver->pricing_strategy);
    if (solver->telemetry_enabled) {
        if (st->use_bland || iter < st->bland_start_iters) {
            solver->telemetry.perf_phase2_bland_pricing_iters++;
        } else if (solver->pricing_strategy == 2 &&
                   st->adaptive_devex_partial &&
                   (iter & DEVEX_PARTIAL_FULL_RESCAN_MASK) != 0) {
            solver->telemetry.perf_phase2_adaptive_devex_partial_iters++;
        }
    }

    st->price_status = pricing_dispatch(tab, solver->pricing_strategy,
                                        (st->use_bland || iter < st->bland_start_iters),
                                        st->adaptive_devex_partial, iter, &st->entering);
    {
        lp_telemetry_record_pricing_timed(solver, 2, t_pricing_ms);
    }

    if (st->price_status != 0) {
        int confirm = phase2_confirm_optimality(solver, iter, &st->entering);
        if (confirm < 0) {
            primal_remove_perturbation(tab);
            return P2_ZONE_RETURN_FAIL;
        }
        if (confirm > 0) {
            return p2_finalize_optimal(solver, tab, st, iter);
        }
    }

    return P2_ZONE_PROCEED;
}

/* ── Zone 3: Ratio test ────────────────────────────────────────────── */

P2ZoneResult p2_zone_ratio(SimplexSolver *solver,
                           SimplexTableau *tab,
                           P2IterState *st,
                           int iter) {
    double t_ratio_ms = lp_telemetry_timer_start();

    st->ratio_status = primal_ratio_test_with_policy(solver,
                                                     tab,
                                                     st->use_bland,
                                                     st->entering,
                                                     &st->leaving,
                                                     &st->theta);
    {
        lp_telemetry_record_ratio_timed(solver, 2, t_ratio_ms);
    }

    if (st->ratio_status != 0) {
        /* No leaving variable found — possibly unbounded.
         * Stale LU factors can produce spurious theta=inf (e.g., lotfi).
         * Refactorize and retry once before declaring UNBOUNDED. */
        int refactor_rc;
        int degen_episode =
            (st->degenerate_count > 0 || st->use_bland || st->perturbation_active);
        {
            double t_refactor_ms = lp_telemetry_timer_start();
            refactor_rc = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
        }
        if (degen_episode) {
            lp_telemetry_record_phase2_degenerate_refactor(
                solver,
                RALPH_REFACTOR_REASON_RATIO_RECOVERY,
                0,
                lp_telemetry_refactor_reason_is_safety_forced(
                    RALPH_REFACTOR_REASON_RATIO_RECOVERY));
        }
        if (refactor_rc == 0) {
            tableau_compute_solution(tab);
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);
            } else {
                tableau_compute_reduced_costs(tab);
                if (solver->pricing_strategy == 4) heap_build(tab);
            }

            /* Re-price: the entering variable may no longer be eligible */
            t_ratio_ms = lp_telemetry_timer_start();
            st->price_status = pricing_dispatch(tab, solver->pricing_strategy,
                                                (st->use_bland || iter < st->bland_start_iters),
                                                st->adaptive_devex_partial, iter, &st->entering);
            {
                lp_telemetry_record_pricing_timed(solver, 2, t_ratio_ms);
            }

            if (st->price_status != 0) {
                int confirm = phase2_confirm_optimality(solver, iter, &st->entering);
                if (confirm < 0) {
                    primal_remove_perturbation(tab);
                    return P2_ZONE_RETURN_FAIL;
                }
                if (confirm > 0) {
                    return p2_finalize_optimal(solver, tab, st, iter);
                }
            }

            /* Retry ratio test with fresh LU */
            t_ratio_ms = lp_telemetry_timer_start();
            st->ratio_status = primal_ratio_test_with_policy(solver,
                                                             tab,
                                                             st->use_bland,
                                                             st->entering,
                                                             &st->leaving,
                                                             &st->theta);
            {
                lp_telemetry_record_ratio_timed(solver, 2, t_ratio_ms);
            }
        }

        if (st->ratio_status != 0) {
            if (st->use_bland) {
                int excluded[P2_RATIO_ALT_ENTERING_LIMIT];
                int excluded_count = 0;
                int alt_entering = -1;

                excluded[excluded_count++] = st->entering;
                while (excluded_count < P2_RATIO_ALT_ENTERING_LIMIT &&
                       pricing_bland_excluding_set(tab,
                                                   excluded,
                                                   excluded_count,
                                                   &alt_entering) == 0) {
                    st->entering = alt_entering;
                    t_ratio_ms = lp_telemetry_timer_start();
                    st->ratio_status = primal_ratio_test_with_policy(solver,
                                                                     tab,
                                                                     st->use_bland,
                                                                     st->entering,
                                                                     &st->leaving,
                                                                     &st->theta);
                    {
                        lp_telemetry_record_ratio_timed(solver, 2, t_ratio_ms);
                    }
                    if (st->ratio_status == 0) {
                        break;
                    }
                    excluded[excluded_count++] = alt_entering;
                }
            }
        }

        if (st->ratio_status != 0) {
            double dir = (tab->var_status[st->entering] == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;
            extract_unbounded_ray(solver, st->entering, dir);
            primal_remove_perturbation(tab);
            solver->current_phase = SIMPLEX_PHASE_UNBOUNDED;
            solver->status = RALPH_STATUS_UNBOUNDED;
            solver->iterations = iter;
            return P2_ZONE_RETURN_FAIL;
        }
        /* Recovery succeeded — fall through to pivot */
    }

    return P2_ZONE_PROCEED;
}

/* ── Zone 4: Pre-pivot ─────────────────────────────────────────────── */

P2ZoneResult p2_zone_pre_pivot(SimplexSolver *solver,
                               SimplexTableau *tab,
                               P2IterState *st,
                               int iter) {
    /* Direction geometry telemetry */
    {
        double phase2_dir_inf = 0.0;
        int phase2_dir_nnz = 0;
        double phase2_pivot_abs = 0.0;

        phase1_direction_shape_from_vector(
            tab->work2,
            tab->m,
            st->leaving,
            &phase2_dir_inf,
            &phase2_dir_nnz,
            &phase2_pivot_abs);
        lp_telemetry_record_phase2_pivot_geometry(
            solver,
            st->theta,
            phase2_dir_inf,
            phase2_pivot_abs);
    }

    /* Repeat entering/leaving tracking */
    if (solver->telemetry_enabled) {
        if (st->entering == st->last_entering) {
            st->repeat_entering_streak++;
            solver->telemetry.perf_phase2_repeat_entering_events++;
            if (st->repeat_entering_streak >
                solver->telemetry.perf_phase2_repeat_entering_max_streak) {
                solver->telemetry.perf_phase2_repeat_entering_max_streak =
                    st->repeat_entering_streak;
            }
        } else {
            st->repeat_entering_streak = 0;
        }
        st->last_entering = st->entering;

        if (st->leaving >= 0 && st->leaving == st->last_leaving) {
            st->repeat_leaving_streak++;
            solver->telemetry.perf_phase2_repeat_leaving_events++;
            if (st->repeat_leaving_streak >
                solver->telemetry.perf_phase2_repeat_leaving_max_streak) {
                solver->telemetry.perf_phase2_repeat_leaving_max_streak =
                    st->repeat_leaving_streak;
            }
        } else {
            st->repeat_leaving_streak = 0;
        }
        st->last_leaving = (st->leaving >= 0) ? st->leaving : -1;
    }

    /* Track degenerate/near-degenerate pivots for cycling prevention
     *
     * Strategy:
     * 1. After 30 degenerate pivots: apply bound perturbation
     * 2. After 100 more degenerate pivots: switch to Bland's rule
     * 3. After 100 non-degenerate pivots: reset and try faster methods
     */
    {
    if (st->theta < P2_NEAR_DEGEN_TOL) {
        if (solver->telemetry_enabled && st->degenerate_count == 0) {
            solver->telemetry.perf_phase2_degenerate_episodes++;
        }
        st->degenerate_count++;
        if (solver->telemetry_enabled &&
            st->degenerate_count > solver->telemetry.perf_phase2_degenerate_streak_max) {
            solver->telemetry.perf_phase2_degenerate_streak_max = st->degenerate_count;
        }
        st->non_degen_streak = 0;

        /* First try perturbation */
        if (st->degenerate_count >= P2_PERTURB_THRESHOLD && !st->perturbation_active && !st->use_bland) {
            primal_apply_perturbation(tab);
            st->perturbation_active = 1;
            if (solver->telemetry_enabled) {
                solver->telemetry.perf_phase2_perturb_applied++;
            }
            if (solver->verbose) {
                LP_LOG_STDOUT("Iter %d: Applying perturbation due to degeneracy\n", iter);
            }
        }

        /* If still cycling after perturbation, use Bland's rule */
        if (st->degenerate_count >= P2_DEGEN_THRESHOLD && !st->use_bland) {
            st->use_bland = 1;
            if (solver->telemetry_enabled) {
                solver->telemetry.perf_phase2_bland_enter_episodes++;
            }
            if (solver->verbose) {
                LP_LOG_STDOUT("Iter %d: Switching to Bland's rule due to potential cycling\n", iter);
            }
        }
    } else {
        /* Only reset after many consecutive non-degenerate pivots */
        st->non_degen_streak++;
        st->degenerate_count = 0;
        if (st->use_bland && st->non_degen_streak >= P2_NON_DEGEN_THRESHOLD) {
            st->use_bland = 0;
            p2_reset_devex_reference(tab);
            if (solver->telemetry_enabled) {
                solver->telemetry.perf_phase2_bland_exit_episodes++;
            }
            st->non_degen_streak = 0;
            if (solver->verbose) {
                LP_LOG_STDOUT("Iter %d: Turning off Bland's rule after %d non-degenerate pivots\n",
                       iter, P2_NON_DEGEN_THRESHOLD);
            }
        }
    }
    }  /* end degeneracy tracking block */

    return P2_ZONE_PROCEED;
}

/* ── Zone 5: Pivot ─────────────────────────────────────────────────── */

P2ZoneResult p2_zone_pivot(SimplexSolver *solver,
                           SimplexTableau *tab,
                           P2IterState *st,
                           int iter) {
    int pivot_rc;
    {
        double t_pivot_ms = lp_telemetry_timer_start();
        pivot_rc = simplex_pivot(tab, st->entering, st->leaving, st->theta, 0);
        lp_telemetry_record_pivot_timed(solver, 2, t_pivot_ms);
    }
    if (pivot_rc != 0) {
        if (solver->verbose) {
            LP_LOG_STDERR("[primal_simplex] Pivot failed at iter %d (entering=%d, leaving=%d, theta=%e), attempting recovery\n",
                    iter, st->entering, st->leaving, st->theta);
        }
        /* Pivot failed - the basis was partially updated in simplex_pivot.
         * Try to recover by refactorizing the current (post-pivot) basis. */
        int rc_refactor;
        int degen_episode =
            (st->degenerate_count > 0 || st->use_bland || st->perturbation_active);
        {
            double t_refactor_ms = lp_telemetry_timer_start();
            rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY);
            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
        }
        if (degen_episode) {
            lp_telemetry_record_phase2_degenerate_refactor(
                solver,
                RALPH_REFACTOR_REASON_PIVOT_RECOVERY,
                0,
                lp_telemetry_refactor_reason_is_safety_forced(
                    RALPH_REFACTOR_REASON_PIVOT_RECOVERY));
        }
        if (rc_refactor == 0) {
            /* Refactorization succeeded - recompute and continue */
            tableau_compute_solution(tab);
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);
            } else {
                tableau_compute_reduced_costs(tab);
                if (solver->pricing_strategy == 4) heap_build(tab);
            }
            if (solver->verbose) {
                LP_LOG_STDERR("[primal_simplex] Recovery via refactorization at iter %d\n", iter);
            }
            return P2_ZONE_CONTINUE;
        }
        /* Refactorization failed - try basis repair */
        if (repair_singular_basis(tab) == 0) {
            tableau_compute_solution(tab);
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);
            } else {
                tableau_compute_reduced_costs(tab);
                if (solver->pricing_strategy == 4) heap_build(tab);
            }
            if (solver->verbose) {
                LP_LOG_STDERR("[primal_simplex] Recovery via basis repair at iter %d\n", iter);
            }
            return P2_ZONE_CONTINUE;
        }
        /* All recovery attempts failed */
        if (solver->verbose) {
            LP_LOG_STDERR("[primal_simplex] ERROR: all recovery attempts failed at iter %d\n", iter);
        }
        primal_remove_perturbation(tab);
        solver->status = RALPH_STATUS_ERROR;
        return P2_ZONE_RETURN_FAIL;
    }

    return P2_ZONE_PROCEED;
}

/* ── Zone 6: Post-pivot ────────────────────────────────────────────── */

P2ZoneResult p2_zone_post_pivot(SimplexSolver *solver,
                                SimplexTableau *tab,
                                P2IterState *st,
                                int iter) {
    /* Refactorize if needed.
     * For two-phase problems, periodic refresh is adaptive (interval + LU health). */
    {
        double phase2_hot_ms_now = phase_hotpath_ms(solver, 2);
        double iter_hot_ms = phase2_hot_ms_now - st->phase2_hot_ms_prev;
        soft_lu_record_iter_cost(solver, 2, iter_hot_ms);
        lp_reinvert_controller_state_record_iter_cost(
            reinvert_state_for_phase(solver, 2), iter_hot_ms);
        st->phase2_hot_ms_prev = phase2_hot_ms_now;
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
                                                       st->lu_soft_health_streak);
    int lu_refactor_nominal = lu_health_decision.refactor_now;
    int lu_refactor_needed = lu_health_decision.refactor_now;
    int lu_soft_cost_deferred = 0;
    int needs_refactor = lu_refactor_needed;
    int periodic_refactor = 0;
    int periodic_refactor_nominal = 0;
    int reinvert_periodic_candidate = 0;
    int reinvert_control_periodic =
        reinvert_controller_controls_periodic_phase(solver, 2);
    int cooldown_eligible = 0;
    double effective_policy_pressure = 0.0;
    double periodic_feedback_bias = periodic_feedback_bias_for_phase(solver, 2);
    LPPeriodicRefactorPolicy periodic_policy = {0, 0, 0.0, 0.0};
    LPReinvertShadowEval reinvert_shadow_eval;
    reinvert_shadow_eval_reset(&reinvert_shadow_eval);
    st->lu_soft_health_streak = lu_health_decision.soft_breach_streak_next;
    if (lu_health_decision.hard_trigger) {
        st->periodic_policy_cooldown = 0;
        st->periodic_policy_pressure_decay = 0.0;
        soft_lu_reset_defer_streak(solver, 2);
        periodic_cost_reset_defer_streak(solver, 2);
    } else if (lu_refactor_needed) {
        periodic_cost_reset_defer_streak(solver, 2);
    } else if (!lu_health_decision.soft_trigger || !solver->policy.soft_lu_cost_gate_enabled) {
        soft_lu_reset_defer_streak(solver, 2);
    }
    if (lu_refactor_needed &&
        solver->policy.soft_lu_cost_gate_enabled &&
        lu_health_decision.soft_trigger &&
        !lu_health_decision.hard_trigger) {
        int cap_blocked = 0;
        int next_consecutive = 0;
        int should_defer = simplex_soft_lu_defer_plan_for_test(
            2,
            tab->m,
            st->use_bland,
            st->degenerate_count,
            lu_get_num_updates(tab->lu),
            lu_get_max_updates(tab->lu),
            lu_get_spike_pool_used(tab->lu),
            lu_get_spike_pool_capacity(tab->lu),
            lu_get_cond_estimate(tab->lu),
            lu_get_growth_factor(tab->lu),
            soft_lu_refactor_cost_ewma(solver, 2),
            soft_lu_iter_cost_ewma(solver, 2),
            soft_lu_consecutive_defers(solver, 2),
            NULL,
            &cap_blocked,
            &next_consecutive);
        if (should_defer) {
            lu_refactor_needed = 0;
            lu_soft_cost_deferred = 1;
            soft_lu_record_defer(solver, 2);
            soft_lu_set_consecutive_defers(solver, 2, next_consecutive);
            needs_refactor = 0;
        } else {
            soft_lu_reset_defer_streak(solver, 2);
            if (cap_blocked) {
                soft_lu_record_cap_forced(solver, 2);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[primal_simplex] Soft LU defer cap reached; forcing periodic LU-health refactor (updates=%d/%d, degen=%d)\n",
                            lu_get_num_updates(tab->lu),
                            lu_get_max_updates(tab->lu),
                            st->degenerate_count);
                }
            }
        }
    }
    if (!lu_refactor_needed) {
        LPPeriodicRefactorPlan periodic_plan = lp_refactor_policy_periodic_plan(
            2,
            iter,
            tab->m,
            lu_get_max_updates(tab->lu),
            lu_get_num_updates(tab->lu),
            lu_get_spike_pool_used(tab->lu),
            lu_get_spike_pool_capacity(tab->lu),
            lu_get_cond_estimate(tab->lu),
            lu_get_growth_factor(tab->lu),
            st->use_bland,
            st->degenerate_count,
            periodic_feedback_bias,
            0,
            st->periodic_policy_cooldown,
            st->periodic_policy_pressure_decay);
        periodic_policy = periodic_plan.policy;
        cooldown_eligible = periodic_plan.cooldown_eligible;
        effective_policy_pressure = periodic_plan.effective_run_pressure;
        periodic_refactor = periodic_plan.should_run;
        periodic_refactor_nominal = periodic_refactor;
        reinvert_periodic_candidate = periodic_refactor_nominal;
        reinvert_shadow_prepare_phase(solver,
                                      tab,
                                      2,
                                      iter,
                                      &lu_health_decision,
                                      periodic_refactor_nominal,
                                      periodic_policy.min_update_age,
                                      st->periodic_policy_cooldown,
                                      reinvert_control_periodic,
                                      &reinvert_periodic_candidate,
                                      &reinvert_shadow_eval);
        if (reinvert_control_periodic) {
            periodic_refactor = reinvert_periodic_candidate;
            periodic_refactor_nominal = periodic_refactor;
            periodic_cost_reset_defer_streak(solver, 2);
        } else {
            if (periodic_refactor &&
                cooldown_eligible &&
                st->periodic_policy_cooldown > 0) {
                periodic_refactor = 0;
            }
            if (periodic_refactor) {
                if (solver->policy.periodic_cost_gate_enabled) {
                    int cap_blocked = 0;
                    int next_consecutive = 0;
                    int gate_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE;
                    int should_defer = simplex_periodic_cost_defer_plan_for_test(
                        2,
                        tab->m,
                        st->use_bland,
                        st->degenerate_count,
                        lu_get_num_updates(tab->lu),
                        lu_get_max_updates(tab->lu),
                        lu_get_spike_pool_used(tab->lu),
                        lu_get_spike_pool_capacity(tab->lu),
                        lu_get_cond_estimate(tab->lu),
                        lu_get_growth_factor(tab->lu),
                        soft_lu_refactor_cost_ewma(solver, 2),
                        soft_lu_iter_cost_ewma(solver, 2),
                        periodic_cost_refactor_samples(solver, 2),
                        periodic_cost_iter_samples(solver, 2),
                        periodic_cost_consecutive_defers(solver, 2),
                        &gate_reason,
                        NULL,
                        &cap_blocked,
                        &next_consecutive);
                    periodic_cost_record_gate_reason(
                        solver, 2, (LPPeriodicCostDampenReason)gate_reason);
                    if (should_defer) {
                        periodic_refactor = 0;
                        periodic_cost_record_defer(solver, 2);
                        periodic_cost_set_consecutive_defers(solver, 2, next_consecutive);
                        if (solver->verbose >= 2) {
                            LP_LOG_STDERR("[primal_simplex] Deferred policy periodic refactor by cost gate (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                                    lu_get_num_updates(tab->lu),
                                    lu_get_max_updates(tab->lu),
                                    st->degenerate_count,
                                    soft_lu_iter_cost_ewma(solver, 2),
                                    soft_lu_refactor_cost_ewma(solver, 2));
                        }
                    } else {
                        periodic_cost_reset_defer_streak(solver, 2);
                        if (cap_blocked) {
                            periodic_cost_record_cap_forced(solver, 2);
                            if (solver->verbose >= 2) {
                                LP_LOG_STDERR("[primal_simplex] Policy periodic defer cap reached; forcing periodic policy refactor (updates=%d/%d, degen=%d)\n",
                                        lu_get_num_updates(tab->lu),
                                        lu_get_max_updates(tab->lu),
                                        st->degenerate_count);
                            }
                        } else if (solver->verbose >= 3) {
                            LP_LOG_STDERR("[primal_simplex] Policy periodic cost gate blocked defer: %s\n",
                                    lp_refactor_policy_periodic_cost_dampen_reason_string(
                                        (LPPeriodicCostDampenReason)gate_reason));
                        }
                    }
                } else {
                    periodic_cost_reset_defer_streak(solver, 2);
                }
            }
        }
        needs_refactor = periodic_refactor;
    } else {
        reinvert_shadow_prepare_phase(solver,
                                      tab,
                                      2,
                                      iter,
                                      &lu_health_decision,
                                      0,
                                      periodic_policy.min_update_age,
                                      st->periodic_policy_cooldown,
                                      0,
                                      NULL,
                                      &reinvert_shadow_eval);
    }
    if (periodic_refactor) {
        periodic_feedback_set_hint(solver, 2, periodic_policy.interval, effective_policy_pressure);
    }
    {
        int shadow_refactor = lp_basis_governor_shadow_decide(
            LP_BASIS_GOV_PHASE2,
            lu_refactor_nominal,
            periodic_refactor_nominal);
        int governed_refactor = lp_basis_governor_decide_refactor(
            &solver->policy.basis_governor,
            LP_BASIS_GOV_PHASE2,
            lu_refactor_nominal,
            periodic_refactor_nominal,
            needs_refactor);
        if (solver->telemetry_enabled) {
            lp_basis_governor_observe_refactor(
                &solver->policy.basis_governor,
                LP_BASIS_GOV_PHASE2,
                shadow_refactor,
                governed_refactor);
        }
        needs_refactor = governed_refactor;
    }
    reinvert_shadow_finalize_phase(solver, 2, &reinvert_shadow_eval, needs_refactor);

    if (needs_refactor) {
        soft_lu_reset_defer_streak(solver, 2);
        periodic_cost_reset_defer_streak(solver, 2);
        runtime_record_periodic_refactor_trigger(solver, 2, lu_refactor_needed);
        int rc_refactor;
        double refactor_elapsed_ms = 0.0;
        int degen_episode =
            (st->degenerate_count > 0 || st->use_bland || st->perturbation_active);
        {
            double t_refactor_ms = lp_telemetry_timer_start();
            rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
            refactor_elapsed_ms = lp_telemetry_timer_elapsed_ms(t_refactor_ms);
            lp_telemetry_add_refactor_runtime_ms(solver, refactor_elapsed_ms);
        }
        if (degen_episode) {
            lp_telemetry_record_phase2_degenerate_refactor(
                solver,
                RALPH_REFACTOR_REASON_PERIODIC,
                lu_refactor_needed,
                lp_telemetry_refactor_reason_is_safety_forced(
                    RALPH_REFACTOR_REASON_PERIODIC));
        }
        if (rc_refactor == 0) {
            soft_lu_record_refactor_cost(solver, 2, refactor_elapsed_ms);
            lp_reinvert_controller_state_record_refactor_cost(
                reinvert_state_for_phase(solver, 2), refactor_elapsed_ms);
        }
        if (lu_refactor_needed && rc_refactor == 0) {
            st->lu_soft_health_streak = 0;
        }
        if (!lu_refactor_needed && periodic_refactor) {
            lp_refactor_policy_periodic_post_refactor_update(
                2,
                cooldown_eligible,
                periodic_policy.interval,
                rc_refactor,
                &st->periodic_policy_cooldown,
                &st->periodic_policy_pressure_decay);
        }
        if (rc_refactor != 0) {
            if (solver->verbose) {
                LP_LOG_STDERR("[primal_simplex] ERROR: refactorization failed at iter %d, attempting repair\n", iter);
            }
            /* Try to repair the singular basis */
            if (repair_singular_basis(tab) != 0) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[primal_simplex] ERROR: basis repair failed at iter %d\n", iter);
                }
                primal_remove_perturbation(tab);
                solver->status = RALPH_STATUS_ERROR;
                solver->iterations = iter;
                return P2_ZONE_RETURN_FAIL;
            }
            if (solver->verbose) {
                LP_LOG_STDERR("[primal_simplex] Basis repaired at iter %d\n", iter);
            }
        }
        /* After refactorization, recompute solution to eliminate drift */
        tableau_compute_solution(tab);
        /* For partial pricing, use lazy RC computation (duals only).
         * For other strategies, compute full RC for incremental updates. */
        if (solver->pricing_strategy == 3) {
            tableau_compute_duals(tab);  /* Lazy mode: duals only */
        } else {
            tableau_compute_reduced_costs(tab);  /* Full RC for incremental updates */
            if (solver->pricing_strategy == 4) heap_build(tab);
        }
    } else if (lu_soft_cost_deferred && solver->verbose >= 2) {
        LP_LOG_STDERR("[primal_simplex] Deferred soft LU-health periodic refactor (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                lu_get_num_updates(tab->lu),
                lu_get_max_updates(tab->lu),
                st->degenerate_count,
                soft_lu_iter_cost_ewma(solver, 2),
                soft_lu_refactor_cost_ewma(solver, 2));
    }

    /* Periodically recompute solution and reduced costs to correct numerical drift.
     * For large, highly-degenerate phase-2 runs with healthy LU metrics, we relax
     * cadence to reduce full-vector recompute overhead. */
    {
        int periodic_recompute_interval =
            lp_refactor_policy_phase2_periodic_recompute_interval(
                tab->m,
                st->use_bland,
                st->degenerate_count,
                lu_get_spike_pool_used(tab->lu),
                lu_get_spike_pool_capacity(tab->lu),
                lu_get_cond_estimate(tab->lu),
                lu_get_growth_factor(tab->lu));
        if (iter > 0 &&
            periodic_recompute_interval > 0 &&
            (iter % periodic_recompute_interval) == 0) {
            tableau_compute_solution(tab);
            /* For partial pricing, use lazy RC. For others, full RC. */
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);  /* Lazy mode */
            } else {
                tableau_compute_reduced_costs(tab);  /* Full recomputation */
                if (solver->pricing_strategy == 4) heap_build(tab);
            }
            if (solver->verbose) {
                int leave_var = (st->leaving >= 0) ? tab->basis[st->leaving] : st->leaving;
                LP_LOG_STDOUT("Iter %d: obj = %.6f, enter=%d, leave=%d, theta=%.2e, rc=%.2e\n",
                       iter, tab->obj_value, st->entering, leave_var, st->theta, tab->rc[st->entering]);
            }
        }
    }

    /* Stall detection: check objective progress after each pivot.
     * If objective hasn't improved for P2_STALL_THRESHOLD iterations,
     * remove+re-apply perturbation with progressive scaling to break
     * the cycling pattern. This catches cases where Bland's rule is
     * active but making negligible progress (O(2^n) worst case).
     * Independent of the degeneracy counter — triggered by objective stagnation. */
    {
    double obj_tol_p2 = fmax(1e-9, 10.0 * RALPH_OPT_TOL);
    double obj_change_p2 = fabs(tab->obj_value - st->last_obj);
    if (obj_change_p2 < obj_tol_p2) {
        st->stall_count++;
        if (st->stall_count >= P2_STALL_THRESHOLD &&
            st->perturb_attempts < PHASE2_DEGEN_ESCAPE_MAX_ATTEMPTS &&
            tab->m >= PHASE2_DEGEN_ESCAPE_MIN_M &&
            st->degenerate_count >= PHASE2_DEGEN_ESCAPE_DEGEN_TRIGGER &&
            periodic_policy_refactor_count(solver, 2) >= PHASE2_DEGEN_ESCAPE_POLICY_TRIGGER) {
            double scale = 4.0 + 2.0 * (double)st->perturb_attempts;
            primal_apply_perturbation_scaled(tab, scale);
            st->perturbation_active = 1;
            if (solver->telemetry_enabled) {
                solver->telemetry.perf_phase2_degen_escape_triggers++;
                solver->telemetry.perf_phase2_perturb_applied++;
                if (!st->use_bland) {
                    solver->telemetry.perf_phase2_bland_enter_episodes++;
                }
            }
            st->use_bland = 1;
            st->degenerate_count = 0;
            /* Keep Bland active briefly before allowing fast pricing again. */
            st->non_degen_streak = -PHASE2_DEGEN_ESCAPE_BLAND_HOLD_ITERS;
            st->stall_count = 0;
            st->perturb_attempts++;
            tableau_compute_solution(tab);
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);
            } else {
                tableau_compute_reduced_costs(tab);
                if (solver->pricing_strategy == 4) heap_build(tab);
            }
            if (solver->verbose) {
                LP_LOG_STDOUT("Iter %d: Phase 2 degen-escape (scale %.1f)\n", iter, scale);
            }
            return P2_ZONE_CONTINUE;
        }
        if (st->stall_count >= P2_STALL_THRESHOLD) {
            st->perturb_attempts++;
            if (st->perturb_attempts <= P2_MAX_PERTURB_ATTEMPTS) {
                double scale = 1.0 + 2.0 * st->perturb_attempts;
                primal_apply_perturbation_scaled(tab, scale);
                st->perturbation_active = 1;
                if (solver->telemetry_enabled) {
                    solver->telemetry.perf_phase2_perturb_applied++;
                    if (st->use_bland) {
                        solver->telemetry.perf_phase2_bland_exit_episodes++;
                    }
                }
                /* Reset Bland's rule — fresh perturbation should break the
                 * cycle, allowing faster pricing to make progress again */
                st->use_bland = 0;
                st->degenerate_count = 0;
                st->non_degen_streak = 0;
                st->stall_count = 0;
                /* Recompute after perturbation change */
                tableau_compute_solution(tab);
                if (solver->pricing_strategy == 3) {
                    tableau_compute_duals(tab);
                } else {
                    tableau_compute_reduced_costs(tab);
                    if (solver->pricing_strategy == 4) heap_build(tab);
                }
                if (solver->verbose) {
                    LP_LOG_STDOUT("Iter %d: Phase 2 stall detected, re-perturbing (attempt %d, scale %.1f)\n",
                           iter, st->perturb_attempts, scale);
                }
            }
            /* else: exhausted attempts, fall through to iteration limit */
        }
    } else {
        st->stall_count = 0;
        st->last_obj = tab->obj_value;
    }
    }  /* end stall detection block */

    return P2_ZONE_PROCEED;
}
