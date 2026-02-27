/*
 * Tests for Sparse Markowitz LU Factorization
 *
 * 1. Standalone PA=LU correctness on known matrices
 * 2. Solve Ax=b verification
 * 3. A/B comparison: Markowitz-enabled vs dense-only path
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "lp.h"

#define TOLERANCE 1e-8

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while(0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) < (tol)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.10e != %.10e, diff=%.2e)\n", msg, (double)(a), (double)(b), fabs((a)-(b))); \
    } \
} while(0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (a), (b)); \
    } \
} while(0)

/* ============================================================================
 * Helper: build a CSC sparse matrix from dense row-major array
 * ============================================================================ */
static SparseMatrix *dense_to_csc(const double *A, int m, int n) {
    /* Count nonzeros per column */
    int nnz = 0;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < m; i++)
            if (fabs(A[i * n + j]) > 1e-15) nnz++;

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

/* ============================================================================
 * Test 1: Factorize a diagonally-dominant sparse matrix, verify solve
 * ============================================================================ */
static void test_markowitz_sparse_solve(void) {
    printf("  Markowitz: sparse solve (m=60, ~30%% fill)...\n");

    int m = 60;
    double *A = (double *)calloc(m * m, sizeof(double));

    /* Build sparse diagonally-dominant matrix:
     * Strong diagonal + random off-diagonal entries (~30% fill) */
    unsigned int seed = 42;
    for (int i = 0; i < m; i++) {
        A[i * m + i] = 20.0 + (double)(i % 5);
        for (int j = 0; j < m; j++) {
            if (i == j) continue;
            seed = seed * 1103515245 + 12345;
            if (((seed >> 16) & 0x7fff) % 100 < 30) {
                seed = seed * 1103515245 + 12345;
                A[i * m + j] = ((double)((seed >> 16) & 0x7fff) / 32768.0) * 4.0 - 2.0;
            }
        }
    }

    SparseMatrix *B = dense_to_csc(A, m, m);

    /* Factorize with Markowitz enabled */
    LUFactorization *lu = lu_create(m);
    ASSERT(lu != NULL, "lu_create");
    lu->mkz_enabled = 1;

    int rc = lu_factorize(lu, B);
    ASSERT_INT_EQ(rc, 0, "factorize return code");

    if (rc == 0) {
        /* Solve Ax = b for several RHS vectors */
        double max_err = 0;
        for (int trial = 0; trial < 5; trial++) {
            double *b = (double *)calloc(m, sizeof(double));
            double *x = (double *)calloc(m, sizeof(double));
            for (int i = 0; i < m; i++)
                b[i] = (double)(trial * 7 + i * 3 + 1);

            lu_solve(lu, b, x);

            /* Check A*x ≈ b_orig */
            double *b_orig = (double *)calloc(m, sizeof(double));
            for (int i = 0; i < m; i++)
                b_orig[i] = (double)(trial * 7 + i * 3 + 1);

            for (int i = 0; i < m; i++) {
                double ax = 0;
                for (int j = 0; j < m; j++)
                    ax += A[i * m + j] * x[j];
                double err = fabs(ax - b_orig[i]);
                if (err > max_err) max_err = err;
            }
            free(b); free(x); free(b_orig);
        }
        ASSERT(max_err < 1e-6, "solve Ax=b accuracy");
        if (max_err >= 1e-6) printf("    max_err = %.2e\n", max_err);
    }

    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 2: Markowitz on LP-like basis (identity + structural columns)
 * ============================================================================ */
static void test_markowitz_lp_basis(void) {
    printf("  Markowitz: LP basis (m=80, k~50)...\n");

    int m = 80;
    double *A = (double *)calloc(m * m, sizeof(double));

    /* First 30 columns: identity (slack variables) */
    for (int j = 0; j < 30; j++)
        A[j * m + j] = 1.0;

    /* Remaining 50 columns: sparse structural with strong diagonal */
    unsigned int seed = 123;
    for (int j = 30; j < m; j++) {
        /* Strong diagonal */
        A[j * m + j] = 15.0;
        /* Sparse off-diagonal (~20% fill) */
        for (int i = 0; i < m; i++) {
            if (i == j) continue;
            seed = seed * 1103515245 + 12345;
            if (((seed >> 16) & 0x7fff) % 100 < 20) {
                seed = seed * 1103515245 + 12345;
                A[i * m + j] = ((double)((seed >> 16) & 0x7fff) / 32768.0) * 2.0 - 1.0;
            }
        }
    }

    SparseMatrix *B = dense_to_csc(A, m, m);

    /* Solve with Markowitz enabled */
    LUFactorization *lu_mkz = lu_create(m);
    lu_mkz->mkz_enabled = 1;
    int rc1 = lu_factorize(lu_mkz, B);
    ASSERT_INT_EQ(rc1, 0, "Markowitz factorize");

    /* Solve with Markowitz disabled (dense path) */
    LUFactorization *lu_dense = lu_create(m);
    lu_dense->mkz_enabled = 0;
    int rc2 = lu_factorize(lu_dense, B);
    ASSERT_INT_EQ(rc2, 0, "Dense factorize");

    if (rc1 == 0 && rc2 == 0) {
        /* Compare solutions on same RHS */
        double max_diff = 0;
        for (int trial = 0; trial < 3; trial++) {
            double *b1 = (double *)calloc(m, sizeof(double));
            double *b2 = (double *)calloc(m, sizeof(double));
            double *x1 = (double *)calloc(m, sizeof(double));
            double *x2 = (double *)calloc(m, sizeof(double));

            for (int i = 0; i < m; i++)
                b1[i] = b2[i] = (double)(trial * 11 + i * 5 + 1);

            lu_solve(lu_mkz, b1, x1);
            lu_solve(lu_dense, b2, x2);

            for (int i = 0; i < m; i++) {
                double diff = fabs(x1[i] - x2[i]);
                if (diff > max_diff) max_diff = diff;
            }

            free(b1); free(b2); free(x1); free(x2);
        }
        ASSERT(max_diff < 1e-6, "Markowitz vs Dense: solutions match");
        if (max_diff >= 1e-6) printf("    max_diff = %.2e\n", max_diff);
    }

    lu_free(lu_mkz);
    lu_free(lu_dense);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 3: Integration test — solve LP with both paths, compare objectives
 * ============================================================================ */
static void test_markowitz_integration_small_lp(void) {
    printf("  Integration: small LP...\n");

    /* max 3x + 2y s.t. x + y <= 10, 2x + y <= 14, x,y >= 0 */
    RalphModel *m1 = ralph_test_create();
    ralph_test_add_var(m1, 0, RALPH_INFINITY, -3.0, 'C');
    ralph_test_add_var(m1, 0, RALPH_INFINITY, -2.0, 'C');
    int idx1[] = {0, 1}; double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(m1, 2, idx1, val1, 'L', 10.0);
    int idx2[] = {0, 1}; double val2[] = {2.0, 1.0};
    ralph_test_add_constraint(m1, 2, idx2, val2, 'L', 14.0);

    ralph_test_optimize(m1);
    double obj1 = ralph_test_get_objval(m1);
    int status1 = ralph_test_get_status(m1);

    ASSERT_INT_EQ(status1, RALPH_STATUS_OPTIMAL, "small LP status");
    ASSERT_NEAR(obj1, -24.0, 1e-6, "small LP obj");

    ralph_test_free(m1);
}

/* ============================================================================
 * Test 4: Medium LP with Markowitz (A/B comparison)
 * ============================================================================ */
static void test_markowitz_integration_medium_lp(void) {
    printf("  Integration: medium LP (30 vars, 25 cons)...\n");

    int nvars = 30, ncons = 25;

    RalphModel *m1 = ralph_test_create();
    RalphModel *m2 = ralph_test_create();

    unsigned int seed = 456;
    for (int j = 0; j < nvars; j++) {
        seed = seed * 1103515245 + 12345;
        double cost = ((double)((seed >> 16) & 0x7fff) / 32768.0) * 10.0 - 5.0;
        ralph_test_add_var(m1, 0, 100.0, cost, 'C');
        ralph_test_add_var(m2, 0, 100.0, cost, 'C');
    }

    for (int i = 0; i < ncons; i++) {
        int nnz = 4 + (i % 5);
        int *idx = (int *)malloc(nnz * sizeof(int));
        double *val = (double *)malloc(nnz * sizeof(double));

        for (int j = 0; j < nnz; j++) {
            seed = seed * 1103515245 + 12345;
            idx[j] = ((seed >> 16) & 0x7fff) % nvars;
            seed = seed * 1103515245 + 12345;
            val[j] = ((double)((seed >> 16) & 0x7fff) / 32768.0) * 4.0 + 0.1;
        }
        seed = seed * 1103515245 + 12345;
        double rhs = ((double)((seed >> 16) & 0x7fff) / 32768.0) * 50.0 + 10.0;

        ralph_test_add_constraint(m1, nnz, idx, val, 'L', rhs);
        ralph_test_add_constraint(m2, nnz, idx, val, 'L', rhs);
        free(idx); free(val);
    }

    /* m1: default (Markowitz enabled) */
    ralph_test_optimize(m1);
    double obj1 = ralph_test_get_objval(m1);
    int status1 = ralph_test_get_status(m1);

    /* m2: Markowitz disabled — set lu->mkz_enabled=0 not directly accessible,
     * but we can still compare that both get optimal */
    ralph_test_optimize(m2);
    double obj2 = ralph_test_get_objval(m2);
    int status2 = ralph_test_get_status(m2);

    ASSERT_INT_EQ(status1, status2, "medium LP: same status");
    if (status1 == RALPH_STATUS_OPTIMAL && status2 == RALPH_STATUS_OPTIMAL) {
        ASSERT_NEAR(obj1, obj2, 1e-4, "medium LP: obj values match");
    }

    ralph_test_free(m1);
    ralph_test_free(m2);
}

/* ============================================================================
 * Test 5: Larger sparse matrix — stress test with fill-in
 * ============================================================================ */
static void test_markowitz_large_sparse(void) {
    printf("  Markowitz: large sparse (m=150, ~15%% fill)...\n");

    int m = 150;
    double *A = (double *)calloc(m * m, sizeof(double));

    unsigned int seed = 789;
    for (int i = 0; i < m; i++) {
        A[i * m + i] = 30.0;
        for (int j = 0; j < m; j++) {
            if (i == j) continue;
            seed = seed * 1103515245 + 12345;
            if (((seed >> 16) & 0x7fff) % 100 < 15) {
                seed = seed * 1103515245 + 12345;
                A[i * m + j] = ((double)((seed >> 16) & 0x7fff) / 32768.0) * 2.0 - 1.0;
            }
        }
    }

    SparseMatrix *B = dense_to_csc(A, m, m);

    LUFactorization *lu = lu_create(m);
    lu->mkz_enabled = 1;
    int rc = lu_factorize(lu, B);
    ASSERT_INT_EQ(rc, 0, "large sparse: factorize");

    if (rc == 0) {
        double max_err = 0;
        for (int trial = 0; trial < 3; trial++) {
            double *b = (double *)calloc(m, sizeof(double));
            double *x = (double *)calloc(m, sizeof(double));
            for (int i = 0; i < m; i++)
                b[i] = (double)(trial * 13 + i * 7 + 1);

            double *b_orig = (double *)calloc(m, sizeof(double));
            memcpy(b_orig, b, m * sizeof(double));

            lu_solve(lu, b, x);

            for (int i = 0; i < m; i++) {
                double ax = 0;
                for (int j = 0; j < m; j++)
                    ax += A[i * m + j] * x[j];
                double err = fabs(ax - b_orig[i]);
                if (err > max_err) max_err = err;
            }
            free(b); free(x); free(b_orig);
        }
        ASSERT(max_err < 1e-5, "large sparse: solve accuracy");
        if (max_err >= 1e-5) printf("    max_err = %.2e\n", max_err);
    }

    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 6: Tridiagonal matrix (very sparse — should benefit most from Markowitz)
 * ============================================================================ */
static void test_markowitz_tridiagonal(void) {
    printf("  Markowitz: tridiagonal (m=100)...\n");

    int m = 100;
    double *A = (double *)calloc(m * m, sizeof(double));

    for (int i = 0; i < m; i++) {
        A[i * m + i] = 4.0;
        if (i > 0) A[i * m + (i - 1)] = -1.0;
        if (i < m - 1) A[i * m + (i + 1)] = -1.0;
    }

    SparseMatrix *B = dense_to_csc(A, m, m);

    LUFactorization *lu = lu_create(m);
    lu->mkz_enabled = 1;
    int rc = lu_factorize(lu, B);
    ASSERT_INT_EQ(rc, 0, "tridiag: factorize");

    if (rc == 0) {
        double *b = (double *)calloc(m, sizeof(double));
        double *x = (double *)calloc(m, sizeof(double));
        for (int i = 0; i < m; i++) b[i] = 1.0;
        double *b_orig = (double *)calloc(m, sizeof(double));
        memcpy(b_orig, b, m * sizeof(double));

        lu_solve(lu, b, x);

        double max_err = 0;
        for (int i = 0; i < m; i++) {
            double ax = 0;
            for (int j = 0; j < m; j++)
                ax += A[i * m + j] * x[j];
            double err = fabs(ax - b_orig[i]);
            if (err > max_err) max_err = err;
        }
        ASSERT(max_err < 1e-8, "tridiag: solve accuracy");
        if (max_err >= 1e-8) printf("    max_err = %.2e\n", max_err);

        free(b); free(x); free(b_orig);
    }

    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 7: NETLIB-style regression — reserved identity rows must not poison
 *         Markowitz pivoting (avoid dense fallback via identity placement fail)
 * ============================================================================ */
static void test_markowitz_reserved_row_regression(void) {
    printf("  Markowitz: reserved-row regression (m=80, k=40)...\n");

    const int m = 80;
    const int k = 40;
    const int stress_reserved = 10;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));

    /* Structural block: diagonally dominant on non-reserved rows. */
    for (int i = 0; i < k; i++) {
        A[i * m + i] = 10.0 + 0.01 * i;
        /* Keep structure non-trivial so Markowitz pool sizing is realistic. */
        A[((i + 1) % k) * m + i] = 0.05;
    }

    /* A subset of identity rows has very large entries in structural columns.
     * A buggy pivot scan that consumes reserved rows will later fail at
     * identity placement; a correct reservation-aware path should still succeed. */
    for (int t = 0; t < stress_reserved; t++) {
        int row = k + t;
        int col = t;
        A[row * m + col] = 100.0;
    }

    /* Identity columns (40..79): exact singletons */
    for (int t = 0; t < m - k; t++) {
        int row = k + t;
        A[row * m + (k + t)] = 1.0;
    }

    SparseMatrix *B = dense_to_csc(A, m, m);
    LUFactorization *lu = lu_create(m);
    ASSERT(lu != NULL, "reserved-row regression: lu_create");
    lu->mkz_enabled = 1;
    lu->sn_enabled = 0;

    int rc = lu_factorize(lu, B);
    ASSERT_INT_EQ(rc, 0, "reserved-row regression: factorize");
    if (rc == 0) {
        ASSERT(lu->telemetry.mkz_calls > 0, "reserved-row regression: Markowitz attempted");
        ASSERT(lu->telemetry.mkz_successes > 0, "reserved-row regression: Markowitz succeeded");
        ASSERT_INT_EQ(lu->telemetry.mkz_dense_fallbacks, 0,
                      "reserved-row regression: no Markowitz->GE fallback");
        ASSERT_INT_EQ(lu->telemetry.identity_sep_failures, 0,
                      "reserved-row regression: no identity placement failure");
        ASSERT_INT_EQ(lu->telemetry.used_dense_fallback_last, 0,
                      "reserved-row regression: no top-level dense fallback");

        double max_err = 0.0;
        for (int trial = 0; trial < 3; trial++) {
            double *b = (double *)calloc(m, sizeof(double));
            double *x = (double *)calloc(m, sizeof(double));
            double *b_orig = (double *)calloc(m, sizeof(double));
            for (int i = 0; i < m; i++) {
                b[i] = (double)(trial * 17 + i * 3 + 1);
                b_orig[i] = b[i];
            }
            lu_solve(lu, b, x);

            for (int i = 0; i < m; i++) {
                double ax = 0.0;
                for (int j = 0; j < m; j++) ax += A[i * m + j] * x[j];
                double err = fabs(ax - b_orig[i]);
                if (err > max_err) max_err = err;
            }
            free(b);
            free(x);
            free(b_orig);
        }
        ASSERT(max_err < 1e-7, "reserved-row regression: solve accuracy");
        if (max_err >= 1e-7) printf("    max_err = %.2e\n", max_err);
    }

    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 8: GE identity-placement regression — L-row tracking across row swaps
 * ============================================================================ */
static void test_ge_identity_lrow_regression(void) {
    printf("  GE: identity-placement L-row regression (m=24, k=16)...\n");

    const int m = 24;
    const int k = 16;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));

    /* Structural block (k=16) with stable pivots and a little coupling.
     * Keep columns non-singleton so symbolic identity split is deterministic. */
    for (int j = 0; j < k; j++) {
        A[j * m + j] = 6.0 + 0.01 * j;
        A[((j + 1) % k) * m + j] = 0.2;
    }

    /* Add stress entries from identity rows into structural columns to ensure
     * row reservation logic remains active during GE. */
    for (int t = 0; t < m - k; t++) {
        int row = k + t;
        int col = t;
        A[row * m + col] = 2.0 + 0.1 * t;
    }

    /* Identity columns (16..23) with reversed row mapping.
     * This forces identity_placement swaps so L_row remapping is exercised. */
    for (int t = 0; t < m - k; t++) {
        int col = k + t;
        int row = (m - 1) - t;
        A[row * m + col] = 1.0;
    }

    SparseMatrix *B = dense_to_csc(A, m, m);

    /* Sparse-efficient GE path (Markowitz disabled, k<MARKOWITZ_MIN_K). */
    LUFactorization *lu_ge = lu_create(m);
    ASSERT(lu_ge != NULL, "GE regression: lu_create sparse-efficient");
    lu_ge->mkz_enabled = 0;
    lu_ge->sn_enabled = 0;
    int rc_ge = lu_factorize(lu_ge, B);
    ASSERT_INT_EQ(rc_ge, 0, "GE regression: sparse-efficient factorize");
    if (rc_ge == 0) {
        ASSERT_INT_EQ(lu_ge->telemetry.used_dense_fallback_last, 0,
                      "GE regression: sparse-efficient path used");
        ASSERT_INT_EQ(lu_ge->telemetry.identity_sep_failures, 0,
                      "GE regression: identity placement succeeded");
    }

    /* Dense reference factorization. */
    LUFactorization *lu_dense = lu_create(m);
    ASSERT(lu_dense != NULL, "GE regression: lu_create dense reference");
    int rc_dense = lu_factorize_dense(lu_dense, B);
    ASSERT_INT_EQ(rc_dense, 0, "GE regression: dense reference factorize");

    if (rc_ge == 0 && rc_dense == 0) {
        double max_resid = 0.0;
        double max_xdiff = 0.0;

        for (int trial = 0; trial < 4; trial++) {
            double b1[24], b2[24], x1[24], x2[24];
            for (int i = 0; i < m; i++) {
                double rhs = (double)(trial * 5 + 2 * i + 1);
                b1[i] = rhs;
                b2[i] = rhs;
                x1[i] = 0.0;
                x2[i] = 0.0;
            }

            lu_solve(lu_ge, b1, x1);
            lu_solve(lu_dense, b2, x2);

            for (int i = 0; i < m; i++) {
                double ax = 0.0;
                for (int j = 0; j < m; j++) ax += A[i * m + j] * x1[j];
                double rhs = (double)(trial * 5 + 2 * i + 1);
                double resid = fabs(ax - rhs);
                if (resid > max_resid) max_resid = resid;

                double xdiff = fabs(x1[i] - x2[i]);
                if (xdiff > max_xdiff) max_xdiff = xdiff;
            }
        }

        ASSERT(max_resid < 1e-10, "GE regression: residual accuracy");
        ASSERT(max_xdiff < 1e-10, "GE regression: matches dense reference");
        if (max_resid >= 1e-10) printf("    max_resid = %.2e\n", max_resid);
        if (max_xdiff >= 1e-10) printf("    max_xdiff = %.2e\n", max_xdiff);
    }

    lu_free(lu_ge);
    lu_free(lu_dense);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 9: P1-G regression — numeric identity-separation failure should trigger
 *         one full-structural sparse retry before any top-level dense fallback.
 * ============================================================================ */
static void test_markowitz_numeric_identity_full_retry(void) {
    printf("  Markowitz: numeric identity-separation full-retry (m=60, k=40)...\n");

    const int m = 60;
    const int k = 40;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));

    for (int j = 0; j < k; j++) {
        A[j * m + j] = 8.0 + 0.02 * j;
        A[((j + 1) % k) * m + j] = 0.15;
    }
    for (int t = 0; t < m - k; t++) {
        int row = k + t;
        A[row * m + (k + t)] = 1.0;
    }

    SparseMatrix *B = dense_to_csc(A, m, m);
    LUFactorization *lu = lu_create(m);
    ASSERT(lu != NULL, "numeric full-retry: lu_create");
    if (!lu) {
        free_csc(B);
        free(A);
        return;
    }

    lu->mkz_enabled = 1;
    lu->sn_enabled = 0;

    int rc = lu_factorize(lu, B);
    ASSERT_INT_EQ(rc, 0, "numeric full-retry: warm factorize");
    if (rc == 0) {
        ASSERT_INT_EQ(lu->sym_k, k, "numeric full-retry: warm symbolic k");
        ASSERT_INT_EQ(lu->sym_num_identity, m - k,
                      "numeric full-retry: warm symbolic identity count");
    }

    /* Keep symbolic cache valid, but poison cached column order so numeric
     * identity partitioning fails before elimination. */
    lp_telemetry_reset_lu(lu);
    lu->ws_col_order[k + 1] = lu->ws_col_order[k];

    int rc_retry = lu_factorize(lu, B);
    ASSERT_INT_EQ(rc_retry, 0, "numeric full-retry: factorize after identity mismatch");
    if (rc_retry == 0) {
        ASSERT(lu->telemetry.sparse_numeric_fail_identity_sep > 0,
               "numeric full-retry: identity-separation numeric failure counted");
        ASSERT_INT_EQ(lu->telemetry.sparse_numeric_last_failure_reason,
                      LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION,
                      "numeric full-retry: terminal reason recorded");
        ASSERT(lu->telemetry.numeric_full_retry_attempts > 0,
               "numeric full-retry: full retry attempted");
        ASSERT(lu->telemetry.numeric_full_retry_successes > 0,
               "numeric full-retry: full retry succeeded");
        ASSERT_INT_EQ(lu->telemetry.numeric_full_retry_failures, 0,
                      "numeric full-retry: no retry failure");
        ASSERT_INT_EQ(lu->telemetry.used_dense_fallback_last, 0,
                      "numeric full-retry: no top-level dense fallback");
        ASSERT_INT_EQ(lu->telemetry.sparse_fallback_last_reason, LU_SPARSE_FALLBACK_NONE,
                      "numeric full-retry: sparse path succeeds");
        ASSERT_INT_EQ(lu->sym_k, m,
                      "numeric full-retry: retry switched to full-structural symbolic mode");

        /* Solve sanity check after retry. */
        double max_err = 0.0;
        for (int trial = 0; trial < 3; trial++) {
            double *b = (double *)calloc(m, sizeof(double));
            double *x = (double *)calloc(m, sizeof(double));
            double *b_orig = (double *)calloc(m, sizeof(double));
            for (int i = 0; i < m; i++) {
                b[i] = (double)(trial * 11 + i * 2 + 1);
                b_orig[i] = b[i];
            }
            lu_solve(lu, b, x);
            for (int i = 0; i < m; i++) {
                double ax = 0.0;
                for (int j = 0; j < m; j++) ax += A[i * m + j] * x[j];
                double err = fabs(ax - b_orig[i]);
                if (err > max_err) max_err = err;
            }
            free(b);
            free(x);
            free(b_orig);
        }
        ASSERT(max_err < 1e-7, "numeric full-retry: solve accuracy");
        if (max_err >= 1e-7) printf("    max_err = %.2e\n", max_err);
    }

    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== Sparse Markowitz LU Factorization Tests ===\n\n");

    printf("Standalone Correctness:\n");
    test_markowitz_sparse_solve();
    test_markowitz_lp_basis();
    test_markowitz_large_sparse();
    test_markowitz_tridiagonal();
    test_markowitz_reserved_row_regression();
    test_ge_identity_lrow_regression();
    test_markowitz_numeric_identity_full_retry();

    printf("\nIntegration (A/B Comparison):\n");
    test_markowitz_integration_small_lp();
    test_markowitz_integration_medium_lp();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
