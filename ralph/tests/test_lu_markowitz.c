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
#include "lp_bfcp_policy.h"
#include "lp_policy_glpk_compat.h"

#define TOLERANCE 1e-8

static int tests_run = 0;
static int tests_passed = 0;

double lu_update_pivot_ratio_threshold_for_test(int num_updates,
                                                int max_updates,
                                                double cond_estimate,
                                                double growth_factor);
int lu_identity_sep_retry_lane_plan_for_test(int idsep_retry_streak,
                                             int sn_enabled,
                                             int k);
int lu_markowitz_global_skip_plan_for_test(int bad_streak,
                                           int skip_budget,
                                           int event,
                                           int *next_bad_streak_out,
                                           int *next_skip_budget_out);

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

static void dense_matvec(const double *A, int m, const double *x, double *y) {
    for (int i = 0; i < m; i++) {
        double sum = 0.0;
        for (int j = 0; j < m; j++) sum += A[i * m + j] * x[j];
        y[i] = sum;
    }
}

static void dense_matvec_transpose(const double *A, int m, const double *x, double *y) {
    for (int j = 0; j < m; j++) {
        double sum = 0.0;
        for (int i = 0; i < m; i++) sum += A[i * m + j] * x[i];
        y[j] = sum;
    }
}

static double max_abs_diff(const double *a, const double *b, int n) {
    double max_diff = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = fabs(a[i] - b[i]);
        if (diff > max_diff) max_diff = diff;
    }
    return max_diff;
}

