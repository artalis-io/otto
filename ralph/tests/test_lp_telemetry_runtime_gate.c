/*
 * Tests for runtime telemetry enable/disable gate behavior.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lp.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
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

static void test_runtime_telemetry_gate(void) {
    printf("  telemetry/gate: runtime enable/disable...\n");

    {
        SimplexSolver solver;
        memset(&solver, 0, sizeof(solver));
        solver.telemetry_enabled = 0;
        solver.perf_refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
        solver.perf_pricing_ms = 3.0;
        solver.perf_ratio_ms = 2.0;
        solver.perf_refactor_count = 5;

        lp_telemetry_add_solver_stage_ms(&solver, LP_SOLVER_STAGE_PHASE2, 11.0);
        lp_telemetry_record_pricing(&solver, 2, 1.5);
        lp_telemetry_record_ratio(&solver, 2, 2.5);
        lp_telemetry_add_refactor_runtime_ms(&solver, 4.0);
        lp_telemetry_record_refactor(&solver, 2, RALPH_REFACTOR_REASON_PERIODIC, 9.0, 100, 80, 400);

        ASSERT_DBL_EQ(solver.perf_pricing_ms, 3.0, "gate off: pricing unchanged");
        ASSERT_DBL_EQ(solver.perf_ratio_ms, 2.0, "gate off: ratio unchanged");
        ASSERT_INT_EQ(solver.perf_refactor_count, 5, "gate off: refactor count unchanged");

        /* Refactor reason staging is behavioral, so it stays active regardless of gate. */
        lp_telemetry_set_refactor_next_reason(&solver, RALPH_REFACTOR_REASON_PERIODIC);
        {
            int reason = RALPH_REFACTOR_REASON_OTHER;
            lp_telemetry_begin_refactor(&solver, &reason);
            ASSERT_INT_EQ(reason, RALPH_REFACTOR_REASON_PERIODIC,
                          "gate off: staged refactor reason still propagated");
            ASSERT_INT_EQ(solver.perf_refactor_next_reason, RALPH_REFACTOR_REASON_OTHER,
                          "gate off: staged reason consumed");
        }
    }

    {
        LUFactorization lu;
        SparseMatrix B;
        memset(&lu, 0, sizeof(lu));
        memset(&B, 0, sizeof(B));
        lu.telemetry_enabled = 0;
        lu.perf_symbolic_calls = 4;
        lu.mkz_calls = 7;
        lu.identity_sep_failures = 2;
        lu.used_dense_fallback_last = 1;
        lu.sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NUMERIC;
        B.nrows = 12;
        B.nnz = 34;

        lp_telemetry_prepare_lu_factorize(&lu, &B);
        lp_telemetry_lu_record_symbolic_call(&lu, 1.25);
        lp_telemetry_lu_mark_mkz_attempt(&lu);
        lp_telemetry_lu_mark_identity_sep_failure(&lu);
        lp_telemetry_lu_mark_dense_fallback(&lu);

        ASSERT_INT_EQ(lu.perf_symbolic_calls, 4, "gate off: LU symbolic calls unchanged");
        ASSERT_INT_EQ(lu.mkz_calls, 7, "gate off: LU mkz calls unchanged");
        ASSERT_INT_EQ(lu.identity_sep_failures, 2, "gate off: LU identity failures unchanged");
        ASSERT_INT_EQ(lu.used_dense_fallback_last, 0,
                      "gate off: lu_prepare still resets last dense fallback flag");
        ASSERT_INT_EQ(lu.sparse_fallback_last_reason, LU_SPARSE_FALLBACK_NONE,
                      "gate off: lu_prepare still resets fallback reason");
    }
}

int main(void) {
    printf("=== LP Telemetry Runtime Gate Tests ===\n");

    test_runtime_telemetry_gate();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
