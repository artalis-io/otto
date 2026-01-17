/*
 * Ralph - LU Factorization Implementation
 *
 * Implements LU factorization with partial pivoting for the basis matrix
 * in the revised simplex method. Supports both initial factorization
 * and efficient updates via eta-file method.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "lp.h"

/* ============================================================================
 * LU Factorization Creation/Destruction
 * ============================================================================ */

LUFactorization* lu_create(int m) {
    LUFactorization *lu = (LUFactorization*)calloc(1, sizeof(LUFactorization));
    if (!lu) return NULL;

    lu->m = m;
    lu->max_updates = 100;  /* Refactorize every 100 updates */

    /* Allocate permutation arrays */
    lu->perm = (int*)malloc(m * sizeof(int));
    lu->perm_inv = (int*)malloc(m * sizeof(int));
    lu->col_perm = (int*)malloc(m * sizeof(int));
    lu->col_perm_inv = (int*)malloc(m * sizeof(int));

    if (!lu->perm || !lu->perm_inv || !lu->col_perm || !lu->col_perm_inv) {
        lu_free(lu);
        return NULL;
    }

    /* Initialize to identity permutation */
    for (int i = 0; i < m; i++) {
        lu->perm[i] = i;
        lu->perm_inv[i] = i;
        lu->col_perm[i] = i;
        lu->col_perm_inv[i] = i;
    }

    /* Eta file for updates (sparse storage) */
    lu->eta_capacity = lu->max_updates;
    lu->num_eta = 0;
    lu->eta_col = (int*)malloc(lu->eta_capacity * sizeof(int));
    lu->eta_indices = (int**)malloc(lu->eta_capacity * sizeof(int*));
    lu->eta_values = (double**)malloc(lu->eta_capacity * sizeof(double*));
    lu->eta_nnz = (int*)malloc(lu->eta_capacity * sizeof(int));

    if (!lu->eta_col || !lu->eta_indices || !lu->eta_values || !lu->eta_nnz) {
        lu_free(lu);
        return NULL;
    }

    for (int i = 0; i < lu->eta_capacity; i++) {
        lu->eta_indices[i] = NULL;
        lu->eta_values[i] = NULL;
        lu->eta_nnz[i] = 0;
    }

    /* Initialize condition number tracking */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    lu->cond_estimate = 1.0;
    lu->growth_factor = 1.0;

    return lu;
}

void lu_free(LUFactorization *lu) {
    if (!lu) return;

    free(lu->L_colptr);
    free(lu->L_rowidx);
    free(lu->L_values);
    free(lu->U_colptr);
    free(lu->U_rowidx);
    free(lu->U_values);
    free(lu->perm);
    free(lu->perm_inv);
    free(lu->col_perm);
    free(lu->col_perm_inv);
    free(lu->eta_col);

    if (lu->eta_indices) {
        for (int i = 0; i < lu->eta_capacity; i++) {
            free(lu->eta_indices[i]);
        }
        free(lu->eta_indices);
    }
    if (lu->eta_values) {
        for (int i = 0; i < lu->eta_capacity; i++) {
            free(lu->eta_values[i]);
        }
        free(lu->eta_values);
    }
    free(lu->eta_nnz);

    free(lu);
}

/* External sparse factorization (from lu_sparse.c) */
int lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B);

/* ============================================================================
 * Main LU Factorization Entry Point
 * ============================================================================ */

/* Try sparse factorization first, fall back to dense if it fails */
int lu_factorize(LUFactorization *lu, const SparseMatrix *B) {
    /* Use sparse LU for larger problems (m >= 20) */
    if (B->nrows >= 20) {
        int result = lu_factorize_sparse(lu, B);
        if (result == 0) return 0;
        /* Fall back to dense if sparse fails */
    }
    return lu_factorize_dense(lu, B);
}

/* ============================================================================
 * Dense LU Factorization (fallback for numerical robustness)
 * ============================================================================ */