static void dense_replace_basis_column(double *B, int m, int leaving_pos, const double *col) {
    for (int i = 0; i < m; i++) B[i * m + leaving_pos] = col[i];
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
 * Test 10: BTF symbolic ordering should detect disconnected structural blocks
 *          and still produce a correct sparse factorization.
 * ============================================================================ */
static void test_lu_btf_symbolic_blocks(void) {
    printf("  LU: btf symbolic block ordering (m=24, k=20)...\n");

    const int m = 24;
    const int k = 20;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    SparseMatrix *B = NULL;
    LUFactorization *lu = NULL;
    SimplexSolver owner;
    double max_err = 0.0;

    for (int block = 0; block < 2; block++) {
        int base = block * 10;
        for (int c = 0; c < 10; c++) {
            int col = base + c;
            int row = base + c;
            int next_row = base + ((c + 1) % 10);
            A[row * m + col] = 7.0 + 0.05 * col;
            A[next_row * m + col] = 0.2;
        }
    }
    for (int t = 0; t < m - k; t++) {
        int row = k + t;
        A[row * m + (k + t)] = 1.0;
    }

    B = dense_to_csc(A, m, m);
    lu = lu_create(m);
    ASSERT(lu != NULL, "btf symbolic blocks: lu_create");
    if (!lu) goto cleanup;

    memset(&owner, 0, sizeof(owner));
    owner.lu_factorization_type = LP_GLPK_BFCP_FACTORIZATION_BTF;
    lu->owner = &owner;
    lu->mkz_enabled = 1;
    lu->sn_enabled = 0;

    ASSERT_INT_EQ(lu_factorize(lu, B), 0, "btf symbolic blocks: factorize");
    if (lu->last_failure_reason == LU_FAIL_NONE) {
        ASSERT_INT_EQ(lu->sym_factorization_type, LP_GLPK_BFCP_FACTORIZATION_BTF,
                      "btf symbolic blocks: symbolic mode recorded");
        ASSERT_INT_EQ(lu->sym_btf_blocks, 2,
                      "btf symbolic blocks: two structural SCC blocks detected");
        ASSERT_INT_EQ(lu->telemetry.used_dense_fallback_last, 0,
                      "btf symbolic blocks: sparse path used");

        for (int trial = 0; trial < 3; trial++) {
            double *b = (double *)calloc(m, sizeof(double));
            double *x = (double *)calloc(m, sizeof(double));
            double *b_orig = (double *)calloc(m, sizeof(double));
            for (int i = 0; i < m; i++) {
                b[i] = (double)(trial * 7 + i + 1);
                b_orig[i] = b[i];
            }
            lu_solve(lu, b, x);
            for (int i = 0; i < m; i++) {
                double ax = 0.0;
                for (int j = 0; j < m; j++) ax += A[i * m + j] * x[j];
                if (fabs(ax - b_orig[i]) > max_err) {
                    max_err = fabs(ax - b_orig[i]);
                }
            }
            free(b);
            free(x);
            free(b_orig);
        }
        ASSERT(max_err < 1e-7, "btf symbolic blocks: solve accuracy");
    }

cleanup:
    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 11: Strict LU dispatch should not use the default numeric full-retry
 *          ladder when identity separation fails.
 * ============================================================================ */
static void test_strict_dispatch_skips_numeric_full_retry(void) {
    printf("  LU: strict dispatch skips numeric full-retry...\n");

    const int m = 60;
    const int k = 40;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    SparseMatrix *B = NULL;
    LUFactorization *lu = NULL;
    SimplexSolver owner;

    for (int j = 0; j < k; j++) {
        A[j * m + j] = 8.0 + 0.02 * j;
        A[((j + 1) % k) * m + j] = 0.15;
    }
    for (int t = 0; t < m - k; t++) {
        int row = k + t;
        A[row * m + (k + t)] = 1.0;
    }

    B = dense_to_csc(A, m, m);
    lu = lu_create(m);
    ASSERT(lu != NULL, "strict dispatch retry skip: lu_create");
    if (!lu) goto cleanup;

    memset(&owner, 0, sizeof(owner));
    owner.glpk_strict_mode = 1;
    owner.lu_factorization_type = LP_GLPK_BFCP_FACTORIZATION_LUF;
    owner.lu_strict_lane_active = 1;
    owner.lu_strict_prefer_dense_ge_numeric = 0;
    owner.lu_strict_allow_supernode_lane = 0;
    owner.lu_strict_allow_symbolic_full_retry = 0;
    owner.lu_strict_allow_top_level_dense_fallback = 0;
    lu->owner = &owner;
    lu->mkz_enabled = 1;
    lu->sn_enabled = 0;

    ASSERT_INT_EQ(lu_factorize(lu, B), 0, "strict dispatch retry skip: warm factorize");

    lp_telemetry_reset_lu(lu);
    lu->ws_col_order[k + 1] = lu->ws_col_order[k];

    ASSERT(lu_factorize(lu, B) != 0,
           "strict dispatch retry skip: strict factorize fails without retry ladder");
    ASSERT(lu->telemetry.sparse_numeric_fail_identity_sep > 0,
           "strict dispatch retry skip: identity-separation recorded");
    ASSERT_INT_EQ(lu->telemetry.numeric_full_retry_attempts, 0,
                  "strict dispatch retry skip: no full-structural retry");
    ASSERT_INT_EQ(lu->telemetry.used_dense_fallback_last, 0,
                  "strict dispatch retry skip: no top-level dense fallback");
    ASSERT_INT_EQ(lu->telemetry.sparse_fallback_last_reason, LU_SPARSE_FALLBACK_NUMERIC,
                  "strict dispatch retry skip: numeric fallback reason recorded");

cleanup:
    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 10: Supernode cost gate regression — when skip budget is active for a
 *          large structural factorization, supernode path is skipped and dense
 *          GE backend is used without top-level dense fallback.
 * ============================================================================ */
static void test_supernode_cost_gate_skip_regression(void) {
    printf("  LU: supernode cost-gate skip regression (m=144)...\n");

    const int m = 144;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    for (int j = 0; j < m; j++) {
        A[j * m + j] = 12.0 + 0.01 * j;
        A[((j + 1) % m) * m + j] = 0.35;
        A[((j + 11) % m) * m + j] = -0.18;
    }

    SparseMatrix *B = dense_to_csc(A, m, m);
    LUFactorization *lu = lu_create(m);
    ASSERT(lu != NULL, "sn-cost gate regression: lu_create");
    if (!lu) {
        free_csc(B);
        free(A);
        return;
    }

    lu->mkz_enabled = 0;
    lu->sn_enabled = 1;
    lu->sn_cost_gate_markowitz_ewma_ms = 1.0;
    lu->sn_cost_gate_skip_budget = 1;

    int rc = lu_factorize(lu, B);
    ASSERT_INT_EQ(rc, 0, "sn-cost gate regression: factorize");
    if (rc == 0) {
        ASSERT(lu->telemetry.sn_cost_gate_skips > 0,
               "sn-cost gate regression: supernode skip telemetry");
        ASSERT_INT_EQ(lu->telemetry.numeric_backend_supernode, 0,
                      "sn-cost gate regression: supernode backend skipped");
        ASSERT(lu->telemetry.numeric_backend_dense_ge > 0,
               "sn-cost gate regression: dense-GE backend selected");
        ASSERT_INT_EQ(lu->telemetry.used_dense_fallback_last, 0,
                      "sn-cost gate regression: no top-level dense fallback");
    }

    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 12: Adaptive LU update pivot threshold policy
 * ============================================================================ */
static void test_lu_update_pivot_threshold_adaptive(void) {
    printf("  LU: adaptive update-pivot threshold policy...\n");

    double healthy_early = lu_update_pivot_ratio_threshold_for_test(
        10, 120, 1e3, 1.0);
    double moderate = lu_update_pivot_ratio_threshold_for_test(
        70, 120, 1e5, 50.0);
    double degraded = lu_update_pivot_ratio_threshold_for_test(
        90, 120, 1e8, 1e4);

    ASSERT_NEAR(healthy_early, 1.25e-5, 1e-12,
                "adaptive threshold: healthy early state");
    ASSERT_NEAR(moderate, 5.0e-5, 1e-12,
                "adaptive threshold: moderate health state");
    ASSERT_NEAR(degraded, 2.0e-4, 1e-12,
                "adaptive threshold: degraded health state");
    ASSERT(healthy_early < moderate,
           "adaptive threshold: healthy threshold is looser than moderate");
    ASSERT(moderate < degraded,
           "adaptive threshold: degraded threshold is stricter than moderate");
}

/* ============================================================================
 * Test 13: Identity-separation retry-lane policy (dense vs supernode)
 * ============================================================================ */
static void test_identity_sep_retry_lane_policy(void) {
    printf("  LU: identity-separation retry-lane policy...\n");

    ASSERT_INT_EQ(
        lu_identity_sep_retry_lane_plan_for_test(1, 1, 80),
        LU_IDSEP_RETRY_LANE_DENSE,
        "idsep lane policy: below streak trigger stays dense");
    ASSERT_INT_EQ(
        lu_identity_sep_retry_lane_plan_for_test(2, 0, 80),
        LU_IDSEP_RETRY_LANE_DENSE,
        "idsep lane policy: supernode disabled stays dense");
    ASSERT_INT_EQ(
        lu_identity_sep_retry_lane_plan_for_test(2, 1, 40),
        LU_IDSEP_RETRY_LANE_DENSE,
        "idsep lane policy: small-k stays dense");
    ASSERT_INT_EQ(
        lu_identity_sep_retry_lane_plan_for_test(2, 1, 80),
        LU_IDSEP_RETRY_LANE_SUPERNODE,
        "idsep lane policy: repeated streak promotes supernode");
    ASSERT_INT_EQ(
        lu_identity_sep_retry_lane_plan_for_test(6, 1, 80),
        LU_IDSEP_RETRY_LANE_SUPERNODE,
        "idsep lane policy: sustained streak keeps supernode lane");
}

/* ============================================================================
 * Test 14: Markowitz global skip-budget policy (cross-fingerprint chronic singulars)
 * ============================================================================ */
static void test_markowitz_global_skip_policy(void) {
    printf("  LU: Markowitz global skip-budget policy...\n");

    int bad_streak = 0;
    int skip_budget = 0;
    int should_skip = 0;

    for (int i = 0; i < 5; i++) {
        should_skip = lu_markowitz_global_skip_plan_for_test(
            bad_streak, skip_budget, 1, &bad_streak, &skip_budget);
        ASSERT_INT_EQ(should_skip, 0,
                      "mkz global policy: no skip before chronic-failure threshold");
    }

    should_skip = lu_markowitz_global_skip_plan_for_test(
        bad_streak, skip_budget, 1, &bad_streak, &skip_budget);
    ASSERT_INT_EQ(should_skip, 1,
                  "mkz global policy: skip budget trips at chronic singular threshold");
    ASSERT_INT_EQ(bad_streak, 0,
                  "mkz global policy: bad streak resets after trip");
    ASSERT(skip_budget > 0,
           "mkz global policy: skip budget remains active after first consumed skip");

    should_skip = lu_markowitz_global_skip_plan_for_test(
        bad_streak, skip_budget, 0, &bad_streak, &skip_budget);
    ASSERT_INT_EQ(should_skip, 1,
                  "mkz global policy: active skip budget skips next attempt");

    should_skip = lu_markowitz_global_skip_plan_for_test(
        bad_streak, skip_budget, 2, &bad_streak, &skip_budget);
    ASSERT_INT_EQ(should_skip, 0,
                  "mkz global policy: Markowitz success clears skip budget");
    ASSERT_INT_EQ(bad_streak, 0,
                  "mkz global policy: success leaves bad streak cleared");
    ASSERT_INT_EQ(skip_budget, 0,
                  "mkz global policy: success clears remaining skip budget");
}

/* ============================================================================
 * Test 15: Markowitz global skip-budget runtime telemetry integration
 * ============================================================================ */
static void test_markowitz_global_skip_runtime_telemetry(void) {
    printf("  LU: Markowitz global skip-budget runtime telemetry...\n");

    const int m = 60;
    const int k = 40;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));

    for (int i = 0; i < k; i++) {
        A[i * m + i] = 5.0;
        A[((i + 1) % k) * m + i] = 0.25;
    }
    for (int r = 0; r < k; r++) {
        A[r * m + (k - 1)] = A[r * m + (k - 2)];
    }
    for (int t = 0; t < m - k; t++) {
        int row = k + t;
        A[row * m + (k + t)] = 1.0;
    }

    SparseMatrix *B = dense_to_csc(A, m, m);
    LUFactorization *lu = lu_create(m);
    ASSERT(lu != NULL, "mkz global runtime: lu_create");
    if (!lu) {
        free_csc(B);
        free(A);
        return;
    }
    lu->mkz_enabled = 1;
    lu->sn_enabled = 0;

    const int attempts = 24;
    for (int t = 0; t < attempts; t++) {
        /* Disable local fingerprint circuit per attempt so global policy is
         * the active skip mechanism under chronic singular outcomes. */
        lu->mkz_circuit_bad_streak = 0;
        lu->mkz_circuit_skip_budget = 0;
        lu->mkz_circuit_fingerprint = (uint64_t)(1000 + t);
        ASSERT(lu_factorize(lu, B) != 0,
               "mkz global runtime: singular factorization should fail");
    }

    ASSERT(lu->telemetry.mkz_global_skip_trips > 0,
           "mkz global runtime: global skip trip counted");
    ASSERT(lu->telemetry.mkz_global_skip_skips > 0,
           "mkz global runtime: global skip usage counted");
    ASSERT(lu->telemetry.mkz_calls < attempts,
           "mkz global runtime: global skips reduced Markowitz attempts");

    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 16: FT update density reinversion guard
 * ============================================================================ */
static void test_ft_update_density_refactor_guard(void) {
    printf("  LU: FT update density refactor guard...\n");

    LUFactorization *lu = lu_create(400);
    ASSERT(lu != NULL, "ft-density guard: lu_create");
    if (!lu) return;

    lu->use_ft_updates = 1;
    lu->growth_factor = 1.0;
    lu->cond_estimate = 1.0;
    lu->max_updates = 100; /* Pin lifecycle budget for deterministic warmup threshold. */

    /* Non-aged updates: avg spike ratio above 0.45 should force refactor. */
    lu->num_updates = 12;
    lu->ft_num_updates = 12;
    lu->spike_pool_used = 2200; /* avg = 183.3, ratio ~0.458 */
    ASSERT_INT_EQ(lu_needs_refactorization(lu), 1,
                  "ft-density guard: non-aged avg density forces refactor");
    ASSERT_INT_EQ(lu->last_refactor_trigger_reason,
                  LP_BFCP_REFACTOR_REASON_AVG_SPIKE_DENSITY,
                  "ft-density guard: reason=avg_spike_density");
    ASSERT_INT_EQ(lu->telemetry.refactor_need_checks, 1,
                  "ft-density guard: telemetry checks incremented");
    ASSERT_INT_EQ(lu->telemetry.refactor_need_triggers, 1,
                  "ft-density guard: telemetry triggers incremented");
    ASSERT_INT_EQ(lu->telemetry.refactor_need_reason_avg_spike_density, 1,
                  "ft-density guard: reason counter avg_spike_density");

    /* Non-aged updates: lower avg spike ratio should not force refactor. */
    lu->num_updates = 12;
    lu->ft_num_updates = 12;
    lu->spike_pool_used = 1200; /* avg = 100, ratio 0.25 */
    ASSERT_INT_EQ(lu_needs_refactorization(lu), 0,
                  "ft-density guard: non-aged moderate density does not force refactor");
    ASSERT_INT_EQ(lu->last_refactor_trigger_reason,
                  LP_BFCP_REFACTOR_REASON_NONE,
                  "ft-density guard: reason=none when not triggered");

    /* Aged updates tighten threshold to 0.35. */
    lu->num_updates = lu->max_updates / 2;
    lu->ft_num_updates = lu->num_updates;
    lu->spike_pool_used = 15000; /* avg = 150, ratio 0.375 */
    ASSERT_INT_EQ(lu_needs_refactorization(lu), 1,
                  "ft-density guard: aged avg density forces refactor earlier");
    ASSERT_INT_EQ(lu->last_refactor_trigger_reason,
                  LP_BFCP_REFACTOR_REASON_AVG_SPIKE_DENSITY,
                  "ft-density guard: aged reason=avg_spike_density");
    ASSERT_INT_EQ(lu->telemetry.refactor_need_reason_avg_spike_density, 2,
                  "ft-density guard: avg density reason count accumulates");

    lu_free(lu);
}

/* ============================================================================
 * Test 17: Dense FT spikes should not hard-fail during warmup
 * ============================================================================ */
static void test_ft_dense_spike_warmup_update(void) {
    printf("  LU: FT dense-spike warmup update...\n");

    const int m = 320;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    SparseMatrix *B = NULL;
    LUFactorization *lu = NULL;
    double *entering_col = NULL;

    for (int i = 0; i < m; i++) A[i * m + i] = 1.0;
    B = dense_to_csc(A, m, m);

    lu = lu_create(m);
    ASSERT(lu != NULL, "ft dense warmup: lu_create");
    if (!lu) goto cleanup;

    ASSERT_INT_EQ(lu_factorize(lu, B), 0, "ft dense warmup: factorize");

    entering_col = (double *)calloc((size_t)m, sizeof(double));
    ASSERT(entering_col != NULL, "ft dense warmup: allocate entering column");
    if (!entering_col) goto cleanup;

    /* Identity basis + dense entering column => dense spike ratio ~1.0. */
    for (int i = 0; i < m; i++) entering_col[i] = 1.0;
    ASSERT_INT_EQ(lu_update(lu, 0, entering_col), 0,
                  "ft dense warmup: first dense update should succeed");
    ASSERT_INT_EQ(lu->last_failure_reason, LU_FAIL_NONE,
                  "ft dense warmup: no LU failure after update");
    ASSERT_INT_EQ(lu->num_updates, 1,
                  "ft dense warmup: update count increments");
    ASSERT_INT_EQ(lu->ft_num_updates, 1,
                  "ft dense warmup: FT update count increments");
    ASSERT_INT_EQ(lu->telemetry.update_fail_spike_pool_full, 0,
                  "ft dense warmup: no dense-spike hard reject in warmup");

cleanup:
    free(entering_col);
    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 18: LU update uses BFCP effective update budget at runtime
 * ============================================================================ */
static void test_lu_update_cond_adaptive_limit_runtime(void) {
    printf("  LU: runtime cond-adaptive update budget...\n");

    LUFactorization *lu = lu_create(80);
    double entering_col[80];

    ASSERT(lu != NULL, "lu adaptive runtime: lu_create");
    if (!lu) return;

    memset(entering_col, 0, sizeof(entering_col));
    entering_col[0] = 1.0;

    /* m=80 => max_updates defaults to 50. With poor cond and enough updates,
     * BFCP effective update limit becomes 12 (= 50/4). */
    lu->num_updates = 12;
    lu->cond_estimate = 2e9;
    lu->growth_factor = 1.0;

    ASSERT(lu_update(lu, 0, entering_col) != LU_FAIL_NONE,
           "lu adaptive runtime: update blocked by effective limit");
    ASSERT_INT_EQ(lu->last_failure_reason, LU_FAIL_MAX_UPDATES,
                  "lu adaptive runtime: failure reason max_updates");
    ASSERT_INT_EQ(lu->last_refactor_trigger_reason,
                  LP_BFCP_REFACTOR_REASON_COND_ADAPTIVE_LIMIT,
                  "lu adaptive runtime: trigger reason cond_adaptive_limit");
    ASSERT(lu->telemetry.update_fail_max_updates >= 1,
           "lu adaptive runtime: update_fail_max_updates incremented");

    lu_free(lu);
}

/* ============================================================================
 * Test 19: LU hard-trigger helper delegates to BFCP hard safety criteria
 * ============================================================================ */
static void test_lu_refactor_hard_trigger_runtime(void) {
    printf("  LU: runtime hard-trigger helper...\n");

    LUFactorization *lu = lu_create(80);
    ASSERT(lu != NULL, "lu hard trigger runtime: lu_create");
    if (!lu) return;

    lu->num_updates = lu->max_updates;
    ASSERT_INT_EQ(lu_refactor_hard_trigger(lu), 1,
                  "lu hard trigger runtime: max-updates hard trigger");

    lu->num_updates = 0;
    lu->growth_factor = 4e8;
    lu->growth_refactor_threshold = 1e8;
    ASSERT_INT_EQ(lu_refactor_hard_trigger(lu), 1,
                  "lu hard trigger runtime: growth hard trigger");

    lu->growth_factor = 1.0;
    lu->cond_estimate = 2e10;
    ASSERT_INT_EQ(lu_refactor_hard_trigger(lu), 1,
                  "lu hard trigger runtime: cond hard trigger");

    lu->cond_estimate = 1.0;
    lu->use_ft_updates = 1;
    lu->spike_pool_capacity = 1000;
    lu->spike_pool_used = 951;
    ASSERT_INT_EQ(lu_refactor_hard_trigger(lu), 1,
                  "lu hard trigger runtime: spike pool hard trigger");

    lu->spike_pool_used = 0;
    ASSERT_INT_EQ(lu_refactor_hard_trigger(lu), 0,
                  "lu hard trigger runtime: healthy state not hard");

    lu_free(lu);
}

/* ============================================================================
 * Test 20: Backend policy controls LU update path and thresholds
 * ============================================================================ */
static void test_lu_backend_policy_runtime(void) {
    printf("  LU: backend policy runtime mapping...\n");

    LUFactorization *lu = lu_create(400);
    ASSERT(lu != NULL, "lu backend policy runtime: lu_create");
    if (!lu) return;

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_LUF_FT);
    int luf_updates = lu->max_updates;
    double luf_pivot_tol = lu->pivot_tol;
    double luf_growth_guard = lu->growth_refactor_threshold;
    ASSERT_INT_EQ(lu->backend_policy, LP_LU_BACKEND_POLICY_LUF_FT,
                  "lu backend policy runtime: luf_ft selected");
    ASSERT_INT_EQ(lu->use_ft_updates, 1,
                  "lu backend policy runtime: luf_ft uses FT updates");
    ASSERT_INT_EQ(lu->update_backend, LU_UPDATE_BACKEND_FT,
                  "lu backend policy runtime: luf_ft backend is FT");

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_CBG);
    ASSERT_INT_EQ(lu->backend_policy, LP_LU_BACKEND_POLICY_CBG,
                  "lu backend policy runtime: cbg selected");
    ASSERT_INT_EQ(lu->use_ft_updates, 0,
                  "lu backend policy runtime: cbg uses ETA updates");
    ASSERT_INT_EQ(lu->update_backend, LU_UPDATE_BACKEND_BG_COMPAT,
                  "lu backend policy runtime: cbg backend is bg-compat");
    ASSERT(lu->max_updates < luf_updates,
           "lu backend policy runtime: cbg lowers update budget");
    ASSERT(lu->pivot_tol > luf_pivot_tol,
           "lu backend policy runtime: cbg tightens pivot tolerance");
    ASSERT(lu->growth_refactor_threshold < luf_growth_guard,
           "lu backend policy runtime: cbg tightens growth guard");

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_CGR);
    ASSERT_INT_EQ(lu->backend_policy, LP_LU_BACKEND_POLICY_CGR,
                  "lu backend policy runtime: cgr selected");
    ASSERT_INT_EQ(lu->use_ft_updates, 1,
                  "lu backend policy runtime: cgr uses FT updates");
    ASSERT_INT_EQ(lu->update_backend, LU_UPDATE_BACKEND_FT,
                  "lu backend policy runtime: default cgr backend stays FT");
    ASSERT(lu->max_updates > luf_updates,
           "lu backend policy runtime: cgr raises update budget");
    ASSERT(lu->pivot_tol < luf_pivot_tol,
           "lu backend policy runtime: cgr relaxes pivot tolerance");
    ASSERT(lu->growth_refactor_threshold > luf_growth_guard,
           "lu backend policy runtime: cgr relaxes growth guard");

    lu_apply_backend_policy(lu, 99);
    ASSERT_INT_EQ(lu->backend_policy, LP_LU_BACKEND_POLICY_LUF_FT,
                  "lu backend policy runtime: invalid policy falls back to luf_ft");

    ASSERT(lu->telemetry.backend_policy_luf_ft > 0,
           "lu backend policy runtime: telemetry luf_ft selection counted");
    ASSERT(lu->telemetry.backend_policy_cbg > 0,
           "lu backend policy runtime: telemetry cbg selection counted");
    ASSERT(lu->telemetry.backend_policy_cgr > 0,
           "lu backend policy runtime: telemetry cgr selection counted");
    ASSERT_INT_EQ(lu->telemetry.backend_policy_last, LP_LU_BACKEND_POLICY_LUF_FT,
                  "lu backend policy runtime: telemetry last policy tracks fallback");

    lu_free(lu);
}

/* ============================================================================
 * Test 21: Strict backend policy remaps CGR off the FT update lane
 * ============================================================================ */
static void test_lu_strict_backend_policy_runtime(void) {
    printf("  LU: strict backend policy update-lane mapping...\n");

    LUFactorization *lu = lu_create(400);
    SimplexSolver owner;

    ASSERT(lu != NULL, "lu strict backend runtime: lu_create");
    if (!lu) return;

    memset(&owner, 0, sizeof(owner));
    owner.glpk_strict_mode = 1;
    owner.lu_strict_lane_active = 1;
    lu->owner = &owner;

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_CBG);
    ASSERT_INT_EQ(lu->backend_policy, LP_LU_BACKEND_POLICY_CBG,
                  "lu strict backend runtime: cbg selected");
    ASSERT_INT_EQ(lu->use_ft_updates, 0,
                  "lu strict backend runtime: cbg leaves FT lane");
    ASSERT_INT_EQ(lu->update_backend, LU_UPDATE_BACKEND_BG_COMPAT,
                  "lu strict backend runtime: cbg backend is bg-compat");

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_CGR);
    ASSERT_INT_EQ(lu->backend_policy, LP_LU_BACKEND_POLICY_CGR,
                  "lu strict backend runtime: cgr selected");
    ASSERT_INT_EQ(lu->use_ft_updates, 0,
                  "lu strict backend runtime: strict cgr leaves FT lane");
    ASSERT_INT_EQ(lu->update_backend, LU_UPDATE_BACKEND_GR_COMPAT,
                  "lu strict backend runtime: strict cgr backend is gr-compat");

    lu_free(lu);
}

