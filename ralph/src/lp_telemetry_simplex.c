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
