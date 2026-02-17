/*
 * Tests for Supernodal LU Factorization (T2.1)
 *
 * Phase 1: Elimination tree + supernode detection
 * Phase 2: Dense micro-kernels (GEMM, TRSM, block factor)
 * Phase 3: Supernodal numeric factorization
 * Phase 4: Integration with existing LP solver (A/B comparison)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "lu_supernode.h"

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
 * Phase 1 Tests: Elimination Tree + Supernode Detection
 * ============================================================================ */

/* Test etree on a simple 3x3 lower triangular matrix */
static void test_etree_3x3(void) {
    printf("  Phase 1: etree 3x3 lower triangular...\n");

    /* Matrix (row-major, 3 rows x 3 cols):
     * [ 1  0  0 ]   row 0
     * [ 2  3  0 ]   row 1
     * [ 4  5  6 ]   row 2
     *
     * Column 0 has nonzeros in rows 0,1,2 → parent is column 1 (first col>0 sharing a row)
     * Column 1 has nonzeros in rows 1,2 → parent is column 2
     * Column 2 is root
     */
    double A[9] = {1, 0, 0, 2, 3, 0, 4, 5, 6};
    int row_perm[3] = {0, 1, 2};
    int etree[3];

    int rc = sn_build_etree(A, 3, 3, row_perm, etree);
    ASSERT_INT_EQ(rc, 0, "etree 3x3: return code");
    ASSERT_INT_EQ(etree[0], 1, "etree 3x3: parent[0]=1");
    ASSERT_INT_EQ(etree[1], 2, "etree 3x3: parent[1]=2");
    ASSERT_INT_EQ(etree[2], -1, "etree 3x3: parent[2]=-1 (root)");
}

/* Test etree on a tridiagonal 5x5 matrix */
static void test_etree_tridiag(void) {
    printf("  Phase 1: etree 5x5 tridiagonal...\n");

    /* Tridiagonal: each row has entries in (j-1, j, j+1)
     * This creates a chain etree: 0->1->2->3->4 */
    int m = 5, k = 5;
    double A[25];
    memset(A, 0, sizeof(A));
    for (int i = 0; i < 5; i++) {
        A[i * 5 + i] = 2.0;
        if (i > 0) A[i * 5 + (i - 1)] = -1.0;
        if (i < 4) A[i * 5 + (i + 1)] = -1.0;
    }

    int row_perm[5] = {0, 1, 2, 3, 4};
    int etree[5];

    int rc = sn_build_etree(A, m, k, row_perm, etree);
    ASSERT_INT_EQ(rc, 0, "etree tridiag: return code");
    ASSERT_INT_EQ(etree[0], 1, "etree tridiag: parent[0]=1");
    ASSERT_INT_EQ(etree[1], 2, "etree tridiag: parent[1]=2");
    ASSERT_INT_EQ(etree[2], 3, "etree tridiag: parent[2]=3");
    ASSERT_INT_EQ(etree[3], 4, "etree tridiag: parent[3]=4");
    ASSERT_INT_EQ(etree[4], -1, "etree tridiag: parent[4]=-1 (root)");
}

/* Test postorder with a chain etree */
static void test_postorder_chain(void) {
    printf("  Phase 1: postorder chain...\n");

    /* Chain: 0->1->2->3 (root=3) */
    int etree[4] = {1, 2, 3, -1};
    int po[4];

    int rc = sn_etree_postorder(etree, 4, po);
    ASSERT_INT_EQ(rc, 0, "postorder chain: return code");
    /* Postorder of a chain: 0,1,2,3 (children before parent) */
    ASSERT_INT_EQ(po[0], 0, "postorder chain: po[0]=0");
    ASSERT_INT_EQ(po[1], 1, "postorder chain: po[1]=1");
    ASSERT_INT_EQ(po[2], 2, "postorder chain: po[2]=2");
    ASSERT_INT_EQ(po[3], 3, "postorder chain: po[3]=3");
}