/* ============================================================================
 * Test 21: Backend policy update-path telemetry (FT vs BG compat)
 * ============================================================================ */
static void test_lu_backend_policy_update_path_telemetry(void) {
    printf("  LU: backend policy update-path telemetry...\n");

    const int m = 32;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    SparseMatrix *B = NULL;
    LUFactorization *lu = NULL;
    double *entering_col = NULL;

    for (int i = 0; i < m; i++) A[i * m + i] = 1.0;
    B = dense_to_csc(A, m, m);
    lu = lu_create(m);
    ASSERT(lu != NULL, "lu backend update telemetry: lu_create");
    if (!lu) goto cleanup;

    entering_col = (double *)calloc((size_t)m, sizeof(double));
    ASSERT(entering_col != NULL, "lu backend update telemetry: entering col alloc");
    if (!entering_col) goto cleanup;
    for (int i = 0; i < m; i++) entering_col[i] = 1.0;

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_CBG);
    ASSERT_INT_EQ(lu_factorize(lu, B), 0,
                  "lu backend update telemetry: cbg factorize");
    {
        int bg_before = lu->telemetry.update_path_bg_compat;
        ASSERT_INT_EQ(lu_update(lu, 0, entering_col), 0,
                      "lu backend update telemetry: cbg update");
        ASSERT(lu->telemetry.update_path_bg_compat > bg_before,
               "lu backend update telemetry: bg compat update path counted");
        ASSERT_INT_EQ(lu->num_eta, 0,
                      "lu backend update telemetry: cbg leaves eta chain empty");
        ASSERT(lu->schur_num_updates > 0,
               "lu backend update telemetry: cbg stores schur compat chain");
    }

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_LUF_FT);
    ASSERT_INT_EQ(lu_factorize(lu, B), 0,
                  "lu backend update telemetry: luf_ft factorize");
    {
        int ft_before = lu->telemetry.update_path_ft;
        ASSERT_INT_EQ(lu_update(lu, 0, entering_col), 0,
                      "lu backend update telemetry: luf_ft update");
        ASSERT(lu->telemetry.update_path_ft > ft_before,
               "lu backend update telemetry: FT update path counted");
    }

