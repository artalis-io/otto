/*
 * Ralph - LP Solver Telemetry Helpers
 *
 * Solver-level reset, refactor bookkeeping, and snapshot export.
 */

#include <string.h>
#include "lp.h"

static int solver_telemetry_enabled(const SimplexSolver *solver) {
    return solver && solver->telemetry_enabled;
}

void lp_telemetry_reset_solver(SimplexSolver *solver) {
    if (!solver) return;
    solver->telemetry.perf_primal_setup_ms = 0.0;
    solver->telemetry.perf_dual_ms = 0.0;
    solver->telemetry.perf_phase1_ms = 0.0;
    solver->telemetry.perf_transition_ms = 0.0;
    solver->telemetry.perf_phase2_ms = 0.0;
    solver->telemetry.perf_pricing_ms = 0.0;
    solver->telemetry.perf_ratio_ms = 0.0;
    solver->telemetry.perf_pivot_ms = 0.0;
    solver->telemetry.perf_refactor_ms = 0.0;
    solver->telemetry.perf_ftran_ms = 0.0;
    solver->telemetry.perf_btran_ms = 0.0;
    solver->telemetry.perf_lu_update_ms = 0.0;
    solver->telemetry.perf_compute_solution_ms = 0.0;
    solver->telemetry.perf_compute_rc_ms = 0.0;
    solver->telemetry.perf_refactor_all_ms = 0.0;
    solver->telemetry.perf_refactor_count = 0;
    solver->telemetry.perf_refactor_last_ms = 0.0;
    solver->telemetry.perf_refactor_max_ms = 0.0;
    solver->telemetry.perf_refactor_last_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->policy.refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->telemetry.perf_refactor_reason_setup = 0;
    solver->telemetry.perf_refactor_reason_transition = 0;
    solver->telemetry.perf_refactor_reason_periodic = 0;
    solver->telemetry.perf_refactor_reason_ratio_recovery = 0;
    solver->telemetry.perf_refactor_reason_pivot_recovery = 0;
    solver->telemetry.perf_refactor_reason_forced_small_pivot = 0;
    solver->telemetry.perf_refactor_reason_update_recovery = 0;
    solver->telemetry.perf_refactor_reason_direction_stabilize = 0;
    solver->telemetry.perf_refactor_reason_infeas_cleanup = 0;
    solver->telemetry.perf_refactor_reason_other = 0;
    solver->telemetry.perf_refactor_periodic_policy = 0;
    solver->telemetry.perf_refactor_periodic_lu_health = 0;
    solver->telemetry.perf_refactor_safety_forced = 0;
    solver->telemetry.perf_basis_fastpath_hits = 0;
    solver->telemetry.perf_basis_cols_rewritten = 0;
    solver->telemetry.perf_basis_tail_shift_bytes = 0ULL;
    solver->telemetry.perf_refactor_last_m = 0;
    solver->telemetry.perf_refactor_last_k = 0;
    solver->telemetry.perf_refactor_last_nnz_B = 0;

    solver->telemetry.perf_phase1_pricing_ms = 0.0;
    solver->telemetry.perf_phase1_ratio_ms = 0.0;
    solver->telemetry.perf_phase1_pivot_ms = 0.0;
    solver->telemetry.perf_phase1_refactor_ms = 0.0;
    solver->telemetry.perf_phase1_compute_solution_ms = 0.0;
    solver->telemetry.perf_phase1_compute_rc_ms = 0.0;
    solver->telemetry.perf_phase1_pricing_calls = 0;
    solver->telemetry.perf_phase1_ratio_calls = 0;
    solver->telemetry.perf_phase1_pivot_calls = 0;
    solver->telemetry.perf_phase1_refactor_calls = 0;
    solver->telemetry.perf_phase1_compute_solution_calls = 0;
    solver->telemetry.perf_phase1_compute_rc_calls = 0;
    solver->telemetry.perf_phase1_refactor_periodic_policy = 0;
    solver->telemetry.perf_phase1_refactor_periodic_lu_health = 0;
    solver->telemetry.perf_phase1_refactor_safety_forced = 0;

    solver->telemetry.perf_phase2_pricing_ms = 0.0;
    solver->telemetry.perf_phase2_ratio_ms = 0.0;
    solver->telemetry.perf_phase2_pivot_ms = 0.0;
    solver->telemetry.perf_phase2_refactor_ms = 0.0;
    solver->telemetry.perf_phase2_compute_solution_ms = 0.0;
    solver->telemetry.perf_phase2_compute_rc_ms = 0.0;
    solver->telemetry.perf_phase2_pricing_calls = 0;
    solver->telemetry.perf_phase2_ratio_calls = 0;
    solver->telemetry.perf_phase2_pivot_calls = 0;
    solver->telemetry.perf_phase2_refactor_calls = 0;
    solver->telemetry.perf_phase2_compute_solution_calls = 0;
    solver->telemetry.perf_phase2_compute_rc_calls = 0;
    solver->telemetry.perf_phase2_refactor_periodic_policy = 0;
    solver->telemetry.perf_phase2_refactor_periodic_lu_health = 0;
    solver->telemetry.perf_phase2_refactor_safety_forced = 0;

    solver->policy.periodic_feedback_bias_phase1 = 0.0;
    solver->policy.periodic_feedback_bias_phase2 = 0.0;
    solver->policy.periodic_feedback_last_reason_phase1 = RALPH_REFACTOR_REASON_OTHER;
    solver->policy.periodic_feedback_last_reason_phase2 = RALPH_REFACTOR_REASON_OTHER;
    solver->policy.periodic_feedback_last_interval_phase1 = 0;
    solver->policy.periodic_feedback_last_interval_phase2 = 0;
    solver->policy.periodic_feedback_hint_interval_phase1 = 0;
    solver->policy.periodic_feedback_hint_interval_phase2 = 0;
    solver->policy.periodic_feedback_hint_pressure_phase1 = 0.0;
    solver->policy.periodic_feedback_hint_pressure_phase2 = 0.0;
    solver->policy.soft_lu_cost_gate_enabled = 1;
    solver->policy.soft_lu_cost_gate_defers_phase1 = 0;
    solver->policy.soft_lu_cost_gate_defers_phase2 = 0;
    solver->policy.soft_lu_consecutive_defers_phase1 = 0;
    solver->policy.soft_lu_consecutive_defers_phase2 = 0;
    solver->policy.soft_lu_defer_cap_forced_phase1 = 0;
    solver->policy.soft_lu_defer_cap_forced_phase2 = 0;
    solver->policy.soft_lu_refactor_cost_ewma_phase1 = 0.0;
    solver->policy.soft_lu_refactor_cost_ewma_phase2 = 0.0;
    solver->policy.soft_lu_iter_cost_ewma_phase1 = 0.0;
    solver->policy.soft_lu_iter_cost_ewma_phase2 = 0.0;
}

