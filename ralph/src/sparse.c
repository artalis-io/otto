/*
 * Ralph - Sparse Matrix Operations Implementation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "sparse.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/* ============================================================================
 * Sparse Matrix Creation/Destruction
 * ============================================================================ */

SparseMatrix* sparse_create(int nrows, int ncols, int nnz_estimate) {
    SparseMatrix *mat = (SparseMatrix*)malloc(sizeof(SparseMatrix));
    if (!mat) return NULL;

    mat->nrows = nrows;
    mat->ncols = ncols;
    mat->nnz = 0;
    mat->capacity = nnz_estimate > 0 ? nnz_estimate : 1;

    mat->colptr = (int*)calloc(ncols + 1, sizeof(int));
    mat->rowidx = (int*)malloc(mat->capacity * sizeof(int));
    mat->values = (double*)malloc(mat->capacity * sizeof(double));

    if (!mat->colptr || !mat->rowidx || !mat->values) {
        sparse_free(mat);
        return NULL;
    }

    return mat;
}

SparseMatrix* sparse_create_from_dense(int nrows, int ncols, const double *dense) {
    /* Count non-zeros */
    int nnz = 0;
    for (int i = 0; i < nrows * ncols; i++) {
        if (fabs(dense[i]) > 1e-15) nnz++;
    }

    SparseMatrix *mat = sparse_create(nrows, ncols, nnz);
    if (!mat) return NULL;

    /* Fill in CSC format (column-major) */
    int idx = 0;
    for (int j = 0; j < ncols; j++) {
        mat->colptr[j] = idx;
        for (int i = 0; i < nrows; i++) {
            double val = dense[i * ncols + j];
            if (fabs(val) > 1e-15) {
                mat->rowidx[idx] = i;
                mat->values[idx] = val;
                idx++;
            }
        }
    }
    mat->colptr[ncols] = idx;
    mat->nnz = idx;

    return mat;
}

SparseMatrix* sparse_copy(const SparseMatrix *src) {
    if (!src) return NULL;

    SparseMatrix *dst = sparse_create(src->nrows, src->ncols, src->nnz);
    if (!dst) return NULL;

    dst->nnz = src->nnz;
    memcpy(dst->colptr, src->colptr, (src->ncols + 1) * sizeof(int));
    memcpy(dst->rowidx, src->rowidx, src->nnz * sizeof(int));
    memcpy(dst->values, src->values, src->nnz * sizeof(double));

    return dst;
}

void sparse_free(SparseMatrix *mat) {
    if (mat) {
        SAFE_FREE(mat->colptr);
        SAFE_FREE(mat->rowidx);
        SAFE_FREE(mat->values);
        free(mat);
    }
}

/* ============================================================================
 * Triplet Format (for incremental building)
 * ============================================================================ */

SparseTriplets* triplets_create(int nrows, int ncols, int nnz_estimate) {
    SparseTriplets *trips = (SparseTriplets*)malloc(sizeof(SparseTriplets));
    if (!trips) return NULL;

    trips->nrows = nrows;
    trips->ncols = ncols;
    trips->nnz = 0;
    trips->capacity = nnz_estimate > 0 ? nnz_estimate : 64;

    trips->row = (int*)malloc(trips->capacity * sizeof(int));
    trips->col = (int*)malloc(trips->capacity * sizeof(int));
    trips->val = (double*)malloc(trips->capacity * sizeof(double));

    if (!trips->row || !trips->col || !trips->val) {
        triplets_free(trips);
        return NULL;
    }

    return trips;
}

void triplets_free(SparseTriplets *trips) {
    if (trips) {
        SAFE_FREE(trips->row);
        SAFE_FREE(trips->col);
        SAFE_FREE(trips->val);
        free(trips);
    }
}

int triplets_add(SparseTriplets *trips, int row, int col, double val) {
    if (!trips) return -1;
    if (fabs(val) < 1e-15) return 0;  /* Skip zeros */

    /* Expand if needed */
    if (trips->nnz >= trips->capacity) {
        int new_cap = trips->capacity * 2;
        int *new_row = (int*)realloc(trips->row, new_cap * sizeof(int));
        int *new_col = (int*)realloc(trips->col, new_cap * sizeof(int));
        double *new_val = (double*)realloc(trips->val, new_cap * sizeof(double));

        if (!new_row || !new_col || !new_val) return -1;

        trips->row = new_row;
        trips->col = new_col;
        trips->val = new_val;
        trips->capacity = new_cap;
    }

    trips->row[trips->nnz] = row;
    trips->col[trips->nnz] = col;
    trips->val[trips->nnz] = val;
    trips->nnz++;

    return 0;
}