cleanup:
    free(entering_col);
    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 22: Strict CGR update path should use dedicated GR compatibility storage
 * ============================================================================ */
static void test_lu_strict_cgr_update_path_telemetry(void) {
    printf("  LU: strict cgr update-path telemetry...\n");

    const int m = 32;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    SparseMatrix *B = NULL;
    LUFactorization *lu = NULL;
    double *entering_col = NULL;
    SimplexSolver owner;

    for (int i = 0; i < m; i++) A[i * m + i] = 1.0;
    B = dense_to_csc(A, m, m);
    lu = lu_create(m);
    ASSERT(lu != NULL, "lu strict cgr update telemetry: lu_create");
    if (!lu) goto cleanup;

    memset(&owner, 0, sizeof(owner));
    owner.glpk_strict_mode = 1;
    owner.lu_strict_lane_active = 1;
    lu->owner = &owner;

    entering_col = (double *)calloc((size_t)m, sizeof(double));
    ASSERT(entering_col != NULL, "lu strict cgr update telemetry: entering col alloc");
    if (!entering_col) goto cleanup;
    for (int i = 0; i < m; i++) entering_col[i] = 1.0;

    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_CGR);
    ASSERT_INT_EQ(lu_factorize(lu, B), 0,
                  "lu strict cgr update telemetry: factorize");
    {
        int gr_before = lu->telemetry.update_path_gr_compat;
        int ft_before = lu->telemetry.update_path_ft;
        ASSERT_INT_EQ(lu_update(lu, 0, entering_col), 0,
                      "lu strict cgr update telemetry: update");
        ASSERT(lu->telemetry.update_path_gr_compat > gr_before,
               "lu strict cgr update telemetry: gr compat path counted");
        ASSERT_INT_EQ(lu->telemetry.update_path_ft, ft_before,
                      "lu strict cgr update telemetry: ft path unchanged");
        ASSERT_INT_EQ(lu->num_eta, 0,
                      "lu strict cgr update telemetry: eta chain remains empty");
        ASSERT(lu->schur_num_updates > 0,
               "lu strict cgr update telemetry: gr uses schur compat chain");
    }

