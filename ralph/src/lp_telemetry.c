/*
 * Ralph - LP/LU Telemetry Helpers
 *
 * Centralizes telemetry timing/counter resets, refactor bookkeeping, and
 * benchmark-facing snapshots.
 */

#include <string.h>
#include "lp.h"

double lp_telemetry_now_ms(void) {
    return sh_perf_now_ms();
}

int lp_telemetry_refactor_reason_is_safety_forced(int reason) {
    switch ((RalphRefactorReason)reason) {
        case RALPH_REFACTOR_REASON_RATIO_RECOVERY:
        case RALPH_REFACTOR_REASON_PIVOT_RECOVERY:
        case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT:
        case RALPH_REFACTOR_REASON_UPDATE_RECOVERY:
        case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE:
        case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP:
            return 1;
        case RALPH_REFACTOR_REASON_OTHER:
        case RALPH_REFACTOR_REASON_SETUP:
        case RALPH_REFACTOR_REASON_PHASE_TRANSITION:
        case RALPH_REFACTOR_REASON_PERIODIC:
        default:
            return 0;
    }
}

void lp_telemetry_reset_solver(SimplexSolver *solver) {
    if (!solver) return;
    solver->perf_primal_setup_ms = 0.0;
    solver->perf_dual_ms = 0.0;
    solver->perf_phase1_ms = 0.0;
    solver->perf_transition_ms = 0.0;
    solver->perf_phase2_ms = 0.0;
    solver->perf_pricing_ms = 0.0;
    solver->perf_ratio_ms = 0.0;
    solver->perf_pivot_ms = 0.0;
    solver->perf_refactor_ms = 0.0;
    solver->perf_ftran_ms = 0.0;
    solver->perf_btran_ms = 0.0;
    solver->perf_lu_update_ms = 0.0;
    solver->perf_compute_solution_ms = 0.0;
    solver->perf_compute_rc_ms = 0.0;
    solver->perf_refactor_all_ms = 0.0;
    solver->perf_refactor_count = 0;
    solver->perf_refactor_last_ms = 0.0;
    solver->perf_refactor_max_ms = 0.0;
    solver->perf_refactor_last_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->perf_refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->perf_refactor_reason_setup = 0;
    solver->perf_refactor_reason_transition = 0;
    solver->perf_refactor_reason_periodic = 0;
    solver->perf_refactor_reason_ratio_recovery = 0;
    solver->perf_refactor_reason_pivot_recovery = 0;
    solver->perf_refactor_reason_forced_small_pivot = 0;
    solver->perf_refactor_reason_update_recovery = 0;
    solver->perf_refactor_reason_direction_stabilize = 0;
    solver->perf_refactor_reason_infeas_cleanup = 0;
    solver->perf_refactor_reason_other = 0;
    solver->perf_refactor_periodic_policy = 0;
    solver->perf_refactor_periodic_lu_health = 0;
    solver->perf_refactor_safety_forced = 0;
    solver->perf_basis_fastpath_hits = 0;
    solver->perf_basis_cols_rewritten = 0;
    solver->perf_basis_tail_shift_bytes = 0ULL;
    solver->perf_refactor_last_m = 0;
    solver->perf_refactor_last_k = 0;
    solver->perf_refactor_last_nnz_B = 0;

    solver->perf_phase1_pricing_ms = 0.0;
    solver->perf_phase1_ratio_ms = 0.0;
    solver->perf_phase1_pivot_ms = 0.0;
    solver->perf_phase1_refactor_ms = 0.0;
    solver->perf_phase1_compute_solution_ms = 0.0;
    solver->perf_phase1_compute_rc_ms = 0.0;
    solver->perf_phase1_pricing_calls = 0;
    solver->perf_phase1_ratio_calls = 0;
    solver->perf_phase1_pivot_calls = 0;
    solver->perf_phase1_refactor_calls = 0;
    solver->perf_phase1_compute_solution_calls = 0;
    solver->perf_phase1_compute_rc_calls = 0;
    solver->perf_phase1_refactor_periodic_policy = 0;
    solver->perf_phase1_refactor_periodic_lu_health = 0;
    solver->perf_phase1_refactor_safety_forced = 0;

    solver->perf_phase2_pricing_ms = 0.0;
    solver->perf_phase2_ratio_ms = 0.0;
    solver->perf_phase2_pivot_ms = 0.0;
    solver->perf_phase2_refactor_ms = 0.0;
    solver->perf_phase2_compute_solution_ms = 0.0;
    solver->perf_phase2_compute_rc_ms = 0.0;
    solver->perf_phase2_pricing_calls = 0;
    solver->perf_phase2_ratio_calls = 0;
    solver->perf_phase2_pivot_calls = 0;
    solver->perf_phase2_refactor_calls = 0;
    solver->perf_phase2_compute_solution_calls = 0;
    solver->perf_phase2_compute_rc_calls = 0;
    solver->perf_phase2_refactor_periodic_policy = 0;
    solver->perf_phase2_refactor_periodic_lu_health = 0;
    solver->perf_phase2_refactor_safety_forced = 0;

    solver->periodic_feedback_bias_phase1 = 0.0;
    solver->periodic_feedback_bias_phase2 = 0.0;
    solver->periodic_feedback_last_reason_phase1 = RALPH_REFACTOR_REASON_OTHER;
    solver->periodic_feedback_last_reason_phase2 = RALPH_REFACTOR_REASON_OTHER;
    solver->periodic_feedback_last_interval_phase1 = 0;
    solver->periodic_feedback_last_interval_phase2 = 0;
    solver->periodic_feedback_hint_interval_phase1 = 0;
    solver->periodic_feedback_hint_interval_phase2 = 0;
    solver->periodic_feedback_hint_pressure_phase1 = 0.0;
    solver->periodic_feedback_hint_pressure_phase2 = 0.0;
}

