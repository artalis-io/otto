/*
 * Ralph - LU Telemetry Helpers
 *
 * LU reset/prepare/snapshot and dense/symbolic timing counters.
 */

#include <string.h>
#include "lp.h"
#include "lp_bfcp_policy.h"

static int lu_telemetry_enabled(const LUFactorization *lu) {
    return lu && lu->telemetry_enabled;
}

void lp_telemetry_reset_lu(LUFactorization *lu) {
    if (!lu) return;
    lu->last_refactor_trigger_reason = LP_BFCP_REFACTOR_REASON_NONE;
    lu->telemetry.mkz_calls = 0;
    lu->telemetry.mkz_successes = 0;
    lu->telemetry.mkz_failures = 0;
    lu->telemetry.mkz_last_failure = 0;
    lu->telemetry.mkz_dense_fallbacks = 0;
    lu->telemetry.mkz_fail_workspace = 0;
    lu->telemetry.mkz_fail_pool = 0;
    lu->telemetry.mkz_fail_singular = 0;
    lu->telemetry.mkz_fail_capacity = 0;
    lu->telemetry.mkz_singular_retry_attempts = 0;
    lu->telemetry.mkz_singular_retry_successes = 0;
    lu->telemetry.mkz_singular_retry_failures = 0;
    lu->telemetry.mkz_reserved_fallback_attempts = 0;
    lu->telemetry.mkz_reserved_fallback_accepts = 0;
    lu->telemetry.mkz_reserved_fallback_rejects = 0;
    lu->telemetry.mkz_circuit_trips = 0;
    lu->telemetry.mkz_circuit_skips = 0;
    lu->telemetry.mkz_circuit_resets = 0;
    lu->telemetry.mkz_global_skip_trips = 0;
    lu->telemetry.mkz_global_skip_skips = 0;
    lu->telemetry.mkz_global_skip_resets = 0;
    lu->telemetry.mkz_profile_retry_attempts = 0;
    lu->telemetry.mkz_profile_retry_successes = 0;
    lu->telemetry.mkz_profile_retry_failures = 0;
    lu->telemetry.mkz_profile_retry_fail_identity_sep = 0;
    lu->telemetry.mkz_profile_retry_fail_backend_exhausted = 0;
    lu->telemetry.mkz_profile_retry_fail_pathological = 0;
    lu->telemetry.mkz_primary_scan_entries = 0;
    lu->telemetry.mkz_rescue_scan_entries = 0;
    lu->telemetry.mkz_reserved_scan_entries = 0;
    lu->telemetry.mkz_update_existing_entries = 0;
    lu->telemetry.mkz_update_fill_candidates = 0;
    lu->telemetry.mkz_hint_fallback_scans = 0;
    lu->telemetry.mkz_hint_fallback_scan_entries = 0;
    lu->telemetry.mkz_affected_columns_total = 0;
    lu->telemetry.mkz_affected_columns_max = 0;
    lu->telemetry.mkz_col_max_scan_entries = 0;
    lu->telemetry.mkz_high_cond_count = 0;
    lu->telemetry.mkz_worst_cond = 0.0;
    lu->telemetry.sparse_dense_fallbacks = 0;
    lu->telemetry.used_dense_fallback_last = 0;
    lu->telemetry.sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NONE;
    lu->telemetry.sparse_fallback_reason_small_matrix = 0;
    lu->telemetry.sparse_fallback_reason_symbolic = 0;
    lu->telemetry.sparse_fallback_reason_numeric = 0;
    lu->telemetry.sparse_numeric_last_failure_reason = LU_SPARSE_NUMERIC_FAIL_NONE;
    lu->telemetry.sparse_numeric_fail_identity_sep = 0;
    lu->telemetry.sparse_numeric_fail_backend_exhausted = 0;
    lu->telemetry.sparse_numeric_fail_pathological = 0;
    lu->telemetry.numeric_full_retry_attempts = 0;
    lu->telemetry.numeric_full_retry_successes = 0;
    lu->telemetry.numeric_full_retry_failures = 0;
    lu->telemetry.identity_sep_failures = 0;
    lu->telemetry.symbolic_failures = 0;
    lu->telemetry.symbolic_fail_workspace = 0;
    lu->telemetry.symbolic_fail_unmatched_no_reserved = 0;
    lu->telemetry.symbolic_fail_inconsistent_identity = 0;
    lu->telemetry.symbolic_full_retry_attempts = 0;
    lu->telemetry.symbolic_full_retry_successes = 0;
    lu->telemetry.symbolic_full_retry_numeric_failures = 0;
    lu->telemetry.symbolic_full_retry_mkz_attempts = 0;
    lu->telemetry.symbolic_full_retry_mkz_successes = 0;
    lu->telemetry.symbolic_full_retry_mkz_failures = 0;
    lu->telemetry.numeric_backend_markowitz = 0;
    lu->telemetry.numeric_backend_supernode = 0;
    lu->telemetry.numeric_backend_dense_ge = 0;
    lu->telemetry.backend_policy_luf_ft = 0;
    lu->telemetry.backend_policy_cbg = 0;
    lu->telemetry.backend_policy_cgr = 0;
    lu->telemetry.backend_policy_last = LP_LU_BACKEND_POLICY_LUF_FT;
    lu->telemetry.update_path_ft = 0;
    lu->telemetry.update_path_eta = 0;
    lu->telemetry.update_path_bg_compat = 0;
    lu->telemetry.update_path_gr_compat = 0;
    lu->telemetry.identity_sep_retry_lane_dense_chosen = 0;
    lu->telemetry.identity_sep_retry_lane_supernode_chosen = 0;
    lu->telemetry.identity_sep_retry_lane_dense_successes = 0;
    lu->telemetry.identity_sep_retry_lane_supernode_successes = 0;
    lu->telemetry.sn_cost_gate_trips = 0;
    lu->telemetry.sn_cost_gate_skips = 0;
    lu->telemetry.sn_cost_gate_resets = 0;
    lu->telemetry.refactor_need_checks = 0;
    lu->telemetry.refactor_need_triggers = 0;
    lu->telemetry.refactor_need_last_reason = 0;
    lu->telemetry.refactor_need_reason_max_updates = 0;
    lu->telemetry.refactor_need_reason_growth_guard = 0;
    lu->telemetry.refactor_need_reason_avg_spike_density = 0;
    lu->telemetry.refactor_need_reason_cond_severe = 0;
    lu->telemetry.refactor_need_reason_cond_adaptive_limit = 0;
    lu->telemetry.refactor_need_reason_spike_pool_warn = 0;
    lu->telemetry.refactor_need_reason_spike_work = 0;
    lu->telemetry.refactor_need_reason_spike_diag_quality = 0;
    lu->telemetry.update_fail_bad_input = 0;
    lu->telemetry.update_fail_max_updates = 0;
    lu->telemetry.update_fail_singular_update = 0;
    lu->telemetry.update_fail_update_pivot_too_small = 0;
    lu->telemetry.update_fail_spike_pool_full = 0;
    lu->telemetry.update_fail_dense_spike_reject = 0;
    lu->telemetry.update_fail_eta_alloc = 0;
    lu->telemetry.perf_factorize_calls = 0;
    lu->telemetry.perf_last_basis_nnz = 0;
    lu->telemetry.perf_last_m = 0;
    lu->telemetry.perf_last_k = 0;
    lu->telemetry.perf_symbolic_calls = 0;
    lu->telemetry.perf_symbolic_cache_hits = 0;
    lu->telemetry.perf_symbolic_cache_misses = 0;
    lu->telemetry.perf_last_symbolic_ms = 0.0;
    lu->telemetry.perf_last_sparse_numeric_ms = 0.0;
    lu->telemetry.perf_last_dense_ge_numeric_ms = 0.0;
    lu->telemetry.perf_last_supernode_numeric_ms = 0.0;
    lu->telemetry.perf_last_dense_factorize_ms = 0.0;
    lu->telemetry.perf_last_a_struct_build_ms = 0.0;
    lu->telemetry.perf_last_markowitz_numeric_ms = 0.0;
    lu->telemetry.perf_last_identity_placement_ms = 0.0;
    lu->telemetry.perf_last_coo_to_csc_ms = 0.0;
    lu->telemetry.perf_total_symbolic_ms = 0.0;
    lu->telemetry.perf_total_sparse_numeric_ms = 0.0;
    lu->telemetry.perf_total_dense_ge_numeric_ms = 0.0;
    lu->telemetry.perf_total_supernode_numeric_ms = 0.0;
    lu->telemetry.perf_total_dense_factorize_ms = 0.0;
    lu->telemetry.perf_total_a_struct_build_ms = 0.0;
    lu->telemetry.perf_total_markowitz_numeric_ms = 0.0;
    lu->telemetry.perf_total_identity_placement_ms = 0.0;
    lu->telemetry.perf_total_coo_to_csc_ms = 0.0;
    lu->telemetry.perf_update_apply_forward_calls = 0;
    lu->telemetry.perf_update_apply_backward_calls = 0;
    lu->telemetry.perf_compact_factor_calls = 0;
    lu->telemetry.perf_compact_solve_calls = 0;
    lu->telemetry.perf_total_update_apply_forward_ms = 0.0;
    lu->telemetry.perf_total_update_apply_backward_ms = 0.0;
    lu->telemetry.perf_total_compact_factor_ms = 0.0;
    lu->telemetry.perf_total_compact_solve_ms = 0.0;
    lu->telemetry.perf_sn_phase_samples = 0;
    lu->telemetry.perf_sn_panel_factor_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_ms = 0.0;
    lu->telemetry.perf_sn_panel_swap_scatter_ms = 0.0;
    lu->telemetry.perf_sn_panel_eliminate_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_entries_total = 0;
    lu->telemetry.perf_sn_panel_pivot_search_size1_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_size1_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_size2_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_size2_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_size3_4_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_size3_4_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_size5_8_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_size5_8_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_size9p_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_size9p_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_present_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_present_entries = 0;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_present_ms = 0.0;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_calls = 0;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_ms = 0.0;
    lu->telemetry.perf_sn_size1_u_emit_calls = 0;
    lu->telemetry.perf_sn_size1_u_emit_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_scan_calls = 0;
    lu->telemetry.perf_sn_size1_update_scan_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_apply_calls = 0;
    lu->telemetry.perf_sn_size1_update_apply_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_row_gather_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_col_indirection_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_outer_product_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_full_calls = 0;
    lu->telemetry.perf_sn_size1_update_full_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols1_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols1_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols2_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols2_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols3_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols3_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols4_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols4_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols5p_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols5p_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows1_8_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows1_8_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows9_32_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows9_32_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows33_128_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows33_128_ms = 0.0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows129p_calls = 0;
    lu->telemetry.perf_sn_size1_update_cols5p_rows129p_ms = 0.0;
    lu->telemetry.perf_sn_u_emit_ms = 0.0;
    lu->telemetry.perf_sn_active_set_ms = 0.0;
    lu->telemetry.perf_sn_pack_blocks_ms = 0.0;
    lu->telemetry.perf_sn_full_update_ms = 0.0;
    lu->telemetry.perf_sn_compact_update_ms = 0.0;
    lu->telemetry.perf_sn_active_row_scan_entries = 0;
    lu->telemetry.perf_sn_active_col_scan_entries = 0;
    lu->telemetry.perf_sn_trailing_rows_total = 0;
    lu->telemetry.perf_sn_trailing_cols_total = 0;
    lu->telemetry.perf_sn_active_rows_total = 0;
    lu->telemetry.perf_sn_active_cols_total = 0;
    lu->telemetry.perf_sn_pack_l_entries_total = 0;
    lu->telemetry.perf_sn_pack_u_entries_total = 0;
    lu->telemetry.perf_sn_dense_triplets_total = 0;
    lu->telemetry.perf_sn_compact_triplets_total = 0;
    lu->telemetry.perf_sn_full_update_calls = 0;
    lu->telemetry.perf_sn_compact_update_calls = 0;
    lu->telemetry.perf_sn_skipped_update_calls = 0;
    lu->telemetry.perf_sn_compact_cols1_calls = 0;
    lu->telemetry.perf_sn_compact_cols1_rows_total = 0;
    lu->telemetry.perf_sn_compact_cols1_ms = 0.0;
    lu->telemetry.perf_sn_compact_cols2_calls = 0;
    lu->telemetry.perf_sn_compact_cols2_rows_total = 0;
    lu->telemetry.perf_sn_compact_cols2_ms = 0.0;
    lu->telemetry.perf_sn_compact_cols3_calls = 0;
    lu->telemetry.perf_sn_compact_cols3_rows_total = 0;
    lu->telemetry.perf_sn_compact_cols3_ms = 0.0;
    lu->telemetry.perf_sn_compact_cols4_calls = 0;
    lu->telemetry.perf_sn_compact_cols4_rows_total = 0;
    lu->telemetry.perf_sn_compact_cols4_ms = 0.0;
    lu->telemetry.perf_sn_compact_cols5p_calls = 0;
    lu->telemetry.perf_sn_compact_cols5p_rows_total = 0;
    lu->telemetry.perf_sn_compact_cols5p_ms = 0.0;
}

