/*
 * Ralph - Sparse LU Factorization Implementation
 *
 * Implements sparse LU factorization with:
 * - Markowitz pivot selection for fill-in reduction
 * - Threshold pivoting for numerical stability
 * - Dynamic sparse storage
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "lp.h"

/* ============================================================================
 * Sparse Working Storage for LU Factorization
 * ============================================================================ */

/* Linked list node for sparse column/row entries */
typedef struct SparseEntry {
    int idx;                    /* Row (for column lists) or column (for row lists) */
    double val;
    struct SparseEntry *next;
} SparseEntry;

/* Working matrix for sparse LU */
typedef struct {
    int m;
    SparseEntry **cols;         /* Column linked lists */
    SparseEntry **rows;         /* Row linked lists (for fast row access) */
    int *col_nnz;               /* Non-zeros per column */
    int *row_nnz;               /* Non-zeros per row */
    int *col_perm;              /* Column permutation (pivot order) */
    int *row_perm;              /* Row permutation */
    int *col_perm_inv;
    int *row_perm_inv;
    int *col_done;              /* 1 if column has been pivoted */
    int *row_done;              /* 1 if row has been pivoted */
    SparseEntry *pool;          /* Memory pool for entries */
    int pool_size;
    int pool_used;
} SparseLUWork;

static SparseLUWork* sparse_work_create(int m, int nnz_estimate) {
    SparseLUWork *work = (SparseLUWork*)calloc(1, sizeof(SparseLUWork));
    if (!work) return NULL;

    work->m = m;

    /* Estimate pool size: original + fill-in (rough estimate: 3x original) */
    work->pool_size = nnz_estimate * 4 + m;
    work->pool = (SparseEntry*)malloc(work->pool_size * sizeof(SparseEntry));
    work->pool_used = 0;

    work->cols = (SparseEntry**)calloc(m, sizeof(SparseEntry*));
    work->rows = (SparseEntry**)calloc(m, sizeof(SparseEntry*));
    work->col_nnz = (int*)calloc(m, sizeof(int));
    work->row_nnz = (int*)calloc(m, sizeof(int));
    work->col_perm = (int*)malloc(m * sizeof(int));
    work->row_perm = (int*)malloc(m * sizeof(int));
    work->col_perm_inv = (int*)malloc(m * sizeof(int));
    work->row_perm_inv = (int*)malloc(m * sizeof(int));
    work->col_done = (int*)calloc(m, sizeof(int));
    work->row_done = (int*)calloc(m, sizeof(int));

    if (!work->pool || !work->cols || !work->rows || !work->col_nnz ||
        !work->row_nnz || !work->col_perm || !work->row_perm ||
        !work->col_perm_inv || !work->row_perm_inv ||
        !work->col_done || !work->row_done) {
        free(work->pool);
        free(work->cols);
        free(work->rows);
        free(work->col_nnz);
        free(work->row_nnz);
        free(work->col_perm);
        free(work->row_perm);
        free(work->col_perm_inv);
        free(work->row_perm_inv);
        free(work->col_done);
        free(work->row_done);
        free(work);
        return NULL;
    }

    /* Initialize permutations to identity */
    for (int i = 0; i < m; i++) {
        work->col_perm[i] = i;
        work->row_perm[i] = i;
        work->col_perm_inv[i] = i;
        work->row_perm_inv[i] = i;
    }

    return work;
}

static void sparse_work_free(SparseLUWork *work) {
    if (!work) return;
    free(work->pool);
    free(work->cols);
    free(work->rows);
    free(work->col_nnz);
    free(work->row_nnz);
    free(work->col_perm);
    free(work->row_perm);
    free(work->col_perm_inv);
    free(work->row_perm_inv);
    free(work->col_done);
    free(work->row_done);
    free(work);
}

static SparseEntry* alloc_entry(SparseLUWork *work) {
    if (work->pool_used >= work->pool_size) {
        /* Pool exhausted - reallocate */
        int new_size = work->pool_size * 2;
        SparseEntry *new_pool = (SparseEntry*)realloc(work->pool,
                                                       new_size * sizeof(SparseEntry));
        if (!new_pool) return NULL;
        work->pool = new_pool;
        work->pool_size = new_size;
    }
    return &work->pool[work->pool_used++];
}

/* Add entry to column list (maintains row order) */
static int add_to_col(SparseLUWork *work, int col, int row, double val) {
    if (fabs(val) < RALPH_ZERO_TOL) return 0;  /* Skip zeros */

    SparseEntry *entry = alloc_entry(work);
    if (!entry) return -1;

    entry->idx = row;
    entry->val = val;

    /* Insert in order */
    SparseEntry **pp = &work->cols[col];
    while (*pp && (*pp)->idx < row) {
        pp = &(*pp)->next;
    }
    entry->next = *pp;
    *pp = entry;

    work->col_nnz[col]++;
    return 0;
}