void lp_telemetry_record_basis_build(SimplexSolver *owner,
                                     int fastpath_hit,
                                     int cols_rewritten,
                                     unsigned long long tail_shift_bytes) {
    if (!solver_telemetry_enabled(owner)) return;
    if (fastpath_hit) owner->telemetry.perf_basis_fastpath_hits++;
    if (cols_rewritten > 0) owner->telemetry.perf_basis_cols_rewritten += cols_rewritten;
    owner->telemetry.perf_basis_tail_shift_bytes += tail_shift_bytes;
}

void lp_telemetry_begin_refactor(SimplexSolver *owner, int *reason_out) {
    if (!reason_out) return;
    *reason_out = RALPH_REFACTOR_REASON_OTHER;
    if (!owner) return;
    *reason_out = owner->policy.refactor_next_reason;
    owner->policy.refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
}

void lp_telemetry_set_refactor_next_reason(SimplexSolver *owner, int reason) {
    if (!owner) return;
    owner->policy.refactor_next_reason = reason;
}

void lp_telemetry_record_refactor(SimplexSolver *owner,
                                  int phase,
                                  int reason,
                                  double elapsed_ms,
                                  int m,
                                  int lu_last_k,
                                  int lu_last_basis_nnz) {
    if (!solver_telemetry_enabled(owner)) return;

    owner->telemetry.perf_refactor_all_ms += elapsed_ms;
    owner->telemetry.perf_refactor_count++;
    owner->telemetry.perf_refactor_last_ms = elapsed_ms;
    if (elapsed_ms > owner->telemetry.perf_refactor_max_ms) {
        owner->telemetry.perf_refactor_max_ms = elapsed_ms;
    }
    owner->telemetry.perf_refactor_last_reason = reason;

    switch ((RalphRefactorReason)reason) {
        case RALPH_REFACTOR_REASON_SETUP:
            owner->telemetry.perf_refactor_reason_setup++;
            break;
        case RALPH_REFACTOR_REASON_PHASE_TRANSITION:
            owner->telemetry.perf_refactor_reason_transition++;
            break;
        case RALPH_REFACTOR_REASON_PERIODIC:
            owner->telemetry.perf_refactor_reason_periodic++;
            break;
        case RALPH_REFACTOR_REASON_RATIO_RECOVERY:
            owner->telemetry.perf_refactor_reason_ratio_recovery++;
            break;
        case RALPH_REFACTOR_REASON_PIVOT_RECOVERY:
            owner->telemetry.perf_refactor_reason_pivot_recovery++;
            break;
        case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT:
            owner->telemetry.perf_refactor_reason_forced_small_pivot++;
            break;
        case RALPH_REFACTOR_REASON_UPDATE_RECOVERY:
            owner->telemetry.perf_refactor_reason_update_recovery++;
            break;
        case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE:
            owner->telemetry.perf_refactor_reason_direction_stabilize++;
            break;
        case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP:
            owner->telemetry.perf_refactor_reason_infeas_cleanup++;
            break;
        case RALPH_REFACTOR_REASON_OTHER:
        default:
            owner->telemetry.perf_refactor_reason_other++;
            break;
    }

    if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
        owner->telemetry.perf_refactor_safety_forced++;
    }

    owner->telemetry.perf_refactor_last_m = m;
    owner->telemetry.perf_refactor_last_k = lu_last_k;
    owner->telemetry.perf_refactor_last_nnz_B = lu_last_basis_nnz;

    if (phase == 1) {
        owner->telemetry.perf_phase1_refactor_ms += elapsed_ms;
        owner->telemetry.perf_phase1_refactor_calls++;
        if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
            owner->telemetry.perf_phase1_refactor_safety_forced++;
        }
    } else if (phase == 2) {
        owner->telemetry.perf_phase2_refactor_ms += elapsed_ms;
        owner->telemetry.perf_phase2_refactor_calls++;
        if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
            owner->telemetry.perf_phase2_refactor_safety_forced++;
        }
    }
}