/* Test supernode detection on a chain etree (all columns become one supernode) */
static void test_supernodes_chain(void) {
    printf("  Phase 1: supernode detection chain...\n");

    /* Dense lower triangular 4x4 → chain etree → one big supernode */
    int m = 4, k = 4;
    double A[16] = {
        1, 0, 0, 0,
        2, 3, 0, 0,
        4, 5, 6, 0,
        7, 8, 9, 10
    };
    int row_perm[4] = {0, 1, 2, 3};

    SNSymbolic *sym = sn_analyze(A, m, k, row_perm);
    /* sn_analyze no longer checks SN_MIN_K (caller decides) */
    ASSERT(sym != NULL, "supernode chain k=4: not NULL");
    if (sym) {
        ASSERT(sym->num_supernodes > 0, "supernode chain k=4: has supernodes");
    }
    sn_symbolic_free(sym);
}

/* Test supernode detection on a larger matrix (k >= 8) */
static void test_supernodes_large(void) {
    printf("  Phase 1: supernode detection k=10...\n");

    /* Dense lower triangular 10x10 */
    int m = 10, k = 10;
    double A[100];
    memset(A, 0, sizeof(A));
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j <= i; j++) {
            A[i * 10 + j] = (double)(i - j + 1);
        }
    }
    int row_perm[10];
    for (int i = 0; i < 10; i++) row_perm[i] = i;

    SNSymbolic *sym = sn_analyze(A, m, k, row_perm);
    ASSERT(sym != NULL, "supernode k=10: not NULL");

    if (sym) {
        ASSERT(sym->num_supernodes >= 1, "supernode k=10: at least 1 supernode");
        ASSERT(sym->num_supernodes <= k, "supernode k=10: at most k supernodes");

        /* Verify partition covers all columns */
        int total = 0;
        for (int s = 0; s < sym->num_supernodes; s++) {
            total += sym->supernodes[s].size;
            ASSERT(sym->supernodes[s].size >= 1, "supernode k=10: each has size >= 1");
        }
        ASSERT_INT_EQ(total, k, "supernode k=10: partition covers all columns");

        /* Verify contiguous and non-overlapping */
        int next_start = 0;
        for (int s = 0; s < sym->num_supernodes; s++) {
            ASSERT_INT_EQ(sym->supernodes[s].start, next_start,
                         "supernode k=10: contiguous");
            next_start += sym->supernodes[s].size;
        }
    }

    sn_symbolic_free(sym);
}

/* ============================================================================
 * Phase 2 Tests: Dense Micro-Kernels
 * ============================================================================ */

/* Test GEMM against naive triple-loop */
static void test_dgemm_basic(void) {
    printf("  Phase 2: GEMM basic...\n");

    /* A (4x3), B (3x5), C (4x5) — C -= A*B */
    int M = 4, K = 3, N = 5;
    double A[12], B[15], C[20], C_ref[20];

    /* Fill with known values */
    for (int i = 0; i < M; i++)
        for (int j = 0; j < K; j++)
            A[i * K + j] = (double)(i * K + j + 1);

    for (int i = 0; i < K; i++)
        for (int j = 0; j < N; j++)
            B[i * N + j] = (double)(i * N + j + 1) * 0.1;

    /* Initialize C and C_ref to same values */
    for (int i = 0; i < M * N; i++) {
        C[i] = (double)(i + 1);
        C_ref[i] = (double)(i + 1);
    }

    /* Reference: naive triple loop */
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++)
            for (int p = 0; p < K; p++)
                C_ref[i * N + j] -= A[i * K + p] * B[p * N + j];

    /* Supernodal GEMM */
    sn_dgemm_update(M, K, N, A, K, B, N, C, N);

    /* Compare */
    int ok = 1;
    for (int i = 0; i < M * N; i++) {
        if (fabs(C[i] - C_ref[i]) > TOLERANCE) {
            printf("    GEMM mismatch at %d: %.10e vs %.10e\n", i, C[i], C_ref[i]);
            ok = 0;
        }
    }
    ASSERT(ok, "GEMM basic: matches naive");
}

/* Test GEMM with non-multiple-of-4 dimensions */
static void test_dgemm_nonmultiple(void) {
    printf("  Phase 2: GEMM non-multiple-of-4...\n");

    int M = 7, K = 5, N = 3;
    double A[35], B[15], C[21], C_ref[21];

    for (int i = 0; i < M * K; i++) A[i] = (double)(i % 7 + 1) * 0.3;
    for (int i = 0; i < K * N; i++) B[i] = (double)(i % 5 + 1) * 0.2;
    for (int i = 0; i < M * N; i++) C[i] = C_ref[i] = (double)(i + 1);

    /* Reference */
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++)
            for (int p = 0; p < K; p++)
                C_ref[i * N + j] -= A[i * K + p] * B[p * N + j];

    sn_dgemm_update(M, K, N, A, K, B, N, C, N);

    int ok = 1;
    for (int i = 0; i < M * N; i++) {
        if (fabs(C[i] - C_ref[i]) > TOLERANCE) ok = 0;
    }
    ASSERT(ok, "GEMM non-multiple: matches naive");
}