/* Perform LU factorization: PA = LU using partial pivoting */
int lu_factorize_dense(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) return -1;
    if (B->nrows != B->ncols || B->nrows != lu->m) return -1;

    int m = lu->m;

    /* Convert sparse matrix to dense for factorization */
    double *A = (double*)calloc(m * m, sizeof(double));
    if (!A) return -1;

    /* Fill dense matrix from sparse (column-major order) */
    for (int j = 0; j < m; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            A[B->rowidx[p] + j * m] = B->values[p];
        }
    }

    /* Initialize permutation to identity */
    for (int i = 0; i < m; i++) {
        lu->perm[i] = i;
    }

    /* Gaussian elimination with partial pivoting */
    for (int k = 0; k < m; k++) {
        /* Find pivot */
        int pivot_row = k;
        double max_val = fabs(A[k + k * m]);

        for (int i = k + 1; i < m; i++) {
            double val = fabs(A[i + k * m]);
            if (val > max_val) {
                max_val = val;
                pivot_row = i;
            }
        }

        /* Check for singular matrix */
        if (max_val < RALPH_PIVOT_TOL) {
            free(A);
            return -1;  /* Singular or near-singular */
        }

        /* Swap rows if necessary */
        if (pivot_row != k) {
            for (int j = 0; j < m; j++) {
                double tmp = A[k + j * m];
                A[k + j * m] = A[pivot_row + j * m];
                A[pivot_row + j * m] = tmp;
            }
            int tmp = lu->perm[k];
            lu->perm[k] = lu->perm[pivot_row];
            lu->perm[pivot_row] = tmp;
        }

        /* Eliminate below diagonal */
        double pivot = A[k + k * m];
        for (int i = k + 1; i < m; i++) {
            double mult = A[i + k * m] / pivot;
            A[i + k * m] = mult;  /* Store L entry */

            for (int j = k + 1; j < m; j++) {
                A[i + j * m] -= mult * A[k + j * m];
            }
        }
    }

    /* Compute inverse permutation */
    for (int i = 0; i < m; i++) {
        lu->perm_inv[lu->perm[i]] = i;
    }

    /* Extract L and U in sparse format */
    /* Count non-zeros */
    int nnz_L = 0, nnz_U = 0;
    for (int j = 0; j < m; j++) {
        for (int i = j + 1; i < m; i++) {
            if (fabs(A[i + j * m]) > RALPH_ZERO_TOL) nnz_L++;
        }
        for (int i = 0; i <= j; i++) {
            if (fabs(A[i + j * m]) > RALPH_ZERO_TOL) nnz_U++;
        }
    }

    /* Add diagonal of L (implicit ones) */
    nnz_L += m;

    /* Free old storage */
    free(lu->L_colptr);
    free(lu->L_rowidx);
    free(lu->L_values);
    free(lu->U_colptr);
    free(lu->U_rowidx);
    free(lu->U_values);

    /* Allocate new storage */
    lu->L_colptr = (int*)malloc((m + 1) * sizeof(int));
    lu->L_rowidx = (int*)malloc(nnz_L * sizeof(int));
    lu->L_values = (double*)malloc(nnz_L * sizeof(double));
    lu->U_colptr = (int*)malloc((m + 1) * sizeof(int));
    lu->U_rowidx = (int*)malloc(nnz_U * sizeof(int));
    lu->U_values = (double*)malloc(nnz_U * sizeof(double));

    if (!lu->L_colptr || !lu->L_rowidx || !lu->L_values ||
        !lu->U_colptr || !lu->U_rowidx || !lu->U_values) {
        free(A);
        return -1;
    }

    /* Fill L (unit lower triangular stored with explicit diagonal) */
    int idx = 0;
    for (int j = 0; j < m; j++) {
        lu->L_colptr[j] = idx;
        /* Diagonal (1.0) */
        lu->L_rowidx[idx] = j;
        lu->L_values[idx] = 1.0;
        idx++;
        /* Below diagonal */
        for (int i = j + 1; i < m; i++) {
            double val = A[i + j * m];
            if (fabs(val) > RALPH_ZERO_TOL) {
                lu->L_rowidx[idx] = i;
                lu->L_values[idx] = val;
                idx++;
            }
        }
    }
    lu->L_colptr[m] = idx;
    lu->nnz_L = idx;

    /* Fill U (upper triangular) */
    idx = 0;
    for (int j = 0; j < m; j++) {
        lu->U_colptr[j] = idx;
        for (int i = 0; i <= j; i++) {
            double val = A[i + j * m];
            if (fabs(val) > RALPH_ZERO_TOL) {
                lu->U_rowidx[idx] = i;
                lu->U_values[idx] = val;
                idx++;
            }
        }
    }
    lu->U_colptr[m] = idx;
    lu->nnz_U = idx;

    /* Clear sparse eta file */
    for (int i = 0; i < lu->num_eta; i++) {
        free(lu->eta_indices[i]);
        free(lu->eta_values[i]);
        lu->eta_indices[i] = NULL;
        lu->eta_values[i] = NULL;
        lu->eta_nnz[i] = 0;
    }
    lu->num_eta = 0;
    lu->num_updates = 0;

    /* Compute condition number estimate from U diagonal */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    for (int j = 0; j < m; j++) {
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            if (lu->U_rowidx[p] == j) {
                double absval = fabs(lu->U_values[p]);
                if (absval < lu->min_diag_U) lu->min_diag_U = absval;
                if (absval > lu->max_diag_U) lu->max_diag_U = absval;
                break;
            }
        }
    }
    if (lu->min_diag_U > RALPH_ZERO_TOL) {
        lu->cond_estimate = lu->max_diag_U / lu->min_diag_U;
    } else {
        lu->cond_estimate = RALPH_INFINITY;
    }
    lu->growth_factor = 1.0;

    free(A);
    return 0;
}