/* Add entry to row list (maintains column order) */
static int add_to_row(SparseLUWork *work, int row, int col, double val) {
    if (fabs(val) < RALPH_ZERO_TOL) return 0;

    SparseEntry *entry = alloc_entry(work);
    if (!entry) return -1;

    entry->idx = col;
    entry->val = val;

    /* Insert in order */
    SparseEntry **pp = &work->rows[row];
    while (*pp && (*pp)->idx < col) {
        pp = &(*pp)->next;
    }
    entry->next = *pp;
    *pp = entry;

    work->row_nnz[row]++;
    return 0;
}

/* Get value from column list */
static double get_col_val(SparseLUWork *work, int col, int row) {
    for (SparseEntry *e = work->cols[col]; e; e = e->next) {
        if (e->idx == row) return e->val;
        if (e->idx > row) break;
    }
    return 0.0;
}

/* Set value in column list (updates existing or adds new) */
static int set_col_val(SparseLUWork *work, int col, int row, double val) {
    SparseEntry **pp = &work->cols[col];

    while (*pp && (*pp)->idx < row) {
        pp = &(*pp)->next;
    }

    if (*pp && (*pp)->idx == row) {
        /* Update existing */
        if (fabs(val) < RALPH_ZERO_TOL) {
            /* Remove entry */
            *pp = (*pp)->next;
            work->col_nnz[col]--;
        } else {
            (*pp)->val = val;
        }
    } else if (fabs(val) >= RALPH_ZERO_TOL) {
        /* Add new entry */
        SparseEntry *entry = alloc_entry(work);
        if (!entry) return -1;
        entry->idx = row;
        entry->val = val;
        entry->next = *pp;
        *pp = entry;
        work->col_nnz[col]++;
    }

    return 0;
}

/* ============================================================================
 * Markowitz Pivot Selection
 * ============================================================================ */

/* Threshold for numerical stability (Markowitz threshold) */
#define MARKOWITZ_THRESHOLD 0.1

/* Select pivot using Markowitz criterion with threshold pivoting */
static int select_pivot(SparseLUWork *work, int step, int *pivot_row, int *pivot_col) {
    int m = work->m;
    int best_row = -1, best_col = -1;
    long long best_cost = (long long)m * m + 1;  /* Markowitz cost */
    double best_val = 0.0;

    /* Find maximum absolute value in each active column for threshold */
    double *col_max = (double*)calloc(m, sizeof(double));
    if (!col_max) return -1;

    for (int j = 0; j < m; j++) {
        if (work->col_done[j]) continue;
        for (SparseEntry *e = work->cols[j]; e; e = e->next) {
            if (work->row_done[e->idx]) continue;
            if (fabs(e->val) > col_max[j]) {
                col_max[j] = fabs(e->val);
            }
        }
    }

    /* Search for best pivot satisfying threshold */
    for (int j = 0; j < m; j++) {
        if (work->col_done[j]) continue;
        if (col_max[j] < RALPH_PIVOT_TOL) continue;  /* Singular column */

        double threshold = MARKOWITZ_THRESHOLD * col_max[j];

        for (SparseEntry *e = work->cols[j]; e; e = e->next) {
            int i = e->idx;
            if (work->row_done[i]) continue;
            if (fabs(e->val) < threshold) continue;  /* Doesn't meet threshold */

            /* Compute Markowitz cost */
            long long cost = (long long)(work->row_nnz[i] - 1) * (work->col_nnz[j] - 1);

            if (cost < best_cost || (cost == best_cost && fabs(e->val) > best_val)) {
                best_cost = cost;
                best_row = i;
                best_col = j;
                best_val = fabs(e->val);
            }
        }
    }

    free(col_max);

    if (best_row < 0 || best_col < 0) {
        return -1;  /* No valid pivot found - singular matrix */
    }

    *pivot_row = best_row;
    *pivot_col = best_col;
    return 0;
}

/* ============================================================================
 * Sparse LU Factorization
 * ============================================================================ */

int lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) return -1;
    if (B->nrows != B->ncols || B->nrows != lu->m) return -1;

    int m = lu->m;

    /* Create working storage */
    SparseLUWork *work = sparse_work_create(m, B->nnz);
    if (!work) return -1;

    /* Copy matrix into working storage (both column and row lists) */
    for (int j = 0; j < m; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int i = B->rowidx[p];
            double val = B->values[p];
            if (add_to_col(work, j, i, val) < 0 ||
                add_to_row(work, i, j, val) < 0) {
                sparse_work_free(work);
                return -1;
            }
        }
    }

    /* Arrays to store L and U entries during factorization */
    int L_cap = B->nnz + m;
    int U_cap = B->nnz + m;
    int *L_i = (int*)malloc(L_cap * sizeof(int));
    int *L_j = (int*)malloc(L_cap * sizeof(int));
    double *L_v = (double*)malloc(L_cap * sizeof(double));
    int *U_i = (int*)malloc(U_cap * sizeof(int));
    int *U_j = (int*)malloc(U_cap * sizeof(int));
    double *U_v = (double*)malloc(U_cap * sizeof(double));
    int L_nnz = 0, U_nnz = 0;

    if (!L_i || !L_j || !L_v || !U_i || !U_j || !U_v) {
        free(L_i); free(L_j); free(L_v);
        free(U_i); free(U_j); free(U_v);
        sparse_work_free(work);
        return -1;
    }

    /* Main elimination loop */
    for (int step = 0; step < m; step++) {
        /* Select pivot */
        int pivot_row, pivot_col;
        if (select_pivot(work, step, &pivot_row, &pivot_col) < 0) {
            free(L_i); free(L_j); free(L_v);
            free(U_i); free(U_j); free(U_v);
            sparse_work_free(work);
            return -1;  /* Singular */
        }

        double pivot_val = get_col_val(work, pivot_col, pivot_row);

        /* Record permutations */
        work->row_perm[step] = pivot_row;
        work->col_perm[step] = pivot_col;
        work->row_done[pivot_row] = 1;
        work->col_done[pivot_col] = 1;

        /* Store U entries from pivot row (in pivot column and to the right in elimination order) */
        for (SparseEntry *e = work->rows[pivot_row]; e; e = e->next) {
            int j = e->idx;
            if (work->col_done[j] && j != pivot_col) continue;  /* Already eliminated */

            /* Ensure capacity */
            if (U_nnz >= U_cap) {
                U_cap *= 2;
                U_i = (int*)realloc(U_i, U_cap * sizeof(int));
                U_j = (int*)realloc(U_j, U_cap * sizeof(int));
                U_v = (double*)realloc(U_v, U_cap * sizeof(double));
                if (!U_i || !U_j || !U_v) {
                    free(L_i); free(L_j); free(L_v);
                    free(U_i); free(U_j); free(U_v);
                    sparse_work_free(work);
                    return -1;
                }
            }

            U_i[U_nnz] = step;  /* Row in factored matrix */
            U_j[U_nnz] = (j == pivot_col) ? step : -1;  /* Will fix column order later */
            U_v[U_nnz] = e->val;

            /* Store actual column for later sorting */
            U_j[U_nnz] = j;
            U_nnz++;
        }

        /* Store L entries (multipliers) and eliminate */
        /* L diagonal is 1 (implicit) */
        if (L_nnz >= L_cap) {
            L_cap *= 2;
            L_i = (int*)realloc(L_i, L_cap * sizeof(int));
            L_j = (int*)realloc(L_j, L_cap * sizeof(int));
            L_v = (double*)realloc(L_v, L_cap * sizeof(double));
        }
        L_i[L_nnz] = step;
        L_j[L_nnz] = step;
        L_v[L_nnz] = 1.0;
        L_nnz++;

        /* For each row with non-zero in pivot column (except pivot row) */
        for (SparseEntry *e = work->cols[pivot_col]; e; e = e->next) {
            int i = e->idx;
            if (i == pivot_row || work->row_done[i]) continue;

            double mult = e->val / pivot_val;

            /* Store L entry */
            if (L_nnz >= L_cap) {
                L_cap *= 2;
                L_i = (int*)realloc(L_i, L_cap * sizeof(int));
                L_j = (int*)realloc(L_j, L_cap * sizeof(int));
                L_v = (double*)realloc(L_v, L_cap * sizeof(double));
            }
            L_i[L_nnz] = i;  /* Original row */
            L_j[L_nnz] = step;  /* Elimination step (column in L) */
            L_v[L_nnz] = mult;
            L_nnz++;

            /* Update row i: subtract mult * (pivot row) */
            for (SparseEntry *pe = work->rows[pivot_row]; pe; pe = pe->next) {
                int j = pe->idx;
                if (work->col_done[j]) continue;  /* Already eliminated column */

                double old_val = get_col_val(work, j, i);
                double new_val = old_val - mult * pe->val;
                set_col_val(work, j, i, new_val);

                /* Update row counts */
                if (fabs(old_val) < RALPH_ZERO_TOL && fabs(new_val) >= RALPH_ZERO_TOL) {
                    work->row_nnz[i]++;
                } else if (fabs(old_val) >= RALPH_ZERO_TOL && fabs(new_val) < RALPH_ZERO_TOL) {
                    work->row_nnz[i]--;
                }
            }

            /* Remove pivot column entry from row i */
            work->row_nnz[i]--;
        }

        /* Clear pivot column counts for remaining rows */
        work->col_nnz[pivot_col] = 0;
    }

    /* Build inverse permutations */
    for (int i = 0; i < m; i++) {
        work->row_perm_inv[work->row_perm[i]] = i;
        work->col_perm_inv[work->col_perm[i]] = i;
    }

    /* Convert L and U to CSC format with proper permutation */
    /* Free old storage */
    free(lu->L_colptr);
    free(lu->L_rowidx);
    free(lu->L_values);
    free(lu->U_colptr);
    free(lu->U_rowidx);
    free(lu->U_values);

    /* Allocate CSC arrays */
    lu->L_colptr = (int*)malloc((m + 1) * sizeof(int));
    lu->L_rowidx = (int*)malloc(L_nnz * sizeof(int));
    lu->L_values = (double*)malloc(L_nnz * sizeof(double));
    lu->U_colptr = (int*)malloc((m + 1) * sizeof(int));
    lu->U_rowidx = (int*)malloc(U_nnz * sizeof(int));
    lu->U_values = (double*)malloc(U_nnz * sizeof(double));

    if (!lu->L_colptr || !lu->L_rowidx || !lu->L_values ||
        !lu->U_colptr || !lu->U_rowidx || !lu->U_values) {
        free(L_i); free(L_j); free(L_v);
        free(U_i); free(U_j); free(U_v);
        sparse_work_free(work);
        return -1;
    }

    /* Convert L to CSC (L is stored with step as column index) */
    /* Count entries per column */
    memset(lu->L_colptr, 0, (m + 1) * sizeof(int));
    for (int k = 0; k < L_nnz; k++) {
        lu->L_colptr[L_j[k] + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->L_colptr[j + 1] += lu->L_colptr[j];
    }

    /* Fill entries */
    int *L_pos = (int*)calloc(m, sizeof(int));
    for (int k = 0; k < L_nnz; k++) {
        int j = L_j[k];
        int pos = lu->L_colptr[j] + L_pos[j]++;
        lu->L_rowidx[pos] = work->row_perm_inv[L_i[k]];  /* Convert to permuted row */
        lu->L_values[pos] = L_v[k];
    }
    free(L_pos);
    lu->nnz_L = L_nnz;

    /* Convert U to CSC (need to map original columns to step order) */
    memset(lu->U_colptr, 0, (m + 1) * sizeof(int));
    for (int k = 0; k < U_nnz; k++) {
        int orig_col = U_j[k];
        int step_col = work->col_perm_inv[orig_col];
        lu->U_colptr[step_col + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->U_colptr[j + 1] += lu->U_colptr[j];
    }

    int *U_pos = (int*)calloc(m, sizeof(int));
    for (int k = 0; k < U_nnz; k++) {
        int orig_col = U_j[k];
        int step_col = work->col_perm_inv[orig_col];
        int pos = lu->U_colptr[step_col] + U_pos[step_col]++;
        /* Row is the step at which this entry's row was the pivot row */
        /* Since U entries come from pivot rows, row index = step */
        /* But we need to figure out which step this row was pivoted */
        /* Actually, U_i already has the step index */
        lu->U_rowidx[pos] = U_i[k];
        lu->U_values[pos] = U_v[k];
    }
    free(U_pos);
    lu->nnz_U = U_nnz;

    /* Copy permutations */
    memcpy(lu->perm, work->row_perm, m * sizeof(int));
    memcpy(lu->perm_inv, work->row_perm_inv, m * sizeof(int));
    memcpy(lu->col_perm, work->col_perm, m * sizeof(int));
    memcpy(lu->col_perm_inv, work->col_perm_inv, m * sizeof(int));

    /* Clear eta file */
    for (int i = 0; i < lu->num_eta; i++) {
        free(lu->eta_vectors[i]);
        lu->eta_vectors[i] = NULL;
    }
    lu->num_eta = 0;
    lu->num_updates = 0;

    /* Cleanup */
    free(L_i); free(L_j); free(L_v);
    free(U_i); free(U_j); free(U_v);
    sparse_work_free(work);

    return 0;
}