void lp_telemetry_prepare_lu_factorize(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu) return;
    lu->telemetry.used_dense_fallback_last = 0;
    lu->telemetry.sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NONE;
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_factorize_calls++;
    lu->telemetry.perf_last_basis_nnz = B ? B->nnz : 0;
    lu->telemetry.perf_last_m = B ? B->nrows : 0;
    lu->telemetry.perf_last_k = 0;
    lu->telemetry.sparse_numeric_last_failure_reason = LU_SPARSE_NUMERIC_FAIL_NONE;
    lu->telemetry.perf_last_symbolic_ms = 0.0;
    lu->telemetry.perf_last_sparse_numeric_ms = 0.0;
    lu->telemetry.perf_last_dense_ge_numeric_ms = 0.0;
    lu->telemetry.perf_last_supernode_numeric_ms = 0.0;
    lu->telemetry.perf_last_dense_factorize_ms = 0.0;
    lu->telemetry.perf_last_a_struct_build_ms = 0.0;
    lu->telemetry.perf_last_markowitz_numeric_ms = 0.0;
    lu->telemetry.perf_last_identity_placement_ms = 0.0;
    lu->telemetry.perf_last_coo_to_csc_ms = 0.0;
}

#define COPY_LU_FIELD(field) out->field = lu->field
#define COPY_LU_TELEM_FIELD(field) out->field = lu->telemetry.field
void lp_telemetry_snapshot_lu(const LUFactorization *lu,
                              LUTelemetrySnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lu) return;

    COPY_LU_FIELD(mkz_enabled);
    COPY_LU_FIELD(sn_enabled);
    COPY_LU_TELEM_FIELD(mkz_calls);
    COPY_LU_TELEM_FIELD(mkz_successes);
    COPY_LU_TELEM_FIELD(mkz_failures);
    COPY_LU_TELEM_FIELD(mkz_last_failure);
    COPY_LU_TELEM_FIELD(mkz_dense_fallbacks);
    COPY_LU_TELEM_FIELD(mkz_fail_workspace);
    COPY_LU_TELEM_FIELD(mkz_fail_pool);
    COPY_LU_TELEM_FIELD(mkz_fail_singular);
    COPY_LU_TELEM_FIELD(mkz_fail_capacity);
    COPY_LU_TELEM_FIELD(mkz_singular_retry_attempts);
    COPY_LU_TELEM_FIELD(mkz_singular_retry_successes);
    COPY_LU_TELEM_FIELD(mkz_singular_retry_failures);
    COPY_LU_TELEM_FIELD(mkz_reserved_fallback_attempts);
    COPY_LU_TELEM_FIELD(mkz_reserved_fallback_accepts);
    COPY_LU_TELEM_FIELD(mkz_reserved_fallback_rejects);
    COPY_LU_TELEM_FIELD(mkz_circuit_trips);
    COPY_LU_TELEM_FIELD(mkz_circuit_skips);
    COPY_LU_TELEM_FIELD(mkz_circuit_resets);
    COPY_LU_TELEM_FIELD(mkz_global_skip_trips);
    COPY_LU_TELEM_FIELD(mkz_global_skip_skips);
    COPY_LU_TELEM_FIELD(mkz_global_skip_resets);
    COPY_LU_TELEM_FIELD(mkz_profile_retry_attempts);
    COPY_LU_TELEM_FIELD(mkz_profile_retry_successes);
    COPY_LU_TELEM_FIELD(mkz_profile_retry_failures);
    COPY_LU_TELEM_FIELD(mkz_profile_retry_fail_identity_sep);
    COPY_LU_TELEM_FIELD(mkz_profile_retry_fail_backend_exhausted);
    COPY_LU_TELEM_FIELD(mkz_profile_retry_fail_pathological);
    COPY_LU_TELEM_FIELD(mkz_primary_scan_entries);
    COPY_LU_TELEM_FIELD(mkz_rescue_scan_entries);
    COPY_LU_TELEM_FIELD(mkz_reserved_scan_entries);
    COPY_LU_TELEM_FIELD(mkz_update_existing_entries);
    COPY_LU_TELEM_FIELD(mkz_update_fill_candidates);
    COPY_LU_TELEM_FIELD(mkz_hint_fallback_scans);
    COPY_LU_TELEM_FIELD(mkz_hint_fallback_scan_entries);
    COPY_LU_TELEM_FIELD(mkz_affected_columns_total);
    COPY_LU_TELEM_FIELD(mkz_affected_columns_max);
    COPY_LU_TELEM_FIELD(mkz_col_max_scan_entries);
    COPY_LU_TELEM_FIELD(mkz_high_cond_count);
    COPY_LU_TELEM_FIELD(mkz_worst_cond);

    COPY_LU_TELEM_FIELD(sparse_dense_fallbacks);
    COPY_LU_TELEM_FIELD(used_dense_fallback_last);
    COPY_LU_TELEM_FIELD(sparse_fallback_last_reason);
    COPY_LU_TELEM_FIELD(sparse_fallback_reason_small_matrix);
    COPY_LU_TELEM_FIELD(sparse_fallback_reason_symbolic);
    COPY_LU_TELEM_FIELD(sparse_fallback_reason_numeric);
    COPY_LU_TELEM_FIELD(sparse_numeric_last_failure_reason);
    COPY_LU_TELEM_FIELD(sparse_numeric_fail_identity_sep);
    COPY_LU_TELEM_FIELD(sparse_numeric_fail_backend_exhausted);
    COPY_LU_TELEM_FIELD(sparse_numeric_fail_pathological);
    COPY_LU_TELEM_FIELD(numeric_full_retry_attempts);
    COPY_LU_TELEM_FIELD(numeric_full_retry_successes);
    COPY_LU_TELEM_FIELD(numeric_full_retry_failures);
    COPY_LU_TELEM_FIELD(identity_sep_failures);
    COPY_LU_TELEM_FIELD(symbolic_failures);
    COPY_LU_TELEM_FIELD(symbolic_fail_workspace);
    COPY_LU_TELEM_FIELD(symbolic_fail_unmatched_no_reserved);
    COPY_LU_TELEM_FIELD(symbolic_fail_inconsistent_identity);
    COPY_LU_TELEM_FIELD(symbolic_full_retry_attempts);
    COPY_LU_TELEM_FIELD(symbolic_full_retry_successes);
    COPY_LU_TELEM_FIELD(symbolic_full_retry_numeric_failures);
    COPY_LU_TELEM_FIELD(symbolic_full_retry_mkz_attempts);
    COPY_LU_TELEM_FIELD(symbolic_full_retry_mkz_successes);
    COPY_LU_TELEM_FIELD(symbolic_full_retry_mkz_failures);
    COPY_LU_TELEM_FIELD(numeric_backend_markowitz);
    COPY_LU_TELEM_FIELD(numeric_backend_supernode);
    COPY_LU_TELEM_FIELD(numeric_backend_dense_ge);
    COPY_LU_TELEM_FIELD(backend_policy_luf_ft);
    COPY_LU_TELEM_FIELD(backend_policy_cbg);
    COPY_LU_TELEM_FIELD(backend_policy_cgr);
    COPY_LU_TELEM_FIELD(backend_policy_last);
    COPY_LU_TELEM_FIELD(update_path_ft);
    COPY_LU_TELEM_FIELD(update_path_eta);
    COPY_LU_TELEM_FIELD(update_path_bg_compat);
    COPY_LU_TELEM_FIELD(update_path_gr_compat);
    COPY_LU_TELEM_FIELD(identity_sep_retry_lane_dense_chosen);
    COPY_LU_TELEM_FIELD(identity_sep_retry_lane_supernode_chosen);
    COPY_LU_TELEM_FIELD(identity_sep_retry_lane_dense_successes);
    COPY_LU_TELEM_FIELD(identity_sep_retry_lane_supernode_successes);
    COPY_LU_TELEM_FIELD(sn_cost_gate_trips);
    COPY_LU_TELEM_FIELD(sn_cost_gate_skips);
    COPY_LU_TELEM_FIELD(sn_cost_gate_resets);
    COPY_LU_TELEM_FIELD(refactor_need_checks);
    COPY_LU_TELEM_FIELD(refactor_need_triggers);
    COPY_LU_TELEM_FIELD(refactor_need_last_reason);
    COPY_LU_TELEM_FIELD(refactor_need_reason_max_updates);
    COPY_LU_TELEM_FIELD(refactor_need_reason_growth_guard);
    COPY_LU_TELEM_FIELD(refactor_need_reason_avg_spike_density);
    COPY_LU_TELEM_FIELD(refactor_need_reason_cond_severe);
    COPY_LU_TELEM_FIELD(refactor_need_reason_cond_adaptive_limit);
    COPY_LU_TELEM_FIELD(refactor_need_reason_spike_pool_warn);
    COPY_LU_TELEM_FIELD(refactor_need_reason_spike_work);
    COPY_LU_TELEM_FIELD(refactor_need_reason_spike_diag_quality);
    COPY_LU_TELEM_FIELD(update_fail_bad_input);
    COPY_LU_TELEM_FIELD(update_fail_max_updates);
    COPY_LU_TELEM_FIELD(update_fail_singular_update);
    COPY_LU_TELEM_FIELD(update_fail_update_pivot_too_small);
    COPY_LU_TELEM_FIELD(update_fail_spike_pool_full);
    COPY_LU_TELEM_FIELD(update_fail_dense_spike_reject);
    COPY_LU_TELEM_FIELD(update_fail_eta_alloc);

    COPY_LU_FIELD(sn_calls);
    COPY_LU_FIELD(sn_successes);
    COPY_LU_FIELD(num_updates);
    COPY_LU_FIELD(max_updates);
    COPY_LU_FIELD(last_failure_reason);
    COPY_LU_FIELD(last_refactor_trigger_reason);

    COPY_LU_TELEM_FIELD(perf_factorize_calls);
    COPY_LU_TELEM_FIELD(perf_last_basis_nnz);
    COPY_LU_TELEM_FIELD(perf_last_m);
    COPY_LU_TELEM_FIELD(perf_last_k);
    COPY_LU_TELEM_FIELD(perf_symbolic_calls);
    COPY_LU_TELEM_FIELD(perf_symbolic_cache_hits);
    COPY_LU_TELEM_FIELD(perf_symbolic_cache_misses);
    COPY_LU_TELEM_FIELD(perf_last_symbolic_ms);
    COPY_LU_TELEM_FIELD(perf_last_sparse_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_last_dense_ge_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_last_supernode_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_last_dense_factorize_ms);
    COPY_LU_TELEM_FIELD(perf_last_a_struct_build_ms);
    COPY_LU_TELEM_FIELD(perf_last_markowitz_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_last_identity_placement_ms);
    COPY_LU_TELEM_FIELD(perf_last_coo_to_csc_ms);
    COPY_LU_TELEM_FIELD(perf_total_symbolic_ms);
    COPY_LU_TELEM_FIELD(perf_total_sparse_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_total_dense_ge_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_total_supernode_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_total_dense_factorize_ms);
    COPY_LU_TELEM_FIELD(perf_total_a_struct_build_ms);
    COPY_LU_TELEM_FIELD(perf_total_markowitz_numeric_ms);
    COPY_LU_TELEM_FIELD(perf_total_identity_placement_ms);
    COPY_LU_TELEM_FIELD(perf_total_coo_to_csc_ms);
    COPY_LU_TELEM_FIELD(perf_update_apply_forward_calls);
    COPY_LU_TELEM_FIELD(perf_update_apply_backward_calls);
    COPY_LU_TELEM_FIELD(perf_compact_factor_calls);
    COPY_LU_TELEM_FIELD(perf_compact_solve_calls);
    COPY_LU_TELEM_FIELD(perf_total_update_apply_forward_ms);
    COPY_LU_TELEM_FIELD(perf_total_update_apply_backward_ms);
    COPY_LU_TELEM_FIELD(perf_total_compact_factor_ms);
    COPY_LU_TELEM_FIELD(perf_total_compact_solve_ms);
    COPY_LU_TELEM_FIELD(perf_sn_phase_samples);
    COPY_LU_TELEM_FIELD(perf_sn_panel_factor_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_swap_scatter_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_eliminate_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_entries_total);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size1_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size1_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size2_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size2_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size3_4_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size3_4_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size5_8_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size5_8_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size9p_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_size9p_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_reserved_present_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_reserved_present_entries);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_reserved_present_ms);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_reserved_alt_chosen_calls);
    COPY_LU_TELEM_FIELD(perf_sn_panel_pivot_search_reserved_alt_chosen_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_u_emit_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_u_emit_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_scan_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_scan_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_apply_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_apply_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_row_gather_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_col_indirection_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_outer_product_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_full_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_full_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols1_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols1_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols2_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols2_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols3_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols3_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols4_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols4_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows1_8_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows1_8_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows9_32_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows9_32_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows33_128_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows33_128_ms);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows129p_calls);
    COPY_LU_TELEM_FIELD(perf_sn_size1_update_cols5p_rows129p_ms);
    COPY_LU_TELEM_FIELD(perf_sn_u_emit_ms);
    COPY_LU_TELEM_FIELD(perf_sn_active_set_ms);
    COPY_LU_TELEM_FIELD(perf_sn_pack_blocks_ms);
    COPY_LU_TELEM_FIELD(perf_sn_full_update_ms);
    COPY_LU_TELEM_FIELD(perf_sn_compact_update_ms);
    COPY_LU_TELEM_FIELD(perf_sn_active_row_scan_entries);
    COPY_LU_TELEM_FIELD(perf_sn_active_col_scan_entries);
    COPY_LU_TELEM_FIELD(perf_sn_trailing_rows_total);
    COPY_LU_TELEM_FIELD(perf_sn_trailing_cols_total);
    COPY_LU_TELEM_FIELD(perf_sn_active_rows_total);
    COPY_LU_TELEM_FIELD(perf_sn_active_cols_total);
    COPY_LU_TELEM_FIELD(perf_sn_pack_l_entries_total);
    COPY_LU_TELEM_FIELD(perf_sn_pack_u_entries_total);
    COPY_LU_TELEM_FIELD(perf_sn_dense_triplets_total);
    COPY_LU_TELEM_FIELD(perf_sn_compact_triplets_total);
    COPY_LU_TELEM_FIELD(perf_sn_full_update_calls);
    COPY_LU_TELEM_FIELD(perf_sn_compact_update_calls);
    COPY_LU_TELEM_FIELD(perf_sn_skipped_update_calls);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols1_calls);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols1_rows_total);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols1_ms);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols2_calls);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols2_rows_total);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols2_ms);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols3_calls);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols3_rows_total);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols3_ms);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols4_calls);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols4_rows_total);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols4_ms);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols5p_calls);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols5p_rows_total);
    COPY_LU_TELEM_FIELD(perf_sn_compact_cols5p_ms);
}
#undef COPY_LU_FIELD
#undef COPY_LU_TELEM_FIELD