/* ============================================================================
 * Solve Systems Using LU Factorization
 * ============================================================================ */

/* Solve Lx = b (forward substitution) */
static void solve_L(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;

    /* Apply row permutation */
    for (int i = 0; i < m; i++) {
        x[i] = b[lu->perm[i]];
    }

    /* Forward substitution */
    for (int j = 0; j < m; j++) {
        /* x[j] already has the right value (L[j,j] = 1) */
        double xj = x[j];

        /* Update remaining elements */
        for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
            int i = lu->L_rowidx[p];
            x[i] -= lu->L_values[p] * xj;
        }
    }
}

/* Solve Ux = b (backward substitution) */
static void solve_U(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;

    vec_copy_data(x, b, m);

    /* Backward substitution */
    for (int j = m - 1; j >= 0; j--) {
        /* Find diagonal element */
        double diag = 0.0;
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            if (lu->U_rowidx[p] == j) {
                diag = lu->U_values[p];
                break;
            }
        }

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[j] = 0.0;  /* Effectively zero row */
            continue;
        }

        x[j] /= diag;
        double xj = x[j];

        /* Update remaining elements */
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            int i = lu->U_rowidx[p];
            if (i < j) {
                x[i] -= lu->U_values[p] * xj;
            }
        }
    }
}

/* Solve L'x = b (backward substitution with L transpose) */
static void solve_Lt(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;

    vec_copy_data(x, b, m);

    /* Backward substitution with L transpose */
    for (int j = m - 1; j >= 0; j--) {
        double sum = 0.0;
        for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
            int i = lu->L_rowidx[p];
            sum += lu->L_values[p] * x[i];
        }
        x[j] -= sum;
        /* L[j,j] = 1, so no division needed */
    }

    /* Apply inverse row permutation */
    double *temp = (double*)malloc(m * sizeof(double));
    if (temp) {
        for (int i = 0; i < m; i++) {
            temp[lu->perm[i]] = x[i];
        }
        vec_copy_data(x, temp, m);
        free(temp);
    }
}

/* Solve U'x = b (forward substitution with U transpose) */
static void solve_Ut(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;

    vec_copy_data(x, b, m);

    /* Forward substitution with U transpose (U' is lower triangular)
     * For i = 0, 1, ..., m-1:
     *   x[i] = (b[i] - sum_{j<i} U'[i,j] * x[j]) / U'[i,i]
     *        = (b[i] - sum_{j<i} U[j,i] * x[j]) / U[i,i]
     * Note: U[j,i] is in column i, row j (for j < i)
     */
    for (int i = 0; i < m; i++) {
        /* Subtract contributions from earlier solved variables:
         * sum of U'[i,j] * x[j] = U[j,i] * x[j] for j < i
         * U[j,i] is in COLUMN i (not column j!) at ROW j
         */
        double sum = 0.0;
        for (int p = lu->U_colptr[i]; p < lu->U_colptr[i + 1]; p++) {
            int j = lu->U_rowidx[p];  /* row index j */
            if (j < i) {
                /* This is U[j,i] = U'[i,j] */
                sum += lu->U_values[p] * x[j];
            }
        }

        /* Find diagonal U[i,i] */
        double diag = 0.0;
        for (int p = lu->U_colptr[i]; p < lu->U_colptr[i + 1]; p++) {
            if (lu->U_rowidx[p] == i) {
                diag = lu->U_values[p];
                break;
            }
        }

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[i] = 0.0;
            continue;
        }

        x[i] = (x[i] - sum) / diag;
    }
}

