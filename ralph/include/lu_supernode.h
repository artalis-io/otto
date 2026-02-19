/*
 * Ralph - Supernodal LU Factorization (T2.1)
 *
 * Groups consecutive columns with similar sparsity into "supernodes",
 * then applies GEMM-like dense operations instead of rank-1 updates.
 * Converts scattered memory accesses into cache-friendly block operations.
 *
 * Builds on top of T1.4 symbolic/numeric separation — replaces only
 * the inner GE loop while reusing identity detection, fingerprint caching,
 * COO->CSC conversion, and all workspace arrays.
 */

#ifndef RALPH_LU_SUPERNODE_H
#define RALPH_LU_SUPERNODE_H

#include <stdlib.h>

/* Supernodal constants */
#define SN_MIN_K          64    /* Minimum structural columns to use supernodal */
#define SN_BLOCK_SIZE     4     /* Micro-kernel tile size */
#define SN_RELAX_ZEROS    4     /* Max extra zeros allowed when merging supernodes */
#define SN_MAX_BLOCK      16    /* Maximum supernode width */

/* A supernode: a contiguous group of columns with similar L-pattern */
typedef struct {
    int start;   /* First column index (in structural ordering) */
    int size;    /* Number of columns in this supernode */
} Supernode;

/* Symbolic analysis result for supernodal factorization */
typedef struct SNSymbolic_tag {
    int k;                   /* Number of structural columns */
    int m;                   /* Total rows */
    int *etree_parent;       /* [k] elimination tree parent */
    int *etree_postorder;    /* [k] postorder traversal */
    Supernode *supernodes;   /* [num_supernodes] */
    int num_supernodes;
    int max_supernode_size;  /* For workspace sizing */
    int max_panel_rows;      /* For workspace sizing */
} SNSymbolic;

/* --- Phase 1: Elimination tree + supernode detection --- */

/* Build column etree from row-major m x k dense matrix with row permutation */
int sn_build_etree(const double *A_struct, int m, int k,
                   const int *row_perm, int *etree_parent);

/* Postorder traversal of etree (children before parent) */
int sn_etree_postorder(const int *etree_parent, int k, int *postorder);

/* Detect supernodes: consecutive cols with similar L-pattern, relaxed merging */
int sn_detect_supernodes(const double *A_struct, int m, int k,
                         const int *row_perm, const int *etree_parent,
                         const int *postorder,
                         Supernode **out, int *num_out);

/* Top-level symbolic: etree + postorder + detection */
SNSymbolic *sn_analyze(const double *A_struct, int m, int k,
                       const int *row_perm);

void sn_symbolic_free(SNSymbolic *sym);

/* --- Phase 2: Dense micro-kernels (WASM-safe, no SIMD intrinsics) --- */

/* C -= A * B via 4x4 tiled micro-kernel (row-major) */
void sn_dgemm_update(int panel_rows, int block_size, int update_cols,
                     const double *A, int lda,
                     const double *B, int ldb,
                     double *C, int ldc);

/* Solve L * X = B in-place (unit lower triangular, row-major) */
void sn_dtrsm_lower(int block_size, int ncols,
                    const double *L, int ldl, double *B, int ldb);

/* Factor panel_rows x block_size panel: partial pivoting -> L + U blocks
 * Returns 0 on success, -1 on singular pivot (below pivot_tol) */
int sn_block_factor(int panel_rows, int block_size,
                    double *panel, int ldp,
                    int *pivot_indices, double pivot_tol);

/* --- Phase 3: Supernodal numeric factorization --- */

/* Factorize using supernodal method, producing COO L/U entries.
 * L_capacity/U_capacity: allocated size of COO arrays (bounds-checked).
 * work/work_capacity: pre-allocated workspace (doubles). If NULL or too small,
 * will allocate internally. Caller should pre-size to 3*m*max_sn_size.
 * Returns 0 on success, -1 on failure (falls back to column-by-column GE). */
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
                 double *work, size_t work_capacity);

#endif /* RALPH_LU_SUPERNODE_H */