void lp_telemetry_lu_record_dense_factorize_ms(LUFactorization *lu,
                                               double elapsed_ms) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_last_dense_factorize_ms = elapsed_ms;
    lu->telemetry.perf_total_dense_factorize_ms += elapsed_ms;
}

void lp_telemetry_lu_record_dense_factorize_timed(LUFactorization *lu,
                                                  double start_ms) {
    lp_telemetry_lu_record_dense_factorize_ms(lu,
                                              lp_telemetry_timer_elapsed_ms(start_ms));
}

void lp_telemetry_lu_record_update_apply_forward_ms(LUFactorization *lu,
                                                    double elapsed_ms) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_update_apply_forward_calls++;
    lu->telemetry.perf_total_update_apply_forward_ms += elapsed_ms;
}

void lp_telemetry_lu_record_update_apply_backward_ms(LUFactorization *lu,
                                                     double elapsed_ms) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_update_apply_backward_calls++;
    lu->telemetry.perf_total_update_apply_backward_ms += elapsed_ms;
}

void lp_telemetry_lu_record_compact_factor_ms(LUFactorization *lu,
                                              double elapsed_ms) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_compact_factor_calls++;
    lu->telemetry.perf_total_compact_factor_ms += elapsed_ms;
}

void lp_telemetry_lu_record_compact_solve_ms(LUFactorization *lu,
                                             double elapsed_ms) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_compact_solve_calls++;
    lu->telemetry.perf_total_compact_solve_ms += elapsed_ms;
}

