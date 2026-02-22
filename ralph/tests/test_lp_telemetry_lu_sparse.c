/*
 * Tests for sparse-LU telemetry helpers (Markowitz + fallback telemetry).
 */

#include <stdio.h>
#include <stdlib.h>
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

static SparseMatrix *dense_to_csc(const double *A, int m, int n) {
    int nnz = 0;
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < m; i++) {
            if (fabs(A[i * n + j]) > 1e-15) nnz++;
        }
    }

    SparseMatrix *sp = (SparseMatrix *)calloc(1, sizeof(SparseMatrix));
    sp->nrows = m;
    sp->ncols = n;
    sp->nnz = nnz;
    sp->colptr = (int *)calloc(n + 1, sizeof(int));
    sp->rowidx = (int *)calloc(nnz, sizeof(int));
    sp->values = (double *)calloc(nnz, sizeof(double));

    int pos = 0;
    for (int j = 0; j < n; j++) {
        sp->colptr[j] = pos;
        for (int i = 0; i < m; i++) {
            double v = A[i * n + j];
            if (fabs(v) > 1e-15) {
                sp->rowidx[pos] = i;
                sp->values[pos] = v;
                pos++;
            }
        }
    }
    sp->colptr[n] = pos;
    return sp;
}

static void free_csc(SparseMatrix *sp) {
    if (!sp) return;
    free(sp->colptr);
    free(sp->rowidx);
    free(sp->values);
    free(sp);
}

static void test_markowitz_failure_reason_counters(void) {
    printf("  telemetry/lu_sparse: Markowitz failure counters...\n");

    const int m = 60;
    const int k = 40;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));

    /* Structural block with duplicate columns -> singular but symbolically matchable. */
    for (int i = 0; i < k; i++) {
        A[i * m + i] = 5.0;
        A[((i + 1) % k) * m + i] = 0.25;
    }
    for (int r = 0; r < k; r++) {
        A[r * m + (k - 1)] = A[r * m + (k - 2)];
    }

    /* Identity columns (k..m-1). */
    for (int t = 0; t < m - k; t++) {
        int row = k + t;
        A[row * m + (k + t)] = 1.0;
    }

    SparseMatrix *B = dense_to_csc(A, m, m);
    LUFactorization *lu = lu_create(m);
    ASSERT(lu != NULL, "failure counters: lu_create");
    lu->mkz_enabled = 1;
    lu->sn_enabled = 0;

    int rc = lu_factorize(lu, B);
    ASSERT(rc != 0, "failure counters: singular matrix should fail factorization");

    ASSERT(lu->mkz_calls > 0, "failure counters: Markowitz attempted");
    ASSERT(lu->mkz_dense_fallbacks > 0, "failure counters: Markowitz fallback recorded");
    ASSERT(lu->mkz_fail_singular > 0, "failure counters: singular reason counted");
    ASSERT_INT_EQ(lu->mkz_fail_workspace, 0, "failure counters: no workspace failure");
    ASSERT_INT_EQ(lu->mkz_fail_capacity, 0, "failure counters: no capacity failure");

    lu_free(lu);
    free_csc(B);
    free(A);
}

