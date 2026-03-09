/*
 * Tests for LU-side LP telemetry helpers.
 *
 * Verifies:
 * 1) LU reset behavior
 * 2) per-factorization prepare behavior
 * 3) LU snapshot export
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "lp_bfcp_policy.h"

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

static void test_lu_reset_prepare_and_snapshot(void) {
    printf("  telemetry/lu: reset + prepare + snapshot...\n");

    LUFactorization lu;
    memset(&lu, 0, sizeof(lu));
    lu.telemetry_enabled = 1;
    lu.mkz_enabled = 1;
    lu.telemetry.perf_factorize_calls = 9;
    lu.telemetry.sparse_dense_fallbacks = 3;
    lu.telemetry.perf_total_sparse_numeric_ms = 99.0;
    lu.telemetry.perf_update_apply_forward_calls = 2;
    lu.telemetry.perf_update_apply_backward_calls = 3;
    lu.telemetry.perf_compact_factor_calls = 1;
    lu.telemetry.perf_compact_solve_calls = 4;
    lu.telemetry.perf_total_update_apply_forward_ms = 1.5;
    lu.telemetry.perf_total_update_apply_backward_ms = 2.5;
    lu.telemetry.perf_total_compact_factor_ms = 0.75;
    lu.telemetry.perf_total_compact_solve_ms = 3.5;
    lu.telemetry.refactor_need_checks = 4;
    lu.telemetry.update_fail_max_updates = 2;
    lu.telemetry.backend_policy_cgr = 7;
    lu.telemetry.backend_policy_last = LP_LU_BACKEND_POLICY_CGR;
    lu.telemetry.update_path_eta = 11;
    lu.telemetry.update_path_bg_compat = 5;
    lu.telemetry.update_path_gr_compat = 3;
    lu.last_refactor_trigger_reason = LP_BFCP_REFACTOR_REASON_MAX_UPDATES;

    lp_telemetry_reset_lu(&lu);

    ASSERT_INT_EQ(lu.mkz_enabled, 1, "lu_reset: preserves mkz_enabled");
    ASSERT_INT_EQ(lu.telemetry.perf_factorize_calls, 0, "lu_reset: factorize calls");
    ASSERT_INT_EQ(lu.telemetry.sparse_dense_fallbacks, 0, "lu_reset: sparse dense fallbacks");
    ASSERT_DBL_EQ(lu.telemetry.perf_total_sparse_numeric_ms, 0.0, "lu_reset: sparse numeric total");
    ASSERT_INT_EQ(lu.telemetry.perf_update_apply_forward_calls, 0,
                  "lu_reset: update apply forward calls");
    ASSERT_INT_EQ(lu.telemetry.perf_update_apply_backward_calls, 0,
                  "lu_reset: update apply backward calls");
    ASSERT_INT_EQ(lu.telemetry.perf_compact_factor_calls, 0,
                  "lu_reset: compact factor calls");
    ASSERT_INT_EQ(lu.telemetry.perf_compact_solve_calls, 0,
                  "lu_reset: compact solve calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_total_update_apply_forward_ms, 0.0,
                  "lu_reset: update apply forward total");
    ASSERT_DBL_EQ(lu.telemetry.perf_total_update_apply_backward_ms, 0.0,
                  "lu_reset: update apply backward total");
    ASSERT_DBL_EQ(lu.telemetry.perf_total_compact_factor_ms, 0.0,
                  "lu_reset: compact factor total");
    ASSERT_DBL_EQ(lu.telemetry.perf_total_compact_solve_ms, 0.0,
                  "lu_reset: compact solve total");
    ASSERT_INT_EQ(lu.telemetry.refactor_need_checks, 0, "lu_reset: refactor need checks");
    ASSERT_INT_EQ(lu.telemetry.update_fail_max_updates, 0, "lu_reset: update fail max updates");
    ASSERT_INT_EQ(lu.telemetry.backend_policy_cgr, 0, "lu_reset: backend policy cgr count");
    ASSERT_INT_EQ(lu.telemetry.backend_policy_last, LP_LU_BACKEND_POLICY_LUF_FT,
                  "lu_reset: backend policy last reset");
    ASSERT_INT_EQ(lu.telemetry.update_path_eta, 0, "lu_reset: eta update path count");
    ASSERT_INT_EQ(lu.telemetry.update_path_bg_compat, 0,
                  "lu_reset: bg compat update path count");
    ASSERT_INT_EQ(lu.telemetry.update_path_gr_compat, 0,
                  "lu_reset: gr compat update path count");

    SparseMatrix B;
    memset(&B, 0, sizeof(B));
    B.nrows = 7;
    B.ncols = 7;
    B.nnz = 21;

    lu.telemetry.perf_last_symbolic_ms = 12.0;
    lu.telemetry.sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NUMERIC;
    lu.telemetry.used_dense_fallback_last = 1;

    lp_telemetry_prepare_lu_factorize(&lu, &B);

    ASSERT_INT_EQ(lu.telemetry.perf_factorize_calls, 1, "lu_prepare: factorize calls increment");
    ASSERT_INT_EQ(lu.telemetry.perf_last_basis_nnz, 21, "lu_prepare: last basis nnz");
    ASSERT_INT_EQ(lu.telemetry.perf_last_m, 7, "lu_prepare: last m");
    ASSERT_INT_EQ(lu.telemetry.sparse_fallback_last_reason, LU_SPARSE_FALLBACK_NONE,
                  "lu_prepare: fallback reason reset");
    ASSERT_INT_EQ(lu.telemetry.used_dense_fallback_last, 0, "lu_prepare: dense fallback flag reset");
    ASSERT_DBL_EQ(lu.telemetry.perf_last_symbolic_ms, 0.0, "lu_prepare: last symbolic ms reset");

    lp_telemetry_lu_record_update_apply_forward_ms(&lu, 0.75);
    lp_telemetry_lu_record_update_apply_backward_ms(&lu, 1.25);
    lp_telemetry_lu_record_compact_factor_ms(&lu, 0.20);
    lp_telemetry_lu_record_compact_solve_ms(&lu, 0.50);

    {
        LUTelemetrySnapshot snap;
        lp_telemetry_snapshot_lu(&lu, &snap);
        ASSERT_INT_EQ(snap.mkz_enabled, 1, "lu_snapshot: mkz_enabled");
        ASSERT_INT_EQ(snap.perf_factorize_calls, 1, "lu_snapshot: factorize calls");
        ASSERT_INT_EQ(snap.perf_last_basis_nnz, 21, "lu_snapshot: last basis nnz");
        ASSERT_INT_EQ(snap.refactor_need_checks, 0, "lu_snapshot: refactor checks");
        ASSERT_INT_EQ(snap.backend_policy_last, LP_LU_BACKEND_POLICY_LUF_FT,
                      "lu_snapshot: backend policy last exported");
        ASSERT_INT_EQ(snap.update_path_ft, 0,
                      "lu_snapshot: update path FT exported");
        ASSERT_INT_EQ(snap.update_path_bg_compat, 0,
                      "lu_snapshot: update path BG compat exported");
        ASSERT_INT_EQ(snap.update_path_gr_compat, 0,
                      "lu_snapshot: update path GR compat exported");
        ASSERT_INT_EQ(snap.last_refactor_trigger_reason,
                      LP_BFCP_REFACTOR_REASON_NONE,
                      "lu_snapshot: last refactor reason exported");
        ASSERT_INT_EQ(snap.perf_update_apply_forward_calls, 1,
                      "lu_snapshot: update apply forward calls");
        ASSERT_INT_EQ(snap.perf_update_apply_backward_calls, 1,
                      "lu_snapshot: update apply backward calls");
        ASSERT_INT_EQ(snap.perf_compact_factor_calls, 1,
                      "lu_snapshot: compact factor calls");
        ASSERT_INT_EQ(snap.perf_compact_solve_calls, 1,
                      "lu_snapshot: compact solve calls");
        ASSERT_DBL_EQ(snap.perf_total_update_apply_forward_ms, 0.75,
                      "lu_snapshot: update apply forward total");
        ASSERT_DBL_EQ(snap.perf_total_update_apply_backward_ms, 1.25,
                      "lu_snapshot: update apply backward total");
        ASSERT_DBL_EQ(snap.perf_total_compact_factor_ms, 0.20,
                      "lu_snapshot: compact factor total");
        ASSERT_DBL_EQ(snap.perf_total_compact_solve_ms, 0.50,
                      "lu_snapshot: compact solve total");
    }
}

int main(void) {
    printf("=== LP Telemetry LU Tests ===\n");

    test_lu_reset_prepare_and_snapshot();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