void lp_telemetry_lu_add_mkz_scan_work(LUFactorization *lu,
                                       uint64_t primary_scan_entries,
                                       uint64_t rescue_scan_entries,
                                       uint64_t reserved_scan_entries,
                                       uint64_t update_existing_entries,
                                       uint64_t update_fill_candidates,
                                       uint64_t hint_fallback_scans,
                                       uint64_t hint_fallback_scan_entries) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_primary_scan_entries += primary_scan_entries;
    lu->telemetry.mkz_rescue_scan_entries += rescue_scan_entries;
    lu->telemetry.mkz_reserved_scan_entries += reserved_scan_entries;
    lu->telemetry.mkz_update_existing_entries += update_existing_entries;
    lu->telemetry.mkz_update_fill_candidates += update_fill_candidates;
    lu->telemetry.mkz_hint_fallback_scans += hint_fallback_scans;
    lu->telemetry.mkz_hint_fallback_scan_entries += hint_fallback_scan_entries;
}

void lp_telemetry_lu_add_mkz_colmax_work(LUFactorization *lu,
                                         uint64_t affected_columns,
                                         uint64_t affected_columns_max,
                                         uint64_t col_max_scan_entries) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_affected_columns_total += affected_columns;
    if (affected_columns_max > lu->telemetry.mkz_affected_columns_max) {
        lu->telemetry.mkz_affected_columns_max = affected_columns_max;
    }
    lu->telemetry.mkz_col_max_scan_entries += col_max_scan_entries;
}