/* Sort key for triplet sorting (thread-safe approach) */
typedef struct {
    int col;
    int row;
    int orig_idx;
} TripletSortKey;

static int triplet_key_cmp(const void *a, const void *b) {
    const TripletSortKey *ka = (const TripletSortKey*)a;
    const TripletSortKey *kb = (const TripletSortKey*)b;
    if (ka->col != kb->col) {
        return ka->col - kb->col;
    }
    return ka->row - kb->row;
}

SparseMatrix* triplets_to_csc(SparseTriplets *trips) {
    if (!trips || trips->nnz == 0) {
        return sparse_create(trips ? trips->nrows : 0, trips ? trips->ncols : 0, 0);
    }

    /* Create sort key array with embedded (col, row, idx) - thread-safe */
    TripletSortKey *keys = (TripletSortKey*)malloc(trips->nnz * sizeof(TripletSortKey));
    if (!keys) return NULL;

    for (int i = 0; i < trips->nnz; i++) {
        keys[i].col = trips->col[i];
        keys[i].row = trips->row[i];
        keys[i].orig_idx = i;
    }

    /* Sort keys by (col, row) - no global state needed */
    qsort(keys, trips->nnz, sizeof(TripletSortKey), triplet_key_cmp);

    SparseMatrix *mat = sparse_create(trips->nrows, trips->ncols, trips->nnz);
    if (!mat) {
        free(keys);
        return NULL;
    }

    /* Build CSC format */
    int *col_counts = (int*)calloc(trips->ncols, sizeof(int));
    if (!col_counts) {
        free(keys);
        sparse_free(mat);
        return NULL;
    }

    /* Count entries per column */
    for (int i = 0; i < trips->nnz; i++) {
        col_counts[trips->col[i]]++;
    }

    /* Compute column pointers */
    mat->colptr[0] = 0;
    for (int j = 0; j < trips->ncols; j++) {
        mat->colptr[j + 1] = mat->colptr[j] + col_counts[j];
    }

    /* Fill in values (using sorted order from keys) */
    int idx = 0;
    for (int k = 0; k < trips->nnz; k++) {
        int orig = keys[k].orig_idx;
        mat->rowidx[idx] = trips->row[orig];
        mat->values[idx] = trips->val[orig];
        idx++;
    }
    mat->nnz = trips->nnz;

    free(keys);
    free(col_counts);

    return mat;
}

/* ============================================================================
 * Matrix-Vector Operations
 * ============================================================================ */

void sparse_matvec(const SparseMatrix *A, const double *x, double *y) {
    vec_set_zero(y, A->nrows);
    sparse_matvec_add(A, x, y);
}

void sparse_matvec_add(const SparseMatrix *A, const double *x, double *y) {
    for (int j = 0; j < A->ncols; j++) {
        double xj = x[j];
        if (fabs(xj) < 1e-15) continue;
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            y[A->rowidx[p]] += A->values[p] * xj;
        }
    }
}

void sparse_matvec_transpose(const SparseMatrix *A, const double *x, double *y) {
    const int *colptr = A->colptr;
    const int *rowidx = A->rowidx;
    const double *values = A->values;

    for (int j = 0; j < A->ncols; j++) {
        double sum = 0.0;
        int start = colptr[j];
        int end = colptr[j + 1];
        /* SIMD reduction on sparse dot product (indirect indexed) */
        #pragma omp simd reduction(+:sum)
        for (int p = start; p < end; p++) {
            sum += values[p] * x[rowidx[p]];
        }
        y[j] = sum;
    }
}

void sparse_matvec_transpose_add(const SparseMatrix *A, const double *x, double *y) {
    const int *colptr = A->colptr;
    const int *rowidx = A->rowidx;
    const double *values = A->values;

    for (int j = 0; j < A->ncols; j++) {
        double sum = 0.0;
        int start = colptr[j];
        int end = colptr[j + 1];
        /* SIMD reduction on sparse dot product (indirect indexed) */
        #pragma omp simd reduction(+:sum)
        for (int p = start; p < end; p++) {
            sum += values[p] * x[rowidx[p]];
        }
        y[j] += sum;
    }
}

/* ============================================================================
 * Column Operations
 * ============================================================================ */

void sparse_get_column(const SparseMatrix *A, int col, double *dense) {
    if (!A || !dense || col < 0 || col >= A->ncols) return;
    vec_set_zero(dense, A->nrows);
    for (int p = A->colptr[col]; p < A->colptr[col + 1]; p++) {
        dense[A->rowidx[p]] = A->values[p];
    }
}

