/*
 * Ralph - Supernodal LU Factorization (T2.1)
 *
 * Phase 1: Elimination tree + supernode detection
 * Phase 2: Dense micro-kernels (4x4 tiled GEMM, TRSM, block factor)
 * Phase 3: Supernodal numeric factorization
 *
 * All functions operate on row-major dense submatrices extracted from
 * the structural portion of an LP basis matrix.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lu_supernode.h"
#include "lp.h"

/* ============================================================================
 * Phase 1: Elimination Tree + Supernode Detection
 * ============================================================================ */

/*
 * Build column elimination tree from the m x k dense row-major matrix.
 *
 * For each column j (0..k-1), etree_parent[j] = smallest j' > j such that
 * L[j',j] != 0 in the Cholesky factor of A'A (or equivalently, the first
 * sub-diagonal nonzero in column j of L in LU of A).
 *
 * For a non-symmetric matrix, we approximate by looking at the nonzero
 * pattern: for each row i that has a nonzero in column j, the first column
 * j' > j with a nonzero in row i is a candidate parent.
 *
 * row_perm maps logical row positions to original rows in A_struct:
 *   A_struct[row_perm[i] * k + j] = entry at logical row i, column j
 */
int sn_build_etree(const double *A_struct, int m, int k,
                   const int *row_perm, int *etree_parent) {
    if (!A_struct || !row_perm || !etree_parent || k <= 0 || m <= 0) return -1;

    /* Initialize all parents to -1 (root) */
    for (int j = 0; j < k; j++) {
        etree_parent[j] = -1;
    }

    /* Union-find ancestor array for path compression.
     * ancestor[j] = representative of j's set in the forest built so far.
     * Invariant: ancestor[j] == j for set representatives. */
    int *ancestor = (int *)calloc(k, sizeof(int));
    if (!ancestor) return -1;
    for (int j = 0; j < k; j++) ancestor[j] = j;

    /* Standard column etree via Liu's algorithm (1990):
     * Process columns left to right. For each column j, look at all rows
     * with nonzeros in column j. For each such row, find the FIRST (leftmost)
     * nonzero column — that column's tree root becomes a child of j. */
    for (int j = 0; j < k; j++) {
        /* For each row that has a nonzero in column j */
        for (int i = 0; i < m; i++) {
            int orig_row = row_perm[i];
            if (fabs(A_struct[(size_t)orig_row * k + j]) <= RALPH_ZERO_TOL) continue;

            /* Find the first nonzero column in this row (before j) */
            for (int j2 = 0; j2 < j; j2++) {
                if (fabs(A_struct[(size_t)orig_row * k + j2]) <= RALPH_ZERO_TOL) continue;

                /* j2 has a nonzero in the same row as j, with j2 < j.
                 * Find root of j2's tree using path compression. */
                int r = j2;
                while (ancestor[r] != r) r = ancestor[r];
                /* Path compression */
                int c = j2;
                while (c != r) { int next = ancestor[c]; ancestor[c] = r; c = next; }

                if (r != j) {
                    etree_parent[r] = j;
                    ancestor[r] = j;
                }
                break; /* Only need the first column < j in this row */
            }
        }
    }

    free(ancestor);
    return 0;
}

/*
 * Postorder traversal: children before parent.
 * Output: postorder[0..k-1] contains column indices in postorder.
 *
 * Uses iterative DFS to avoid stack overflow on deep trees.
 */
int sn_etree_postorder(const int *etree_parent, int k, int *postorder) {
    if (!etree_parent || !postorder || k <= 0) return -1;

    /* Build child lists */
    int *first_child = (int *)calloc(k, sizeof(int));
    int *next_sibling = (int *)calloc(k, sizeof(int));
    if (!first_child || !next_sibling) {
        free(first_child);
        free(next_sibling);
        return -1;
    }

    for (int j = 0; j < k; j++) {
        first_child[j] = -1;
        next_sibling[j] = -1;
    }

    /* Link children: iterate in reverse so first_child list is in order */
    for (int j = k - 1; j >= 0; j--) {
        int p = etree_parent[j];
        if (p >= 0 && p < k) {
            next_sibling[j] = first_child[p];
            first_child[p] = j;
        }
    }

    /* Iterative DFS with explicit stack.
     * Stack needs up to 2*k entries: each node can be on the stack simultaneously
     * with all its children (parent stays while children are pushed on top). */
    int *stack = (int *)calloc(2 * k, sizeof(int));
    int *visited = (int *)calloc(k, sizeof(int));
    if (!stack || !visited) {
        free(first_child);
        free(next_sibling);
        free(stack);
        free(visited);
        return -1;
    }

    int po_idx = 0;
    /* Process all roots (nodes with parent == -1 or parent >= k) */
    for (int root = 0; root < k; root++) {
        int p = etree_parent[root];
        if (p >= 0 && p < k) continue; /* Not a root */

        int top = 0;
        stack[top++] = root;

        while (top > 0) {
            int node = stack[top - 1];

            if (!visited[node]) {
                visited[node] = 1;
                /* Push children directly from linked list.
                 * Children are already in forward order (built in reverse in the
                 * linking step above), so pushing them in forward order means
                 * the LAST child ends up on top of stack — processed first.
                 * This gives right-to-left DFS, which is fine for postorder. */
                int c = first_child[node];
                while (c >= 0) {
                    if (top >= 2 * k) break; /* Safety: stack bounds check */
                    stack[top++] = c;
                    c = next_sibling[c];
                }
            } else {
                /* All children processed, emit this node */
                top--;
                postorder[po_idx++] = node;
            }
        }
    }

    /* If tree is disconnected, ensure all nodes are covered */
    if (po_idx < k) {
        for (int j = 0; j < k; j++) {
            if (!visited[j]) {
                postorder[po_idx++] = j;
            }
        }
    }

    free(first_child);
    free(next_sibling);
    free(stack);
    free(visited);
    return 0;
}

