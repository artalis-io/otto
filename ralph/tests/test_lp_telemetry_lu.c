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

#define ASSERT_U64_EQ(a, b, msg) do { \
    tests_run++; \
    if ((uint64_t)(a) == (uint64_t)(b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%llu != %llu)\n", msg, \
               (unsigned long long)(uint64_t)(a), \
               (unsigned long long)(uint64_t)(b)); \
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
    lu.telemetry.perf_sn_phase_samples = 2;
    lu.telemetry.perf_sn_panel_factor_ms = 0.9;
    lu.telemetry.perf_sn_panel_pivot_search_ms = 0.2;
    lu.telemetry.perf_sn_panel_swap_scatter_ms = 0.3;
    lu.telemetry.perf_sn_panel_eliminate_ms = 0.4;
    lu.telemetry.perf_sn_u_emit_ms = 1.1;
    lu.telemetry.perf_sn_active_set_ms = 1.3;
    lu.telemetry.perf_sn_pack_blocks_ms = 1.5;
    lu.telemetry.perf_sn_full_update_ms = 1.7;
    lu.telemetry.perf_sn_compact_update_ms = 1.9;
    lu.telemetry.perf_sn_active_row_scan_entries = 21;
    lu.telemetry.perf_sn_full_update_calls = 2;
    lu.telemetry.perf_sn_compact_cols1_calls = 7;
    lu.telemetry.perf_sn_compact_cols1_rows_total = 70;
    lu.telemetry.perf_sn_compact_cols1_ms = 7.5;
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
    ASSERT_U64_EQ(lu.telemetry.perf_sn_phase_samples, 0,
                  "lu_reset: supernode phase samples");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_factor_ms, 0.0,
                  "lu_reset: supernode panel factor total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_ms, 0.0,
                  "lu_reset: supernode panel pivot total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_swap_scatter_ms, 0.0,
                  "lu_reset: supernode panel swap/scatter total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_eliminate_ms, 0.0,
                  "lu_reset: supernode panel eliminate total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_u_emit_ms, 0.0,
                  "lu_reset: supernode U emit total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_active_set_ms, 0.0,
                  "lu_reset: supernode active-set total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_pack_blocks_ms, 0.0,
                  "lu_reset: supernode pack total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_full_update_ms, 0.0,
                  "lu_reset: supernode full update total");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_compact_update_ms, 0.0,
                  "lu_reset: supernode compact update total");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_active_row_scan_entries, 0,
                  "lu_reset: supernode row scan entries");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_full_update_calls, 0,
                  "lu_reset: supernode full update calls");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_compact_cols1_calls, 0,
                  "lu_reset: compact cols1 calls");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_compact_cols1_rows_total, 0,
                  "lu_reset: compact cols1 rows");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_compact_cols1_ms, 0.0,
                  "lu_reset: compact cols1 ms");
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
    lp_telemetry_lu_add_mkz_scan_work(&lu, 11, 12, 13, 14, 15, 16, 17);
    lp_telemetry_lu_add_mkz_colmax_work(&lu, 18, 19, 20);
    lp_telemetry_lu_add_supernode_work(&lu, 20, 20.1, 20.15, 20.16, 20.17,
                                       20.2, 20.3, 20.4, 20.5, 20.6,
                                       21, 22, 23, 24, 25, 26, 27, 28,
                                       29, 30, 31, 32, 33,
                                       34, 35, 35.5,
                                       36, 37, 37.5,
                                       38, 39, 39.5,
                                       40, 41, 41.5,
                                       42, 43, 43.5);

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
        ASSERT_U64_EQ(snap.perf_sn_phase_samples, 20,
                      "lu_snapshot: supernode phase samples");
        ASSERT_DBL_EQ(snap.perf_sn_panel_factor_ms, 20.1,
                      "lu_snapshot: supernode panel factor total");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_ms, 20.15,
                      "lu_snapshot: supernode panel pivot total");
        ASSERT_DBL_EQ(snap.perf_sn_panel_swap_scatter_ms, 20.16,
                      "lu_snapshot: supernode panel swap/scatter total");
        ASSERT_DBL_EQ(snap.perf_sn_panel_eliminate_ms, 20.17,
                      "lu_snapshot: supernode panel eliminate total");
        ASSERT_DBL_EQ(snap.perf_sn_u_emit_ms, 20.2,
                      "lu_snapshot: supernode U emit total");
        ASSERT_DBL_EQ(snap.perf_sn_active_set_ms, 20.3,
                      "lu_snapshot: supernode active-set total");
        ASSERT_DBL_EQ(snap.perf_sn_pack_blocks_ms, 20.4,
                      "lu_snapshot: supernode pack total");
        ASSERT_DBL_EQ(snap.perf_sn_full_update_ms, 20.5,
                      "lu_snapshot: supernode full update total");
        ASSERT_DBL_EQ(snap.perf_sn_compact_update_ms, 20.6,
                      "lu_snapshot: supernode compact update total");
        ASSERT_U64_EQ(snap.mkz_primary_scan_entries, 11,
                      "lu_snapshot: mkz primary scan entries");
        ASSERT_U64_EQ(snap.mkz_rescue_scan_entries, 12,
                      "lu_snapshot: mkz rescue scan entries");
        ASSERT_U64_EQ(snap.mkz_reserved_scan_entries, 13,
                      "lu_snapshot: mkz reserved scan entries");
        ASSERT_U64_EQ(snap.mkz_update_existing_entries, 14,
                      "lu_snapshot: mkz update existing entries");
        ASSERT_U64_EQ(snap.mkz_update_fill_candidates, 15,
                      "lu_snapshot: mkz update fill candidates");
        ASSERT_U64_EQ(snap.mkz_hint_fallback_scans, 16,
                      "lu_snapshot: mkz hint fallback scans");
        ASSERT_U64_EQ(snap.mkz_hint_fallback_scan_entries, 17,
                      "lu_snapshot: mkz hint fallback scan entries");
        ASSERT_U64_EQ(snap.mkz_affected_columns_total, 18,
                      "lu_snapshot: mkz affected columns total");
        ASSERT_U64_EQ(snap.mkz_affected_columns_max, 19,
                      "lu_snapshot: mkz affected columns max");
        ASSERT_U64_EQ(snap.mkz_col_max_scan_entries, 20,
                      "lu_snapshot: mkz col_max scan entries");
        ASSERT_U64_EQ(snap.perf_sn_active_row_scan_entries, 21,
                      "lu_snapshot: supernode active row scans");
        ASSERT_U64_EQ(snap.perf_sn_active_col_scan_entries, 22,
                      "lu_snapshot: supernode active col scans");
        ASSERT_U64_EQ(snap.perf_sn_trailing_rows_total, 23,
                      "lu_snapshot: supernode trailing rows");
        ASSERT_U64_EQ(snap.perf_sn_trailing_cols_total, 24,
                      "lu_snapshot: supernode trailing cols");
        ASSERT_U64_EQ(snap.perf_sn_active_rows_total, 25,
                      "lu_snapshot: supernode active rows");
        ASSERT_U64_EQ(snap.perf_sn_active_cols_total, 26,
                      "lu_snapshot: supernode active cols");
        ASSERT_U64_EQ(snap.perf_sn_pack_l_entries_total, 27,
                      "lu_snapshot: supernode L pack entries");
        ASSERT_U64_EQ(snap.perf_sn_pack_u_entries_total, 28,
                      "lu_snapshot: supernode U pack entries");
        ASSERT_U64_EQ(snap.perf_sn_dense_triplets_total, 29,
                      "lu_snapshot: supernode dense triplets");
        ASSERT_U64_EQ(snap.perf_sn_compact_triplets_total, 30,
                      "lu_snapshot: supernode compact triplets");
        ASSERT_U64_EQ(snap.perf_sn_full_update_calls, 31,
                      "lu_snapshot: supernode full update calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_update_calls, 32,
                      "lu_snapshot: supernode compact update calls");
        ASSERT_U64_EQ(snap.perf_sn_skipped_update_calls, 33,
                      "lu_snapshot: supernode skipped update calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols1_calls, 34,
                      "lu_snapshot: compact cols1 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols1_rows_total, 35,
                      "lu_snapshot: compact cols1 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols1_ms, 35.5,
                      "lu_snapshot: compact cols1 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols2_calls, 36,
                      "lu_snapshot: compact cols2 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols2_rows_total, 37,
                      "lu_snapshot: compact cols2 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols2_ms, 37.5,
                      "lu_snapshot: compact cols2 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols3_calls, 38,
                      "lu_snapshot: compact cols3 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols3_rows_total, 39,
                      "lu_snapshot: compact cols3 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols3_ms, 39.5,
                      "lu_snapshot: compact cols3 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols4_calls, 40,
                      "lu_snapshot: compact cols4 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols4_rows_total, 41,
                      "lu_snapshot: compact cols4 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols4_ms, 41.5,
                      "lu_snapshot: compact cols4 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols5p_calls, 42,
                      "lu_snapshot: compact cols5p calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols5p_rows_total, 43,
                      "lu_snapshot: compact cols5p rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols5p_ms, 43.5,
                      "lu_snapshot: compact cols5p ms");
    }
}

int main(void) {
    printf("=== LP Telemetry LU Tests ===\n");

    test_lu_reset_prepare_and_snapshot();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
