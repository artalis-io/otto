/*
 * Ralph - Sparse Matrix Operations
 *
 * Compressed Sparse Column (CSC) format implementation.
 *
 * BOUNDS CHECKING:
 * All functions validate input parameters and return safe defaults on error:
 *   - void functions: return early (no-op)
 *   - numeric functions: return 0 or 0.0
 *   - pointer functions: return NULL
 *
 * INDEX REQUIREMENTS:
 *   - row indices must be in [0, A->nrows)
 *   - col indices must be in [0, A->ncols)
 *   - index arrays (col_indices, row_indices) must have all elements in valid range
 *
 * NULL SAFETY:
 * All functions check for NULL matrix pointers and output buffer pointers.
 */

#ifndef RALPH_SPARSE_H
#define RALPH_SPARSE_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Safe free macro - NULLs pointer after freeing to prevent double-free */
#ifndef SAFE_FREE
#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)
#endif

/* Compressed Sparse Column (CSC) matrix
 *
 * Storage: Column-major sparse format where each column's non-zeros are stored
 * contiguously. Row indices within each column are sorted ascending.
 *
 * colptr[j] = start index of column j in rowidx/values arrays
 * colptr[ncols] = nnz (total non-zeros)
 * Column j has entries at indices [colptr[j], colptr[j+1])
 */
typedef struct {
    int nrows;      /* Number of rows */
    int ncols;      /* Number of columns */
    int nnz;        /* Number of non-zeros */
    int capacity;   /* Allocated capacity for non-zeros */
    int *colptr;    /* Column pointers (size ncols+1) */
    int *rowidx;    /* Row indices (size nnz), sorted within each column */
    double *values; /* Non-zero values (size nnz) */
} SparseMatrix;

/* Dense vector operations */
typedef struct {
    int size;
    double *data;
} DenseVector;

/* Sparse matrix creation/destruction */
SparseMatrix* sparse_create(int nrows, int ncols, int nnz_estimate);
SparseMatrix* sparse_create_from_dense(int nrows, int ncols, const double *dense);
SparseMatrix* sparse_copy(const SparseMatrix *src);
void sparse_free(SparseMatrix *mat);

/* Building sparse matrices incrementally */
typedef struct {
    int nrows;
    int ncols;
    int nnz;
    int capacity;
    int *row;
    int *col;
    double *val;
} SparseTriplets;

SparseTriplets* triplets_create(int nrows, int ncols, int nnz_estimate);
void triplets_free(SparseTriplets *trips);
int triplets_add(SparseTriplets *trips, int row, int col, double val);
SparseMatrix* triplets_to_csc(SparseTriplets *trips);

/* Sparse matrix-vector operations
 * All require: A != NULL, x != NULL, y != NULL
 * Vector sizes: x has A->ncols elements, y has A->nrows elements (or vice versa for transpose)
 */
void sparse_matvec(const SparseMatrix *A, const double *x, double *y);           /* y = A*x */
void sparse_matvec_add(const SparseMatrix *A, const double *x, double *y);       /* y += A*x */
void sparse_matvec_transpose(const SparseMatrix *A, const double *x, double *y); /* y = A'*x */
void sparse_matvec_transpose_add(const SparseMatrix *A, const double *x, double *y);

/* Column operations
 * All validate: A != NULL, col in [0, A->ncols), output pointers != NULL
 * On invalid input: void functions return early, numeric functions return 0
 */
void sparse_get_column(const SparseMatrix *A, int col, double *dense);
int sparse_get_column_nnz(const SparseMatrix *A, int col);
void sparse_get_column_sparse(const SparseMatrix *A, int col,
                              int *nnz, const int **rowidx, const double **values);
void sparse_axpy_column(const SparseMatrix *A, int col, double alpha, double *y); /* y += alpha*A[:,col] */
double sparse_dot_column(const SparseMatrix *A, int col, const double *y);        /* y'*A[:,col] */

/* Element/row access (less efficient in CSC format)
 * Validates: A != NULL, row in [0, A->nrows), col in [0, A->ncols)
 * On invalid input: returns 0.0 or returns early
 */
double sparse_get_element(const SparseMatrix *A, int row, int col);
void sparse_get_row(const SparseMatrix *A, int row, double *dense);

/* Submatrix extraction
 * Validates: A != NULL, indices array != NULL, all indices in valid range
 * On invalid input: returns NULL
 *
 * sparse_get_columns: extracts columns col_indices[0..ncols-1]
 *   - Each col_indices[k] must be in [0, A->ncols)
 *   - Returns new matrix with ncols columns, A->nrows rows
 *
 * sparse_get_rows: extracts rows row_indices[0..nrows-1]
 *   - Each row_indices[k] must be in [0, A->nrows)
 *   - Returns new matrix with nrows rows, A->ncols columns
 */
SparseMatrix* sparse_get_columns(const SparseMatrix *A, int ncols, const int *col_indices);
SparseMatrix* sparse_get_rows(const SparseMatrix *A, int nrows, const int *row_indices);

/* Matrix properties */
int sparse_is_empty(const SparseMatrix *A);
double sparse_norm_inf(const SparseMatrix *A);
double sparse_norm_1(const SparseMatrix *A);

/* Dense vector operations */
DenseVector* vec_create(int size);
DenseVector* vec_create_zero(int size);
DenseVector* vec_copy(const DenseVector *src);
void vec_free(DenseVector *vec);

void vec_set_zero(double *x, int n);
void vec_copy_data(double *dst, const double *src, int n);
void vec_axpy(int n, double alpha, const double *x, double *y);  /* y += alpha*x */
void vec_scale(int n, double alpha, double *x);                   /* x *= alpha */
double vec_dot(int n, const double *x, const double *y);          /* x'*y */
double vec_norm_2(int n, const double *x);
double vec_norm_inf(int n, const double *x);
int vec_argmax_abs(int n, const double *x);

/* Utility */
void sparse_print(const SparseMatrix *A, const char *name);
void sparse_print_dense(const SparseMatrix *A, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_SPARSE_H */