/* Test TRSM: L * X = B check */
static void test_dtrsm_basic(void) {
    printf("  Phase 2: TRSM basic...\n");

    /* L is 3x3 unit lower triangular:
     * [ 1   0   0 ]
     * [ 2   1   0 ]
     * [ 3   4   1 ]
     *
     * B is 3x2:
     * [ 1  2 ]
     * [ 3  4 ]
     * [ 5  6 ]
     *
     * Solve L*X = B → X = L^{-1}*B
     */
    double L[9] = {1, 0, 0, 2, 1, 0, 3, 4, 1};
    double B[6] = {1, 2, 3, 4, 5, 6};
    double B_orig[6] = {1, 2, 3, 4, 5, 6};

    sn_dtrsm_lower(3, 2, L, 3, B, 2);

    /* Verify: L * X should equal B_orig */
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 2; j++) {
            double sum = 0;
            for (int p = 0; p <= i; p++) {
                sum += L[i * 3 + p] * B[p * 2 + j];
            }
            if (fabs(sum - B_orig[i * 2 + j]) > TOLERANCE) {
                printf("    TRSM check: L*X[%d,%d]=%.10e, expected %.10e\n",
                       i, j, sum, B_orig[i * 2 + j]);
                ok = 0;
            }
        }
    }
    ASSERT(ok, "TRSM basic: L*X = B");
}

/* Test block factor: PA = LU reconstruction */
static void test_block_factor_basic(void) {
    printf("  Phase 2: block factor basic...\n");

    /* 4x3 panel (4 rows, 3 columns) */
    double panel[12] = {
        2, 1, 3,
        4, 3, 7,
        1, 2, 1,
        3, 1, 5
    };
    double panel_orig[12];
    memcpy(panel_orig, panel, sizeof(panel_orig));

    int pivots[3];
    int rc = sn_block_factor(4, 3, panel, 3, pivots, 1e-10);
    ASSERT_INT_EQ(rc, 0, "block factor: return code");

    /* Reconstruct PA = LU from the factored panel.
     * panel[i][j] for i<j and i==j is U
     * panel[i][j] for i>j is L (multipliers)
     * L has implicit 1 diagonal.
     * pivots[] records row swaps. */

    /* Apply row permutation to original panel to get PA */
    double PA[12];
    memcpy(PA, panel_orig, sizeof(PA));
    for (int j = 0; j < 3; j++) {
        if (pivots[j] != j) {
            for (int c = 0; c < 3; c++) {
                double tmp = PA[j * 3 + c];
                PA[j * 3 + c] = PA[pivots[j] * 3 + c];
                PA[pivots[j] * 3 + c] = tmp;
            }
        }
    }

    /* Compute LU product */
    double LU[12];
    memset(LU, 0, sizeof(LU));
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            double sum = 0;
            int min_ij = (i < j) ? i : j;
            for (int p = 0; p <= min_ij; p++) {
                double l_ip = (i == p) ? 1.0 : ((i > p) ? panel[i * 3 + p] : 0.0);
                double u_pj = (p <= j) ? panel[p * 3 + j] : 0.0;
                sum += l_ip * u_pj;
            }
            LU[i * 3 + j] = sum;
        }
    }

    /* Compare PA and LU */
    int ok = 1;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            if (fabs(PA[i * 3 + j] - LU[i * 3 + j]) > TOLERANCE) {
                printf("    block factor: PA[%d,%d]=%.6f, LU=%.6f\n",
                       i, j, PA[i * 3 + j], LU[i * 3 + j]);
                ok = 0;
            }
        }
    }
    ASSERT(ok, "block factor: PA = LU reconstruction");
}

/* ============================================================================
 * Phase 3 Tests: Supernodal Numeric Factorization
 * ============================================================================ */