void lp_telemetry_reset_lu(LUFactorization *lu) {
    if (!lu) return;
    lu->mkz_calls = 0;
    lu->mkz_successes = 0;
    lu->mkz_failures = 0;
    lu->mkz_last_failure = 0;
    lu->mkz_dense_fallbacks = 0;
    lu->mkz_fail_workspace = 0;
    lu->mkz_fail_pool = 0;
    lu->mkz_fail_singular = 0;
    lu->mkz_fail_capacity = 0;
    lu->sparse_dense_fallbacks = 0;
    lu->used_dense_fallback_last = 0;
    lu->sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NONE;
    lu->sparse_fallback_reason_small_matrix = 0;
    lu->sparse_fallback_reason_symbolic = 0;
    lu->sparse_fallback_reason_numeric = 0;
    lu->identity_sep_failures = 0;
    lu->perf_factorize_calls = 0;
    lu->perf_last_basis_nnz = 0;
    lu->perf_last_m = 0;
    lu->perf_last_k = 0;
    lu->perf_symbolic_calls = 0;
    lu->perf_symbolic_cache_hits = 0;
    lu->perf_symbolic_cache_misses = 0;
    lu->perf_last_symbolic_ms = 0.0;
    lu->perf_last_sparse_numeric_ms = 0.0;
    lu->perf_last_dense_ge_numeric_ms = 0.0;
    lu->perf_last_supernode_numeric_ms = 0.0;
    lu->perf_last_dense_factorize_ms = 0.0;
    lu->perf_last_a_struct_build_ms = 0.0;
    lu->perf_last_markowitz_numeric_ms = 0.0;
    lu->perf_last_identity_placement_ms = 0.0;
    lu->perf_last_coo_to_csc_ms = 0.0;
    lu->perf_total_symbolic_ms = 0.0;
    lu->perf_total_sparse_numeric_ms = 0.0;
    lu->perf_total_dense_ge_numeric_ms = 0.0;
    lu->perf_total_supernode_numeric_ms = 0.0;
    lu->perf_total_dense_factorize_ms = 0.0;
    lu->perf_total_a_struct_build_ms = 0.0;
    lu->perf_total_markowitz_numeric_ms = 0.0;
    lu->perf_total_identity_placement_ms = 0.0;
    lu->perf_total_coo_to_csc_ms = 0.0;
}

void lp_telemetry_prepare_lu_factorize(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu) return;
    lu->used_dense_fallback_last = 0;
    lu->sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NONE;
    lu->perf_factorize_calls++;
    lu->perf_last_basis_nnz = B ? B->nnz : 0;
    lu->perf_last_m = B ? B->nrows : 0;
    lu->perf_last_k = 0;
    lu->perf_last_symbolic_ms = 0.0;
    lu->perf_last_sparse_numeric_ms = 0.0;
    lu->perf_last_dense_ge_numeric_ms = 0.0;
    lu->perf_last_supernode_numeric_ms = 0.0;
    lu->perf_last_dense_factorize_ms = 0.0;
    lu->perf_last_a_struct_build_ms = 0.0;
    lu->perf_last_markowitz_numeric_ms = 0.0;
    lu->perf_last_identity_placement_ms = 0.0;
    lu->perf_last_coo_to_csc_ms = 0.0;
}