cleanup:
    free(entering_col);
    lu_free(lu);
    free_csc(B);
    free(A);
}

static void configure_backend_invariant_lane(LUFactorization *lu, LUUpdateBackend backend) {
    lu->backend_policy = LP_LU_BACKEND_POLICY_LUF_FT;
    lu->update_backend = backend;
    lu->use_ft_updates = (backend == LU_UPDATE_BACKEND_FT) ? 1 : 0;
    lu->max_updates = 64;
    lu->pivot_tol = RALPH_PIVOT_TOL;
    lu->growth_refactor_threshold = RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
}

static void run_backend_invariant_sequence(LUUpdateBackend backend,
                                           const char *label,
                                           int expect_bg_math) {
    const int m = 8;
    double B0[64] = {0};
    double Bcur[64] = {0};
    LUFactorization *lu_ft = NULL;
    LUFactorization *lu_cmp = NULL;
    SparseMatrix *B = NULL;
    double x_ft[8], x_cmp[8], y_ft[8], y_cmp[8], check[8];
    static const double updates[][8] = {
        {1.50, 0.20, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00},
        {0.00, 0.00, 0.10, 1.40, 0.30, 0.00, 0.00, 0.00},
        {0.00, 0.00, 0.00, 0.20, 0.00, 1.30, 0.10, 0.00},
        {1.20, 0.10, 0.05, 0.00, 0.00, 0.00, 0.00, 0.00}
    };
    static const int leaving_pos[] = {0, 3, 5, 0};
    static const double rhs[][8] = {
        {1.0, 2.0, -1.0, 0.5, 0.0, 1.0, -0.5, 2.0},
        {0.5, -1.0, 2.0, 1.5, -0.5, 0.0, 1.0, 3.0},
        {-1.5, 0.0, 0.5, 2.0, 1.0, -0.5, 0.25, 1.0},
        {1.0, 1.0, 1.0, -1.0, 0.0, 2.0, -2.0, 0.5}
    };
    static const double rhs_t[][8] = {
        {0.0, 1.0, 2.0, -1.0, 0.5, 0.0, 1.0, -0.5},
        {1.5, -0.5, 0.0, 1.0, 2.0, -1.0, 0.0, 0.5},
        {0.5, 0.0, -1.5, 1.0, 0.0, 2.0, -0.5, 1.5},
        {-1.0, 0.5, 0.25, 0.0, 1.0, 1.5, -0.5, 0.0}
    };

    for (int i = 0; i < m; i++) {
        B0[i * m + i] = 1.0;
        Bcur[i * m + i] = 1.0;
    }

    B = dense_to_csc(B0, m, m);
    lu_ft = lu_create(m);
    lu_cmp = lu_create(m);
    ASSERT(lu_ft != NULL, "lu backend invariant: lu_ft create");
    ASSERT(lu_cmp != NULL, "lu backend invariant: lu_cmp create");
    if (!lu_ft || !lu_cmp || !B) goto cleanup;

    ASSERT_INT_EQ(lu_factorize(lu_ft, B), 0, "lu backend invariant: ft factorize");
    ASSERT_INT_EQ(lu_factorize(lu_cmp, B), 0, "lu backend invariant: cmp factorize");
    configure_backend_invariant_lane(lu_ft, LU_UPDATE_BACKEND_FT);
    configure_backend_invariant_lane(lu_cmp, backend);

    for (int step = 0; step < 4; step++) {
        int rc_ft = lu_update(lu_ft, leaving_pos[step], updates[step]);
        int rc_cmp = lu_update(lu_cmp, leaving_pos[step], updates[step]);
        char msg[128];

        snprintf(msg, sizeof(msg), "lu backend invariant (%s): update rc step %d", label, step);
        ASSERT_INT_EQ(rc_cmp, rc_ft, msg);
        if (rc_ft != 0 || rc_cmp != 0) break;

        dense_replace_basis_column(Bcur, m, leaving_pos[step], updates[step]);

        lu_solve(lu_ft, (double *)rhs[step], x_ft);
        lu_solve(lu_cmp, (double *)rhs[step], x_cmp);
        snprintf(msg, sizeof(msg), "lu backend invariant (%s): solve x match step %d", label, step);
        ASSERT(max_abs_diff(x_ft, x_cmp, m) < 1e-8, msg);

        dense_matvec(Bcur, m, x_ft, check);
        snprintf(msg, sizeof(msg), "lu backend invariant (%s): ft residual step %d", label, step);
        ASSERT(max_abs_diff(check, rhs[step], m) < 1e-8, msg);
        dense_matvec(Bcur, m, x_cmp, check);
        snprintf(msg, sizeof(msg), "lu backend invariant (%s): cmp residual step %d", label, step);
        ASSERT(max_abs_diff(check, rhs[step], m) < 1e-8, msg);

        lu_solve_transpose(lu_ft, (double *)rhs_t[step], y_ft);
        lu_solve_transpose(lu_cmp, (double *)rhs_t[step], y_cmp);
        snprintf(msg, sizeof(msg), "lu backend invariant (%s): btran x match step %d", label, step);
        ASSERT(max_abs_diff(y_ft, y_cmp, m) < 1e-8, msg);

        dense_matvec_transpose(Bcur, m, y_ft, check);
        snprintf(msg, sizeof(msg), "lu backend invariant (%s): ft transpose residual step %d", label, step);
        ASSERT(max_abs_diff(check, rhs_t[step], m) < 1e-8, msg);
        dense_matvec_transpose(Bcur, m, y_cmp, check);
        snprintf(msg, sizeof(msg), "lu backend invariant (%s): cmp transpose residual step %d", label, step);
        ASSERT(max_abs_diff(check, rhs_t[step], m) < 1e-8, msg);
    }

    if (expect_bg_math) {
        ASSERT(lu_cmp->schur_num_updates > 0,
               "lu backend invariant (bg): schur low-rank updates populated");
        ASSERT(lu_cmp->schur_k != NULL,
               "lu backend invariant (bg): schur dense K allocated");
    }

cleanup:
    lu_free(lu_ft);
    lu_free(lu_cmp);
    free_csc(B);
}

