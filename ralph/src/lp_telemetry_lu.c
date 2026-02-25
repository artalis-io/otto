/*
 * Ralph - LU Telemetry Helpers
 *
 * LU reset/prepare/snapshot and dense/symbolic timing counters.
 */

#include <string.h>
#include "lp.h"

static int lu_telemetry_enabled(const LUFactorization *lu) {
    return lu && lu->telemetry_enabled;
}

void lp_telemetry_reset_lu(LUFactorization *lu) {
    if (!lu) return;
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
    lu->telemetry.sparse_dense_fallbacks = 0;
    lu->telemetry.used_dense_fallback_last = 0;
    lu->telemetry.sparse_fallback_last_reason = LU_SPARSE_FALLBACK_NONE;
    lu->telemetry.sparse_fallback_reason_small_matrix = 0;
    lu->telemetry.sparse_fallback_reason_symbolic = 0;
    lu->telemetry.sparse_fallback_reason_numeric = 0;
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

    COPY_LU_TELEM_FIELD(sparse_dense_fallbacks);
    COPY_LU_TELEM_FIELD(used_dense_fallback_last);
    COPY_LU_TELEM_FIELD(sparse_fallback_last_reason);
    COPY_LU_TELEM_FIELD(sparse_fallback_reason_small_matrix);
    COPY_LU_TELEM_FIELD(sparse_fallback_reason_symbolic);
    COPY_LU_TELEM_FIELD(sparse_fallback_reason_numeric);
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

    COPY_LU_FIELD(sn_calls);
    COPY_LU_FIELD(sn_successes);
    COPY_LU_FIELD(num_updates);
    COPY_LU_FIELD(max_updates);
    COPY_LU_FIELD(last_failure_reason);

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