void lp_telemetry_record_basis_build(SimplexSolver *owner,
                                     int fastpath_hit,
                                     int cols_rewritten,
                                     unsigned long long tail_shift_bytes) {
    if (!owner) return;
    if (fastpath_hit) owner->perf_basis_fastpath_hits++;
    if (cols_rewritten > 0) owner->perf_basis_cols_rewritten += cols_rewritten;
    owner->perf_basis_tail_shift_bytes += tail_shift_bytes;
}

void lp_telemetry_begin_refactor(SimplexSolver *owner, int *reason_out) {
    if (!reason_out) return;
    *reason_out = RALPH_REFACTOR_REASON_OTHER;
    if (!owner) return;
    *reason_out = owner->perf_refactor_next_reason;
    owner->perf_refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
}

void lp_telemetry_set_refactor_next_reason(SimplexSolver *owner, int reason) {
    if (!owner) return;
    owner->perf_refactor_next_reason = reason;
}

void lp_telemetry_record_refactor(SimplexSolver *owner,
                                  int phase,
                                  int reason,
                                  double elapsed_ms,
                                  int m,
                                  int lu_last_k,
                                  int lu_last_basis_nnz) {
    if (!owner) return;

    owner->perf_refactor_all_ms += elapsed_ms;
    owner->perf_refactor_count++;
    owner->perf_refactor_last_ms = elapsed_ms;
    if (elapsed_ms > owner->perf_refactor_max_ms) {
        owner->perf_refactor_max_ms = elapsed_ms;
    }
    owner->perf_refactor_last_reason = reason;

    switch ((RalphRefactorReason)reason) {
        case RALPH_REFACTOR_REASON_SETUP:
            owner->perf_refactor_reason_setup++;
            break;
        case RALPH_REFACTOR_REASON_PHASE_TRANSITION:
            owner->perf_refactor_reason_transition++;
            break;
        case RALPH_REFACTOR_REASON_PERIODIC:
            owner->perf_refactor_reason_periodic++;
            break;
        case RALPH_REFACTOR_REASON_RATIO_RECOVERY:
            owner->perf_refactor_reason_ratio_recovery++;
            break;
        case RALPH_REFACTOR_REASON_PIVOT_RECOVERY:
            owner->perf_refactor_reason_pivot_recovery++;
            break;
        case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT:
            owner->perf_refactor_reason_forced_small_pivot++;
            break;
        case RALPH_REFACTOR_REASON_UPDATE_RECOVERY:
            owner->perf_refactor_reason_update_recovery++;
            break;
        case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE:
            owner->perf_refactor_reason_direction_stabilize++;
            break;
        case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP:
            owner->perf_refactor_reason_infeas_cleanup++;
            break;
        case RALPH_REFACTOR_REASON_OTHER:
        default:
            owner->perf_refactor_reason_other++;
            break;
    }

    if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
        owner->perf_refactor_safety_forced++;
    }

    owner->perf_refactor_last_m = m;
    owner->perf_refactor_last_k = lu_last_k;
    owner->perf_refactor_last_nnz_B = lu_last_basis_nnz;

    if (phase == 1) {
        owner->perf_phase1_refactor_ms += elapsed_ms;
        owner->perf_phase1_refactor_calls++;
        if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
            owner->perf_phase1_refactor_safety_forced++;
        }
    } else if (phase == 2) {
        owner->perf_phase2_refactor_ms += elapsed_ms;
        owner->perf_phase2_refactor_calls++;
        if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
            owner->perf_phase2_refactor_safety_forced++;
        }
    }
}

#define COPY_SOLVER_FIELD(field) out->field = solver->field
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
    COPY_SOLVER_FIELD(perf_refactor_next_reason);
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

    COPY_SOLVER_FIELD(periodic_feedback_bias_phase1);
    COPY_SOLVER_FIELD(periodic_feedback_bias_phase2);
    COPY_SOLVER_FIELD(periodic_feedback_last_reason_phase1);
    COPY_SOLVER_FIELD(periodic_feedback_last_reason_phase2);
    COPY_SOLVER_FIELD(periodic_feedback_last_interval_phase1);
    COPY_SOLVER_FIELD(periodic_feedback_last_interval_phase2);
    COPY_SOLVER_FIELD(periodic_feedback_hint_interval_phase1);
    COPY_SOLVER_FIELD(periodic_feedback_hint_interval_phase2);
    COPY_SOLVER_FIELD(periodic_feedback_hint_pressure_phase1);
    COPY_SOLVER_FIELD(periodic_feedback_hint_pressure_phase2);
}
#undef COPY_SOLVER_FIELD