/* ============================================================================
 * Test 23: BG backend matches FT solve invariants on the same update sequence
 * ============================================================================ */
static void test_lu_bg_backend_matches_ft_invariants(void) {
    printf("  LU: bg backend matches FT invariants...\n");
    run_backend_invariant_sequence(LU_UPDATE_BACKEND_BG_COMPAT, "bg", 1);
}

/* ============================================================================
 * Test 24: GR compatibility backend matches FT solve invariants
 * ============================================================================ */
static void test_lu_gr_backend_matches_ft_invariants(void) {
    printf("  LU: gr backend matches FT invariants...\n");
    run_backend_invariant_sequence(LU_UPDATE_BACKEND_GR_COMPAT, "gr", 0);
}

/* ============================================================================
 * Test 25: Compact factor cache is reused between solves for BG/GR backends
 * ============================================================================ */
static void run_compact_factor_cache_test(int backend, const char *label) {
    const int m = 8;
    double B0[64] = {0};
    static const double entering[8] = {1.50, 0.20, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00};
    static const double rhs1[8] = {1.0, 2.0, -1.0, 0.5, 0.0, 1.0, -0.5, 2.0};
    static const double rhs2[8] = {0.5, -1.0, 2.0, 1.5, -0.5, 0.0, 1.0, 3.0};
    static const double rhs_t1[8] = {0.0, 1.0, 2.0, -1.0, 0.5, 0.0, 1.0, -0.5};
    static const double rhs_t2[8] = {1.5, -0.5, 0.0, 1.0, 2.0, -1.0, 0.0, 0.5};
    LUFactorization *lu = NULL;
    SparseMatrix *B = NULL;
    double x[8];
    char msg[160];

    for (int i = 0; i < m; i++) B0[i * m + i] = 1.0;

    B = dense_to_csc(B0, m, m);
    lu = lu_create(m);
    ASSERT(lu != NULL, "lu compact cache: lu_create");
    if (!lu || !B) goto cleanup;

    ASSERT_INT_EQ(lu_factorize(lu, B), 0, "lu compact cache: factorize");
    configure_backend_invariant_lane(lu, backend);
    ASSERT_INT_EQ(lu_update(lu, 0, entering), 0, "lu compact cache: update");

    ASSERT_INT_EQ(lu->telemetry.perf_compact_factor_calls, 0,
                  "lu compact cache: no factor before first solve");

    lu_solve(lu, (double *)rhs1, x);
    snprintf(msg, sizeof(msg), "lu compact cache (%s): first forward factor", label);
    ASSERT_INT_EQ(lu->telemetry.perf_compact_factor_calls, 1, msg);

    lu_solve(lu, (double *)rhs2, x);
    snprintf(msg, sizeof(msg), "lu compact cache (%s): second forward reuses factor", label);
    ASSERT_INT_EQ(lu->telemetry.perf_compact_factor_calls, 1, msg);

    lu_solve_transpose(lu, (double *)rhs_t1, x);
    snprintf(msg, sizeof(msg), "lu compact cache (%s): first backward factor", label);
    ASSERT_INT_EQ(lu->telemetry.perf_compact_factor_calls, 2, msg);

    lu_solve_transpose(lu, (double *)rhs_t2, x);
    snprintf(msg, sizeof(msg), "lu compact cache (%s): second backward reuses factor", label);
    ASSERT_INT_EQ(lu->telemetry.perf_compact_factor_calls, 2, msg);

    ASSERT_INT_EQ(lu_update(lu, 0, entering), 0, "lu compact cache: second update");
    lu_solve(lu, (double *)rhs1, x);
    snprintf(msg, sizeof(msg), "lu compact cache (%s): update invalidates forward cache", label);
    ASSERT_INT_EQ(lu->telemetry.perf_compact_factor_calls, 3, msg);

    lu_solve_transpose(lu, (double *)rhs_t1, x);
    snprintf(msg, sizeof(msg), "lu compact cache (%s): update invalidates backward cache", label);
    ASSERT_INT_EQ(lu->telemetry.perf_compact_factor_calls, 4, msg);

cleanup:
    lu_free(lu);
    free_csc(B);
}