/*
 * Test sn_factorize on a known 10x10 dense matrix, verify by solving Ax=b.
 *
 * Convention: L entries are at "time of computation" positions. The solve
 * procedure applies P first, then forward-solves L, then backward-solves U.
 * This matches Ralph's lu_solve convention (NOT textbook PA=LU).
 */
static void test_sn_factorize_known_matrix(void) {
    printf("  Phase 3: supernodal factorize known matrix...\n");

    int m = 10, k = 10;
    double A[100], A_orig[100];
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < k; j++) {
            A[i * k + j] = (double)((i + 1) * (j + 2)) + 0.5;
        }
        A[i * k + i] += 50.0; /* Strong diagonal dominance */
    }
    memcpy(A_orig, A, sizeof(A));

    int row_perm[10], row_pos[10];
    for (int i = 0; i < m; i++) { row_perm[i] = i; row_pos[i] = i; }

    SNSymbolic *sym = sn_analyze(A, m, k, row_perm);
    ASSERT(sym != NULL, "sn factorize known: symbolic analysis");
    if (!sym) return;

    int cap = m * k;
    int *Lr = (int *)calloc(cap, sizeof(int));
    int *Lc = (int *)calloc(cap, sizeof(int));
    double *Lv = (double *)calloc(cap, sizeof(double));
    int *Ur = (int *)calloc(cap, sizeof(int));
    int *Uc = (int *)calloc(cap, sizeof(int));
    double *Uv = (double *)calloc(cap, sizeof(double));
    int Lnnz = 0, Unnz = 0;

    int rc = sn_factorize(A, m, k, row_perm, row_pos, 1e-10,
                          sym->supernodes, sym->num_supernodes,
                          NULL, 0, 0, 0, NULL,
                          Lr, Lc, Lv, &Lnnz,
                          Ur, Uc, Uv, &Unnz,
                          NULL, 0);
    ASSERT_INT_EQ(rc, 0, "sn factorize known: return code");

    if (rc == 0) {
        ASSERT(Lnnz > 0, "sn factorize known: L has entries");
        ASSERT(Unnz > 0, "sn factorize known: U has entries");

        /* Build dense L and U from COO */
        double *L = (double *)calloc(m * k, sizeof(double));
        double *U = (double *)calloc(k * k, sizeof(double));
        for (int i = 0; i < Lnnz; i++)
            if (Lr[i] < m && Lc[i] < k) L[Lr[i] * k + Lc[i]] = Lv[i];
        for (int i = 0; i < Unnz; i++)
            if (Ur[i] < k && Uc[i] < k) U[Ur[i] * k + Uc[i]] = Uv[i];

        /* Verify by solving Ax=b for 3 different RHS vectors */
        double max_err = 0;
        for (int trial = 0; trial < 3; trial++) {
            double b[10], y[10], x[10];
            for (int i = 0; i < m; i++)
                b[i] = (double)(trial * 7 + i * 3 + 1);

            /* y = Pb (apply row permutation) */
            for (int i = 0; i < m; i++)
                y[i] = b[row_perm[i]];

            /* Forward solve: Lz = y */
            for (int j = 0; j < k; j++) {
                double yj = y[j];
                for (int i = j + 1; i < m; i++)
                    y[i] -= L[i * k + j] * yj;
            }

            /* Backward solve: Ux = y */
            for (int j = k - 1; j >= 0; j--) {
                y[j] /= U[j * k + j];
                double xj = y[j];
                for (int i = 0; i < j; i++)
                    y[i] -= U[i * k + j] * xj;
            }
            memcpy(x, y, k * sizeof(double));

            /* Check A_orig * x ≈ b */
            for (int i = 0; i < m; i++) {
                double ax = 0;
                for (int j = 0; j < k; j++)
                    ax += A_orig[i * k + j] * x[j];
                double err = fabs(ax - b[i]);
                if (err > max_err) max_err = err;
            }
        }
        ASSERT(max_err < 1e-6, "sn factorize known: solve Ax=b (max_err < 1e-6)");
        if (max_err >= 1e-6)
            printf("    max_err = %.2e\n", max_err);

        free(L);
        free(U);
    }

    free(Lr); free(Lc); free(Lv);
    free(Ur); free(Uc); free(Uv);
    sn_symbolic_free(sym);
}