#define COPY_LU_FIELD(field) out->field = lu->field
void lp_telemetry_snapshot_lu(const LUFactorization *lu,
                              LUTelemetrySnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lu) return;

    COPY_LU_FIELD(mkz_enabled);
    COPY_LU_FIELD(sn_enabled);
    COPY_LU_FIELD(mkz_calls);
    COPY_LU_FIELD(mkz_successes);
    COPY_LU_FIELD(mkz_failures);
    COPY_LU_FIELD(mkz_last_failure);
    COPY_LU_FIELD(mkz_dense_fallbacks);
    COPY_LU_FIELD(mkz_fail_workspace);
    COPY_LU_FIELD(mkz_fail_pool);
    COPY_LU_FIELD(mkz_fail_singular);
    COPY_LU_FIELD(mkz_fail_capacity);

    COPY_LU_FIELD(sparse_dense_fallbacks);
    COPY_LU_FIELD(used_dense_fallback_last);
    COPY_LU_FIELD(sparse_fallback_last_reason);
    COPY_LU_FIELD(sparse_fallback_reason_small_matrix);
    COPY_LU_FIELD(sparse_fallback_reason_symbolic);
    COPY_LU_FIELD(sparse_fallback_reason_numeric);
    COPY_LU_FIELD(identity_sep_failures);

    COPY_LU_FIELD(sn_calls);
    COPY_LU_FIELD(sn_successes);
    COPY_LU_FIELD(num_updates);
    COPY_LU_FIELD(max_updates);
    COPY_LU_FIELD(last_failure_reason);

    COPY_LU_FIELD(perf_factorize_calls);
    COPY_LU_FIELD(perf_last_basis_nnz);
    COPY_LU_FIELD(perf_last_m);
    COPY_LU_FIELD(perf_last_k);
    COPY_LU_FIELD(perf_symbolic_calls);
    COPY_LU_FIELD(perf_symbolic_cache_hits);
    COPY_LU_FIELD(perf_symbolic_cache_misses);
    COPY_LU_FIELD(perf_last_symbolic_ms);
    COPY_LU_FIELD(perf_last_sparse_numeric_ms);
    COPY_LU_FIELD(perf_last_dense_ge_numeric_ms);
    COPY_LU_FIELD(perf_last_supernode_numeric_ms);
    COPY_LU_FIELD(perf_last_dense_factorize_ms);
    COPY_LU_FIELD(perf_last_a_struct_build_ms);
    COPY_LU_FIELD(perf_last_markowitz_numeric_ms);
    COPY_LU_FIELD(perf_last_identity_placement_ms);
    COPY_LU_FIELD(perf_last_coo_to_csc_ms);
    COPY_LU_FIELD(perf_total_symbolic_ms);
    COPY_LU_FIELD(perf_total_sparse_numeric_ms);
    COPY_LU_FIELD(perf_total_dense_ge_numeric_ms);
    COPY_LU_FIELD(perf_total_supernode_numeric_ms);
    COPY_LU_FIELD(perf_total_dense_factorize_ms);
    COPY_LU_FIELD(perf_total_a_struct_build_ms);
    COPY_LU_FIELD(perf_total_markowitz_numeric_ms);
    COPY_LU_FIELD(perf_total_identity_placement_ms);
    COPY_LU_FIELD(perf_total_coo_to_csc_ms);
}
#undef COPY_LU_FIELD

void lp_telemetry_add_solver_stage_ms(SimplexSolver *solver,
                                      LPSolverStage stage,
                                      double elapsed_ms) {
    if (!solver) return;
    switch (stage) {
        case LP_SOLVER_STAGE_PRIMAL_SETUP:
            solver->perf_primal_setup_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_DUAL:
            solver->perf_dual_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_PHASE1:
            solver->perf_phase1_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_TRANSITION:
            solver->perf_transition_ms += elapsed_ms;
            break;
        case LP_SOLVER_STAGE_PHASE2:
            solver->perf_phase2_ms += elapsed_ms;
            break;
        default:
            break;
    }
}

void lp_telemetry_add_refactor_runtime_ms(SimplexSolver *solver,
                                          double elapsed_ms) {
    if (!solver) return;
    solver->perf_refactor_ms += elapsed_ms;
}