/*
 * Detect fundamental supernodes with relaxed merging.
 *
 * A fundamental supernode is a maximal set of consecutive columns
 * {j, j+1, ..., j+s-1} where:
 * - etree_parent[j+i] = j+i+1 for i < s-1 (chain in etree)
 * - The nonzero row patterns of L columns are nested
 *
 * Relaxed merging allows up to SN_RELAX_ZEROS extra fill-in zeros
 * when combining adjacent supernodes, capped at SN_MAX_BLOCK width.
 */
int sn_detect_supernodes(const double *A_struct, int m, int k,
                         const int *row_perm, const int *etree_parent,
                         const int *postorder,
                         Supernode **out, int *num_out) {
    if (!A_struct || !row_perm || !etree_parent || !postorder || !out || !num_out)
        return -1;
    if (k <= 0) {
        *out = NULL;
        *num_out = 0;
        return 0;
    }

    /* Count nonzeros below diagonal for each column (L-pattern) */
    int *col_nnz_below = (int *)calloc(k, sizeof(int));
    if (!col_nnz_below) return -1;

    for (int j = 0; j < k; j++) {
        int nnz = 0;
        for (int i = j + 1; i < m; i++) {
            int orig_row = row_perm[i];
            if (fabs(A_struct[(size_t)orig_row * k + j]) > RALPH_ZERO_TOL) {
                nnz++;
            }
        }
        col_nnz_below[j] = nnz;
    }

    /* Allocate maximum possible supernodes (one per column) */
    Supernode *snodes = (Supernode *)calloc(k, sizeof(Supernode));
    if (!snodes) { free(col_nnz_below); return -1; }

    int nsn = 0;

    /* Walk columns in natural order, grouping into supernodes */
    int j = 0;
    while (j < k) {
        int start = j;
        int size = 1;

        /* Try to extend supernode by merging consecutive columns */
        while (j + size < k && size < SN_MAX_BLOCK) {
            int prev = j + size - 1;
            int next = j + size;

            /* Check etree chain: parent[prev] == next */
            if (etree_parent[prev] != next) break;

            /* Check nested L-pattern: nnz should decrease by ~1 each column.
             * Allow relaxation: extra zeros <= SN_RELAX_ZEROS */
            int expected_nnz = col_nnz_below[prev] - 1;
            int actual_nnz = col_nnz_below[next];
            int extra_zeros = expected_nnz - actual_nnz;
            if (extra_zeros < 0) extra_zeros = -extra_zeros;

            if (extra_zeros > SN_RELAX_ZEROS) break;

            size++;
        }

        snodes[nsn].start = start;
        snodes[nsn].size = size;
        nsn++;

        j += size;
    }

    /* Shrink allocation */
    if (nsn < k) {
        Supernode *trimmed = (Supernode *)realloc(snodes, nsn * sizeof(Supernode));
        if (trimmed) snodes = trimmed;
    }

    free(col_nnz_below);

    *out = snodes;
    *num_out = nsn;
    return 0;
}

/*
 * Top-level symbolic analysis: builds etree, postorders, detects supernodes.
 */
SNSymbolic *sn_analyze(const double *A_struct, int m, int k,
                       const int *row_perm) {
    if (!A_struct || !row_perm || k <= 0 || m <= 0) return NULL;

    SNSymbolic *sym = (SNSymbolic *)calloc(1, sizeof(SNSymbolic));
    if (!sym) return NULL;

    sym->k = k;
    sym->m = m;

    /* Allocate etree arrays */
    sym->etree_parent = (int *)calloc(k, sizeof(int));
    sym->etree_postorder = (int *)calloc(k, sizeof(int));
    if (!sym->etree_parent || !sym->etree_postorder) {
        sn_symbolic_free(sym);
        return NULL;
    }

    /* Build etree */
    if (sn_build_etree(A_struct, m, k, row_perm, sym->etree_parent) < 0) {
        sn_symbolic_free(sym);
        return NULL;
    }

    /* Postorder */
    if (sn_etree_postorder(sym->etree_parent, k, sym->etree_postorder) < 0) {
        sn_symbolic_free(sym);
        return NULL;
    }

    /* Detect supernodes */
    if (sn_detect_supernodes(A_struct, m, k, row_perm,
                             sym->etree_parent, sym->etree_postorder,
                             &sym->supernodes, &sym->num_supernodes) < 0) {
        sn_symbolic_free(sym);
        return NULL;
    }

    /* Compute max sizes for workspace sizing */
    sym->max_supernode_size = 0;
    sym->max_panel_rows = 0;
    for (int s = 0; s < sym->num_supernodes; s++) {
        if (sym->supernodes[s].size > sym->max_supernode_size)
            sym->max_supernode_size = sym->supernodes[s].size;
        int panel_rows = m - sym->supernodes[s].start;
        if (panel_rows > sym->max_panel_rows)
            sym->max_panel_rows = panel_rows;
    }

    return sym;
}