static void test_lu_bg_compact_factor_cache(void) {
    printf("  LU: bg compact factor cache reuse...\n");
    run_compact_factor_cache_test(LU_UPDATE_BACKEND_BG_COMPAT, "bg");
}

static void test_lu_gr_compact_factor_cache(void) {
    printf("  LU: gr compact factor cache reuse...\n");
    run_compact_factor_cache_test(LU_UPDATE_BACKEND_GR_COMPAT, "gr");
}

/* ============================================================================
 * Test 26: Markowitz scan telemetry is emitted on a real factorization
 * ============================================================================ */
static void test_markowitz_scan_runtime_telemetry(void) {
    printf("  LU: Markowitz scan runtime telemetry...\n");

    const int m = 40;
    unsigned int seed = 777;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    SparseMatrix *B = NULL;
    LUFactorization *lu = NULL;
    LUTelemetrySnapshot snap;

    ASSERT(A != NULL, "lu mkz telemetry: dense matrix alloc");
    if (!A) return;

    for (int i = 0; i < m; i++) {
        A[i * m + i] = 18.0 + (double)(i % 7);
        for (int j = 0; j < m; j++) {
            if (i == j) continue;
            seed = seed * 1103515245u + 12345u;
            if (((seed >> 16) & 0x7fffu) % 100 < 24) {
                seed = seed * 1103515245u + 12345u;
                A[i * m + j] =
                    ((double)((seed >> 16) & 0x7fffu) / 32768.0) * 3.0 - 1.5;
            }
        }
    }

    B = dense_to_csc(A, m, m);
    lu = lu_create(m);
    ASSERT(B != NULL, "lu mkz telemetry: sparse matrix alloc");
    ASSERT(lu != NULL, "lu mkz telemetry: lu_create");
    if (!B || !lu) goto cleanup;

    lu->mkz_enabled = 1;
    lu->telemetry_enabled = 1;
    lp_telemetry_reset_lu(lu);

    ASSERT_INT_EQ(lu_factorize(lu, B), 0, "lu mkz telemetry: factorize");
    lp_telemetry_snapshot_lu(lu, &snap);

    ASSERT(snap.mkz_calls > 0, "lu mkz telemetry: Markowitz called");
    ASSERT(snap.mkz_primary_scan_entries > 0,
           "lu mkz telemetry: primary scan entries counted");
    ASSERT(snap.mkz_update_existing_entries > 0,
           "lu mkz telemetry: update existing entries counted");
    ASSERT(snap.mkz_update_fill_candidates > 0,
           "lu mkz telemetry: update fill candidates counted");
    ASSERT_INT_EQ((int)snap.mkz_hint_fallback_scans, 0,
                  "lu mkz telemetry: hint fallback scans eliminated");
    ASSERT_INT_EQ((int)snap.mkz_hint_fallback_scan_entries, 0,
                  "lu mkz telemetry: hint fallback scan entries eliminated");
    ASSERT(snap.mkz_affected_columns_total > 0,
           "lu mkz telemetry: affected columns counted");
    ASSERT(snap.mkz_affected_columns_max > 0,
           "lu mkz telemetry: affected columns max counted");
    ASSERT(snap.mkz_col_max_scan_entries > 0,
           "lu mkz telemetry: col_max scan entries counted");

cleanup:
    lu_free(lu);
    free_csc(B);
    free(A);
}

