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
    lu->telemetry.identity_sep_failures++;
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
    lu->telemetry.perf_last_k = last_k;
    lu->telemetry.perf_last_a_struct_build_ms = a_struct_build_ms;
    lu->telemetry.perf_last_markowitz_numeric_ms = markowitz_numeric_ms;
    lu->telemetry.perf_last_supernode_numeric_ms = supernode_numeric_ms;
    lu->telemetry.perf_last_dense_ge_numeric_ms = dense_ge_numeric_ms;
    lu->telemetry.perf_last_sparse_numeric_ms =
        markowitz_numeric_ms + supernode_numeric_ms + dense_ge_numeric_ms;
    lu->telemetry.perf_last_identity_placement_ms = identity_placement_ms;
    lu->telemetry.perf_last_coo_to_csc_ms = coo_to_csc_ms;
    lu->telemetry.perf_total_a_struct_build_ms += a_struct_build_ms;
    lu->telemetry.perf_total_markowitz_numeric_ms += markowitz_numeric_ms;
    lu->telemetry.perf_total_supernode_numeric_ms += supernode_numeric_ms;
    lu->telemetry.perf_total_dense_ge_numeric_ms += dense_ge_numeric_ms;
    lu->telemetry.perf_total_sparse_numeric_ms += lu->telemetry.perf_last_sparse_numeric_ms;
    lu->telemetry.perf_total_identity_placement_ms += identity_placement_ms;
    lu->telemetry.perf_total_coo_to_csc_ms += coo_to_csc_ms;
}

void lp_telemetry_lu_set_sparse_fallback_reason(LUFactorization *lu,
                                                int reason) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.sparse_fallback_last_reason = reason;
    switch ((LUSparseFallbackReason)reason) {
        case LU_SPARSE_FALLBACK_SMALL_MATRIX:
            lu->telemetry.sparse_fallback_reason_small_matrix++;
            break;
        case LU_SPARSE_FALLBACK_SYMBOLIC:
            lu->telemetry.sparse_fallback_reason_symbolic++;
            break;
        case LU_SPARSE_FALLBACK_NUMERIC:
            lu->telemetry.sparse_fallback_reason_numeric++;
            break;
        case LU_SPARSE_FALLBACK_NONE:
        default:
            break;
    }
}

void lp_telemetry_lu_mark_symbolic_failure(LUFactorization *lu,
                                           int reason) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.symbolic_failures++;
    if (reason == LU_SYMBOLIC_FAIL_WORKSPACE) {
        lu->telemetry.symbolic_fail_workspace++;
    } else if (reason == LU_SYMBOLIC_FAIL_UNMATCHED_NO_RESERVED) {
        lu->telemetry.symbolic_fail_unmatched_no_reserved++;
    } else if (reason == LU_SYMBOLIC_FAIL_INCONSISTENT_IDENTITY) {
        lu->telemetry.symbolic_fail_inconsistent_identity++;
    }
}

void lp_telemetry_lu_mark_symbolic_full_retry_attempt(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.symbolic_full_retry_attempts++;
}

void lp_telemetry_lu_mark_symbolic_full_retry_success(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.symbolic_full_retry_successes++;
}

void lp_telemetry_lu_mark_symbolic_full_retry_numeric_failure(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.symbolic_full_retry_numeric_failures++;
}

void lp_telemetry_lu_mark_sparse_success(LUFactorization *lu) {
    lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_NONE);
}

void lp_telemetry_lu_mark_dense_fallback(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.used_dense_fallback_last = 1;
    lu->telemetry.sparse_dense_fallbacks++;
}

void lp_telemetry_lu_clear_mkz_last_failure(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_last_failure = MKZ_FAIL_NONE;
}

void lp_telemetry_lu_mark_mkz_attempt(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_calls++;
}

void lp_telemetry_lu_mark_mkz_success(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_successes++;
    lu->telemetry.mkz_last_failure = MKZ_FAIL_NONE;
}

void lp_telemetry_lu_mark_mkz_failure(LUFactorization *lu,
                                      int rc) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_failures++;
    lu->telemetry.mkz_last_failure = rc;
    lu->telemetry.mkz_dense_fallbacks++;
}

void lp_telemetry_lu_mark_mkz_failure_reason(LUFactorization *lu,
                                             int rc) {
    if (!lu_telemetry_enabled(lu)) return;
    if (rc == MKZ_FAIL_WORKSPACE) {
        lu->telemetry.mkz_fail_workspace++;
    } else if (rc == MKZ_FAIL_POOL) {
        lu->telemetry.mkz_fail_pool++;
    } else if (rc == MKZ_FAIL_SINGULAR) {
        lu->telemetry.mkz_fail_singular++;
    } else if (rc == MKZ_FAIL_CAPACITY) {
        lu->telemetry.mkz_fail_capacity++;
    }
}

void lp_telemetry_lu_mark_mkz_singular_retry_attempt(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_singular_retry_attempts++;
}

void lp_telemetry_lu_mark_mkz_singular_retry_success(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_singular_retry_successes++;
}

void lp_telemetry_lu_mark_mkz_singular_retry_failure(LUFactorization *lu) {
    if (!lu_telemetry_enabled(lu)) return;
    lu->telemetry.mkz_singular_retry_failures++;
}