int sparse_get_column_nnz(const SparseMatrix *A, int col) {
    if (!A || col < 0 || col >= A->ncols) return 0;
    return A->colptr[col + 1] - A->colptr[col];
}

void sparse_get_column_sparse(const SparseMatrix *A, int col,
                              int *nnz, const int **rowidx, const double **values) {
    if (!A || col < 0 || col >= A->ncols || !nnz || !rowidx || !values) {
        if (nnz) *nnz = 0;
        if (rowidx) *rowidx = NULL;
        if (values) *values = NULL;
        return;
    }
    int start = A->colptr[col];
    *nnz = A->colptr[col + 1] - start;
    *rowidx = &A->rowidx[start];
    *values = &A->values[start];
}

void sparse_axpy_column(const SparseMatrix *A, int col, double alpha, double *y) {
    if (!A || !y || col < 0 || col >= A->ncols) return;
    for (int p = A->colptr[col]; p < A->colptr[col + 1]; p++) {
        y[A->rowidx[p]] += alpha * A->values[p];
    }
}

double sparse_dot_column(const SparseMatrix *A, int col, const double *y) {
    if (!A || !y || col < 0 || col >= A->ncols) return 0.0;
    double result = 0.0;
    int start = A->colptr[col];
    int end = A->colptr[col + 1];
    const int *rowidx = A->rowidx;
    const double *values = A->values;

    /* SIMD reduction on sparse dot product */
    #pragma omp simd reduction(+:result)
    for (int p = start; p < end; p++) {
        result += values[p] * y[rowidx[p]];
    }
    return result;
}

/* ============================================================================
 * Element/Row Access (less efficient in CSC)
 * ============================================================================ */

double sparse_get_element(const SparseMatrix *A, int row, int col) {
    if (!A || row < 0 || row >= A->nrows || col < 0 || col >= A->ncols) return 0.0;
    for (int p = A->colptr[col]; p < A->colptr[col + 1]; p++) {
        if (A->rowidx[p] == row) return A->values[p];
        if (A->rowidx[p] > row) break;  /* Assuming sorted rows */
    }
    return 0.0;
}

void sparse_get_row(const SparseMatrix *A, int row, double *dense) {
    if (!A || !dense || row < 0 || row >= A->nrows) return;
    vec_set_zero(dense, A->ncols);
    for (int j = 0; j < A->ncols; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (A->rowidx[p] == row) {
                dense[j] = A->values[p];
                break;
            }
            if (A->rowidx[p] > row) break;
        }
    }
}

/* ============================================================================
 * Submatrix Operations
 * ============================================================================ */

SparseMatrix* sparse_get_columns(const SparseMatrix *A, int ncols, const int *col_indices) {
    if (!A || ncols <= 0 || !col_indices) return NULL;

    /* Validate all column indices before proceeding */
    for (int k = 0; k < ncols; k++) {
        if (col_indices[k] < 0 || col_indices[k] >= A->ncols) return NULL;
    }

    /* Count non-zeros */
    int nnz = 0;
    for (int k = 0; k < ncols; k++) {
        int j = col_indices[k];
        nnz += A->colptr[j + 1] - A->colptr[j];
    }

    SparseMatrix *B = sparse_create(A->nrows, ncols, nnz);
    if (!B) return NULL;

    int idx = 0;
    for (int k = 0; k < ncols; k++) {
        int j = col_indices[k];
        B->colptr[k] = idx;
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            B->rowidx[idx] = A->rowidx[p];
            B->values[idx] = A->values[p];
            idx++;
        }
    }
    B->colptr[ncols] = idx;
    B->nnz = idx;

    return B;
}

SparseMatrix* sparse_get_rows(const SparseMatrix *A, int nrows, const int *row_indices) {
    if (!A || nrows <= 0 || !row_indices) return NULL;

    /* Validate all row indices before proceeding */
    for (int k = 0; k < nrows; k++) {
        if (row_indices[k] < 0 || row_indices[k] >= A->nrows) return NULL;
    }

    /* Create row mapping */
    int *row_map = (int*)malloc(A->nrows * sizeof(int));
    if (!row_map) return NULL;

    for (int i = 0; i < A->nrows; i++) row_map[i] = -1;
    for (int k = 0; k < nrows; k++) row_map[row_indices[k]] = k;

    /* Count non-zeros */
    int nnz = 0;
    for (int j = 0; j < A->ncols; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (row_map[A->rowidx[p]] >= 0) nnz++;
        }
    }

    SparseMatrix *B = sparse_create(nrows, A->ncols, nnz);
    if (!B) {
        free(row_map);
        return NULL;
    }

    int idx = 0;
    for (int j = 0; j < A->ncols; j++) {
        B->colptr[j] = idx;
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int new_row = row_map[A->rowidx[p]];
            if (new_row >= 0) {
                B->rowidx[idx] = new_row;
                B->values[idx] = A->values[p];
                idx++;
            }
        }
    }
    B->colptr[A->ncols] = idx;
    B->nnz = idx;

    free(row_map);
    return B;
}