static void test_sparse_fallback_reason_and_stage_telemetry(void) {
    printf("  telemetry/lu_sparse: fallback reasons + stage timers...\n");

    /* Case A: small matrix forces sparse->dense fallback with explicit reason. */
    {
        const int m = 10;
        double *A = (double *)calloc((size_t)m * m, sizeof(double));
        for (int i = 0; i < m; i++) {
            A[i * m + i] = 4.0 + 0.1 * i;
            if (i + 1 < m) A[(i + 1) * m + i] = 0.2;
        }
        SparseMatrix *B = dense_to_csc(A, m, m);
        LUFactorization *lu = lu_create(m);

        int rc = lu_factorize(lu, B);
        ASSERT_INT_EQ(rc, 0, "telemetry small: factorize");
        ASSERT_INT_EQ(lu->used_dense_fallback_last, 1, "telemetry small: dense fallback used");
        ASSERT_INT_EQ(lu->sparse_fallback_last_reason, LU_SPARSE_FALLBACK_SMALL_MATRIX,
                      "telemetry small: fallback reason=small_matrix");
        ASSERT(lu->sparse_fallback_reason_small_matrix > 0,
               "telemetry small: small-matrix counter incremented");
        ASSERT_INT_EQ(lu->sparse_fallback_reason_symbolic, 0,
                      "telemetry small: no symbolic fallback count");
        ASSERT_INT_EQ(lu->sparse_fallback_reason_numeric, 0,
                      "telemetry small: no numeric fallback count");
        ASSERT_INT_EQ(lu->perf_symbolic_calls, 0,
                      "telemetry small: symbolic path not called");
        ASSERT(lu->perf_last_dense_factorize_ms >= 0.0,
               "telemetry small: dense factorize timer captured");

        lu_free(lu);
        free_csc(B);
        free(A);
    }

    /* Case B: sparse path succeeds and records symbolic/numeric stage timing. */
    {
        const int m = 60;
        const int k = 40;
        double *A = (double *)calloc((size_t)m * m, sizeof(double));

        for (int j = 0; j < k; j++) {
            A[j * m + j] = 8.0 + 0.01 * j;
            A[((j + 1) % k) * m + j] = 0.1;
        }
        for (int t = 0; t < m - k; t++) {
            int row = k + t;
            A[row * m + (k + t)] = 1.0;
        }

        SparseMatrix *B = dense_to_csc(A, m, m);
        LUFactorization *lu = lu_create(m);
        lu->mkz_enabled = 1;
        lu->sn_enabled = 0;

        int rc = lu_factorize(lu, B);
        ASSERT_INT_EQ(rc, 0, "telemetry sparse: factorize");
        ASSERT_INT_EQ(lu->used_dense_fallback_last, 0, "telemetry sparse: no dense fallback");
        ASSERT_INT_EQ(lu->sparse_fallback_last_reason, LU_SPARSE_FALLBACK_NONE,
                      "telemetry sparse: fallback reason=none");
        ASSERT(lu->perf_symbolic_calls > 0, "telemetry sparse: symbolic called");
        ASSERT(lu->perf_symbolic_cache_misses > 0, "telemetry sparse: symbolic miss recorded");
        ASSERT(lu->perf_last_symbolic_ms >= 0.0, "telemetry sparse: symbolic timer captured");
        ASSERT(lu->perf_last_sparse_numeric_ms >= 0.0, "telemetry sparse: sparse numeric timer captured");
        ASSERT(lu->perf_total_sparse_numeric_ms >= lu->perf_last_sparse_numeric_ms,
               "telemetry sparse: sparse numeric total accumulates");

        lu_free(lu);
        free_csc(B);
        free(A);
    }

    /* Case C: full-structural basis (k=m) should stay on sparse path and avoid
     * symbolic fallback despite having no identity columns. */
    {
        const int m = 60;
        double *A = (double *)calloc((size_t)m * m, sizeof(double));

        for (int j = 0; j < m; j++) {
            A[j * m + j] = 9.0 + 0.02 * j;
            A[((j + 1) % m) * m + j] = 0.3;
            A[((j + 7) % m) * m + j] = -0.2;
        }

        SparseMatrix *B = dense_to_csc(A, m, m);
        LUFactorization *lu = lu_create(m);
        lu->mkz_enabled = 1;
        lu->sn_enabled = 0;

        int rc = lu_factorize(lu, B);
        ASSERT_INT_EQ(rc, 0, "telemetry full-structural: factorize");
        ASSERT_INT_EQ(lu->used_dense_fallback_last, 0,
                      "telemetry full-structural: no dense fallback");
        ASSERT_INT_EQ(lu->sparse_fallback_last_reason, LU_SPARSE_FALLBACK_NONE,
                      "telemetry full-structural: fallback reason=none");
        ASSERT_INT_EQ(lu->sym_num_identity, 0,
                      "telemetry full-structural: no identity columns");
        ASSERT_INT_EQ(lu->sym_k, m, "telemetry full-structural: k=m");
        ASSERT(lu->perf_symbolic_calls > 0,
               "telemetry full-structural: symbolic called");

        lu_free(lu);
        free_csc(B);
        free(A);
    }
}

int main(void) {
    printf("=== LP Telemetry Sparse-LU Tests ===\n");

    test_markowitz_failure_reason_counters();
    test_sparse_fallback_reason_and_stage_telemetry();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
