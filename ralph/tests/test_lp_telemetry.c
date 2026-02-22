/*
 * Tests for LP telemetry module.
 *
 * Verifies:
 * 1) solver/LU telemetry reset behavior
 * 2) refactor + basis event accounting
 * 3) LU per-factorization preparation
 * 4) snapshot export helpers
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lp.h"

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
    printf("  telemetry: solver reset + refactor accounting...\n");

    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));

    solver.perf_pricing_ms = 12.0;
    solver.perf_refactor_count = 3;
    solver.perf_refactor_next_reason = RALPH_REFACTOR_REASON_SETUP;
    solver.perf_basis_fastpath_hits = 7;
    solver.periodic_feedback_bias_phase2 = 0.2;

    lp_telemetry_reset_solver(&solver);

    ASSERT_DBL_EQ(solver.perf_pricing_ms, 0.0, "reset: perf_pricing_ms");
    ASSERT_INT_EQ(solver.perf_refactor_count, 0, "reset: refactor_count");
    ASSERT_INT_EQ(solver.perf_refactor_next_reason, RALPH_REFACTOR_REASON_OTHER,
                  "reset: next reason");
    ASSERT_INT_EQ(solver.perf_basis_fastpath_hits, 0, "reset: basis_fastpath_hits");
    ASSERT_DBL_EQ(solver.periodic_feedback_bias_phase2, 0.0,
                  "reset: periodic feedback phase2");

    lp_telemetry_record_basis_build(&solver, 1, 2, 128ULL);
    ASSERT_INT_EQ(solver.perf_basis_fastpath_hits, 1, "basis: fastpath hit");
    ASSERT_INT_EQ(solver.perf_basis_cols_rewritten, 2, "basis: cols rewritten");
    ASSERT_ULL_EQ(solver.perf_basis_tail_shift_bytes, 128ULL, "basis: tail shift bytes");

    lp_telemetry_set_refactor_next_reason(&solver, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
    {
        int reason = -1;
        lp_telemetry_begin_refactor(&solver, &reason);
        ASSERT_INT_EQ(reason, RALPH_REFACTOR_REASON_RATIO_RECOVERY,
                      "begin_refactor returns staged reason");
        ASSERT_INT_EQ(solver.perf_refactor_next_reason, RALPH_REFACTOR_REASON_OTHER,
                      "begin_refactor clears staged reason");

        lp_telemetry_record_refactor(&solver, 2, reason, 5.5, 100, 80, 1234);
    }

    ASSERT_INT_EQ(solver.perf_refactor_count, 1, "record: refactor count");
    ASSERT_DBL_EQ(solver.perf_refactor_all_ms, 5.5, "record: refactor all ms");
    ASSERT_DBL_EQ(solver.perf_refactor_last_ms, 5.5, "record: refactor last ms");
    ASSERT_DBL_EQ(solver.perf_refactor_max_ms, 5.5, "record: refactor max ms");
    ASSERT_INT_EQ(solver.perf_refactor_reason_ratio_recovery, 1,
                  "record: ratio recovery reason");
    ASSERT_INT_EQ(solver.perf_refactor_safety_forced, 1, "record: safety forced global");
    ASSERT_INT_EQ(solver.perf_phase2_refactor_calls, 1, "record: phase2 refactor calls");
    ASSERT_DBL_EQ(solver.perf_phase2_refactor_ms, 5.5, "record: phase2 refactor ms");
    ASSERT_INT_EQ(solver.perf_phase2_refactor_safety_forced, 1,
                  "record: phase2 safety forced");
    ASSERT_INT_EQ(solver.perf_refactor_last_m, 100, "record: last m");
    ASSERT_INT_EQ(solver.perf_refactor_last_k, 80, "record: last k");
    ASSERT_INT_EQ(solver.perf_refactor_last_nnz_B, 1234, "record: last nnz_B");

    lp_telemetry_record_refactor(&solver, 1, RALPH_REFACTOR_REASON_SETUP, 2.0, 40, 20, 300);
    ASSERT_INT_EQ(solver.perf_refactor_reason_setup, 1, "record: setup reason");
    ASSERT_INT_EQ(solver.perf_phase1_refactor_calls, 1, "record: phase1 refactor calls");
    ASSERT_DBL_EQ(solver.perf_phase1_refactor_ms, 2.0, "record: phase1 refactor ms");
    ASSERT_INT_EQ(solver.perf_phase1_refactor_safety_forced, 0,
                  "record: phase1 safety remains 0 for setup");
}

static void test_refactor_reason_classifier(void) {
    printf("  telemetry: refactor reason classifier...\n");

    ASSERT(lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_RATIO_RECOVERY),
           "classifier: ratio recovery is safety-forced");
    ASSERT(lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_PIVOT_RECOVERY),
           "classifier: pivot recovery is safety-forced");
    ASSERT(!lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_SETUP),
           "classifier: setup is not safety-forced");
    ASSERT(!lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_PERIODIC),
           "classifier: periodic is not safety-forced");
}

static void test_lu_reset_prepare_and_snapshot(void) {
    printf("  telemetry: LU reset/prepare/snapshot...\n");

    LUFactorization lu;
    memset(&lu, 0, sizeof(lu));
    lu.mkz_enabled = 1;
    lu.perf_factorize_calls = 9;
    lu.sparse_dense_fallbacks = 3;
    lu.perf_total_sparse_numeric_ms = 99.0;

    lp_telemetry_reset_lu(&lu);

    ASSERT_INT_EQ(lu.mkz_enabled, 1, "lu_reset: preserves mkz_enabled");
    ASSERT_INT_EQ(lu.perf_factorize_calls, 0, "lu_reset: factorize calls");
    ASSERT_INT_EQ(lu.sparse_dense_fallbacks, 0, "lu_reset: sparse dense fallbacks");
    ASSERT_DBL_EQ(lu.perf_total_sparse_numeric_ms, 0.0, "lu_reset: sparse numeric total");

    SparseMatrix B;
    memset(&B, 0, sizeof(B));
    B.nrows = 7;
    B.ncols = 7;
    B.nnz = 21;

    lu.perf_last_symbolic_ms = 12.0;
    lu.sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NUMERIC;
    lu.used_dense_fallback_last = 1;

    lp_telemetry_prepare_lu_factorize(&lu, &B);

    ASSERT_INT_EQ(lu.perf_factorize_calls, 1, "lu_prepare: factorize calls increment");
    ASSERT_INT_EQ(lu.perf_last_basis_nnz, 21, "lu_prepare: last basis nnz");
    ASSERT_INT_EQ(lu.perf_last_m, 7, "lu_prepare: last m");
    ASSERT_INT_EQ(lu.sparse_fallback_last_reason, LU_SPARSE_FALLBACK_NONE,
                  "lu_prepare: fallback reason reset");
    ASSERT_INT_EQ(lu.used_dense_fallback_last, 0, "lu_prepare: dense fallback flag reset");
    ASSERT_DBL_EQ(lu.perf_last_symbolic_ms, 0.0, "lu_prepare: last symbolic ms reset");

    {
        LUTelemetrySnapshot snap;
        lp_telemetry_snapshot_lu(&lu, &snap);
        ASSERT_INT_EQ(snap.mkz_enabled, 1, "lu_snapshot: mkz_enabled");
        ASSERT_INT_EQ(snap.perf_factorize_calls, 1, "lu_snapshot: factorize calls");
        ASSERT_INT_EQ(snap.perf_last_basis_nnz, 21, "lu_snapshot: last basis nnz");
    }
}

static void test_solver_snapshot(void) {
    printf("  telemetry: solver snapshot...\n");

    SimplexSolver solver;
    LPSolverTelemetrySnapshot snap;
    memset(&solver, 0, sizeof(solver));

    solver.perf_phase2_ms = 42.25;
    solver.perf_refactor_reason_periodic = 11;
    solver.perf_basis_tail_shift_bytes = 4096ULL;
    solver.perf_phase1_pricing_calls = 17;
    solver.periodic_feedback_hint_pressure_phase2 = 0.55;

    lp_telemetry_snapshot_solver(&solver, &snap);

    ASSERT_DBL_EQ(snap.perf_phase2_ms, 42.25, "solver_snapshot: phase2_ms");
    ASSERT_INT_EQ(snap.perf_refactor_reason_periodic, 11,
                  "solver_snapshot: refactor_reason_periodic");
    ASSERT_ULL_EQ(snap.perf_basis_tail_shift_bytes, 4096ULL,
                  "solver_snapshot: basis_tail_shift_bytes");
    ASSERT_INT_EQ(snap.perf_phase1_pricing_calls, 17,
                  "solver_snapshot: phase1_pricing_calls");
    ASSERT_DBL_EQ(snap.periodic_feedback_hint_pressure_phase2, 0.55,
                  "solver_snapshot: feedback pressure phase2");
}

int main(void) {
    printf("=== LP Telemetry Tests ===\n");

    test_solver_reset_and_refactor_accounting();
    test_refactor_reason_classifier();
    test_lu_reset_prepare_and_snapshot();
    test_solver_snapshot();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

