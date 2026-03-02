/*
 * Ralph - Simplex Iteration Telemetry Helpers
 *
 * Per-stage and per-iteration counters for primal/dual simplex execution.
 */

#include "lp.h"

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
}

void lp_telemetry_add_ftran_timed(SimplexSolver *solver,
                                  double start_ms) {
    lp_telemetry_add_ftran_ms(solver,
                              lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_add_btran_ms(SimplexSolver *solver,
                               double elapsed_ms) {
    if (!solver_telemetry_enabled(solver)) return;
    solver->telemetry.perf_btran_ms += elapsed_ms;
}

void lp_telemetry_add_btran_timed(SimplexSolver *solver,
                                  double start_ms) {
    lp_telemetry_add_btran_ms(solver,
                              lp_telemetry_timer_elapsed_ms(start_ms));
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