void lp_telemetry_add_ftran_ms(SimplexSolver *solver,
                               double elapsed_ms) {
    if (!solver) return;
    solver->perf_ftran_ms += elapsed_ms;
}

void lp_telemetry_add_btran_ms(SimplexSolver *solver,
                               double elapsed_ms) {
    if (!solver) return;
    solver->perf_btran_ms += elapsed_ms;
}

void lp_telemetry_add_lu_update_ms(SimplexSolver *solver,
                                   double elapsed_ms) {
    if (!solver) return;
    solver->perf_lu_update_ms += elapsed_ms;
}

void lp_telemetry_record_compute_solution(SimplexSolver *solver,
                                          int phase,
                                          double elapsed_ms) {
    if (!solver) return;
    solver->perf_compute_solution_ms += elapsed_ms;
    if (phase == 1) {
        solver->perf_phase1_compute_solution_ms += elapsed_ms;
        solver->perf_phase1_compute_solution_calls++;
    } else if (phase == 2) {
        solver->perf_phase2_compute_solution_ms += elapsed_ms;
        solver->perf_phase2_compute_solution_calls++;
    }
}

void lp_telemetry_record_compute_reduced_costs(SimplexSolver *solver,
                                               int phase,
                                               double elapsed_ms) {
    if (!solver) return;
    solver->perf_compute_rc_ms += elapsed_ms;
    if (phase == 1) {
        solver->perf_phase1_compute_rc_ms += elapsed_ms;
        solver->perf_phase1_compute_rc_calls++;
    } else if (phase == 2) {
        solver->perf_phase2_compute_rc_ms += elapsed_ms;
        solver->perf_phase2_compute_rc_calls++;
    }
}

void lp_telemetry_record_pricing(SimplexSolver *solver,
                                 int phase,
                                 double elapsed_ms) {
    if (!solver) return;
    solver->perf_pricing_ms += elapsed_ms;
    if (phase == 1) {
        solver->perf_phase1_pricing_ms += elapsed_ms;
        solver->perf_phase1_pricing_calls++;
    } else if (phase == 2) {
        solver->perf_phase2_pricing_ms += elapsed_ms;
        solver->perf_phase2_pricing_calls++;
    }
}

void lp_telemetry_record_ratio(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms) {
    if (!solver) return;
    solver->perf_ratio_ms += elapsed_ms;
    if (phase == 1) {
        solver->perf_phase1_ratio_ms += elapsed_ms;
        solver->perf_phase1_ratio_calls++;
    } else if (phase == 2) {
        solver->perf_phase2_ratio_ms += elapsed_ms;
        solver->perf_phase2_ratio_calls++;
    }
}

void lp_telemetry_record_pivot(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms) {
    if (!solver) return;
    solver->perf_pivot_ms += elapsed_ms;
    if (phase == 1) {
        solver->perf_phase1_pivot_ms += elapsed_ms;
        solver->perf_phase1_pivot_calls++;
    } else if (phase == 2) {
        solver->perf_phase2_pivot_ms += elapsed_ms;
        solver->perf_phase2_pivot_calls++;
    }
}

void lp_telemetry_record_periodic_refactor_trigger(SimplexSolver *solver,
                                                   int phase,
                                                   int lu_health_triggered) {
    if (!solver) return;
    if (lu_health_triggered) {
        solver->perf_refactor_periodic_lu_health++;
        if (phase == 1) solver->perf_phase1_refactor_periodic_lu_health++;
        else if (phase == 2) solver->perf_phase2_refactor_periodic_lu_health++;
    } else {
        solver->perf_refactor_periodic_policy++;
        if (phase == 1) solver->perf_phase1_refactor_periodic_policy++;
        else if (phase == 2) solver->perf_phase2_refactor_periodic_policy++;
    }
}

void lp_telemetry_lu_record_dense_factorize_ms(LUFactorization *lu,
                                               double elapsed_ms) {
    if (!lu) return;
    lu->perf_last_dense_factorize_ms = elapsed_ms;
    lu->perf_total_dense_factorize_ms += elapsed_ms;
}

void lp_telemetry_lu_record_symbolic_cache_hit(LUFactorization *lu) {
    if (!lu) return;
    lu->perf_symbolic_cache_hits++;
}

void lp_telemetry_lu_record_symbolic_cache_miss(LUFactorization *lu) {
    if (!lu) return;
    lu->perf_symbolic_cache_misses++;
}