/* Test supernodal on a random diagonally-dominant matrix, verify by solve */
static void test_sn_factorize_random(void) {
    printf("  Phase 3: supernodal factorize random...\n");

    int m = 12, k = 12;
    double A[144], A_orig[144];
    unsigned int seed = 42;
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < k; j++) {
            seed = seed * 1103515245 + 12345;
            A[i * k + j] = ((double)((seed >> 16) & 0x7fff) / 32768.0) - 0.5;
        }
        A[i * k + i] += 10.0;
    }
    memcpy(A_orig, A, sizeof(A));

    int row_perm[12], row_pos[12];
    for (int i = 0; i < m; i++) { row_perm[i] = i; row_pos[i] = i; }

    SNSymbolic *sym = sn_analyze(A, m, k, row_perm);
    ASSERT(sym != NULL, "sn factorize random: symbolic analysis");
    if (!sym) return;

    int cap = m * k * 2;
    int *Lr = (int *)calloc(cap, sizeof(int));
    int *Lc = (int *)calloc(cap, sizeof(int));
    double *Lv = (double *)calloc(cap, sizeof(double));
    int *Ur = (int *)calloc(cap, sizeof(int));
    int *Uc = (int *)calloc(cap, sizeof(int));
    double *Uv = (double *)calloc(cap, sizeof(double));
    int Lnnz = 0, Unnz = 0;

    int rc = sn_factorize(A, m, k, row_perm, row_pos, 1e-10,
                          sym->supernodes, sym->num_supernodes,
                          NULL, 0, 0, 0, NULL,
                          Lr, Lc, Lv, &Lnnz,
                          Ur, Uc, Uv, &Unnz,
                          NULL, 0);
    ASSERT_INT_EQ(rc, 0, "sn factorize random: return code");

    if (rc == 0) {
        /* Build dense L and U from COO */
        double *L = (double *)calloc(m * k, sizeof(double));
        double *U = (double *)calloc(k * k, sizeof(double));
        for (int i = 0; i < Lnnz; i++)
            if (Lr[i] < m && Lc[i] < k) L[Lr[i] * k + Lc[i]] = Lv[i];
        for (int i = 0; i < Unnz; i++)
            if (Ur[i] < k && Uc[i] < k) U[Ur[i] * k + Uc[i]] = Uv[i];

        /* Verify by solving Ax=b */
        double max_err = 0;
        for (int trial = 0; trial < 3; trial++) {
            double b[12], y[12], x[12];
            for (int i = 0; i < m; i++)
                b[i] = (double)(trial * 11 + i * 5 + 1);

            for (int i = 0; i < m; i++)
                y[i] = b[row_perm[i]];
            for (int j = 0; j < k; j++) {
                double yj = y[j];
                for (int i = j + 1; i < m; i++)
                    y[i] -= L[i * k + j] * yj;
            }
            for (int j = k - 1; j >= 0; j--) {
                y[j] /= U[j * k + j];
                double xj = y[j];
                for (int i = 0; i < j; i++)
                    y[i] -= U[i * k + j] * xj;
            }
            memcpy(x, y, k * sizeof(double));

            for (int i = 0; i < m; i++) {
                double ax = 0;
                for (int j = 0; j < k; j++)
                    ax += A_orig[i * k + j] * x[j];
                double err = fabs(ax - b[i]);
                if (err > max_err) max_err = err;
            }
        }
        ASSERT(max_err < 1e-6, "sn factorize random: solve Ax=b (max_err < 1e-6)");
        if (max_err >= 1e-6)
            printf("    max_err = %.2e\n", max_err);

        free(L); free(U);
    }

    free(Lr); free(Lc); free(Lv);
    free(Ur); free(Uc); free(Uv);
    sn_symbolic_free(sym);
}

/* ============================================================================
 * Phase 4 Tests: Integration (A/B comparison with existing path)
 * ============================================================================ */