/* Apply eta updates: E_n^-1 * ... * E_1^-1 * x
 * Each E^-1 is identity except column 'col' which contains the eta vector.
 * E^-1 * x: x[i] += eta[i] * x[col] for i != col, x[col] = eta[col] * x[col]
 * Now uses sparse eta storage for O(nnz) instead of O(m).
 */
static void apply_eta_forward(const LUFactorization *lu, double *x) {
    for (int k = 0; k < lu->num_eta; k++) {
        int col = lu->eta_col[k];
        int *indices = lu->eta_indices[k];
        double *values = lu->eta_values[k];
        int nnz = lu->eta_nnz[k];
        double xc = x[col];  /* Save original x[col] before modifying */

        /* Update only non-zero components */
        for (int p = 0; p < nnz; p++) {
            int i = indices[p];
            if (i == col) {
                x[i] = values[p] * xc;
            } else {
                x[i] += values[p] * xc;
            }
        }
    }
}

/* Apply eta updates transpose: (E_1^-1)' * ... * (E_n^-1)' * x
 * Applied in reverse order for the transpose solve.
 * (E^-1)' * x: x[col] = eta' * x, other components unchanged.
 * Now uses sparse eta storage for O(nnz) instead of O(m).
 */
static void apply_eta_backward(const LUFactorization *lu, double *x) {
    for (int k = lu->num_eta - 1; k >= 0; k--) {
        int col = lu->eta_col[k];
        int *indices = lu->eta_indices[k];
        double *values = lu->eta_values[k];
        int nnz = lu->eta_nnz[k];

        /* Compute new x[col] = eta' * x (sparse dot product) */
        double xc = 0.0;
        for (int p = 0; p < nnz; p++) {
            xc += values[p] * x[indices[p]];
        }
        x[col] = xc;
    }
}

/* Solve Bx = b where B = basis matrix */
void lu_solve(const LUFactorization *lu, double *rhs, double *solution) {
    int m = lu->m;
    double *work = (double*)malloc(m * sizeof(double));
    double *work2 = (double*)malloc(m * sizeof(double));
    if (!work || !work2) {
        free(work);
        free(work2);
        return;
    }

    /* For sparse LU with column pivoting: PAQ = LU
     * Solve Bx = b  =>  PAQx = Pb  =>  LUQ'x = Pb
     * Let z = Q'x, then LUz = Pb
     * 1. Solve Ly = Pb (forward subst with row perm)
     * 2. Solve Uz = y (backward subst)
     * 3. x = Qz (apply column permutation)
     */

    /* First: solve Ly = Pb */
    solve_L(lu, rhs, work);

    /* Then: solve Uz = y */
    solve_U(lu, work, work2);

    /* Apply eta updates (in step coordinates, before column permutation) */
    apply_eta_forward(lu, work2);

    /* Apply column permutation: x[col_perm[i]] = z[i] */
    for (int i = 0; i < m; i++) {
        solution[lu->col_perm[i]] = work2[i];
    }

    free(work);
    free(work2);
}