void lp_telemetry_record_refactor_with_lu(SimplexSolver *owner,
                                          int phase,
                                          int reason,
                                          double elapsed_ms,
                                          int m,
                                          const LUFactorization *lu) {
    int lu_last_k = lu ? lu->telemetry.perf_last_k : 0;
    int lu_last_basis_nnz = lu ? lu->telemetry.perf_last_basis_nnz : 0;
    lp_telemetry_record_refactor(owner,
                                 phase,
                                 reason,
                                 elapsed_ms,
                                 m,
                                 lu_last_k,
                                 lu_last_basis_nnz);
}

void lp_telemetry_record_refactor_with_lu_timed(SimplexSolver *owner,
                                                int phase,
                                                int reason,
                                                double start_ms,
                                                int m,
                                                const LUFactorization *lu) {
    lp_telemetry_record_refactor_with_lu(owner,
                                         phase,
                                         reason,
                                         lp_telemetry_timer_elapsed_ms(start_ms),
                                         m,
                                         lu);
}

#define COPY_SOLVER_FIELD(field) out->field = solver->telemetry.field
void lp_telemetry_snapshot_solver(const SimplexSolver *solver,
                                  LPSolverTelemetrySnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!solver) return;

    COPY_SOLVER_FIELD(perf_primal_setup_ms);
    COPY_SOLVER_FIELD(perf_dual_ms);
    COPY_SOLVER_FIELD(perf_phase1_ms);
    COPY_SOLVER_FIELD(perf_transition_ms);
    COPY_SOLVER_FIELD(perf_phase2_ms);
    COPY_SOLVER_FIELD(perf_pricing_ms);
    COPY_SOLVER_FIELD(perf_ratio_ms);
    COPY_SOLVER_FIELD(perf_pivot_ms);
    COPY_SOLVER_FIELD(perf_refactor_ms);
    COPY_SOLVER_FIELD(perf_ftran_ms);
    COPY_SOLVER_FIELD(perf_btran_ms);
    COPY_SOLVER_FIELD(perf_lu_update_ms);
    COPY_SOLVER_FIELD(perf_compute_solution_ms);
    COPY_SOLVER_FIELD(perf_compute_rc_ms);
    COPY_SOLVER_FIELD(perf_refactor_all_ms);
    COPY_SOLVER_FIELD(perf_refactor_count);
    COPY_SOLVER_FIELD(perf_refactor_last_ms);
    COPY_SOLVER_FIELD(perf_refactor_max_ms);
    COPY_SOLVER_FIELD(perf_refactor_last_reason);
    out->perf_refactor_next_reason = solver->policy.refactor_next_reason;
    COPY_SOLVER_FIELD(perf_refactor_reason_setup);
    COPY_SOLVER_FIELD(perf_refactor_reason_transition);
    COPY_SOLVER_FIELD(perf_refactor_reason_periodic);
    COPY_SOLVER_FIELD(perf_refactor_reason_ratio_recovery);
    COPY_SOLVER_FIELD(perf_refactor_reason_pivot_recovery);
    COPY_SOLVER_FIELD(perf_refactor_reason_forced_small_pivot);
    COPY_SOLVER_FIELD(perf_refactor_reason_update_recovery);
    COPY_SOLVER_FIELD(perf_refactor_reason_direction_stabilize);
    COPY_SOLVER_FIELD(perf_refactor_reason_infeas_cleanup);
    COPY_SOLVER_FIELD(perf_refactor_reason_other);
    COPY_SOLVER_FIELD(perf_refactor_periodic_policy);
    COPY_SOLVER_FIELD(perf_refactor_periodic_lu_health);
    COPY_SOLVER_FIELD(perf_refactor_safety_forced);
    COPY_SOLVER_FIELD(perf_basis_fastpath_hits);
    COPY_SOLVER_FIELD(perf_basis_cols_rewritten);
    COPY_SOLVER_FIELD(perf_basis_tail_shift_bytes);
    COPY_SOLVER_FIELD(perf_refactor_last_m);
    COPY_SOLVER_FIELD(perf_refactor_last_k);
    COPY_SOLVER_FIELD(perf_refactor_last_nnz_B);

    COPY_SOLVER_FIELD(perf_phase1_pricing_ms);
    COPY_SOLVER_FIELD(perf_phase1_ratio_ms);
    COPY_SOLVER_FIELD(perf_phase1_pivot_ms);
    COPY_SOLVER_FIELD(perf_phase1_refactor_ms);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ms);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ms);
    COPY_SOLVER_FIELD(perf_phase1_pricing_calls);
    COPY_SOLVER_FIELD(perf_phase1_ratio_calls);
    COPY_SOLVER_FIELD(perf_phase1_pivot_calls);
    COPY_SOLVER_FIELD(perf_phase1_refactor_calls);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_calls);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_calls);
    COPY_SOLVER_FIELD(perf_phase1_refactor_periodic_policy);
    COPY_SOLVER_FIELD(perf_phase1_refactor_periodic_lu_health);
    COPY_SOLVER_FIELD(perf_phase1_refactor_safety_forced);

    COPY_SOLVER_FIELD(perf_phase2_pricing_ms);
    COPY_SOLVER_FIELD(perf_phase2_ratio_ms);
    COPY_SOLVER_FIELD(perf_phase2_pivot_ms);
    COPY_SOLVER_FIELD(perf_phase2_refactor_ms);
    COPY_SOLVER_FIELD(perf_phase2_compute_solution_ms);
    COPY_SOLVER_FIELD(perf_phase2_compute_rc_ms);
    COPY_SOLVER_FIELD(perf_phase2_pricing_calls);
    COPY_SOLVER_FIELD(perf_phase2_ratio_calls);
    COPY_SOLVER_FIELD(perf_phase2_pivot_calls);
    COPY_SOLVER_FIELD(perf_phase2_refactor_calls);
    COPY_SOLVER_FIELD(perf_phase2_compute_solution_calls);
    COPY_SOLVER_FIELD(perf_phase2_compute_rc_calls);
    COPY_SOLVER_FIELD(perf_phase2_refactor_periodic_policy);
    COPY_SOLVER_FIELD(perf_phase2_refactor_periodic_lu_health);
    COPY_SOLVER_FIELD(perf_phase2_refactor_safety_forced);

    out->periodic_feedback_bias_phase1 = solver->policy.periodic_feedback_bias_phase1;
    out->periodic_feedback_bias_phase2 = solver->policy.periodic_feedback_bias_phase2;
    out->periodic_feedback_last_reason_phase1 = solver->policy.periodic_feedback_last_reason_phase1;
    out->periodic_feedback_last_reason_phase2 = solver->policy.periodic_feedback_last_reason_phase2;
    out->periodic_feedback_last_interval_phase1 = solver->policy.periodic_feedback_last_interval_phase1;
    out->periodic_feedback_last_interval_phase2 = solver->policy.periodic_feedback_last_interval_phase2;
    out->periodic_feedback_hint_interval_phase1 = solver->policy.periodic_feedback_hint_interval_phase1;
    out->periodic_feedback_hint_interval_phase2 = solver->policy.periodic_feedback_hint_interval_phase2;
    out->periodic_feedback_hint_pressure_phase1 = solver->policy.periodic_feedback_hint_pressure_phase1;
    out->periodic_feedback_hint_pressure_phase2 = solver->policy.periodic_feedback_hint_pressure_phase2;
    out->soft_lu_cost_gate_enabled = solver->policy.soft_lu_cost_gate_enabled;
    out->soft_lu_cost_gate_defers_phase1 = solver->policy.soft_lu_cost_gate_defers_phase1;
    out->soft_lu_cost_gate_defers_phase2 = solver->policy.soft_lu_cost_gate_defers_phase2;
    out->soft_lu_consecutive_defers_phase1 = solver->policy.soft_lu_consecutive_defers_phase1;
    out->soft_lu_consecutive_defers_phase2 = solver->policy.soft_lu_consecutive_defers_phase2;
    out->soft_lu_defer_cap_forced_phase1 = solver->policy.soft_lu_defer_cap_forced_phase1;
    out->soft_lu_defer_cap_forced_phase2 = solver->policy.soft_lu_defer_cap_forced_phase2;
    out->soft_lu_refactor_cost_ewma_phase1 = solver->policy.soft_lu_refactor_cost_ewma_phase1;
    out->soft_lu_refactor_cost_ewma_phase2 = solver->policy.soft_lu_refactor_cost_ewma_phase2;
    out->soft_lu_iter_cost_ewma_phase1 = solver->policy.soft_lu_iter_cost_ewma_phase1;
    out->soft_lu_iter_cost_ewma_phase2 = solver->policy.soft_lu_iter_cost_ewma_phase2;
}
#undef COPY_SOLVER_FIELD