void sn_symbolic_free(SNSymbolic *sym) {
    if (!sym) return;
    free(sym->etree_parent);
    free(sym->etree_postorder);
    free(sym->supernodes);
    free(sym);
}

/* ============================================================================
 * Phase 2: Dense Micro-Kernels
 * ============================================================================
 *
 * WASM-safe: no SIMD intrinsics. Structured for compiler auto-vectorization
 * with -ffast-math -O3. All matrices are row-major.
 */

/*
 * GEMM update: C -= A * B
 *
 * A is panel_rows x block_size (lda stride)
 * B is block_size x update_cols (ldb stride)
 * C is panel_rows x update_cols (ldc stride)
 *
 * Uses 4x4 micro-kernel for the inner loop.
 */
void sn_dgemm_update(int panel_rows, int block_size, int update_cols,
                     const double *A, int lda,
                     const double *B, int ldb,
                     double *C, int ldc) {
    /* Main 4x4 tiled loop */
    int i;
    for (i = 0; i + 3 < panel_rows; i += 4) {
        int j;
        for (j = 0; j + 3 < update_cols; j += 4) {
            /* 4x4 micro-kernel: accumulate C[i..i+3][j..j+3] -= A * B */
            double c00 = 0, c01 = 0, c02 = 0, c03 = 0;
            double c10 = 0, c11 = 0, c12 = 0, c13 = 0;
            double c20 = 0, c21 = 0, c22 = 0, c23 = 0;
            double c30 = 0, c31 = 0, c32 = 0, c33 = 0;

            for (int p = 0; p < block_size; p++) {
                double a0 = A[(i + 0) * lda + p];
                double a1 = A[(i + 1) * lda + p];
                double a2 = A[(i + 2) * lda + p];
                double a3 = A[(i + 3) * lda + p];

                double b0 = B[p * ldb + (j + 0)];
                double b1 = B[p * ldb + (j + 1)];
                double b2 = B[p * ldb + (j + 2)];
                double b3 = B[p * ldb + (j + 3)];

                c00 += a0 * b0; c01 += a0 * b1; c02 += a0 * b2; c03 += a0 * b3;
                c10 += a1 * b0; c11 += a1 * b1; c12 += a1 * b2; c13 += a1 * b3;
                c20 += a2 * b0; c21 += a2 * b1; c22 += a2 * b2; c23 += a2 * b3;
                c30 += a3 * b0; c31 += a3 * b1; c32 += a3 * b2; c33 += a3 * b3;
            }

            C[(i + 0) * ldc + (j + 0)] -= c00;
            C[(i + 0) * ldc + (j + 1)] -= c01;
            C[(i + 0) * ldc + (j + 2)] -= c02;
            C[(i + 0) * ldc + (j + 3)] -= c03;
            C[(i + 1) * ldc + (j + 0)] -= c10;
            C[(i + 1) * ldc + (j + 1)] -= c11;
            C[(i + 1) * ldc + (j + 2)] -= c12;
            C[(i + 1) * ldc + (j + 3)] -= c13;
            C[(i + 2) * ldc + (j + 0)] -= c20;
            C[(i + 2) * ldc + (j + 1)] -= c21;
            C[(i + 2) * ldc + (j + 2)] -= c22;
            C[(i + 2) * ldc + (j + 3)] -= c23;
            C[(i + 3) * ldc + (j + 0)] -= c30;
            C[(i + 3) * ldc + (j + 1)] -= c31;
            C[(i + 3) * ldc + (j + 2)] -= c32;
            C[(i + 3) * ldc + (j + 3)] -= c33;
        }

        /* Cleanup columns (j remainder) */
        for (; j < update_cols; j++) {
            for (int ii = i; ii < i + 4; ii++) {
                double sum = 0.0;
                for (int p = 0; p < block_size; p++) {
                    sum += A[ii * lda + p] * B[p * ldb + j];
                }
                C[ii * ldc + j] -= sum;
            }
        }
    }

    /* Cleanup rows (i remainder) */
    for (; i < panel_rows; i++) {
        for (int j = 0; j < update_cols; j++) {
            double sum = 0.0;
            for (int p = 0; p < block_size; p++) {
                sum += A[i * lda + p] * B[p * ldb + j];
            }
            C[i * ldc + j] -= sum;
        }
    }
}

/*
 * Scattered-row GEMM update:
 *   A_struct[row_perm[row_base + i], col_base + j] -= A * B
 *
 * This avoids copying the trailing Schur block into a temporary dense C block
 * and then copying it back after the update.
 */