/* Solve B'x = b (for computing row prices) */
void lu_solve_transpose(const LUFactorization *lu, double *rhs, double *solution) {
    int m = lu->m;
    double *work = (double*)malloc(m * sizeof(double));
    if (!work) return;

    /* For sparse LU with column pivoting: PAQ = LU
     * So B = P'LUQ', and B' = QU'L'P
     * With eta updates: B_new' = E_n' * ... * E_1' * B'
     * Solve B_new'x = b:
     * 1. Apply inverse col perm: y[i] = b[col_perm[i]] (converts to step coords)
     * 2. Apply eta updates in reverse (in step coordinates)
     * 3. Solve U'z = y
     * 4. Solve L'w = z, then apply P': x[perm[i]] = w[i]
     */

    /* Apply inverse column permutation: y[i] = b[col_perm[i]] */
    for (int i = 0; i < m; i++) {
        work[i] = rhs[lu->col_perm[i]];
    }

    /* Apply eta updates in reverse (in step coordinates) */
    apply_eta_backward(lu, work);

    /* Solve U'z = y */
    solve_Ut(lu, work, solution);

    /* Solve L'x = z and apply P' (solve_Lt handles the row permutation) */
    solve_Lt(lu, solution, work);

    vec_copy_data(solution, work, m);

    free(work);
}

/* ============================================================================
 * Sparse LU Solves - Exploit RHS Sparsity
 * ============================================================================ */

/*
 * Sparse forward solve for L: Solve Lx = b where b is sparse
 *
 * This exploits the sparsity pattern: if b[j] = 0 and no earlier
 * column has affected row j, then x[j] = 0.
 *
 * Uses DFS to find reachable nodes from non-zero RHS entries.
 */
static void solve_L_sparse(const LUFactorization *lu,
                           int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                           double *x, int *xi, int *top) {
    int m = lu->m;

    /* Clear solution */
    memset(x, 0, m * sizeof(double));

    /* Build permuted RHS and find initial non-zeros */
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            /* Find position in permuted system */
            int perm_row = lu->perm_inv[orig_row];
            x[perm_row] = rhs_val[k];
        }
    }

    /* Apply row permutation to get initial pattern */
    *top = m;
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            int perm_row = lu->perm_inv[orig_row];
            xi[--(*top)] = perm_row;
        }
    }

    /* Forward substitution only for reachable entries */
    /* Note: For simplicity, we do full forward sub since L is typically sparse */
    for (int j = 0; j < m; j++) {
        double xj = x[j];
        if (fabs(xj) < RALPH_ZERO_TOL) continue;

        /* Update remaining elements */
        for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
            int i = lu->L_rowidx[p];
            x[i] -= lu->L_values[p] * xj;
        }
    }
}

/*
 * Sparse FTRAN: Solve Bx = b where b is sparse
 *
 * For sparse RHS, we can potentially save work by:
 * 1. Tracking which entries of x can become non-zero (reachability)
 * 2. Only computing those entries
 *
 * However, for simplicity and to avoid overhead on small problems,
 * we use a hybrid approach:
 * - Build dense RHS from sparse input
 * - Use standard solve (which already exploits L/U sparsity)
 * - The key benefit is avoiding dense column extraction in caller
 */
void lu_solve_sparse(const LUFactorization *lu,
                     int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                     double *solution) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* For very sparse RHS (< 10% fill), use sparse path */
    if (nnz_rhs < m / 10 && nnz_rhs > 0) {
        double *work = (double*)calloc(m, sizeof(double));
        double *work2 = (double*)calloc(m, sizeof(double));
        int *xi = (int*)malloc(m * sizeof(int));

        if (work && work2 && xi) {
            int top;
            /* Sparse L solve */
            solve_L_sparse(lu, nnz_rhs, rhs_idx, rhs_val, work, xi, &top);

            /* Standard U solve (U is typically also sparse) */
            solve_U(lu, work, work2);

            /* Apply eta updates */
            apply_eta_forward(lu, work2);

            /* Apply column permutation */
            for (int i = 0; i < m; i++) {
                solution[lu->col_perm[i]] = work2[i];
            }

            free(work);
            free(work2);
            free(xi);
            return;
        }
        free(work);
        free(work2);
        free(xi);
    }

    /* Fallback: Build dense RHS from sparse input */
    double *rhs = (double*)calloc(m, sizeof(double));
    if (!rhs) return;

    for (int k = 0; k < nnz_rhs; k++) {
        if (rhs_idx[k] >= 0 && rhs_idx[k] < m) {
            rhs[rhs_idx[k]] = rhs_val[k];
        }
    }

    /* Use standard solve */
    lu_solve(lu, rhs, solution);

    free(rhs);
}

/*
 * Sparse BTRAN: Solve B'x = b where b is sparse
 *
 * Used for computing dual prices when the objective is sparse.
 */
