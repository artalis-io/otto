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
    lu.telemetry.perf_sn_panel_pivot_search_calls = 4;
    lu.telemetry.perf_sn_panel_pivot_search_entries_total = 5;
    lu.telemetry.perf_sn_panel_pivot_search_size1_calls = 6;
    lu.telemetry.perf_sn_panel_pivot_search_size1_ms = 0.21;
    lu.telemetry.perf_sn_panel_pivot_search_size2_calls = 7;
    lu.telemetry.perf_sn_panel_pivot_search_size2_ms = 0.22;
    lu.telemetry.perf_sn_panel_pivot_search_size3_4_calls = 8;
    lu.telemetry.perf_sn_panel_pivot_search_size3_4_ms = 0.23;
    lu.telemetry.perf_sn_panel_pivot_search_size5_8_calls = 9;
    lu.telemetry.perf_sn_panel_pivot_search_size5_8_ms = 0.24;
    lu.telemetry.perf_sn_panel_pivot_search_size9p_calls = 10;
    lu.telemetry.perf_sn_panel_pivot_search_size9p_ms = 0.25;
    lu.telemetry.perf_sn_panel_pivot_search_reserved_present_calls = 11;
    lu.telemetry.perf_sn_panel_pivot_search_reserved_present_entries = 12;
    lu.telemetry.perf_sn_panel_pivot_search_reserved_present_ms = 0.26;
    lu.telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_calls = 13;
    lu.telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_ms = 0.27;
    lu.telemetry.perf_sn_size1_u_emit_calls = 14;
    lu.telemetry.perf_sn_size1_u_emit_ms = 0.28;
    lu.telemetry.perf_sn_size1_update_scan_calls = 15;
    lu.telemetry.perf_sn_size1_update_scan_ms = 0.29;
    lu.telemetry.perf_sn_size1_update_apply_calls = 16;
    lu.telemetry.perf_sn_size1_update_apply_ms = 0.30;
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
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_calls, 0,
                  "lu_reset: supernode panel pivot calls");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_entries_total, 0,
                  "lu_reset: supernode panel pivot entries");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_size1_calls, 0,
                  "lu_reset: supernode panel size1 calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_size1_ms, 0.0,
                  "lu_reset: supernode panel size1 ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_size2_calls, 0,
                  "lu_reset: supernode panel size2 calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_size2_ms, 0.0,
                  "lu_reset: supernode panel size2 ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_size3_4_calls, 0,
                  "lu_reset: supernode panel size3_4 calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_size3_4_ms, 0.0,
                  "lu_reset: supernode panel size3_4 ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_size5_8_calls, 0,
                  "lu_reset: supernode panel size5_8 calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_size5_8_ms, 0.0,
                  "lu_reset: supernode panel size5_8 ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_size9p_calls, 0,
                  "lu_reset: supernode panel size9p calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_size9p_ms, 0.0,
                  "lu_reset: supernode panel size9p ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_reserved_present_calls, 0,
                  "lu_reset: supernode reserved-present calls");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_reserved_present_entries, 0,
                  "lu_reset: supernode reserved-present entries");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_reserved_present_ms, 0.0,
                  "lu_reset: supernode reserved-present ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_calls, 0,
                  "lu_reset: supernode reserved-alt calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_ms, 0.0,
                  "lu_reset: supernode reserved-alt ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_size1_u_emit_calls, 0,
                  "lu_reset: supernode size1 u-emit calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_size1_u_emit_ms, 0.0,
                  "lu_reset: supernode size1 u-emit ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_size1_update_scan_calls, 0,
                  "lu_reset: supernode size1 update-scan calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_size1_update_scan_ms, 0.0,
                  "lu_reset: supernode size1 update-scan ms");
    ASSERT_U64_EQ(lu.telemetry.perf_sn_size1_update_apply_calls, 0,
                  "lu_reset: supernode size1 update-apply calls");
    ASSERT_DBL_EQ(lu.telemetry.perf_sn_size1_update_apply_ms, 0.0,
                  "lu_reset: supernode size1 update-apply ms");
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
                                       21, 22, 23, 23.5, 24, 24.5, 25, 25.5,
                                       26, 26.5, 27, 27.5, 28, 29, 29.5, 30, 30.5,
                                       31, 31.1, 32, 32.1, 33, 33.1,
                                       34.1, 34.2, 34.3, 34.4, 34.5,
                                       35, 36, 37, 38, 39, 40, 41, 42,
                                       43, 44, 45, 46, 47,
                                       48, 49, 49.5,
                                       50, 51, 51.5,
                                       52, 53, 53.5,
                                       54, 55, 55.5,
                                       56, 57, 57.5);

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
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_calls, 21,
                      "lu_snapshot: supernode panel pivot calls");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_entries_total, 22,
                      "lu_snapshot: supernode panel pivot entries");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_size1_calls, 23,
                      "lu_snapshot: supernode panel size1 calls");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_size1_ms, 23.5,
                      "lu_snapshot: supernode panel size1 ms");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_size2_calls, 24,
                      "lu_snapshot: supernode panel size2 calls");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_size2_ms, 24.5,
                      "lu_snapshot: supernode panel size2 ms");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_size3_4_calls, 25,
                      "lu_snapshot: supernode panel size3_4 calls");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_size3_4_ms, 25.5,
                      "lu_snapshot: supernode panel size3_4 ms");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_size5_8_calls, 26,
                      "lu_snapshot: supernode panel size5_8 calls");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_size5_8_ms, 26.5,
                      "lu_snapshot: supernode panel size5_8 ms");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_size9p_calls, 27,
                      "lu_snapshot: supernode panel size9p calls");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_size9p_ms, 27.5,
                      "lu_snapshot: supernode panel size9p ms");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_reserved_present_calls, 28,
                      "lu_snapshot: supernode reserved-present calls");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_reserved_present_entries, 29,
                      "lu_snapshot: supernode reserved-present entries");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_reserved_present_ms, 29.5,
                      "lu_snapshot: supernode reserved-present ms");
        ASSERT_U64_EQ(snap.perf_sn_panel_pivot_search_reserved_alt_chosen_calls, 30,
                      "lu_snapshot: supernode reserved-alt calls");
        ASSERT_DBL_EQ(snap.perf_sn_panel_pivot_search_reserved_alt_chosen_ms, 30.5,
                      "lu_snapshot: supernode reserved-alt ms");
        ASSERT_U64_EQ(snap.perf_sn_size1_u_emit_calls, 31,
                      "lu_snapshot: supernode size1 u-emit calls");
        ASSERT_DBL_EQ(snap.perf_sn_size1_u_emit_ms, 31.1,
                      "lu_snapshot: supernode size1 u-emit ms");
        ASSERT_U64_EQ(snap.perf_sn_size1_update_scan_calls, 32,
                      "lu_snapshot: supernode size1 update-scan calls");
        ASSERT_DBL_EQ(snap.perf_sn_size1_update_scan_ms, 32.1,
                      "lu_snapshot: supernode size1 update-scan ms");
        ASSERT_U64_EQ(snap.perf_sn_size1_update_apply_calls, 33,
                      "lu_snapshot: supernode size1 update-apply calls");
        ASSERT_DBL_EQ(snap.perf_sn_size1_update_apply_ms, 33.1,
                      "lu_snapshot: supernode size1 update-apply ms");
        ASSERT_DBL_EQ(snap.perf_sn_panel_swap_scatter_ms, 20.16,
                      "lu_snapshot: supernode panel swap/scatter total");
        ASSERT_DBL_EQ(snap.perf_sn_panel_eliminate_ms, 20.17,
                      "lu_snapshot: supernode panel eliminate total");
        ASSERT_DBL_EQ(snap.perf_sn_u_emit_ms, 34.1,
                      "lu_snapshot: supernode U emit total");
        ASSERT_DBL_EQ(snap.perf_sn_active_set_ms, 34.2,
                      "lu_snapshot: supernode active-set total");
        ASSERT_DBL_EQ(snap.perf_sn_pack_blocks_ms, 34.3,
                      "lu_snapshot: supernode pack total");
        ASSERT_DBL_EQ(snap.perf_sn_full_update_ms, 34.4,
                      "lu_snapshot: supernode full update total");
        ASSERT_DBL_EQ(snap.perf_sn_compact_update_ms, 34.5,
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
        ASSERT_U64_EQ(snap.perf_sn_active_row_scan_entries, 35,
                      "lu_snapshot: supernode active row scans");
        ASSERT_U64_EQ(snap.perf_sn_active_col_scan_entries, 36,
                      "lu_snapshot: supernode active col scans");
        ASSERT_U64_EQ(snap.perf_sn_trailing_rows_total, 37,
                      "lu_snapshot: supernode trailing rows");
        ASSERT_U64_EQ(snap.perf_sn_trailing_cols_total, 38,
                      "lu_snapshot: supernode trailing cols");
        ASSERT_U64_EQ(snap.perf_sn_active_rows_total, 39,
                      "lu_snapshot: supernode active rows");
        ASSERT_U64_EQ(snap.perf_sn_active_cols_total, 40,
                      "lu_snapshot: supernode active cols");
        ASSERT_U64_EQ(snap.perf_sn_pack_l_entries_total, 41,
                      "lu_snapshot: supernode L pack entries");
        ASSERT_U64_EQ(snap.perf_sn_pack_u_entries_total, 42,
                      "lu_snapshot: supernode U pack entries");
        ASSERT_U64_EQ(snap.perf_sn_dense_triplets_total, 43,
                      "lu_snapshot: supernode dense triplets");
        ASSERT_U64_EQ(snap.perf_sn_compact_triplets_total, 44,
                      "lu_snapshot: supernode compact triplets");
        ASSERT_U64_EQ(snap.perf_sn_full_update_calls, 45,
                      "lu_snapshot: supernode full update calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_update_calls, 46,
                      "lu_snapshot: supernode compact update calls");
        ASSERT_U64_EQ(snap.perf_sn_skipped_update_calls, 47,
                      "lu_snapshot: supernode skipped update calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols1_calls, 48,
                      "lu_snapshot: compact cols1 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols1_rows_total, 49,
                      "lu_snapshot: compact cols1 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols1_ms, 49.5,
                      "lu_snapshot: compact cols1 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols2_calls, 50,
                      "lu_snapshot: compact cols2 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols2_rows_total, 51,
                      "lu_snapshot: compact cols2 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols2_ms, 51.5,
                      "lu_snapshot: compact cols2 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols3_calls, 52,
                      "lu_snapshot: compact cols3 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols3_rows_total, 53,
                      "lu_snapshot: compact cols3 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols3_ms, 53.5,
                      "lu_snapshot: compact cols3 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols4_calls, 54,
                      "lu_snapshot: compact cols4 calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols4_rows_total, 55,
                      "lu_snapshot: compact cols4 rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols4_ms, 55.5,
                      "lu_snapshot: compact cols4 ms");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols5p_calls, 56,
                      "lu_snapshot: compact cols5p calls");
        ASSERT_U64_EQ(snap.perf_sn_compact_cols5p_rows_total, 57,
                      "lu_snapshot: compact cols5p rows");
        ASSERT_DBL_EQ(snap.perf_sn_compact_cols5p_ms, 57.5,
                      "lu_snapshot: compact cols5p ms");
    }
}

int main(void) {
    printf("=== LP Telemetry LU Tests ===\n");

    test_lu_reset_prepare_and_snapshot();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
