/*
 * Ralph - Sparse LU Telemetry Helpers
 *
 * Markowitz/sparse fallback counters and sparse numeric stage telemetry.
 */

#include "lp.h"

static int lu_telemetry_enabled(const LUFactorization *lu) {
    return lu && lu->telemetry_enabled;
}

void lp_telemetry_lu_mark_identity_sep_failure(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
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
    if (!lu_telemetry_enabled(lu)) return;
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
    if (!lu_telemetry_enabled(lu)) return;
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
    if (!lu_telemetry_enabled(lu)) return;
    lu->used_dense_fallback_last = 1;
    lu->sparse_dense_fallbacks++;
}

void lp_telemetry_lu_clear_mkz_last_failure(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->mkz_last_failure = MKZ_FAIL_NONE;
}

void lp_telemetry_lu_mark_mkz_attempt(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->mkz_calls++;
}

void lp_telemetry_lu_mark_mkz_success(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->mkz_successes++;
    lu->mkz_last_failure = MKZ_FAIL_NONE;
}

void lp_telemetry_lu_mark_mkz_failure(LUFactorization *lu,
                                      int rc) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->mkz_failures++;
    lu->mkz_last_failure = rc;
    lu->mkz_dense_fallbacks++;
}

void lp_telemetry_lu_mark_mkz_failure_reason(LUFactorization *lu,
                                             int rc) {
    if (!lu_telemetry_enabled(lu)) return;
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