static void sn_dgemm_update_scattered_rows(int panel_rows, int block_size, int update_cols,
                                           const double *A, int lda,
                                           const double *B, int ldb,
                                           double *A_struct, int k,
                                           const int *row_perm,
                                           int row_base, int col_base) {
    int i;
    for (i = 0; i + 3 < panel_rows; i += 4) {
        double *c0 = A_struct + (size_t)row_perm[row_base + i + 0] * (size_t)k + col_base;
        double *c1 = A_struct + (size_t)row_perm[row_base + i + 1] * (size_t)k + col_base;
        double *c2 = A_struct + (size_t)row_perm[row_base + i + 2] * (size_t)k + col_base;
        double *c3 = A_struct + (size_t)row_perm[row_base + i + 3] * (size_t)k + col_base;
        int j;

        for (j = 0; j + 3 < update_cols; j += 4) {
            double c00 = 0, c01 = 0, c02 = 0, c03 = 0;
            double c10 = 0, c11 = 0, c12 = 0, c13 = 0;
            double c20 = 0, c21 = 0, c22 = 0, c23 = 0;
            double c30 = 0, c31 = 0, c32 = 0, c33 = 0;

            for (int p = 0; p < block_size; p++) {
                double a0 = A[(i + 0) * lda + p];
                double a1 = A[(i + 1) * lda + p];
                double a2 = A[(i + 2) * lda + p];
                double a3 = A[(i + 3) * lda + p];

                double b0 = B[p * ldb + (j + 0)];
                double b1 = B[p * ldb + (j + 1)];
                double b2 = B[p * ldb + (j + 2)];
                double b3 = B[p * ldb + (j + 3)];

                c00 += a0 * b0; c01 += a0 * b1; c02 += a0 * b2; c03 += a0 * b3;
                c10 += a1 * b0; c11 += a1 * b1; c12 += a1 * b2; c13 += a1 * b3;
                c20 += a2 * b0; c21 += a2 * b1; c22 += a2 * b2; c23 += a2 * b3;
                c30 += a3 * b0; c31 += a3 * b1; c32 += a3 * b2; c33 += a3 * b3;
            }

            c0[j + 0] -= c00; c0[j + 1] -= c01; c0[j + 2] -= c02; c0[j + 3] -= c03;
            c1[j + 0] -= c10; c1[j + 1] -= c11; c1[j + 2] -= c12; c1[j + 3] -= c13;
            c2[j + 0] -= c20; c2[j + 1] -= c21; c2[j + 2] -= c22; c2[j + 3] -= c23;
            c3[j + 0] -= c30; c3[j + 1] -= c31; c3[j + 2] -= c32; c3[j + 3] -= c33;
        }

        for (; j < update_cols; j++) {
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            for (int p = 0; p < block_size; p++) {
                double b = B[p * ldb + j];
                s0 += A[(i + 0) * lda + p] * b;
                s1 += A[(i + 1) * lda + p] * b;
                s2 += A[(i + 2) * lda + p] * b;
                s3 += A[(i + 3) * lda + p] * b;
            }
            c0[j] -= s0;
            c1[j] -= s1;
            c2[j] -= s2;
            c3[j] -= s3;
        }
    }

    for (; i < panel_rows; i++) {
        double *c = A_struct + (size_t)row_perm[row_base + i] * (size_t)k + col_base;
        for (int j = 0; j < update_cols; j++) {
            double sum = 0.0;
            for (int p = 0; p < block_size; p++) {
                sum += A[i * lda + p] * B[p * ldb + j];
            }
            c[j] -= sum;
        }
    }
}

/*
 * Scattered-row/column GEMM update:
 *   A_struct[row_perm[row_base + row_idx[i]], col_base + col_idx[j]] -= A * B
 *
 * Used when the supernode Schur update is sparse in either the trailing rows or
 * trailing columns. This keeps the arithmetic exact while avoiding dense work
 * over structurally zero update regions.
 */
static void sn_dgemm_update_scattered_rows_cols(int panel_rows, int block_size, int update_cols,
                                                const double *A, int lda,
                                                const double *B, int ldb,
                                                double *A_struct, int k,
                                                const int *row_perm,
                                                const int *row_idx, int row_base,
                                                const int *col_idx, int col_base) {
    int i;
    for (i = 0; i + 3 < panel_rows; i += 4) {
        double *c0 = A_struct + (size_t)row_perm[row_base + row_idx[i + 0]] * (size_t)k + col_base;
        double *c1 = A_struct + (size_t)row_perm[row_base + row_idx[i + 1]] * (size_t)k + col_base;
        double *c2 = A_struct + (size_t)row_perm[row_base + row_idx[i + 2]] * (size_t)k + col_base;
        double *c3 = A_struct + (size_t)row_perm[row_base + row_idx[i + 3]] * (size_t)k + col_base;
        int j;

        for (j = 0; j + 3 < update_cols; j += 4) {
            int cj0 = col_idx[j + 0];
            int cj1 = col_idx[j + 1];
            int cj2 = col_idx[j + 2];
            int cj3 = col_idx[j + 3];
            double c00 = 0, c01 = 0, c02 = 0, c03 = 0;
            double c10 = 0, c11 = 0, c12 = 0, c13 = 0;
            double c20 = 0, c21 = 0, c22 = 0, c23 = 0;
            double c30 = 0, c31 = 0, c32 = 0, c33 = 0;

            for (int p = 0; p < block_size; p++) {
                double a0 = A[(i + 0) * lda + p];
                double a1 = A[(i + 1) * lda + p];
                double a2 = A[(i + 2) * lda + p];
                double a3 = A[(i + 3) * lda + p];

                double b0 = B[p * ldb + (j + 0)];
                double b1 = B[p * ldb + (j + 1)];
                double b2 = B[p * ldb + (j + 2)];
                double b3 = B[p * ldb + (j + 3)];

                c00 += a0 * b0; c01 += a0 * b1; c02 += a0 * b2; c03 += a0 * b3;
                c10 += a1 * b0; c11 += a1 * b1; c12 += a1 * b2; c13 += a1 * b3;
                c20 += a2 * b0; c21 += a2 * b1; c22 += a2 * b2; c23 += a2 * b3;
                c30 += a3 * b0; c31 += a3 * b1; c32 += a3 * b2; c33 += a3 * b3;
            }

            c0[cj0] -= c00; c0[cj1] -= c01; c0[cj2] -= c02; c0[cj3] -= c03;
            c1[cj0] -= c10; c1[cj1] -= c11; c1[cj2] -= c12; c1[cj3] -= c13;
            c2[cj0] -= c20; c2[cj1] -= c21; c2[cj2] -= c22; c2[cj3] -= c23;
            c3[cj0] -= c30; c3[cj1] -= c31; c3[cj2] -= c32; c3[cj3] -= c33;
        }

        for (; j < update_cols; j++) {
            int cj = col_idx[j];
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            for (int p = 0; p < block_size; p++) {
                double b = B[p * ldb + j];
                s0 += A[(i + 0) * lda + p] * b;
                s1 += A[(i + 1) * lda + p] * b;
                s2 += A[(i + 2) * lda + p] * b;
                s3 += A[(i + 3) * lda + p] * b;
            }
            c0[cj] -= s0;
            c1[cj] -= s1;
            c2[cj] -= s2;
            c3[cj] -= s3;
        }
    }

    for (; i < panel_rows; i++) {
        double *c = A_struct + (size_t)row_perm[row_base + row_idx[i]] * (size_t)k + col_base;
        for (int j = 0; j < update_cols; j++) {
            int cj = col_idx[j];
            double sum = 0.0;
            for (int p = 0; p < block_size; p++) {
                sum += A[i * lda + p] * B[p * ldb + j];
            }
            c[cj] -= sum;
        }
    }
}