void lu_solve_transpose_sparse(const LUFactorization *lu,
                               int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                               double *solution) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Build dense RHS from sparse input */
    double *rhs = (double*)calloc(m, sizeof(double));
    if (!rhs) return;

    for (int k = 0; k < nnz_rhs; k++) {
        if (rhs_idx[k] >= 0 && rhs_idx[k] < m) {
            rhs[rhs_idx[k]] = rhs_val[k];
        }
    }

    /* Use standard solve */
    lu_solve_transpose(lu, rhs, solution);

    free(rhs);
}

/* ============================================================================
 * Basis Updates via Eta File
 * ============================================================================ */

/* Update factorization when basis column changes */
int lu_update(LUFactorization *lu, int leaving_pos, const double *entering_col) {
    if (!lu || !entering_col) return -1;
    if (lu->num_eta >= lu->max_updates) return -1;  /* Need refactorization */

    int m = lu->m;

    /* Convert leaving_pos to step coordinates (eta is in step coordinates) */
    int step_pos = lu->col_perm_inv[leaving_pos];

    /* Solve for eta column: L * U * eta = entering_col */
    /* First transform entering column */
    double *work = (double*)malloc(m * sizeof(double));
    double *eta = (double*)malloc(m * sizeof(double));
    if (!work || !eta) {
        free(work);
        free(eta);
        return -1;
    }

    /* Solve L * y = P * entering_col */
    solve_L(lu, entering_col, work);

    /* Solve U * eta = y (eta is in step coordinates) */
    solve_U(lu, work, eta);

    /* Apply existing eta updates */
    apply_eta_forward(lu, eta);

    /* Check pivot element (in step coordinates) */
    if (fabs(eta[step_pos]) < RALPH_PIVOT_TOL) {
        free(work);
        free(eta);
        return -1;  /* Singular update */
    }

    /* Normalize eta column and count non-zeros */
    double pivot = eta[step_pos];
    int nnz = 0;
    double max_eta = 0.0;
    for (int i = 0; i < m; i++) {
        if (i == step_pos) {
            eta[i] = 1.0 / pivot;
        } else {
            eta[i] = -eta[i] / pivot;
        }
        double absval = fabs(eta[i]);
        if (absval > max_eta) max_eta = absval;
        if (absval > RALPH_ZERO_TOL) nnz++;
    }

    /* Convert to sparse storage */
    int *indices = (int*)malloc(nnz * sizeof(int));
    double *values = (double*)malloc(nnz * sizeof(double));
    if (!indices || !values) {
        free(work);
        free(eta);
        free(indices);
        free(values);
        return -1;
    }

    int p = 0;
    for (int i = 0; i < m; i++) {
        if (fabs(eta[i]) > RALPH_ZERO_TOL) {
            indices[p] = i;
            values[p] = eta[i];
            p++;
        }
    }

    /* Store sparse eta update */
    lu->eta_col[lu->num_eta] = step_pos;
    lu->eta_indices[lu->num_eta] = indices;
    lu->eta_values[lu->num_eta] = values;
    lu->eta_nnz[lu->num_eta] = nnz;
    lu->num_eta++;
    lu->num_updates++;

    /* Track growth factor */
    if (max_eta > lu->growth_factor) {
        lu->growth_factor = max_eta;
    }

    free(work);
    free(eta);
    return 0;
}

int lu_needs_refactorization(const LUFactorization *lu) {
    if (!lu) return 0;

    /* Refactorize if max updates reached */
    if (lu->num_updates >= lu->max_updates) return 1;

    /* Refactorize early if condition has degraded significantly */
    if (lu->growth_factor > 1e6) return 1;

    return 0;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

void lu_print(const LUFactorization *lu) {
    if (!lu) {
        printf("NULL LU factorization\n");
        return;
    }

    printf("LU Factorization: %d x %d\n", lu->m, lu->m);
    printf("  L: %d non-zeros\n", lu->nnz_L);
    printf("  U: %d non-zeros\n", lu->nnz_U);
    printf("  Updates: %d / %d\n", lu->num_updates, lu->max_updates);

    printf("  Row permutation: [");
    for (int i = 0; i < lu->m && i < 10; i++) {
        printf("%d ", lu->perm[i]);
    }
    printf("...]\n");
}