/* ============================================================================
 * Matrix Properties
 * ============================================================================ */

int sparse_is_empty(const SparseMatrix *A) {
    return A == NULL || A->nnz == 0;
}

double sparse_norm_inf(const SparseMatrix *A) {
    /* Max row sum */
    double *row_sums = (double*)calloc(A->nrows, sizeof(double));
    if (!row_sums) return -1.0;

    for (int j = 0; j < A->ncols; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            row_sums[A->rowidx[p]] += fabs(A->values[p]);
        }
    }

    double max_sum = 0.0;
    for (int i = 0; i < A->nrows; i++) {
        if (row_sums[i] > max_sum) max_sum = row_sums[i];
    }

    free(row_sums);
    return max_sum;
}

double sparse_norm_1(const SparseMatrix *A) {
    /* Max column sum */
    double max_sum = 0.0;
    for (int j = 0; j < A->ncols; j++) {
        double col_sum = 0.0;
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            col_sum += fabs(A->values[p]);
        }
        if (col_sum > max_sum) max_sum = col_sum;
    }
    return max_sum;
}

/* ============================================================================
 * Dense Vector Operations
 * ============================================================================ */

DenseVector* vec_create(int size) {
    DenseVector *vec = (DenseVector*)malloc(sizeof(DenseVector));
    if (!vec) return NULL;

    vec->size = size;
    vec->data = (double*)malloc(size * sizeof(double));
    if (!vec->data) {
        free(vec);
        return NULL;
    }

    return vec;
}

DenseVector* vec_create_zero(int size) {
    DenseVector *vec = vec_create(size);
    if (vec) vec_set_zero(vec->data, size);
    return vec;
}

DenseVector* vec_copy(const DenseVector *src) {
    if (!src) return NULL;
    DenseVector *dst = vec_create(src->size);
    if (dst) memcpy(dst->data, src->data, src->size * sizeof(double));
    return dst;
}

void vec_free(DenseVector *vec) {
    if (vec) {
        free(vec->data);
        free(vec);
    }
}

void vec_set_zero(double *x, int n) {
    memset(x, 0, n * sizeof(double));
}

void vec_copy_data(double *dst, const double *src, int n) {
    memcpy(dst, src, n * sizeof(double));
}

void vec_axpy(int n, double alpha, const double *x, double *y) {
    for (int i = 0; i < n; i++) {
        y[i] += alpha * x[i];
    }
}

void vec_scale(int n, double alpha, double *x) {
    for (int i = 0; i < n; i++) {
        x[i] *= alpha;
    }
}

double vec_dot(int n, const double *x, const double *y) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += x[i] * y[i];
    }
    return sum;
}

double vec_norm_2(int n, const double *x) {
    return sqrt(vec_dot(n, x, x));
}

double vec_norm_inf(int n, const double *x) {
    double max_val = 0.0;
    for (int i = 0; i < n; i++) {
        double abs_val = fabs(x[i]);
        if (abs_val > max_val) max_val = abs_val;
    }
    return max_val;
}

int vec_argmax_abs(int n, const double *x) {
    int max_idx = 0;
    double max_val = 0.0;
    for (int i = 0; i < n; i++) {
        double abs_val = fabs(x[i]);
        if (abs_val > max_val) {
            max_val = abs_val;
            max_idx = i;
        }
    }
    return max_idx;
}

/* ============================================================================
 * Utility/Debug
 * ============================================================================ */

void sparse_print(const SparseMatrix *A, const char *name) {
    printf("Sparse Matrix %s: %d x %d, %d non-zeros\n",
           name ? name : "", A->nrows, A->ncols, A->nnz);
    for (int j = 0; j < A->ncols; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            printf("  (%d, %d) = %g\n", A->rowidx[p], j, A->values[p]);
        }
    }
}

void sparse_print_dense(const SparseMatrix *A, const char *name) {
    printf("Matrix %s (%d x %d):\n", name ? name : "", A->nrows, A->ncols);
    for (int i = 0; i < A->nrows; i++) {
        printf("  ");
        for (int j = 0; j < A->ncols; j++) {
            printf("%8.3f ", sparse_get_element(A, i, j));
        }
        printf("\n");
    }
}
