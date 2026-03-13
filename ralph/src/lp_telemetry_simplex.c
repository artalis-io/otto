/*
 * Ralph - Simplex Iteration Telemetry Helpers
 *
 * Per-stage and per-iteration counters for primal/dual simplex execution.
 */

#include "lp.h"

#include <math.h>

static int solver_telemetry_enabled(const SimplexSolver *solver) {
    return solver && solver->telemetry_enabled;
}

void lp_telemetry_add_solver_stage_ms(SimplexSolver *solver,
                                      LPSolverStage stage,
                                      double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    switch (stage) {
        case LP_SOLVER_STAGE_PRIMAL_SETUP:
            solver->telemetry.perf_primal_setup_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_DUAL:
            solver->telemetry.perf_dual_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_PHASE1:
            solver->telemetry.perf_phase1_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_TRANSITION:
            solver->telemetry.perf_transition_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_PHASE2:
            solver->telemetry.perf_phase2_ms += elapsed_ms;
            break;
        default:
            break;
    }
}

void lp_telemetry_add_solver_stage_timed(SimplexSolver *solver,
                                         LPSolverStage stage,
                                         double start_ms) {
    lp_telemetry_add_solver_stage_ms(solver,
                                     stage,
                                     lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_refactor_runtime_ms(SimplexSolver *solver,
                                          double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_refactor_ms += elapsed_ms;
}

void lp_telemetry_add_refactor_runtime_timed(SimplexSolver *solver,
                                             double start_ms) {
    lp_telemetry_add_refactor_runtime_ms(solver,
                                         lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_ftran_ms(SimplexSolver *solver,
                               double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_ftran_ms += elapsed_ms;
    solver->telemetry.perf_ftran_calls++;
}

void lp_telemetry_add_ftran_timed(SimplexSolver *solver,
                                  double start_ms) {
    lp_telemetry_add_ftran_ms(solver,
                              lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_ftran_base_ms(SimplexSolver *solver,
                                    double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_ftran_base_ms += elapsed_ms;
}

void lp_telemetry_add_ftran_base_timed(SimplexSolver *solver,
                                       double start_ms) {
    lp_telemetry_add_ftran_base_ms(solver,
                                   lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_ftran_update_apply_ms(SimplexSolver *solver,
                                            double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_ftran_update_apply_ms += elapsed_ms;
    solver->telemetry.perf_ftran_update_apply_calls++;
}

void lp_telemetry_add_ftran_update_apply_timed(SimplexSolver *solver,
                                               double start_ms) {
    lp_telemetry_add_ftran_update_apply_ms(solver,
                                           lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_btran_ms(SimplexSolver *solver,
                               double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_btran_ms += elapsed_ms;
    solver->telemetry.perf_btran_calls++;
}

void lp_telemetry_add_btran_timed(SimplexSolver *solver,
                                  double start_ms) {
    lp_telemetry_add_btran_ms(solver,
                              lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_btran_base_ms(SimplexSolver *solver,
                                    double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_btran_base_ms += elapsed_ms;
}

void lp_telemetry_add_btran_base_timed(SimplexSolver *solver,
                                       double start_ms) {
    lp_telemetry_add_btran_base_ms(solver,
                                   lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_btran_update_apply_ms(SimplexSolver *solver,
                                            double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_btran_update_apply_ms += elapsed_ms;
    solver->telemetry.perf_btran_update_apply_calls++;
}

void lp_telemetry_add_btran_update_apply_timed(SimplexSolver *solver,
                                               double start_ms) {
    lp_telemetry_add_btran_update_apply_ms(solver,
                                           lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_record_ftran_nnz(SimplexSolver *solver,
                                   int rhs_nnz,
                                   int sol_nnz) {
    if (!solver_telemetry_enabled(solver)) return;
    if (rhs_nnz < 0 || sol_nnz < 0) return;
    solver->telemetry.perf_ftran_nnz_samples++;
    solver->telemetry.perf_ftran_rhs_nnz_total += (long long)rhs_nnz;
    solver->telemetry.perf_ftran_sol_nnz_total += (long long)sol_nnz;
}

void lp_telemetry_record_btran_nnz(SimplexSolver *solver,
                                   int rhs_nnz,
                                   int sol_nnz) {
    if (!solver_telemetry_enabled(solver)) return;
    if (rhs_nnz < 0 || sol_nnz < 0) return;
    solver->telemetry.perf_btran_nnz_samples++;
    solver->telemetry.perf_btran_rhs_nnz_total += (long long)rhs_nnz;
    solver->telemetry.perf_btran_sol_nnz_total += (long long)sol_nnz;
}

void lp_telemetry_add_lu_update_ms(SimplexSolver *solver,
                                   double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_lu_update_ms += elapsed_ms;
}

void lp_telemetry_add_lu_update_timed(SimplexSolver *solver,
                                      double start_ms) {
    lp_telemetry_add_lu_update_ms(solver,
                                  lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_record_compute_solution(SimplexSolver *solver,
                                          int phase,
                                          double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_compute_solution_ms += elapsed_ms;
    if (phase == 1) {
        solver->telemetry.perf_phase1_compute_solution_ms += elapsed_ms;
        solver->telemetry.perf_phase1_compute_solution_calls++;
    } else if (phase == 2) {
        solver->telemetry.perf_phase2_compute_solution_ms += elapsed_ms;
        solver->telemetry.perf_phase2_compute_solution_calls++;
    }
}

void lp_telemetry_record_compute_solution_timed(SimplexSolver *solver,
                                                int phase,
                                                double start_ms) {
    lp_telemetry_record_compute_solution(solver,
                                         phase,
                                         lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_record_phase1_compute_solution_context(SimplexSolver *solver,
                                                         LPPhase1ComputeContext context) {
    if (!solver_telemetry_enabled(solver)) return;
    switch (context) {
        case LP_PHASE1_COMPUTE_CTX_RECOMPUTE_FULL:
            solver->telemetry.perf_phase1_compute_solution_ctx_recompute_full++;
            break;
        case LP_PHASE1_COMPUTE_CTX_RECOMPUTE_GUARD_FORCED_FULL:
            solver->telemetry.perf_phase1_compute_solution_ctx_recompute_guard_forced_full++;
            break;
        case LP_PHASE1_COMPUTE_CTX_INIT:
            solver->telemetry.perf_phase1_compute_solution_ctx_init++;
            break;
        case LP_PHASE1_COMPUTE_CTX_NO_ENTERING_CLEANUP:
            solver->telemetry.perf_phase1_compute_solution_ctx_no_entering_cleanup++;
            break;
        case LP_PHASE1_COMPUTE_CTX_INFEAS_CLEANUP:
            solver->telemetry.perf_phase1_compute_solution_ctx_infeas_cleanup++;
            break;
        case LP_PHASE1_COMPUTE_CTX_REFACTOR_FAIL_CONTINUE:
            solver->telemetry.perf_phase1_compute_solution_ctx_refactor_fail_continue++;
            break;
        case LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY:
            solver->telemetry.perf_phase1_compute_solution_ctx_refactor_failure_recovery++;
            break;
        case LP_PHASE1_COMPUTE_CTX_REFACTOR_SUCCESS:
            solver->telemetry.perf_phase1_compute_solution_ctx_refactor_success++;
            break;
        case LP_PHASE1_COMPUTE_CTX_DRIFT_REFRESH:
            solver->telemetry.perf_phase1_compute_solution_ctx_drift_refresh++;
            break;
        case LP_PHASE1_COMPUTE_CTX_DUAL_RESCUE:
            solver->telemetry.perf_phase1_compute_solution_ctx_dual_rescue++;
            break;
        case LP_PHASE1_COMPUTE_CTX_OTHER:
        case LP_PHASE1_COMPUTE_CTX_RECOMPUTE_RC_ONLY:
        default:
            solver->telemetry.perf_phase1_compute_solution_ctx_other++;
            break;
    }
}

void lp_telemetry_record_compute_reduced_costs(SimplexSolver *solver,
                                               int phase,
                                               double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_compute_rc_ms += elapsed_ms;
    if (phase == 1) {
        solver->telemetry.perf_phase1_compute_rc_ms += elapsed_ms;
        solver->telemetry.perf_phase1_compute_rc_calls++;
    } else if (phase == 2) {
        solver->telemetry.perf_phase2_compute_rc_ms += elapsed_ms;
        solver->telemetry.perf_phase2_compute_rc_calls++;
    }
}

void lp_telemetry_record_compute_reduced_costs_timed(SimplexSolver *solver,
                                                     int phase,
                                                     double start_ms) {
    lp_telemetry_record_compute_reduced_costs(solver,
                                              phase,
                                              lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_record_phase1_compute_rc_context(SimplexSolver *solver,
                                                   LPPhase1ComputeContext context) {
    if (!solver_telemetry_enabled(solver)) return;
    switch (context) {
        case LP_PHASE1_COMPUTE_CTX_RECOMPUTE_FULL:
            solver->telemetry.perf_phase1_compute_rc_ctx_recompute_full++;
            break;
        case LP_PHASE1_COMPUTE_CTX_RECOMPUTE_RC_ONLY:
            solver->telemetry.perf_phase1_compute_rc_ctx_recompute_rc_only++;
            break;
        case LP_PHASE1_COMPUTE_CTX_RECOMPUTE_GUARD_FORCED_FULL:
            solver->telemetry.perf_phase1_compute_rc_ctx_recompute_guard_forced_full++;
            break;
        case LP_PHASE1_COMPUTE_CTX_INIT:
            solver->telemetry.perf_phase1_compute_rc_ctx_init++;
            break;
        case LP_PHASE1_COMPUTE_CTX_INFEAS_CLEANUP:
            solver->telemetry.perf_phase1_compute_rc_ctx_infeas_cleanup++;
            break;
        case LP_PHASE1_COMPUTE_CTX_REFACTOR_FAIL_CONTINUE:
            solver->telemetry.perf_phase1_compute_rc_ctx_refactor_fail_continue++;
            break;
        case LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY:
            solver->telemetry.perf_phase1_compute_rc_ctx_refactor_failure_recovery++;
            break;
        case LP_PHASE1_COMPUTE_CTX_REFACTOR_SUCCESS:
            solver->telemetry.perf_phase1_compute_rc_ctx_refactor_success++;
            break;
        case LP_PHASE1_COMPUTE_CTX_DRIFT_REFRESH:
            solver->telemetry.perf_phase1_compute_rc_ctx_drift_refresh++;
            break;
        case LP_PHASE1_COMPUTE_CTX_DUAL_RESCUE:
            solver->telemetry.perf_phase1_compute_rc_ctx_dual_rescue++;
            break;
        case LP_PHASE1_COMPUTE_CTX_OTHER:
        case LP_PHASE1_COMPUTE_CTX_NO_ENTERING_CLEANUP:
        default:
            solver->telemetry.perf_phase1_compute_rc_ctx_other++;
            break;
    }
}

void lp_telemetry_record_phase1_entering_exclusion(SimplexSolver *solver,
                                                   int repeated_slot) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_entering_exclusions++;
    if (repeated_slot) {
        solver->telemetry.perf_phase1_entering_exclusion_repeats++;
    }
}

void lp_telemetry_record_phase1_entering_exclusion_hit(SimplexSolver *solver,
                                                       int rerouted) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_entering_exclusion_hits++;
    if (rerouted) {
        solver->telemetry.perf_phase1_entering_exclusion_reroutes++;
    } else {
        solver->telemetry.perf_phase1_entering_exclusion_no_alt++;
    }
}

void lp_telemetry_record_phase1_dir_skip_entering(SimplexSolver *solver,
                                                  int same_entering,
                                                  int streak) {
    if (!solver_telemetry_enabled(solver)) return;
    if (same_entering) {
        solver->telemetry.perf_phase1_dir_skip_same_entering_repeats++;
    }
    if (streak > solver->telemetry.perf_phase1_dir_skip_same_entering_max_streak) {
        solver->telemetry.perf_phase1_dir_skip_same_entering_max_streak = streak;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_entering(
    SimplexSolver *solver,
    int same_entering,
    int streak) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_events++;
    if (same_entering) {
        solver->telemetry.perf_phase1_failed_stabilize_same_entering_repeats++;
    }
    if (streak > solver->telemetry.perf_phase1_failed_stabilize_same_entering_max_streak) {
        solver->telemetry.perf_phase1_failed_stabilize_same_entering_max_streak = streak;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_site(
    SimplexSolver *solver,
    int used_alternate) {
    if (!solver_telemetry_enabled(solver)) return;
    if (used_alternate) {
        solver->telemetry.perf_phase1_failed_stabilize_alternate_failures++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_primary_failures++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_arm(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_arms++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_alternate(
    SimplexSolver *solver,
    int same_alt,
    int streak) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_found++;
    if (same_alt) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats++;
    }
    if (streak >
        solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak =
            streak;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_no_alt(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_no_alt++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(
    SimplexSolver *solver,
    int stabilized) {
    if (!solver_telemetry_enabled(solver)) return;
    if (stabilized) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_failed++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_arm(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_arms++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_alternate(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_found++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_no_alt(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_no_alt++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_fallback_same_alt(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(
    SimplexSolver *solver,
    int stabilized) {
    if (!solver_telemetry_enabled(solver)) return;
    if (stabilized) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_failed++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_pool_sample(
    SimplexSolver *solver,
    int eligible_count,
    int best_differs_from_bland) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_pool_samples++;
    solver->telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_total +=
        eligible_count;
    if (eligible_count >
        solver->telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_max) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_max =
            eligible_count;
    }
    if (eligible_count == 1) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_pool_singleton_samples++;
    }
    if (best_differs_from_bland) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_pool_best_differs_samples++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_selector_eval(
    SimplexSolver *solver,
    int best_differs_from_bland,
    double bland_score,
    double best_score) {
    double score_ratio = 0.0;

    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_selector_eval_samples++;
    if (best_differs_from_bland) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples++;
    }
    if (!isfinite(bland_score) || !isfinite(best_score) ||
        bland_score <= 0.0 || best_score <= 0.0) {
        return;
    }
    score_ratio = best_score / bland_score;
    solver->telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total +=
        score_ratio;
    if (score_ratio >
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max =
            score_ratio;
    }
    if (score_ratio >= 2.0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2++;
    }
    if (score_ratio >= 4.0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_shadow(
    SimplexSolver *solver,
    int ratio_success,
    int dir_stable,
    double dir_inf,
    int dir_nnz,
    double pivot_abs) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_samples++;
    if (!ratio_success) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_ratio_failed++;
        return;
    }
    if (dir_stable) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_stable++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_failed++;
    }
    if (dir_nnz > 0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total += dir_nnz;
        if (dir_nnz >
            solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max) {
            solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max = dir_nnz;
        }
    }
    if (isfinite(dir_inf) && dir_inf >= 0.0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_total += dir_inf;
        if (dir_inf >
            solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max) {
            solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max = dir_inf;
        }
    }
    if (isfinite(pivot_abs) && pivot_abs >= 0.0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total += pivot_abs;
        if (pivot_abs >
            solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max) {
            solver->telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max =
                pivot_abs;
        }
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(
    SimplexSolver *solver,
    int used_guarded,
    int eligible_count) {
    if (!solver_telemetry_enabled(solver)) return;
    if (used_guarded) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_arms++;
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total +=
            eligible_count;
        if (eligible_count >
            solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max) {
            solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max =
                eligible_count;
        }
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_bland_arms++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(
    SimplexSolver *solver,
    int used_guarded,
    int stabilized) {
    if (!solver_telemetry_enabled(solver)) return;
    if (used_guarded) {
        if (stabilized) {
            solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized++;
        } else {
            solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed++;
        }
    } else {
        if (stabilized) {
            solver->telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized++;
        } else {
            solver->telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_failed++;
        }
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_selector_ratio_failure(
    SimplexSolver *solver,
    int used_guarded) {
    if (!solver_telemetry_enabled(solver)) return;
    if (used_guarded) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_selector_dir_failure(
    SimplexSolver *solver,
    int used_guarded) {
    if (!solver_telemetry_enabled(solver)) return;
    if (used_guarded) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_selector_bland_dir_failed++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_selector_guarded_fallback(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_dir_fail_shape(
    SimplexSolver *solver,
    double dir_inf,
    int dir_nnz,
    double pivot_abs) {
    double inf_ratio;
    double pivot_ratio = 0.0;

    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_shape_samples++;
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_total += dir_nnz;
    if (dir_nnz > solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max = dir_nnz;
    }
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total += dir_inf;
    if (dir_inf > solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max = dir_inf;
    }
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total += pivot_abs;
    if (pivot_abs >
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max = pivot_abs;
    }

    inf_ratio = dir_inf / RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER;
    if (inf_ratio <= 30.0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30++;
    } else if (inf_ratio <= 100.0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100++;
    } else if (inf_ratio <= 1000.0) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000++;
    }

    if (dir_inf > 0.0) {
        pivot_ratio = pivot_abs / dir_inf;
    }
    if (pivot_ratio <= 1e-8) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8++;
    } else if (pivot_ratio <= 1e-6) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6++;
    } else if (pivot_ratio <= 1e-4) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_arm(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_arms++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_no_alt(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_outcome(
    SimplexSolver *solver,
    int stabilized) {
    if (!solver_telemetry_enabled(solver)) return;
    if (stabilized) {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized++;
    } else {
        solver->telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_failed++;
    }
}

void lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_arm(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_guard_arms++;
}

void lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_original_exclusion(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions++;
}

void lp_telemetry_record_pricing(SimplexSolver *solver,
                                 int phase,
                                 double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_pricing_ms += elapsed_ms;
    if (phase == 1) {
        solver->telemetry.perf_phase1_pricing_ms += elapsed_ms;
        solver->telemetry.perf_phase1_pricing_calls++;
    } else if (phase == 2) {
        solver->telemetry.perf_phase2_pricing_ms += elapsed_ms;
        solver->telemetry.perf_phase2_pricing_calls++;
    }
}

void lp_telemetry_record_pricing_timed(SimplexSolver *solver,
                                       int phase,
                                       double start_ms) {
    lp_telemetry_record_pricing(solver,
                                phase,
                                lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_record_ratio(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_ratio_ms += elapsed_ms;
    if (phase == 1) {
        solver->telemetry.perf_phase1_ratio_ms += elapsed_ms;
        solver->telemetry.perf_phase1_ratio_calls++;
    } else if (phase == 2) {
        solver->telemetry.perf_phase2_ratio_ms += elapsed_ms;
        solver->telemetry.perf_phase2_ratio_calls++;
    }
}

void lp_telemetry_record_ratio_timed(SimplexSolver *solver,
                                     int phase,
                                     double start_ms) {
    lp_telemetry_record_ratio(solver,
                              phase,
                              lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_record_pivot(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_pivot_ms += elapsed_ms;
    if (phase == 1) {
        solver->telemetry.perf_phase1_pivot_ms += elapsed_ms;
        solver->telemetry.perf_phase1_pivot_calls++;
    } else if (phase == 2) {
        solver->telemetry.perf_phase2_pivot_ms += elapsed_ms;
        solver->telemetry.perf_phase2_pivot_calls++;
    }
}

void lp_telemetry_record_pivot_timed(SimplexSolver *solver,
                                     int phase,
                                     double start_ms) {
    lp_telemetry_record_pivot(solver,
                              phase,
                              lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_record_periodic_refactor_trigger(SimplexSolver *solver,
                                                   int phase,
                                                   int lu_health_triggered) {
    if (!solver_telemetry_enabled(solver)) return;
    if (lu_health_triggered) {
        solver->telemetry.perf_refactor_periodic_lu_health++;
        if (phase == 1) solver->telemetry.perf_phase1_refactor_periodic_lu_health++;
        else if (phase == 2) solver->telemetry.perf_phase2_refactor_periodic_lu_health++;
    } else {
        solver->telemetry.perf_refactor_periodic_policy++;
        if (phase == 1) solver->telemetry.perf_phase1_refactor_periodic_policy++;
        else if (phase == 2) solver->telemetry.perf_phase2_refactor_periodic_policy++;
    }
}

void lp_telemetry_record_phase1_dir_stabilize_force(SimplexSolver *solver,
                                                    int force_extreme_dir,
                                                    int force_lu_health) {
    if (!solver_telemetry_enabled(solver)) return;
    if (force_extreme_dir) {
        solver->telemetry.perf_phase1_dir_stabilize_force_extreme_dir++;
    }
    if (force_lu_health) {
        solver->telemetry.perf_phase1_dir_stabilize_force_lu_health++;
    }
}

void lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(
    SimplexSolver *solver,
    double dir_inf_ratio) {
    if (!solver_telemetry_enabled(solver)) return;

    solver->telemetry.perf_phase1_dir_stabilize_cooldown_candidates++;
    if (!(dir_inf_ratio > 0.0)) return;

    if (dir_inf_ratio <= 3.0) {
        solver->telemetry.perf_phase1_dir_stabilize_ratio_le_3++;
    } else if (dir_inf_ratio <= 10.0) {
        solver->telemetry.perf_phase1_dir_stabilize_ratio_le_10++;
    } else if (dir_inf_ratio <= 30.0) {
        solver->telemetry.perf_phase1_dir_stabilize_ratio_le_30++;
    } else if (dir_inf_ratio <= 100.0) {
        solver->telemetry.perf_phase1_dir_stabilize_ratio_le_100++;
    } else {
        solver->telemetry.perf_phase1_dir_stabilize_ratio_gt_100++;
        if (dir_inf_ratio > 300.0) {
            solver->telemetry.perf_phase1_dir_stabilize_ratio_gt_300++;
            if (dir_inf_ratio > 1000.0) {
                solver->telemetry.perf_phase1_dir_stabilize_ratio_gt_1000++;
            }
        }
    }
}

void lp_telemetry_record_phase1_dir_stabilize_skip(SimplexSolver *solver,
                                                   int used_full_recompute) {
    if (!solver_telemetry_enabled(solver)) return;
    if (used_full_recompute) {
        solver->telemetry.perf_phase1_dir_stabilize_skip_full++;
    } else {
        solver->telemetry.perf_phase1_dir_stabilize_skip_rc_only++;
    }
}

void lp_telemetry_record_phase1_dir_stabilize_skip_no_recompute(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_dir_stabilize_skip_no_recompute++;
}

void lp_telemetry_record_phase1_dir_stabilize_skip_guard_refresh(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_dir_stabilize_skip_guard_refresh++;
}

enum {
    LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_TRIGGER = 1,
    LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_SUPPRESS_LU_HEALTH = 2,
    LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_HARD_BYPASS = 3,
    LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_SUPPRESS_FORCE_PIVOT_MODE = 4
};

enum {
    LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_NO_PIVOT_FORCE = 1,
    LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_FORCE_EXTREME_DIR = 2,
    LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_FORCE_LU_HEALTH = 3,
    LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_FORCE_PIVOT_MODE = 4,
    LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_LADDER_FORCE = 5
};

void lp_telemetry_record_phase1_dir_stabilize_escape_gate(
    SimplexSolver *solver,
    int event) {
    if (!solver_telemetry_enabled(solver)) return;
    if (event == LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_TRIGGER) {
        solver->telemetry.perf_phase1_dir_stabilize_escape_gate_triggers++;
    } else if (event == LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_SUPPRESS_LU_HEALTH) {
        solver->telemetry.perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health++;
    } else if (event == LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_SUPPRESS_FORCE_PIVOT_MODE) {
        solver->telemetry.perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode++;
    } else if (event == LP_PHASE1_DIR_STABILIZE_ESCAPE_GATE_HARD_BYPASS) {
        solver->telemetry.perf_phase1_dir_stabilize_escape_gate_hard_bypass++;
    }
}

void lp_telemetry_record_phase1_dir_stabilize_refactor_trigger(
    SimplexSolver *solver,
    int trigger) {
    if (!solver_telemetry_enabled(solver)) return;
    switch (trigger) {
        case LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_NO_PIVOT_FORCE:
            solver->telemetry.perf_phase1_dir_stabilize_refactor_from_no_pivot_force++;
            break;
        case LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_FORCE_EXTREME_DIR:
            solver->telemetry.perf_phase1_dir_stabilize_refactor_from_force_extreme_dir++;
            break;
        case LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_FORCE_LU_HEALTH:
            solver->telemetry.perf_phase1_dir_stabilize_refactor_from_force_lu_health++;
            break;
        case LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_FORCE_PIVOT_MODE:
            solver->telemetry.perf_phase1_dir_stabilize_refactor_from_force_pivot_mode++;
            break;
        case LP_PHASE1_DIR_STABILIZE_REFACTOR_FROM_LADDER_FORCE:
            solver->telemetry.perf_phase1_dir_stabilize_refactor_from_ladder_force++;
            break;
        default:
            break;
    }
}

void lp_telemetry_record_phase1_force_pivot_relax(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_force_pivot_relax_applied++;
}

void lp_telemetry_record_phase1_force_extreme_relax(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_force_extreme_relax_applied++;
}

void lp_telemetry_record_phase1_recompute(SimplexSolver *solver,
                                          LPPhase1RecomputeReason reason) {
    if (!solver_telemetry_enabled(solver)) return;

    switch (reason) {
        case LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN:
            solver->telemetry.perf_phase1_recompute_after_ratio_breakdown++;
            break;
        case LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP:
            solver->telemetry.perf_phase1_recompute_after_dir_skip++;
            break;
        case LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR:
            solver->telemetry.perf_phase1_recompute_after_dir_refactor++;
            break;
        case LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY:
            solver->telemetry.perf_phase1_recompute_after_pivot_fail_recovery++;
            break;
        case LP_PHASE1_RECOMPUTE_REASON_PERTURB:
            solver->telemetry.perf_phase1_recompute_after_perturb++;
            break;
        default:
            break;
    }
}

void lp_telemetry_record_phase1_recompute_rc_only(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_recompute_rc_only_calls++;
}

void lp_telemetry_record_phase1_recompute_guard_forced_full(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_recompute_rc_guard_forced_full++;
}

void lp_telemetry_record_phase1_ratio_breakdown_retry(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_ratio_breakdown_retries++;
}

void lp_telemetry_record_phase1_ratio_breakdown_escalation(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_ratio_breakdown_escalations++;
}

void lp_telemetry_record_phase1_pivot_fail_recovery_exclusion(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_pivot_fail_recovery_exclusions++;
}

void lp_telemetry_record_phase1_no_pivot_event(SimplexSolver *solver,
                                               LPPhase1NoPivotForceReason reason) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_no_pivot_events++;
    switch (reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
            solver->telemetry.perf_phase1_no_pivot_events_ratio_breakdown++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            solver->telemetry.perf_phase1_no_pivot_events_dir_skip++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            solver->telemetry.perf_phase1_no_pivot_events_pivot_fail++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            break;
    }
}

void lp_telemetry_record_phase1_no_pivot_force(SimplexSolver *solver,
                                               LPPhase1NoPivotForceReason reason) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_no_pivot_forced_refactor++;
    switch (reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
            solver->telemetry.perf_phase1_no_pivot_forced_ratio_breakdown++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            solver->telemetry.perf_phase1_no_pivot_forced_dir_skip++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            solver->telemetry.perf_phase1_no_pivot_forced_pivot_fail++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            break;
    }
}

void lp_telemetry_record_phase1_no_pivot_no_progress(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_no_pivot_no_progress_events++;
}

void lp_telemetry_record_phase1_no_pivot_ladder_retry(SimplexSolver *solver,
                                                      LPPhase1NoPivotForceReason reason) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_no_pivot_ladder_retry_defers++;
    switch (reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
            solver->telemetry.perf_phase1_no_pivot_ladder_retry_ratio_breakdown++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            solver->telemetry.perf_phase1_no_pivot_ladder_retry_dir_skip++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            solver->telemetry.perf_phase1_no_pivot_ladder_retry_pivot_fail++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            break;
    }
}

void lp_telemetry_record_phase1_no_pivot_ladder_dual_rescue(SimplexSolver *solver,
                                                             LPPhase1NoPivotForceReason reason,
                                                             int success) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts++;
    switch (reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
            solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            break;
    }
    if (success) {
        solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_successes++;
    } else {
        solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_failures++;
    }
}

void lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
    SimplexSolver *solver,
    LPPhase1NoPivotForceReason reason) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors++;
    switch (reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
            solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors_dir_skip++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail++;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            break;
    }
}

void lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(
    SimplexSolver *solver,
    int forced_refactor) {
    if (!solver_telemetry_enabled(solver)) return;
    if (forced_refactor) {
        solver->telemetry.perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces++;
    } else {
        solver->telemetry.perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks++;
    }
}

void lp_telemetry_record_phase1_direct_dual_rescue(SimplexSolver *solver,
                                                   int success) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_direct_dual_rescue_attempts++;
    if (success) {
        solver->telemetry.perf_phase1_direct_dual_rescue_successes++;
    } else {
        solver->telemetry.perf_phase1_direct_dual_rescue_failures++;
    }
}

void lp_telemetry_record_phase1_direct_dual_rescue_guard(
    SimplexSolver *solver,
    int fail_cap_block) {
    if (!solver_telemetry_enabled(solver)) return;
    if (fail_cap_block) {
        solver->telemetry.perf_phase1_direct_dual_rescue_guard_fail_cap_blocks++;
    } else {
        solver->telemetry.perf_phase1_direct_dual_rescue_guard_cooldown_blocks++;
    }
}

void lp_telemetry_record_phase1_soft_lu_policy_cooldown_defer(
    SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_phase1_soft_lu_policy_cooldown_defers++;
}

void lp_telemetry_record_dual_ratio_no_entering(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_dual_ratio_no_entering++;
}

void lp_telemetry_record_dual_theta_nonpositive(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_dual_theta_nonpositive++;
}

void lp_telemetry_record_dual_pivot_reject_small(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_dual_pivot_reject_small++;
}

void lp_telemetry_record_dual_bound_flip_applied(SimplexSolver *solver,
                                                 int flips) {
    if (!solver_telemetry_enabled(solver)) return;
    if (flips <= 0) return;
    solver->telemetry.perf_dual_bound_flip_applied += flips;
}

void lp_telemetry_record_dual_bound_flip_applied_startup(SimplexSolver *solver,
                                                         int flips) {
    if (!solver_telemetry_enabled(solver)) return;
    if (flips <= 0) return;
    solver->telemetry.perf_dual_bound_flip_applied += flips;
    solver->telemetry.perf_dual_bound_flip_startup += flips;
}

void lp_telemetry_record_dual_bound_flip_applied_iterative(SimplexSolver *solver,
                                                           int flips) {
    if (!solver_telemetry_enabled(solver)) return;
    if (flips <= 0) return;
    solver->telemetry.perf_dual_bound_flip_applied += flips;
    solver->telemetry.perf_dual_bound_flip_iterative += flips;
}

void lp_telemetry_record_dual_lu_hard_trigger(SimplexSolver *solver) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_dual_lu_hard_trigger++;
}

void lp_telemetry_record_reinvert_shadow(SimplexSolver *solver,
                                         int phase,
                                         LPReinvertDecision suggested_decision,
                                         LPReinvertReason suggested_reason,
                                         int suggested_refactor,
                                         int actual_refactor) {
    int *checks = NULL;
    int *allow = NULL;
    int *defer = NULL;
    int *force = NULL;
    int *actual_yes = NULL;
    int *actual_no = NULL;
    int *disagree = NULL;
    int *last_reason = NULL;
    int suggested = suggested_refactor ? 1 : 0;
    int actual = actual_refactor ? 1 : 0;

    if (!solver_telemetry_enabled(solver)) return;

    if (phase == 1) {
        checks = &solver->telemetry.perf_reinvert_shadow_checks_phase1;
        allow = &solver->telemetry.perf_reinvert_shadow_suggest_allow_phase1;
        defer = &solver->telemetry.perf_reinvert_shadow_suggest_defer_phase1;
        force = &solver->telemetry.perf_reinvert_shadow_suggest_force_phase1;
        actual_yes = &solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_phase1;
        actual_no = &solver->telemetry.perf_reinvert_shadow_actual_refactor_no_phase1;
        disagree = &solver->telemetry.perf_reinvert_shadow_disagree_phase1;
        last_reason = &solver->telemetry.perf_reinvert_shadow_last_reason_phase1;
    } else if (phase == 2) {
        checks = &solver->telemetry.perf_reinvert_shadow_checks_phase2;
        allow = &solver->telemetry.perf_reinvert_shadow_suggest_allow_phase2;
        defer = &solver->telemetry.perf_reinvert_shadow_suggest_defer_phase2;
        force = &solver->telemetry.perf_reinvert_shadow_suggest_force_phase2;
        actual_yes = &solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_phase2;
        actual_no = &solver->telemetry.perf_reinvert_shadow_actual_refactor_no_phase2;
        disagree = &solver->telemetry.perf_reinvert_shadow_disagree_phase2;
        last_reason = &solver->telemetry.perf_reinvert_shadow_last_reason_phase2;
    } else {
        checks = &solver->telemetry.perf_reinvert_shadow_checks_dual;
        allow = &solver->telemetry.perf_reinvert_shadow_suggest_allow_dual;
        defer = &solver->telemetry.perf_reinvert_shadow_suggest_defer_dual;
        force = &solver->telemetry.perf_reinvert_shadow_suggest_force_dual;
        actual_yes = &solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_dual;
        actual_no = &solver->telemetry.perf_reinvert_shadow_actual_refactor_no_dual;
        disagree = &solver->telemetry.perf_reinvert_shadow_disagree_dual;
        last_reason = &solver->telemetry.perf_reinvert_shadow_last_reason_dual;
    }

    (*checks)++;
    if (actual) (*actual_yes)++;
    else (*actual_no)++;

    if (suggested_decision == LP_REINVERT_DECISION_FORCE) {
        (*force)++;
    } else if (suggested_decision == LP_REINVERT_DECISION_ALLOW) {
        (*allow)++;
    } else {
        (*defer)++;
    }

    *last_reason = (int)suggested_reason;
    if (suggested != actual) {
        (*disagree)++;
    }
}