void lp_telemetry_lu_record_symbolic_call(LUFactorization *lu,
                                          double elapsed_ms) {
    if (!lu) return;
    lu->perf_symbolic_calls++;
    lu->perf_last_symbolic_ms = elapsed_ms;
    lu->perf_total_symbolic_ms += elapsed_ms;
}

void lp_telemetry_lu_mark_identity_sep_failure(LUFactorization *lu) {
    if (!lu) return;
    lu->identity_sep_failures++;
}

void lp_telemetry_lu_record_numeric_stages(LUFactorization *lu,
                                           int last_k,
                                           double a_struct_build_ms,
                                           double markowitz_numeric_ms,
                                           double supernode_numeric_ms,
                                           double dense_ge_numeric_ms,
                                           double identity_placement_ms,
                                           double coo_to_csc_ms) {
    if (!lu) return;
    lu->perf_last_k = last_k;
    lu->perf_last_a_struct_build_ms = a_struct_build_ms;
    lu->perf_last_markowitz_numeric_ms = markowitz_numeric_ms;
    lu->perf_last_supernode_numeric_ms = supernode_numeric_ms;
    lu->perf_last_dense_ge_numeric_ms = dense_ge_numeric_ms;
    lu->perf_last_sparse_numeric_ms =
        markowitz_numeric_ms + supernode_numeric_ms + dense_ge_numeric_ms;
    lu->perf_last_identity_placement_ms = identity_placement_ms;
    lu->perf_last_coo_to_csc_ms = coo_to_csc_ms;
    lu->perf_total_a_struct_build_ms += a_struct_build_ms;
    lu->perf_total_markowitz_numeric_ms += markowitz_numeric_ms;
    lu->perf_total_supernode_numeric_ms += supernode_numeric_ms;
    lu->perf_total_dense_ge_numeric_ms += dense_ge_numeric_ms;
    lu->perf_total_sparse_numeric_ms += lu->perf_last_sparse_numeric_ms;
    lu->perf_total_identity_placement_ms += identity_placement_ms;
    lu->perf_total_coo_to_csc_ms += coo_to_csc_ms;
}

void lp_telemetry_lu_set_sparse_fallback_reason(LUFactorization *lu,
                                                int reason) {
    if (!lu) return;
    lu->sparse_fallback_last_reason = reason;
    switch ((LUSparseFallbackReason)reason) {
        case LU_SPARSE_FALLBACK_SMALL_MATRIX:
            lu->sparse_fallback_reason_small_matrix++;
            break;
        case LU_SPARSE_FALLBACK_SYMBOLIC:
            lu->sparse_fallback_reason_symbolic++;
            break;
        case LU_SPARSE_FALLBACK_NUMERIC:
            lu->sparse_fallback_reason_numeric++;
            break;
        case LU_SPARSE_FALLBACK_NONE:
        default:
            break;
    }
}

void lp_telemetry_lu_mark_sparse_success(LUFactorization *lu) {
    lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_NONE);
}

void lp_telemetry_lu_mark_dense_fallback(LUFactorization *lu) {
    if (!lu) return;
    lu->used_dense_fallback_last = 1;
    lu->sparse_dense_fallbacks++;
}

void lp_telemetry_lu_clear_mkz_last_failure(LUFactorization *lu) {
    if (!lu) return;
    lu->mkz_last_failure = MKZ_FAIL_NONE;
}

void lp_telemetry_lu_mark_mkz_attempt(LUFactorization *lu) {
    if (!lu) return;
    lu->mkz_calls++;
}

void lp_telemetry_lu_mark_mkz_success(LUFactorization *lu) {
    if (!lu) return;
    lu->mkz_successes++;
    lu->mkz_last_failure = MKZ_FAIL_NONE;
}

void lp_telemetry_lu_mark_mkz_failure(LUFactorization *lu,
                                      int rc) {
    if (!lu) return;
    lu->mkz_failures++;
    lu->mkz_last_failure = rc;
    lu->mkz_dense_fallbacks++;
}

void lp_telemetry_lu_mark_mkz_failure_reason(LUFactorization *lu,
                                             int rc) {
    if (!lu) return;
    if (rc == MKZ_FAIL_WORKSPACE) {
        lu->mkz_fail_workspace++;
    } else if (rc == MKZ_FAIL_POOL) {
        lu->mkz_fail_pool++;
    } else if (rc == MKZ_FAIL_SINGULAR) {
        lu->mkz_fail_singular++;
    } else if (rc == MKZ_FAIL_CAPACITY) {
        lu->mkz_fail_capacity++;
    }
}
