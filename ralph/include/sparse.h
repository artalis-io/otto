/*
 * Ralph - Sparse Matrix Operations
 *
 * Compressed Sparse Column (CSC) format implementation
 */

#ifndef RALPH_SPARSE_H
#define RALPH_SPARSE_H

#include <stddef.h>

/* Compressed Sparse Column matrix */
typedef struct {
    int nrows;      /* Number of rows */
    int ncols;      /* Number of columns */
    int nnz;        /* Number of non-zeros */
    int capacity;   /* Allocated capacity for non-zeros */
    int *colptr;    /* Column pointers (size ncols+1) */
    int *rowidx;    /* Row indices (size nnz) */
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

/* Sparse matrix operations */
void sparse_matvec(const SparseMatrix *A, const double *x, double *y);           /* y = A*x */
void sparse_matvec_add(const SparseMatrix *A, const double *x, double *y);       /* y += A*x */
void sparse_matvec_transpose(const SparseMatrix *A, const double *x, double *y); /* y = A'*x */
void sparse_matvec_transpose_add(const SparseMatrix *A, const double *x, double *y);

/* Column operations */
void sparse_get_column(const SparseMatrix *A, int col, double *dense);
int sparse_get_column_nnz(const SparseMatrix *A, int col);
void sparse_axpy_column(const SparseMatrix *A, int col, double alpha, double *y); /* y += alpha*A[:,col] */

/* Row operations (less efficient in CSC) */
double sparse_get_element(const SparseMatrix *A, int row, int col);
void sparse_get_row(const SparseMatrix *A, int row, double *dense);

/* Submatrix operations */
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

#endif /* RALPH_SPARSE_H */