/*
 * TRSM: Solve L * X = B in-place (B overwritten with X).
 *
 * L is block_size x block_size unit lower triangular (row-major, ldl stride).
 * B is block_size x ncols (row-major, ldb stride).
 *
 * Forward substitution: for each row i, B[i,:] -= sum(L[i,j]*B[j,:] for j<i)
 */
void sn_dtrsm_lower(int block_size, int ncols,
                    const double *L, int ldl, double *B, int ldb) {
    for (int i = 1; i < block_size; i++) {
        for (int j = 0; j < i; j++) {
            double lij = L[i * ldl + j];
            if (fabs(lij) < 1e-15) continue;

            /* B[i,:] -= lij * B[j,:] */
            for (int c = 0; c < ncols; c++) {
                B[i * ldb + c] -= lij * B[j * ldb + c];
            }
        }
    }
}

/*
 * Block LU factorization with partial pivoting.
 *
 * Factors panel[0..panel_rows-1][0..block_size-1] in-place.
 * After factorization:
 *   - Upper triangle of panel[0..block_size-1][0..block_size-1] contains U
 *   - Below-diagonal of column j contains L multipliers (NOT unit diagonal)
 *   - pivot_indices[j] = row swapped with row j (0-indexed relative to panel start)
 *
 * Returns 0 on success, -1 if a pivot is below pivot_tol (singular).
 */
int sn_block_factor(int panel_rows, int block_size,
                    double *panel, int ldp,
                    int *pivot_indices, double pivot_tol) {
    for (int j = 0; j < block_size; j++) {
        /* Find pivot in column j, rows j..panel_rows-1 */
        int best_row = j;
        double best_val = fabs(panel[j * ldp + j]);

        for (int i = j + 1; i < panel_rows; i++) {
            double val = fabs(panel[i * ldp + j]);
            if (val > best_val) {
                best_val = val;
                best_row = i;
            }
        }

        pivot_indices[j] = best_row;

        if (best_val < pivot_tol) {
            return -1; /* Singular */
        }

        /* Swap rows j and best_row in the full panel width */
        if (best_row != j) {
            for (int c = 0; c < ldp; c++) {
                double tmp = panel[j * ldp + c];
                panel[j * ldp + c] = panel[best_row * ldp + c];
                panel[best_row * ldp + c] = tmp;
            }
        }

        /* Compute multipliers and eliminate */
        double pivot = panel[j * ldp + j];
        double inv_pivot = 1.0 / pivot;

        for (int i = j + 1; i < panel_rows; i++) {
            double mult = panel[i * ldp + j] * inv_pivot;
            panel[i * ldp + j] = mult; /* Store L multiplier */

            /* Update remaining columns of this row */
            for (int c = j + 1; c < block_size; c++) {
                panel[i * ldp + c] -= mult * panel[j * ldp + c];
            }
        }
    }
    return 0;
}

/* ============================================================================
 * Phase 3: Supernodal Numeric Factorization
 * ============================================================================
 *
 * Processes supernodes in order. For each supernode:
 * 1. sn_block_factor() — panel LU with partial pivoting
 * 2. sn_dtrsm_lower()  — compute U panel (columns right of supernode)
 * 3. sn_dgemm_update() — Schur complement update
 * 4. Emit L/U entries into COO arrays
 */