void lp_telemetry_lu_add_supernode_work(LUFactorization *lu,
                                        uint64_t phase_samples,
                                        double panel_factor_ms,
                                        double panel_pivot_search_ms,
                                        double panel_swap_scatter_ms,
                                        double panel_eliminate_ms,
                                        uint64_t panel_pivot_search_calls,
                                        uint64_t panel_pivot_search_entries_total,
                                        uint64_t panel_pivot_search_size1_calls,
                                        double panel_pivot_search_size1_ms,
                                        uint64_t panel_pivot_search_size2_calls,
                                        double panel_pivot_search_size2_ms,
                                        uint64_t panel_pivot_search_size3_4_calls,
                                        double panel_pivot_search_size3_4_ms,
                                        uint64_t panel_pivot_search_size5_8_calls,
                                        double panel_pivot_search_size5_8_ms,
                                        uint64_t panel_pivot_search_size9p_calls,
                                        double panel_pivot_search_size9p_ms,
                                        uint64_t panel_pivot_search_reserved_present_calls,
                                        uint64_t panel_pivot_search_reserved_present_entries,
                                        double panel_pivot_search_reserved_present_ms,
                                        uint64_t panel_pivot_search_reserved_alt_chosen_calls,
                                        double panel_pivot_search_reserved_alt_chosen_ms,
                                        uint64_t size1_u_emit_calls,
                                        double size1_u_emit_ms,
                                        uint64_t size1_update_scan_calls,
                                        double size1_update_scan_ms,
                                        uint64_t size1_update_apply_calls,
                                        double size1_update_apply_ms,
                                        double size1_update_row_gather_ms,
                                        double size1_update_col_indirection_ms,
                                        double size1_update_outer_product_ms,
                                        uint64_t size1_update_full_calls,
                                        double size1_update_full_ms,
                                        uint64_t size1_update_cols1_calls,
                                        double size1_update_cols1_ms,
                                        uint64_t size1_update_cols2_calls,
                                        double size1_update_cols2_ms,
                                        uint64_t size1_update_cols3_calls,
                                        double size1_update_cols3_ms,
                                        uint64_t size1_update_cols4_calls,
                                        double size1_update_cols4_ms,
                                        uint64_t size1_update_cols5p_calls,
                                        double size1_update_cols5p_ms,
                                        uint64_t size1_update_cols5p_rows1_8_calls,
                                        double size1_update_cols5p_rows1_8_ms,
                                        uint64_t size1_update_cols5p_rows9_32_calls,
                                        double size1_update_cols5p_rows9_32_ms,
                                        uint64_t size1_update_cols5p_rows33_128_calls,
                                        double size1_update_cols5p_rows33_128_ms,
                                        uint64_t size1_update_cols5p_rows129p_calls,
                                        double size1_update_cols5p_rows129p_ms,
                                        double u_emit_ms,
                                        double active_set_ms,
                                        double pack_blocks_ms,
                                        double full_update_ms,
                                        double compact_update_ms,
                                        uint64_t active_row_scan_entries,
                                        uint64_t active_col_scan_entries,
                                        uint64_t trailing_rows_total,
                                        uint64_t trailing_cols_total,
                                        uint64_t active_rows_total,
                                        uint64_t active_cols_total,
                                        uint64_t pack_l_entries_total,
                                        uint64_t pack_u_entries_total,
                                        uint64_t dense_triplets_total,
                                        uint64_t compact_triplets_total,
                                        uint64_t full_update_calls,
                                        uint64_t compact_update_calls,
                                        uint64_t skipped_update_calls,
                                        uint64_t compact_cols1_calls,
                                        uint64_t compact_cols1_rows_total,
                                        double compact_cols1_ms,
                                        uint64_t compact_cols2_calls,
                                        uint64_t compact_cols2_rows_total,
                                        double compact_cols2_ms,
                                        uint64_t compact_cols3_calls,
                                        uint64_t compact_cols3_rows_total,
                                        double compact_cols3_ms,
                                        uint64_t compact_cols4_calls,
                                        uint64_t compact_cols4_rows_total,
                                        double compact_cols4_ms,
                                        uint64_t compact_cols5p_calls,
                                        uint64_t compact_cols5p_rows_total,
                                        double compact_cols5p_ms) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_sn_phase_samples += phase_samples;
    lu->telemetry.perf_sn_panel_factor_ms += panel_factor_ms;
    lu->telemetry.perf_sn_panel_pivot_search_ms += panel_pivot_search_ms;
    lu->telemetry.perf_sn_panel_swap_scatter_ms += panel_swap_scatter_ms;
    lu->telemetry.perf_sn_panel_eliminate_ms += panel_eliminate_ms;
    lu->telemetry.perf_sn_panel_pivot_search_calls += panel_pivot_search_calls;
    lu->telemetry.perf_sn_panel_pivot_search_entries_total +=
        panel_pivot_search_entries_total;
    lu->telemetry.perf_sn_panel_pivot_search_size1_calls +=
        panel_pivot_search_size1_calls;
    lu->telemetry.perf_sn_panel_pivot_search_size1_ms +=
        panel_pivot_search_size1_ms;
    lu->telemetry.perf_sn_panel_pivot_search_size2_calls +=
        panel_pivot_search_size2_calls;
    lu->telemetry.perf_sn_panel_pivot_search_size2_ms +=
        panel_pivot_search_size2_ms;
    lu->telemetry.perf_sn_panel_pivot_search_size3_4_calls +=
        panel_pivot_search_size3_4_calls;
    lu->telemetry.perf_sn_panel_pivot_search_size3_4_ms +=
        panel_pivot_search_size3_4_ms;
    lu->telemetry.perf_sn_panel_pivot_search_size5_8_calls +=
        panel_pivot_search_size5_8_calls;
    lu->telemetry.perf_sn_panel_pivot_search_size5_8_ms +=
        panel_pivot_search_size5_8_ms;
    lu->telemetry.perf_sn_panel_pivot_search_size9p_calls +=
        panel_pivot_search_size9p_calls;
    lu->telemetry.perf_sn_panel_pivot_search_size9p_ms +=
        panel_pivot_search_size9p_ms;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_present_calls +=
        panel_pivot_search_reserved_present_calls;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_present_entries +=
        panel_pivot_search_reserved_present_entries;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_present_ms +=
        panel_pivot_search_reserved_present_ms;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_calls +=
        panel_pivot_search_reserved_alt_chosen_calls;
    lu->telemetry.perf_sn_panel_pivot_search_reserved_alt_chosen_ms +=
        panel_pivot_search_reserved_alt_chosen_ms;
    lu->telemetry.perf_sn_size1_u_emit_calls += size1_u_emit_calls;
    lu->telemetry.perf_sn_size1_u_emit_ms += size1_u_emit_ms;
    lu->telemetry.perf_sn_size1_update_scan_calls += size1_update_scan_calls;
    lu->telemetry.perf_sn_size1_update_scan_ms += size1_update_scan_ms;
    lu->telemetry.perf_sn_size1_update_apply_calls += size1_update_apply_calls;
    lu->telemetry.perf_sn_size1_update_apply_ms += size1_update_apply_ms;
    lu->telemetry.perf_sn_size1_update_row_gather_ms +=
        size1_update_row_gather_ms;
    lu->telemetry.perf_sn_size1_update_col_indirection_ms +=
        size1_update_col_indirection_ms;
    lu->telemetry.perf_sn_size1_update_outer_product_ms +=
        size1_update_outer_product_ms;
    lu->telemetry.perf_sn_size1_update_full_calls +=
        size1_update_full_calls;
    lu->telemetry.perf_sn_size1_update_full_ms += size1_update_full_ms;
    lu->telemetry.perf_sn_size1_update_cols1_calls +=
        size1_update_cols1_calls;
    lu->telemetry.perf_sn_size1_update_cols1_ms += size1_update_cols1_ms;
    lu->telemetry.perf_sn_size1_update_cols2_calls +=
        size1_update_cols2_calls;
    lu->telemetry.perf_sn_size1_update_cols2_ms += size1_update_cols2_ms;
    lu->telemetry.perf_sn_size1_update_cols3_calls +=
        size1_update_cols3_calls;
    lu->telemetry.perf_sn_size1_update_cols3_ms += size1_update_cols3_ms;
    lu->telemetry.perf_sn_size1_update_cols4_calls +=
        size1_update_cols4_calls;
    lu->telemetry.perf_sn_size1_update_cols4_ms += size1_update_cols4_ms;
    lu->telemetry.perf_sn_size1_update_cols5p_calls +=
        size1_update_cols5p_calls;
    lu->telemetry.perf_sn_size1_update_cols5p_ms += size1_update_cols5p_ms;
    lu->telemetry.perf_sn_size1_update_cols5p_rows1_8_calls +=
        size1_update_cols5p_rows1_8_calls;
    lu->telemetry.perf_sn_size1_update_cols5p_rows1_8_ms +=
        size1_update_cols5p_rows1_8_ms;
    lu->telemetry.perf_sn_size1_update_cols5p_rows9_32_calls +=
        size1_update_cols5p_rows9_32_calls;
    lu->telemetry.perf_sn_size1_update_cols5p_rows9_32_ms +=
        size1_update_cols5p_rows9_32_ms;
    lu->telemetry.perf_sn_size1_update_cols5p_rows33_128_calls +=
        size1_update_cols5p_rows33_128_calls;
    lu->telemetry.perf_sn_size1_update_cols5p_rows33_128_ms +=
        size1_update_cols5p_rows33_128_ms;
    lu->telemetry.perf_sn_size1_update_cols5p_rows129p_calls +=
        size1_update_cols5p_rows129p_calls;
    lu->telemetry.perf_sn_size1_update_cols5p_rows129p_ms +=
        size1_update_cols5p_rows129p_ms;
    lu->telemetry.perf_sn_u_emit_ms += u_emit_ms;
    lu->telemetry.perf_sn_active_set_ms += active_set_ms;
    lu->telemetry.perf_sn_pack_blocks_ms += pack_blocks_ms;
    lu->telemetry.perf_sn_full_update_ms += full_update_ms;
    lu->telemetry.perf_sn_compact_update_ms += compact_update_ms;
    lu->telemetry.perf_sn_active_row_scan_entries += active_row_scan_entries;
    lu->telemetry.perf_sn_active_col_scan_entries += active_col_scan_entries;
    lu->telemetry.perf_sn_trailing_rows_total += trailing_rows_total;
    lu->telemetry.perf_sn_trailing_cols_total += trailing_cols_total;
    lu->telemetry.perf_sn_active_rows_total += active_rows_total;
    lu->telemetry.perf_sn_active_cols_total += active_cols_total;
    lu->telemetry.perf_sn_pack_l_entries_total += pack_l_entries_total;
    lu->telemetry.perf_sn_pack_u_entries_total += pack_u_entries_total;
    lu->telemetry.perf_sn_dense_triplets_total += dense_triplets_total;
    lu->telemetry.perf_sn_compact_triplets_total += compact_triplets_total;
    lu->telemetry.perf_sn_full_update_calls += full_update_calls;
    lu->telemetry.perf_sn_compact_update_calls += compact_update_calls;
    lu->telemetry.perf_sn_skipped_update_calls += skipped_update_calls;
    lu->telemetry.perf_sn_compact_cols1_calls += compact_cols1_calls;
    lu->telemetry.perf_sn_compact_cols1_rows_total += compact_cols1_rows_total;
    lu->telemetry.perf_sn_compact_cols1_ms += compact_cols1_ms;
    lu->telemetry.perf_sn_compact_cols2_calls += compact_cols2_calls;
    lu->telemetry.perf_sn_compact_cols2_rows_total += compact_cols2_rows_total;
    lu->telemetry.perf_sn_compact_cols2_ms += compact_cols2_ms;
    lu->telemetry.perf_sn_compact_cols3_calls += compact_cols3_calls;
    lu->telemetry.perf_sn_compact_cols3_rows_total += compact_cols3_rows_total;
    lu->telemetry.perf_sn_compact_cols3_ms += compact_cols3_ms;
    lu->telemetry.perf_sn_compact_cols4_calls += compact_cols4_calls;
    lu->telemetry.perf_sn_compact_cols4_rows_total += compact_cols4_rows_total;
    lu->telemetry.perf_sn_compact_cols4_ms += compact_cols4_ms;
    lu->telemetry.perf_sn_compact_cols5p_calls += compact_cols5p_calls;
    lu->telemetry.perf_sn_compact_cols5p_rows_total += compact_cols5p_rows_total;
    lu->telemetry.perf_sn_compact_cols5p_ms += compact_cols5p_ms;
}

void lp_telemetry_lu_record_symbolic_cache_hit(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_symbolic_cache_hits++;
}

void lp_telemetry_lu_record_symbolic_cache_miss(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_symbolic_cache_misses++;
}

void lp_telemetry_lu_record_symbolic_call(LUFactorization *lu,
                                          double elapsed_ms) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.perf_symbolic_calls++;
    lu->telemetry.perf_last_symbolic_ms = elapsed_ms;
    lu->telemetry.perf_total_symbolic_ms += elapsed_ms;
}

void lp_telemetry_lu_record_symbolic_call_timed(LUFactorization *lu,
                                                double start_ms) {
    lp_telemetry_lu_record_symbolic_call(lu,
                                         lp_telemetry_timer_elapsed_ms(start_ms));
}
