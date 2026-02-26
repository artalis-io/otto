/*
 * Tests for solver-side LP telemetry helpers.
 *
 * Verifies:
 * 1) solver reset behavior
 * 2) refactor + basis event accounting
 * 3) refactor reason classifier
 * 4) solver snapshot export
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "lp_refactor_policy.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

#define ASSERT_ULL_EQ(a, b, msg) do { \
    tests_run++; \
    if ((unsigned long long)(a) == (unsigned long long)(b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%llu != %llu)\n", msg, \
               (unsigned long long)(a), (unsigned long long)(b)); \
    } \
} while (0)

#define ASSERT_DBL_EQ(a, b, msg) do { \
    tests_run++; \
    if (fabs((double)(a) - (double)(b)) <= 1e-12) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12f != %.12f)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

static void test_solver_reset_and_refactor_accounting(void) {
    printf("  telemetry/solver: reset + refactor accounting...\n");

    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));
    solver.telemetry_enabled = 1;

    solver.telemetry.perf_pricing_ms = 12.0;
    solver.telemetry.perf_refactor_count = 3;
    solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir = 5;
    solver.telemetry.perf_phase1_dir_stabilize_force_lu_health = 4;
    solver.policy.refactor_next_reason = RALPH_REFACTOR_REASON_SETUP;
    solver.telemetry.perf_basis_fastpath_hits = 7;
    solver.policy.periodic_feedback_bias_phase2 = 0.2;
    solver.policy.soft_lu_cost_gate_enabled = 0;
    solver.policy.soft_lu_cost_gate_defers_phase2 = 5;
    solver.policy.soft_lu_consecutive_defers_phase2 = 3;
    solver.policy.soft_lu_defer_cap_forced_phase2 = 4;
    solver.policy.soft_lu_refactor_cost_ewma_phase2 = 9.5;
    solver.policy.periodic_cost_gate_enabled = 0;
    solver.policy.periodic_cost_gate_defers_phase2 = 6;
    solver.policy.periodic_cost_consecutive_defers_phase2 = 2;
    solver.policy.periodic_cost_defer_cap_forced_phase2 = 3;
    solver.policy.periodic_cost_gate_checks_phase2 = 9;
    solver.policy.periodic_cost_gate_block_ratio_phase2 = 4;
    solver.policy.periodic_cost_gate_block_warmup_phase2 = 2;
    solver.policy.periodic_cost_gate_last_reason_phase2 = LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO;
    solver.policy.periodic_cost_iter_samples_phase2 = 12;
    solver.policy.periodic_cost_refactor_samples_phase2 = 3;
    solver.policy.basis_governor_mode = LP_BASIS_GOV_MODE_CONTROL_PHASE2;
    lp_basis_governor_set_mode(&solver.policy.basis_governor,
                               solver.policy.basis_governor_mode);
    solver.policy.basis_governor.shadow_refactor_yes_phase1 = 4;
    solver.policy.basis_governor.shadow_disagree_lu_backend = 2;

    lp_telemetry_reset_solver(&solver);

    ASSERT_DBL_EQ(solver.telemetry.perf_pricing_ms, 0.0, "reset: perf_pricing_ms");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_count, 0, "reset: refactor_count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir, 0,
                  "reset: phase1 dir force extreme");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_lu_health, 0,
                  "reset: phase1 dir force lu health");
    ASSERT_INT_EQ(solver.policy.refactor_next_reason, RALPH_REFACTOR_REASON_OTHER,
                  "reset: next reason");
    ASSERT_INT_EQ(solver.telemetry.perf_basis_fastpath_hits, 0, "reset: basis_fastpath_hits");
    ASSERT_DBL_EQ(solver.policy.periodic_feedback_bias_phase2, 0.0,
                  "reset: periodic feedback phase2");
    ASSERT_INT_EQ(solver.policy.soft_lu_cost_gate_enabled, 1,
                  "reset: soft lu cost gate enabled");
    ASSERT_INT_EQ(solver.policy.soft_lu_cost_gate_defers_phase2, 0,
                  "reset: soft lu defers phase2");
    ASSERT_INT_EQ(solver.policy.soft_lu_consecutive_defers_phase2, 0,
                  "reset: soft lu consecutive defers phase2");
    ASSERT_INT_EQ(solver.policy.soft_lu_defer_cap_forced_phase2, 0,
                  "reset: soft lu cap forced phase2");
    ASSERT_DBL_EQ(solver.policy.soft_lu_refactor_cost_ewma_phase2, 0.0,
                  "reset: soft lu refactor ewma phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_enabled, 1,
                  "reset: periodic cost gate enabled");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_defers_phase2, 0,
                  "reset: periodic cost gate defers phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_consecutive_defers_phase2, 0,
                  "reset: periodic cost consecutive defers phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_defer_cap_forced_phase2, 0,
                  "reset: periodic cost cap forced phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_checks_phase2, 0,
                  "reset: periodic cost checks phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_block_ratio_phase2, 0,
                  "reset: periodic cost ratio block phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_block_warmup_phase2, 0,
                  "reset: periodic cost warmup block phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_last_reason_phase2,
                  LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE,
                  "reset: periodic cost last reason phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_iter_samples_phase2, 0,
                  "reset: periodic cost iter samples phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_refactor_samples_phase2, 0,
                  "reset: periodic cost refactor samples phase2");
    ASSERT_INT_EQ(solver.policy.basis_governor_mode, LP_BASIS_GOV_MODE_CONTROL_PHASE2,
                  "reset: basis governor mode preserved");
    ASSERT_INT_EQ(lp_basis_governor_get_mode(&solver.policy.basis_governor),
                  LP_BASIS_GOV_MODE_CONTROL_PHASE2,
                  "reset: governor state mode preserved");
    ASSERT_INT_EQ(solver.policy.basis_governor.shadow_refactor_yes_phase1, 0,
                  "reset: basis governor yes phase1");
    ASSERT_INT_EQ(solver.policy.basis_governor.shadow_disagree_lu_backend, 0,
                  "reset: basis governor lu disagreement");

    lp_telemetry_record_basis_build(&solver, 1, 2, 128ULL);
    ASSERT_INT_EQ(solver.telemetry.perf_basis_fastpath_hits, 1, "basis: fastpath hit");
    ASSERT_INT_EQ(solver.telemetry.perf_basis_cols_rewritten, 2, "basis: cols rewritten");
    ASSERT_ULL_EQ(solver.telemetry.perf_basis_tail_shift_bytes, 128ULL, "basis: tail shift bytes");

    lp_telemetry_set_refactor_next_reason(&solver, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
    {
        int reason = -1;
        lp_telemetry_begin_refactor(&solver, &reason);
        ASSERT_INT_EQ(reason, RALPH_REFACTOR_REASON_RATIO_RECOVERY,
                      "begin_refactor returns staged reason");
        ASSERT_INT_EQ(solver.policy.refactor_next_reason, RALPH_REFACTOR_REASON_OTHER,
                      "begin_refactor clears staged reason");

        lp_telemetry_record_refactor(&solver, 2, reason, 5.5, 100, 80, 1234);
    }

    ASSERT_INT_EQ(solver.telemetry.perf_refactor_count, 1, "record: refactor count");
    ASSERT_DBL_EQ(solver.telemetry.perf_refactor_all_ms, 5.5, "record: refactor all ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_refactor_last_ms, 5.5, "record: refactor last ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_refactor_max_ms, 5.5, "record: refactor max ms");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_reason_ratio_recovery, 1,
                  "record: ratio recovery reason");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_safety_forced, 1, "record: safety forced global");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_refactor_calls, 1, "record: phase2 refactor calls");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase2_refactor_ms, 5.5, "record: phase2 refactor ms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_refactor_safety_forced, 1,
                  "record: phase2 safety forced");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_last_m, 100, "record: last m");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_last_k, 80, "record: last k");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_last_nnz_B, 1234, "record: last nnz_B");

    lp_telemetry_record_refactor(&solver, 1, RALPH_REFACTOR_REASON_SETUP, 2.0, 40, 20, 300);
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_reason_setup, 1, "record: setup reason");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_refactor_calls, 1, "record: phase1 refactor calls");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_refactor_ms, 2.0, "record: phase1 refactor ms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_refactor_safety_forced, 0,
                  "record: phase1 safety remains 0 for setup");

    lp_telemetry_record_phase1_dir_stabilize_force(&solver, 1, 0);
    lp_telemetry_record_phase1_dir_stabilize_force(&solver, 0, 1);
    lp_telemetry_record_phase1_dir_stabilize_force(&solver, 1, 1);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir, 2,
                  "record: phase1 dir force extreme count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_lu_health, 2,
                  "record: phase1 dir force lu health count");
}

static void test_refactor_reason_classifier(void) {
    printf("  telemetry/solver: refactor reason classifier...\n");

    ASSERT(lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_RATIO_RECOVERY),
           "classifier: ratio recovery is safety-forced");
    ASSERT(lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_PIVOT_RECOVERY),
           "classifier: pivot recovery is safety-forced");
    ASSERT(!lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_SETUP),
           "classifier: setup is not safety-forced");
    ASSERT(!lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_PERIODIC),
           "classifier: periodic is not safety-forced");
}

static void test_solver_snapshot(void) {
    printf("  telemetry/solver: snapshot...\n");

    SimplexSolver solver;
    LPSolverTelemetrySnapshot snap;
    memset(&solver, 0, sizeof(solver));

    solver.telemetry.perf_phase2_ms = 42.25;
    solver.telemetry.perf_refactor_reason_periodic = 11;
    solver.telemetry.perf_basis_tail_shift_bytes = 4096ULL;
    solver.telemetry.perf_phase1_pricing_calls = 17;
    solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir = 6;
    solver.telemetry.perf_phase1_dir_stabilize_force_lu_health = 9;
    solver.policy.periodic_feedback_hint_pressure_phase2 = 0.55;
    solver.policy.soft_lu_cost_gate_enabled = 1;
    solver.policy.soft_lu_cost_gate_defers_phase1 = 3;
    solver.policy.soft_lu_consecutive_defers_phase1 = 2;
    solver.policy.soft_lu_defer_cap_forced_phase1 = 1;
    solver.policy.soft_lu_refactor_cost_ewma_phase2 = 7.25;
    solver.policy.periodic_cost_gate_enabled = 1;
    solver.policy.periodic_cost_gate_defers_phase1 = 4;
    solver.policy.periodic_cost_consecutive_defers_phase1 = 1;
    solver.policy.periodic_cost_defer_cap_forced_phase1 = 2;
    solver.policy.periodic_cost_gate_checks_phase1 = 7;
    solver.policy.periodic_cost_gate_block_ratio_phase1 = 3;
    solver.policy.periodic_cost_gate_block_warmup_phase1 = 1;
    solver.policy.periodic_cost_gate_last_reason_phase1 = LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO;
    solver.policy.periodic_cost_iter_samples_phase1 = 19;
    solver.policy.periodic_cost_refactor_samples_phase1 = 4;
    solver.policy.basis_governor_mode = LP_BASIS_GOV_MODE_SHADOW;
    lp_basis_governor_set_mode(&solver.policy.basis_governor,
                               solver.policy.basis_governor_mode);
    solver.policy.basis_governor.shadow_refactor_yes_phase1 = 8;
    solver.policy.basis_governor.shadow_disagree_primal_refactor = 3;

    lp_telemetry_snapshot_solver(&solver, &snap);

    ASSERT_DBL_EQ(snap.perf_phase2_ms, 42.25, "solver_snapshot: phase2_ms");
    ASSERT_INT_EQ(snap.perf_refactor_reason_periodic, 11,
                  "solver_snapshot: refactor_reason_periodic");
    ASSERT_ULL_EQ(snap.perf_basis_tail_shift_bytes, 4096ULL,
                  "solver_snapshot: basis_tail_shift_bytes");
    ASSERT_INT_EQ(snap.perf_phase1_pricing_calls, 17,
                  "solver_snapshot: phase1_pricing_calls");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_force_extreme_dir, 6,
                  "solver_snapshot: phase1 dir force extreme");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_force_lu_health, 9,
                  "solver_snapshot: phase1 dir force lu health");
    ASSERT_DBL_EQ(snap.periodic_feedback_hint_pressure_phase2, 0.55,
                  "solver_snapshot: feedback pressure phase2");
    ASSERT_INT_EQ(snap.soft_lu_cost_gate_enabled, 1,
                  "solver_snapshot: soft lu gate enabled");
    ASSERT_INT_EQ(snap.soft_lu_cost_gate_defers_phase1, 3,
                  "solver_snapshot: soft lu defers phase1");
    ASSERT_INT_EQ(snap.soft_lu_consecutive_defers_phase1, 2,
                  "solver_snapshot: soft lu consecutive defers phase1");
    ASSERT_INT_EQ(snap.soft_lu_defer_cap_forced_phase1, 1,
                  "solver_snapshot: soft lu cap forced phase1");
    ASSERT_DBL_EQ(snap.soft_lu_refactor_cost_ewma_phase2, 7.25,
                  "solver_snapshot: soft lu refactor ewma phase2");
    ASSERT_INT_EQ(snap.periodic_cost_gate_enabled, 1,
                  "solver_snapshot: periodic cost gate enabled");
    ASSERT_INT_EQ(snap.periodic_cost_gate_defers_phase1, 4,
                  "solver_snapshot: periodic cost defers phase1");
    ASSERT_INT_EQ(snap.periodic_cost_consecutive_defers_phase1, 1,
                  "solver_snapshot: periodic cost consecutive defers phase1");
    ASSERT_INT_EQ(snap.periodic_cost_defer_cap_forced_phase1, 2,
                  "solver_snapshot: periodic cost cap forced phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_checks_phase1, 7,
                  "solver_snapshot: periodic cost checks phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_block_ratio_phase1, 3,
                  "solver_snapshot: periodic cost ratio block phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_block_warmup_phase1, 1,
                  "solver_snapshot: periodic cost warmup block phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_last_reason_phase1,
                  LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO,
                  "solver_snapshot: periodic cost last reason phase1");
    ASSERT_INT_EQ(snap.periodic_cost_iter_samples_phase1, 19,
                  "solver_snapshot: periodic cost iter samples phase1");
    ASSERT_INT_EQ(snap.periodic_cost_refactor_samples_phase1, 4,
                  "solver_snapshot: periodic cost refactor samples phase1");
    ASSERT_INT_EQ(snap.basis_governor_mode, LP_BASIS_GOV_MODE_SHADOW,
                  "solver_snapshot: basis governor mode");
    ASSERT_INT_EQ(snap.shadow_refactor_yes_phase1, 8,
                  "solver_snapshot: shadow refactor yes phase1");
    ASSERT_INT_EQ(snap.shadow_disagree_primal_refactor, 3,
                  "solver_snapshot: shadow disagree primal refactor");
}

int main(void) {
    printf("=== LP Telemetry Solver Tests ===\n");

    test_solver_reset_and_refactor_accounting();
    test_refactor_reason_classifier();
    test_solver_snapshot();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