int sn_factorize(double *A_struct, int m, int k,
                 int *row_perm, int *row_pos, double pivot_tol,
                 const int *row_reserved,
                 const Supernode *supernodes, int num_supernodes,
                 const int *redundant_rows, int num_redundant,
                 int allow_regularization, int max_regularizations,
                 int *num_regularized,
                 int *L_row, int *L_col, double *L_val, int *L_nnz,
                 int L_capacity,
                 int *U_row, int *U_col, double *U_val, int *U_nnz,
                 int U_capacity,
                 double *work, size_t work_capacity,
                 SNSupernodeWork *stats) {
    int rc = 0;
    unsigned char *row_active_orig_buf = NULL;
    unsigned char *col_active_local_buf = NULL;
    int *touched_rows_buf = NULL;
    int *touched_cols_buf = NULL;
    int *active_rows_buf = NULL;
    int *active_cols_buf = NULL;

    if (!A_struct || !row_perm || !row_pos || !supernodes ||
        !L_row || !L_col || !L_val || !L_nnz ||
        !U_row || !U_col || !U_val || !U_nnz ||
        L_capacity <= 0 || U_capacity <= 0)
        return -1;

    *L_nnz = 0;
    *U_nnz = 0;
    if (num_regularized) *num_regularized = 0;
    if (stats) memset(stats, 0, sizeof(*stats));

    if (m > 0 && k > 0) {
        row_active_orig_buf = (unsigned char *)calloc((size_t)m, sizeof(unsigned char));
        col_active_local_buf = (unsigned char *)calloc((size_t)k, sizeof(unsigned char));
        touched_rows_buf = (int *)malloc((size_t)m * sizeof(int));
        touched_cols_buf = (int *)malloc((size_t)k * sizeof(int));
        active_rows_buf = (int *)malloc((size_t)m * sizeof(int));
        active_cols_buf = (int *)malloc((size_t)k * sizeof(int));
        if (!row_active_orig_buf || !col_active_local_buf ||
            !touched_rows_buf || !touched_cols_buf ||
            !active_rows_buf || !active_cols_buf) {
            rc = -1;
            goto cleanup;
        }
    }

    /* Bounds-check macro for COO array writes */
    #define SN_EMIT_L(r, c, v) do { \
        if (*L_nnz >= L_capacity) { rc = -1; goto cleanup; } \
        L_row[*L_nnz] = (r); L_col[*L_nnz] = (c); L_val[*L_nnz] = (v); \
        (*L_nnz)++; \
    } while(0)
    #define SN_EMIT_U(r, c, v) do { \
        if (*U_nnz >= U_capacity) { rc = -1; goto cleanup; } \
        U_row[*U_nnz] = (r); U_col[*U_nnz] = (c); U_val[*U_nnz] = (v); \
        (*U_nnz)++; \
    } while(0)

    /* Process each supernode */
    for (int s = 0; s < num_supernodes; s++) {
        int sn_start = supernodes[s].start;
        int sn_size = supernodes[s].size;
        int panel_rows = m - sn_start;
        int trailing_rows = m - (sn_start + sn_size);
        int trailing_cols = k - (sn_start + sn_size);
        unsigned char *row_active_orig = row_active_orig_buf;
        unsigned char *col_active_local = col_active_local_buf;
        int *touched_rows = touched_rows_buf;
        int *touched_cols = touched_cols_buf;
        int *active_rows = active_rows_buf;
        int *active_cols = active_cols_buf;
        int touched_row_count = 0;
        int touched_col_count = 0;

        if (panel_rows <= 0 || sn_size <= 0) continue;

        if (!(trailing_rows > 0 && trailing_cols > 0)) {
            row_active_orig = NULL;
            col_active_local = NULL;
            touched_rows = NULL;
            touched_cols = NULL;
            active_rows = NULL;
            active_cols = NULL;
        }

        /* ---- Step 1: Block factor the panel ---- */
        /* The panel is A_struct[row_perm[sn_start..m-1], sn_start..sn_start+sn_size-1]
         * We work on the submatrix starting at logical row sn_start, column sn_start,
         * which in the row-major layout is:
         *   A_struct[row_perm[i] * k + sn_start] for i = sn_start..m-1
         *
         * We need to build a contiguous panel for block_factor.
         * However, since A_struct is row-major with stride k, we can use it directly
         * by pointing into the right offset, BUT the rows aren't contiguous
         * because row_perm scrambles them.
         *
         * Strategy: work directly on A_struct using row_perm indirection.
         * This avoids copying but means block_factor needs to handle row_perm.
         *
         * Actually, for simplicity and correctness, we do column-by-column
         * elimination within the supernode, which is equivalent to block_factor
         * but respects the row_perm indirection. This is the same as the existing
         * column-by-column path but processes sn_size columns at once before
         * doing the GEMM update on remaining columns.
         */

        for (int j_local = 0; j_local < sn_size; j_local++) {
            int step = sn_start + j_local;

            /* Prefer non-reserved rows, but allow reserved rows when they are
             * materially stronger pivots (numerical safety). */
            int pivot_row = -1;
            double max_val = 0.0;
            int alt_row = -1;
            double alt_val = 0.0;

            for (int i = step; i < m; i++) {
                int orig_row = row_perm[i];
                double val = fabs(A_struct[(size_t)orig_row * k + step]);
                if (val > max_val) {
                    max_val = val;
                    pivot_row = i;
                }
                if (row_reserved && row_reserved[orig_row]) continue;
                if (val > alt_val) {
                    alt_val = val;
                    alt_row = i;
                }
            }
            if (alt_row >= 0 && alt_val >= 0.1 * max_val) {
                pivot_row = alt_row;
                max_val = alt_val;
            }

            if (max_val < pivot_tol) {
                /* Try regularization */
                int can_regularize = 0;

                if (redundant_rows && num_redundant > 0) {
                    for (int i = step; i < m; i++) {
                        int orig_row = row_perm[i];
                        if (row_reserved && row_reserved[orig_row]) continue;
                        if (redundant_rows[orig_row]) {
                            can_regularize = 1;
                            pivot_row = i;
                            break;
                        }
                    }
                    for (int i = step; i < m; i++) {
                        if (can_regularize) break;
                        int orig_row = row_perm[i];
                        if (redundant_rows[orig_row]) {
                            can_regularize = 1;
                            pivot_row = i;
                            break;
                        }
                    }
                }

                if (!can_regularize && allow_regularization &&
                    num_regularized && *num_regularized < max_regularizations) {
                    can_regularize = 1;
                    pivot_row = step;
                    for (int i = step; i < m; i++) {
                        int orig_row = row_perm[i];
                        if (row_reserved && row_reserved[orig_row]) continue;
                        pivot_row = i;
                        break;
                    }
                }

                if (can_regularize) {
                    if (num_regularized) (*num_regularized)++;
                    if (pivot_row != step) {
                        int a = row_perm[step], b = row_perm[pivot_row];
                        row_perm[step] = b; row_perm[pivot_row] = a;
                        row_pos[b] = step; row_pos[a] = pivot_row;
                        pivot_row = step; /* Prevent double-swap below */
                    }
                    int piv_orig = row_perm[step];
                    A_struct[(size_t)piv_orig * k + step] = 1.0;
                    max_val = 1.0;
                } else {
                    rc = -1; /* Singular, fall back */
                    goto cleanup;
                }
            }

            /* Swap rows */
            if (pivot_row != step) {
                int a = row_perm[step], b = row_perm[pivot_row];
                row_perm[step] = b; row_perm[pivot_row] = a;
                row_pos[b] = step; row_pos[a] = pivot_row;
            }

            int piv_orig = row_perm[step];
            double pivot_val = A_struct[(size_t)piv_orig * k + step];

            /* Store L diagonal */
            SN_EMIT_L(piv_orig, step, 1.0);

            /* Store U row for columns within supernode (step..sn_start+sn_size-1) */
            for (int jj = step; jj < sn_start + sn_size; jj++) {
                double val = A_struct[(size_t)piv_orig * k + jj];
                if (fabs(val) > RALPH_ZERO_TOL || jj == step) {
                    SN_EMIT_U(step, jj, val);
                }
            }

            /* Compute multipliers and eliminate within supernode columns only */
            for (int i = step + 1; i < m; i++) {
                int row_orig = row_perm[i];
                double a_ik = A_struct[(size_t)row_orig * k + step];

                if (fabs(a_ik) < RALPH_ZERO_TOL) continue;

                double mult = a_ik / pivot_val;

                /* Store multiplier back in A_struct for GEMM extraction */
                A_struct[(size_t)row_orig * k + step] = mult;
                if (row_active_orig && !row_active_orig[row_orig]) {
                    row_active_orig[row_orig] = 1;
                    touched_rows[touched_row_count++] = row_orig;
                }

                /* Store L multiplier */
                SN_EMIT_L(row_orig, step, mult);

                /* Update remaining columns within supernode */
                for (int jj = step + 1; jj < sn_start + sn_size; jj++) {
                    A_struct[(size_t)row_orig * k + jj] -= mult * A_struct[(size_t)piv_orig * k + jj];
                }
            }
        }

        /* ---- Step 2: Store U entries for columns right of supernode ---- */
        /* For each row in the supernode (step sn_start..sn_start+sn_size-1),
         * emit U entries for columns sn_start+sn_size..k-1 */
        for (int j_local = 0; j_local < sn_size; j_local++) {
            int step = sn_start + j_local;
            int piv_orig = row_perm[step];

            for (int jj = sn_start + sn_size; jj < k; jj++) {
                double val = A_struct[(size_t)piv_orig * k + jj];
                if (fabs(val) > RALPH_ZERO_TOL) {
                    if (col_active_local) {
                        int col_local = jj - (sn_start + sn_size);
                        if (!col_active_local[col_local]) {
                            col_active_local[col_local] = 1;
                            touched_cols[touched_col_count++] = col_local;
                        }
                    }
                    SN_EMIT_U(step, jj, val);
                }
            }
        }

        /* ---- Step 3: Schur complement update using GEMM ---- */
        /* C[i, j] -= L[i, sn] * U[sn, j]
         * where i = sn_start+sn_size..m-1, j = sn_start+sn_size..k-1
         *
         * L[i, sn] = multipliers stored in A_struct (column step, rows > sn)
         *            Already applied above in elimination. The update for
         *            columns right of the supernode hasn't been done yet.
         *
         * Actually, the column-by-column elimination above only updated columns
         * within the supernode. Columns right of the supernode still need the
         * rank-sn_size update. We do this as a GEMM.
         */
        if (trailing_rows > 0 && trailing_cols > 0) {
            if (stats) {
                stats->trailing_rows_total += (uint64_t)trailing_rows;
                stats->trailing_cols_total += (uint64_t)trailing_cols;
            }

            int active_row_count = 0;
            for (int i = 0; i < trailing_rows; i++) {
                int orig_row = row_perm[sn_start + sn_size + i];
                if (stats) stats->active_row_scan_entries++;
                if (row_active_orig[orig_row]) active_rows[active_row_count++] = i;
            }

            int active_col_count = 0;
            for (int jj = 0; jj < trailing_cols; jj++) {
                if (stats) stats->active_col_scan_entries++;
                if (col_active_local[jj]) active_cols[active_col_count++] = jj;
            }

            if (active_row_count == 0 || active_col_count == 0) {
                if (stats) stats->skipped_update_calls++;
                for (int i = 0; i < touched_row_count; i++) row_active_orig[touched_rows[i]] = 0;
                for (int j = 0; j < touched_col_count; j++) col_active_local[touched_cols[j]] = 0;
                continue;
            }

            if (stats) {
                stats->active_rows_total += (uint64_t)active_row_count;
                stats->active_cols_total += (uint64_t)active_col_count;
                stats->pack_l_entries_total += (uint64_t)active_row_count * (uint64_t)sn_size;
                stats->pack_u_entries_total += (uint64_t)active_col_count * (uint64_t)sn_size;
                stats->dense_triplets_total +=
                    (uint64_t)trailing_rows * (uint64_t)sn_size * (uint64_t)trailing_cols;
                stats->compact_triplets_total +=
                    (uint64_t)active_row_count * (uint64_t)sn_size * (uint64_t)active_col_count;
            }

            /* Workspace layout: [L_block | U_block]
             * L: active_rows * sn_size
             * U: sn_size * active_cols */
            size_t L_sz = (size_t)active_row_count * sn_size;
            size_t U_sz = (size_t)sn_size * active_col_count;
            size_t need = L_sz + U_sz;

            double *L_block, *U_block;
            int used_work = 0;
            if (work && work_capacity >= need) {
                L_block = work;
                U_block = work + L_sz;
                used_work = 1;
            } else {
                L_block = (double *)calloc(need, sizeof(double));
                if (!L_block) { rc = -1; goto cleanup; }
                U_block = L_block + L_sz;
            }

            /* Fill compact L_block: active trailing rows by supernode columns. */
            for (int i = 0; i < active_row_count; i++) {
                int orig_row = row_perm[sn_start + sn_size + active_rows[i]];
                for (int j_local = 0; j_local < sn_size; j_local++) {
                    L_block[i * sn_size + j_local] =
                        A_struct[(size_t)orig_row * k + (sn_start + j_local)];
                }
            }

            /* Fill compact U_block: supernode rows by active trailing columns. */
            for (int j_local = 0; j_local < sn_size; j_local++) {
                int piv_orig = row_perm[sn_start + j_local];
                for (int jj = 0; jj < active_col_count; jj++) {
                    U_block[j_local * active_col_count + jj] =
                        A_struct[(size_t)piv_orig * k + (sn_start + sn_size + active_cols[jj])];
                }
            }

            if (active_row_count == trailing_rows && active_col_count == trailing_cols) {
                if (stats) stats->full_update_calls++;
                sn_dgemm_update_scattered_rows(trailing_rows, sn_size, trailing_cols,
                                               L_block, sn_size,
                                               U_block, trailing_cols,
                                               A_struct, k, row_perm,
                                               sn_start + sn_size,
                                               sn_start + sn_size);
            } else {
                double t_compact_update_ms = 0.0;
                if (stats) {
                    stats->compact_update_calls++;
                    if (active_col_count == 1) {
                        stats->compact_cols1_calls++;
                        stats->compact_cols1_rows_total += (uint64_t)active_row_count;
                    } else if (active_col_count == 2) {
                        stats->compact_cols2_calls++;
                        stats->compact_cols2_rows_total += (uint64_t)active_row_count;
                    } else if (active_col_count == 3) {
                        stats->compact_cols3_calls++;
                        stats->compact_cols3_rows_total += (uint64_t)active_row_count;
                    } else if (active_col_count == 4) {
                        stats->compact_cols4_calls++;
                        stats->compact_cols4_rows_total += (uint64_t)active_row_count;
                    } else {
                        stats->compact_cols5p_calls++;
                        stats->compact_cols5p_rows_total += (uint64_t)active_row_count;
                    }
                    t_compact_update_ms = lp_telemetry_timer_start();
                }
                sn_dgemm_update_scattered_rows_cols(active_row_count, sn_size, active_col_count,
                                                    L_block, sn_size,
                                                    U_block, active_col_count,
                                                    A_struct, k, row_perm,
                                                    active_rows, sn_start + sn_size,
                                                    active_cols, sn_start + sn_size);
                if (stats) {
                    double compact_update_ms = lp_telemetry_timer_elapsed_ms(t_compact_update_ms);
                    if (active_col_count == 1) {
                        stats->compact_cols1_ms += compact_update_ms;
                    } else if (active_col_count == 2) {
                        stats->compact_cols2_ms += compact_update_ms;
                    } else if (active_col_count == 3) {
                        stats->compact_cols3_ms += compact_update_ms;
                    } else if (active_col_count == 4) {
                        stats->compact_cols4_ms += compact_update_ms;
                    } else {
                        stats->compact_cols5p_ms += compact_update_ms;
                    }
                }
            }

            if (!used_work) free(L_block);
            for (int i = 0; i < touched_row_count; i++) row_active_orig[touched_rows[i]] = 0;
            for (int j = 0; j < touched_col_count; j++) col_active_local[touched_cols[j]] = 0;
        }
    }

cleanup:
    #undef SN_EMIT_L
    #undef SN_EMIT_U
    free(row_active_orig_buf);
    free(col_active_local_buf);
    free(touched_rows_buf);
    free(touched_cols_buf);
    free(active_rows_buf);
    free(active_cols_buf);
    return rc;
}