/* Solve a small LP with and without supernodal, compare results */
static void test_integration_small_lp(void) {
    printf("  Phase 4: integration small LP...\n");

    /* Simple LP: max 3x + 2y
     * s.t. x + y <= 10
     *      2x + y <= 14
     *      x, y >= 0
     * Optimal: x=4, y=6, obj=24
     */
    RalphModel *model1 = ralph_create();
    ralph_add_var(model1, 0, RALPH_INFINITY, -3.0, 'C');  /* x (min => negate) */
    ralph_add_var(model1, 0, RALPH_INFINITY, -2.0, 'C');  /* y */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_add_constraint(model1, 2, idx1, val1, 'L', 10.0);
    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};
    ralph_add_constraint(model1, 2, idx2, val2, 'L', 14.0);

    /* Solve without supernodal */
    ralph_optimize(model1);
    double obj1 = ralph_get_objval(model1);
    int status1 = ralph_get_status(model1);

    /* Solve with supernodal */
    RalphModel *model2 = ralph_create();
    ralph_add_var(model2, 0, RALPH_INFINITY, -3.0, 'C');
    ralph_add_var(model2, 0, RALPH_INFINITY, -2.0, 'C');
    ralph_add_constraint(model2, 2, idx1, val1, 'L', 10.0);
    ralph_add_constraint(model2, 2, idx2, val2, 'L', 14.0);
    ralph_set_int_param(model2, "lu_supernode", 1);
    ralph_optimize(model2);
    double obj2 = ralph_get_objval(model2);
    int status2 = ralph_get_status(model2);

    ASSERT_INT_EQ(status1, RALPH_STATUS_OPTIMAL, "integration LP: status1 optimal");
    ASSERT_INT_EQ(status2, RALPH_STATUS_OPTIMAL, "integration LP: status2 optimal");
    ASSERT_NEAR(obj1, obj2, 1e-6, "integration LP: obj values match");
    ASSERT_NEAR(obj1, -24.0, 1e-6, "integration LP: obj = -24 (minimization)");

    ralph_free(model1);
    ralph_free(model2);
}

/* Solve a medium LP with both paths and compare */
static void test_integration_medium_lp(void) {
    printf("  Phase 4: integration medium LP...\n");

    /* Build a larger LP: 20 vars, 15 constraints */
    int nvars = 20, ncons = 15;

    RalphModel *model1 = ralph_create();
    RalphModel *model2 = ralph_create();

    /* Add variables with random costs and bounds */
    unsigned int seed = 123;
    for (int j = 0; j < nvars; j++) {
        seed = seed * 1103515245 + 12345;
        double cost = ((double)((seed >> 16) & 0x7fff) / 32768.0) * 10.0 - 5.0;
        ralph_add_var(model1, 0, 100.0, cost, 'C');
        ralph_add_var(model2, 0, 100.0, cost, 'C');
    }

    /* Add constraints with random coefficients */
    for (int i = 0; i < ncons; i++) {
        int nnz = 5 + (i % 4);  /* 5-8 nonzeros per constraint */
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

        ralph_add_constraint(model1, nnz, idx, val, 'L', rhs);
        ralph_add_constraint(model2, nnz, idx, val, 'L', rhs);

        free(idx);
        free(val);
    }

    ralph_optimize(model1);
    double obj1 = ralph_get_objval(model1);
    int status1 = ralph_get_status(model1);

    ralph_set_int_param(model2, "lu_supernode", 1);
    ralph_optimize(model2);
    double obj2 = ralph_get_objval(model2);
    int status2 = ralph_get_status(model2);

    ASSERT_INT_EQ(status1, status2, "integration medium: same status");
    if (status1 == RALPH_STATUS_OPTIMAL && status2 == RALPH_STATUS_OPTIMAL) {
        ASSERT_NEAR(obj1, obj2, 1e-4, "integration medium: obj values match");
    }

    ralph_free(model1);
    ralph_free(model2);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== Supernodal LU Factorization Tests (T2.1) ===\n\n");

    printf("Phase 1: Elimination Tree + Supernode Detection\n");
    test_etree_3x3();
    test_etree_tridiag();
    test_postorder_chain();
    test_supernodes_chain();
    test_supernodes_large();

    printf("\nPhase 2: Dense Micro-Kernels\n");
    test_dgemm_basic();
    test_dgemm_nonmultiple();
    test_dtrsm_basic();
    test_block_factor_basic();

    printf("\nPhase 3: Supernodal Numeric Factorization\n");
    test_sn_factorize_known_matrix();
    test_sn_factorize_random();

    printf("\nPhase 4: Integration (A/B Comparison)\n");
    test_integration_small_lp();
    test_integration_medium_lp();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