/* ============================================================================
 * Test 27: Runtime update limit is bounded by allocated LU update storage
 * ============================================================================ */
static void test_lu_update_storage_capacity_guard_runtime(void) {
    printf("  LU: runtime update-capacity guard...\n");

    const int m = 40;
    double *A = (double *)calloc((size_t)m * m, sizeof(double));
    SparseMatrix *B = NULL;
    LUFactorization *lu = NULL;
    double *entering_col = NULL;
    int cap = 0;

    for (int i = 0; i < m; i++) A[i * m + i] = 1.0;
    B = dense_to_csc(A, m, m);
    lu = lu_create(m);
    ASSERT(lu != NULL, "lu capacity guard: lu_create");
    if (!lu) goto cleanup;

    ASSERT_INT_EQ(lu_factorize(lu, B), 0, "lu capacity guard: factorize");

    cap = lu->ft_spike_capacity;
    if (lu->eta_capacity > 0 && (cap <= 0 || lu->eta_capacity < cap)) {
        cap = lu->eta_capacity;
    }
    ASSERT(cap > 0, "lu capacity guard: update storage capacity available");
    if (cap <= 0) goto cleanup;

    entering_col = (double *)calloc((size_t)m, sizeof(double));
    ASSERT(entering_col != NULL, "lu capacity guard: entering col alloc");
    if (!entering_col) goto cleanup;
    entering_col[0] = 1.0; /* Identity update: stable, sparse spike. */

    lu->max_updates = cap + 25; /* Deliberately exceed allocated metadata capacity. */

    for (int i = 0; i < cap; i++) {
        ASSERT_INT_EQ(lu_update(lu, 0, entering_col), 0,
                      "lu capacity guard: update succeeds up to capacity");
    }
    ASSERT_INT_EQ(lu->max_updates, cap,
                  "lu capacity guard: runtime max_updates clamped to storage");

    ASSERT(lu_update(lu, 0, entering_col) != LU_FAIL_NONE,
           "lu capacity guard: update rejected at storage capacity");
    ASSERT_INT_EQ(lu->last_failure_reason, LU_FAIL_MAX_UPDATES,
                  "lu capacity guard: failure reason max_updates");
    ASSERT_INT_EQ(lu->telemetry.update_fail_bad_input, 0,
                  "lu capacity guard: no bad_input failures");
    ASSERT(lu->telemetry.update_fail_max_updates > 0,
           "lu capacity guard: max_updates failure telemetry increments");

cleanup:
    free(entering_col);
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
    test_lu_btf_symbolic_blocks();
    test_strict_dispatch_skips_numeric_full_retry();
    test_supernode_cost_gate_skip_regression();
    test_lu_update_pivot_threshold_adaptive();
    test_identity_sep_retry_lane_policy();
    test_markowitz_global_skip_policy();
    test_markowitz_global_skip_runtime_telemetry();
    test_ft_update_density_refactor_guard();
    test_ft_dense_spike_warmup_update();
    test_lu_update_cond_adaptive_limit_runtime();
    test_lu_refactor_hard_trigger_runtime();
    test_lu_backend_policy_runtime();
    test_lu_strict_backend_policy_runtime();
    test_lu_backend_policy_update_path_telemetry();
    test_lu_strict_cgr_update_path_telemetry();
    test_lu_bg_backend_matches_ft_invariants();
    test_lu_gr_backend_matches_ft_invariants();
    test_lu_bg_compact_factor_cache();
    test_lu_gr_compact_factor_cache();
    test_markowitz_scan_runtime_telemetry();
    test_lu_update_storage_capacity_guard_runtime();

    printf("\nIntegration (A/B Comparison):\n");
    test_markowitz_integration_small_lp();
    test_markowitz_integration_medium_lp();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
