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

    /* Forrest-Tomlin update structures */
    lu->use_ft_updates = 1;  /* Enable FT updates */
    lu->ft_num_updates = 0;
    lu->ft_col_order = (int*)malloc(m * sizeof(int));
    lu->ft_col_order_inv = (int*)malloc(m * sizeof(int));

    if (!lu->ft_col_order || !lu->ft_col_order_inv) {
        lu_free(lu);
        return NULL;
    }

    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }

    /* Spike storage for FT updates */
    lu->ft_spike_capacity = lu->max_updates;
    lu->ft_spike_col = (int*)malloc(lu->ft_spike_capacity * sizeof(int));
    lu->ft_spike_idx = (int**)malloc(lu->ft_spike_capacity * sizeof(int*));
    lu->ft_spike_val = (double**)malloc(lu->ft_spike_capacity * sizeof(double*));
    lu->ft_spike_nnz = (int*)malloc(lu->ft_spike_capacity * sizeof(int));

    if (!lu->ft_spike_col || !lu->ft_spike_idx || !lu->ft_spike_val || !lu->ft_spike_nnz) {
        lu_free(lu);
        return NULL;
    }

    for (int i = 0; i < lu->ft_spike_capacity; i++) {
        lu->ft_spike_idx[i] = NULL;
        lu->ft_spike_val[i] = NULL;
        lu->ft_spike_nnz[i] = 0;
    }

    /* Initialize condition number tracking */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    lu->cond_estimate = 1.0;
    lu->growth_factor = 1.0;

    /* Pre-allocate workspace for hyper-sparse operations */
    lu->hs_work1 = (double*)calloc(m, sizeof(double));
    lu->hs_work2 = (double*)calloc(m, sizeof(double));
    lu->hs_marked = (int*)calloc(m, sizeof(int));
    lu->hs_idx = (int*)malloc(m * sizeof(int));
    lu->hs_val = (double*)malloc(m * sizeof(double));
    lu->perm_work = (double*)malloc(m * sizeof(double));

    if (!lu->hs_work1 || !lu->hs_work2 || !lu->hs_marked || !lu->hs_idx || !lu->hs_val || !lu->perm_work) {
        lu_free(lu);
        return NULL;
    }

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

    /* Free Forrest-Tomlin structures */
    free(lu->ft_col_order);
    free(lu->ft_col_order_inv);
    free(lu->ft_spike_col);

    if (lu->ft_spike_idx) {
        for (int i = 0; i < lu->ft_spike_capacity; i++) {
            free(lu->ft_spike_idx[i]);
        }
        free(lu->ft_spike_idx);
    }
    if (lu->ft_spike_val) {
        for (int i = 0; i < lu->ft_spike_capacity; i++) {
            free(lu->ft_spike_val[i]);
        }
        free(lu->ft_spike_val);
    }
    free(lu->ft_spike_nnz);

    /* Free hyper-sparse workspace */
    free(lu->hs_work1);
    free(lu->hs_work2);
    free(lu->hs_marked);
    free(lu->hs_idx);
    free(lu->hs_val);
    free(lu->perm_work);

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

    /* Clear Forrest-Tomlin spikes */
    for (int i = 0; i < lu->ft_num_updates; i++) {
        free(lu->ft_spike_idx[i]);
        free(lu->ft_spike_val[i]);
        lu->ft_spike_idx[i] = NULL;
        lu->ft_spike_val[i] = NULL;
        lu->ft_spike_nnz[i] = 0;
    }
    lu->ft_num_updates = 0;
    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }

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

/* Forward declarations for Forrest-Tomlin spike functions */
static void apply_ft_spikes_forward(const LUFactorization *lu, double *x);
static void apply_ft_spikes_backward(const LUFactorization *lu, double *x);

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

    /* Apply inverse row permutation using pre-allocated workspace */
    double *temp = lu->perm_work;
    for (int i = 0; i < m; i++) {
        temp[lu->perm[i]] = x[i];
    }
    vec_copy_data(x, temp, m);
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
    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;
    double *work2 = lu->hs_work2;

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

    /* Apply updates (in step coordinates, before column permutation) */
    if (lu->use_ft_updates && lu->ft_num_updates > 0) {
        apply_ft_spikes_forward(lu, work2);
    } else if (lu->num_eta > 0) {
        apply_eta_forward(lu, work2);
    }

    /* Apply column permutation: x[col_perm[i]] = z[i] */
    for (int i = 0; i < m; i++) {
        solution[lu->col_perm[i]] = work2[i];
    }
}

/* Solve B'x = b (for computing row prices) */
void lu_solve_transpose(const LUFactorization *lu, double *rhs, double *solution) {
    int m = lu->m;
    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;

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

    /* Apply updates in reverse (in step coordinates) */
    if (lu->use_ft_updates && lu->ft_num_updates > 0) {
        apply_ft_spikes_backward(lu, work);
    } else if (lu->num_eta > 0) {
        apply_eta_backward(lu, work);
    }

    /* Solve U'z = y */
    solve_Ut(lu, work, solution);

    /* Solve L'x = z and apply P' (solve_Lt handles the row permutation) */
    solve_Lt(lu, solution, work);

    vec_copy_data(solution, work, m);
}

/* ============================================================================
 * Sparse LU Solves - Exploit RHS Sparsity
 * ============================================================================
 *
 * These routines exploit sparsity in both L/U factors AND the RHS vector.
 * Key optimization: when solving Lx = b with sparse b, we only need to
 * compute x[j] for j in the "reach" of the nonzero pattern of b.
 */

/*
 * Sparse FTRAN: Solve Bx = b where b is sparse
 *
 * Simplified version that:
 * 1. Falls back to dense for non-sparse RHS
 * 2. Uses selective forward/backward substitution for sparse RHS
 */
void lu_solve_sparse(const LUFactorization *lu,
                     int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                     double *solution) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;
    double *work2 = lu->hs_work2;

    /* Clear workspace */
    memset(work, 0, m * sizeof(double));

    /* If RHS is too dense, use direct dense solve path
     * (must inline to avoid aliasing issues with workspace)
     */
    if (nnz_rhs > m / 10) {
        /* Build dense RHS vector in work */
        for (int k = 0; k < nnz_rhs; k++) {
            if (rhs_idx[k] >= 0 && rhs_idx[k] < m)
                work[rhs_idx[k]] = rhs_val[k];
        }

        /* Inline the dense solve: Ly=Pb, Uz=y, apply updates, apply col perm */
        solve_L(lu, work, work2);        /* work2 = L^{-1} * P * work */
        solve_U(lu, work2, work);        /* work = U^{-1} * work2 */

        if (lu->use_ft_updates && lu->ft_num_updates > 0) {
            apply_ft_spikes_forward(lu, work);
        } else if (lu->num_eta > 0) {
            apply_eta_forward(lu, work);
        }

        for (int i = 0; i < m; i++) {
            solution[lu->col_perm[i]] = work[i];
        }
        return;
    }

    /* Sparse path: Build permuted RHS */
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            work[lu->perm_inv[orig_row]] = rhs_val[k];
        }
    }

    /* Forward solve L: for each column j in order */
    for (int j = 0; j < m; j++) {
        double xj = work[j];
        if (fabs(xj) < RALPH_ZERO_TOL) continue;

        /* Update rows below j where L[i,j] != 0 */
        for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
            work[lu->L_rowidx[p]] -= lu->L_values[p] * xj;
        }
    }

    /* Backward solve U */
    solve_U(lu, work, work2);

    /* Apply updates */
    if (lu->use_ft_updates && lu->ft_num_updates > 0) {
        apply_ft_spikes_forward(lu, work2);
    } else if (lu->num_eta > 0) {
        apply_eta_forward(lu, work2);
    }

    /* Apply column permutation */
    for (int i = 0; i < m; i++) {
        solution[lu->col_perm[i]] = work2[i];
    }
}

/*
 * Sparse BTRAN: Solve B'x = b where b is sparse
 *
 * Used for computing dual prices (pi = c_B * B^{-1}).
 * Exploits sparsity of the cost vector.
 *
 * For PAQ = LU, we have B' = QU'L'P
 * So B'^{-1}b = P'L'^{-1}U'^{-1}Q'b
 */
void lu_solve_transpose_sparse(const LUFactorization *lu,
                               int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                               double *solution) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;
    double *work2 = lu->hs_work2;

    /* Clear workspace */
    memset(work, 0, m * sizeof(double));

    /* If RHS is too dense, use direct dense solve path
     * (must inline to avoid aliasing issues with workspace)
     */
    if (nnz_rhs > m / 10) {
        /* Build dense RHS vector in work */
        for (int k = 0; k < nnz_rhs; k++) {
            if (rhs_idx[k] >= 0 && rhs_idx[k] < m)
                work[rhs_idx[k]] = rhs_val[k];
        }

        /* Inline the dense transpose solve */
        /* Apply inverse column permutation: work2[i] = work[col_perm[i]] */
        for (int i = 0; i < m; i++) {
            work2[i] = work[lu->col_perm[i]];
        }

        /* Apply updates in reverse (in step coordinates) */
        if (lu->use_ft_updates && lu->ft_num_updates > 0) {
            apply_ft_spikes_backward(lu, work2);
        } else if (lu->num_eta > 0) {
            apply_eta_backward(lu, work2);
        }

        /* Solve U'z = work2 */
        solve_Ut(lu, work2, work);

        /* Solve L'x = z and apply P' */
        solve_Lt(lu, work, work2);

        vec_copy_data(solution, work2, m);
        return;
    }

    /* Sparse path: Apply inverse column permutation */
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_idx = rhs_idx[k];
        if (orig_idx >= 0 && orig_idx < m) {
            /* Find which step position this original index maps to */
            int step_pos = lu->col_perm_inv[orig_idx];
            work[step_pos] = rhs_val[k];
        }
    }

    /* Apply updates in reverse */
    if (lu->use_ft_updates && lu->ft_num_updates > 0) {
        apply_ft_spikes_backward(lu, work);
    } else if (lu->num_eta > 0) {
        apply_eta_backward(lu, work);
    }

    /* Solve U'z = work (U' is lower triangular) */
    solve_Ut(lu, work, work2);

    /* Solve L'x = z and apply P' */
    solve_Lt(lu, work2, solution);
}

/* ============================================================================
 * Hyper-Sparse Triangular Solves with Reach Computation
 * ============================================================================
 *
 * For very sparse RHS vectors (e.g., single column of A), we can dramatically
 * reduce work by only computing entries in the "reach" of the nonzero pattern.
 *
 * Reach of b in L: all rows i such that x[i] may be nonzero after solving Lx=b
 * This is computed via DFS on the graph where j -> i if L[i,j] != 0.
 *
 * Key insight: For a column a_j with k nonzeros, the reach typically has
 * O(k * avg_L_col_nnz) entries, much smaller than m.
 */

/*
 * Compute reach of sparse RHS through lower triangular L using DFS.
 * Returns indices in topological order (increasing for lower triangular).
 *
 * reach_out: output array of size m (will contain reached indices)
 * reach_nnz: output count of reached indices
 * marked: workspace array of size m (will be modified)
 */
static void compute_reach_L(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked) {
    int m = lu->m;
    *reach_nnz = 0;

    /* Clear marks for RHS indices and their descendants */
    /* We use marked[i] = 1 for "in reach", 2 for "processed" */

    /* Stack-based DFS from each nonzero in RHS */
    int *stack = reach_out + m/2;  /* Use second half of reach_out as stack */
    int stack_top;

    for (int k = 0; k < nnz_rhs; k++) {
        int start = rhs_idx[k];
        if (start < 0 || start >= m || marked[start]) continue;

        /* DFS from start */
        stack_top = 0;
        stack[stack_top++] = start;

        while (stack_top > 0) {
            int j = stack[stack_top - 1];

            if (marked[j] == 0) {
                /* First visit: mark as in-progress */
                marked[j] = 1;
            }

            /* Find unvisited child */
            int found_child = 0;
            for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
                int i = lu->L_rowidx[p];  /* L[i,j] != 0, so j affects i */
                if (marked[i] == 0) {
                    stack[stack_top++] = i;
                    found_child = 1;
                    break;
                }
            }

            if (!found_child) {
                /* All children visited, add j to reach in reverse postorder */
                stack_top--;
                marked[j] = 2;  /* Fully processed */
            }
        }
    }

    /* Collect reached indices in topological order (increasing for L) */
    for (int j = 0; j < m; j++) {
        if (marked[j] == 2) {
            reach_out[(*reach_nnz)++] = j;
        }
    }

    /* Clear marks for next use */
    for (int i = 0; i < *reach_nnz; i++) {
        marked[reach_out[i]] = 0;
    }
}

/*
 * Compute reach of sparse RHS through upper triangular U using DFS.
 * Returns indices in reverse topological order (decreasing for upper triangular).
 */
static void compute_reach_U(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked) {
    int m = lu->m;
    *reach_nnz = 0;

    /* For U, we need to find rows affected by nonzeros.
     * U is upper triangular: U[i,j] != 0 for i <= j.
     * If x[j] is nonzero and U[i,j] != 0, then x[i] is affected.
     * So we go from j to i where i < j.
     */

    int *stack = reach_out + m/2;
    int stack_top;

    for (int k = 0; k < nnz_rhs; k++) {
        int start = rhs_idx[k];
        if (start < 0 || start >= m || marked[start]) continue;

        stack_top = 0;
        stack[stack_top++] = start;

        while (stack_top > 0) {
            int j = stack[stack_top - 1];

            if (marked[j] == 0) {
                marked[j] = 1;
            }

            /* Find unvisited predecessor (row i < j where U[i,j] != 0) */
            int found_child = 0;
            for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
                int i = lu->U_rowidx[p];
                if (i < j && marked[i] == 0) {
                    stack[stack_top++] = i;
                    found_child = 1;
                    break;
                }
            }

            if (!found_child) {
                stack_top--;
                marked[j] = 2;
            }
        }
    }

    /* Collect in decreasing order for U solve */
    for (int j = m - 1; j >= 0; j--) {
        if (marked[j] == 2) {
            reach_out[(*reach_nnz)++] = j;
        }
    }

    /* Clear marks */
    for (int i = 0; i < *reach_nnz; i++) {
        marked[reach_out[i]] = 0;
    }
}

/*
 * Sparse forward solve: Lx = b where b and x are sparse.
 *
 * Input:
 *   nnz_b, b_idx, b_val: sparse RHS (in permuted coordinates)
 *   x: dense output vector (zeroed on entry for non-reach indices)
 *   reach, reach_nnz: precomputed reach (or NULL to compute)
 *   marked: workspace of size m
 *
 * Output:
 *   x: solution values at reach indices
 *   x_idx, x_nnz: sparse representation of result
 */
static void solve_L_sparse(const LUFactorization *lu,
                           int nnz_b, const int *b_idx, const double *b_val,
                           double *x,
                           int *x_idx, int *x_nnz,
                           int *marked) {
    int m = lu->m;

    /* Compute reach if not provided */
    int *reach = x_idx;  /* Reuse output array */
    int reach_nnz;
    compute_reach_L(lu, nnz_b, b_idx, reach, &reach_nnz, marked);

    /* Initialize x with RHS values */
    for (int k = 0; k < nnz_b; k++) {
        int j = b_idx[k];
        if (j >= 0 && j < m) {
            x[j] = b_val[k];
        }
    }

    /* Forward substitution only on reach (already in topological order) */
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];
        double xj = x[j];  /* L[j,j] = 1, so no division needed */

        if (fabs(xj) > RALPH_ZERO_TOL) {
            /* Update successors */
            for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
                int i = lu->L_rowidx[p];
                x[i] -= lu->L_values[p] * xj;
            }
        }
    }

    /* Build sparse output (indices already in reach) */
    *x_nnz = 0;
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];
        if (fabs(x[j]) > RALPH_ZERO_TOL) {
            x_idx[*x_nnz] = j;
            (*x_nnz)++;
        }
    }
}

/*
 * Sparse backward solve: Ux = b where b and x are sparse.
 */
static void solve_U_sparse(const LUFactorization *lu,
                           int nnz_b, const int *b_idx, const double *b_val,
                           double *x,
                           int *x_idx, int *x_nnz,
                           int *marked) {
    int m = lu->m;

    /* Compute reach */
    int *reach = x_idx;
    int reach_nnz;
    compute_reach_U(lu, nnz_b, b_idx, reach, &reach_nnz, marked);

    /* Initialize x with RHS values */
    for (int k = 0; k < nnz_b; k++) {
        int j = b_idx[k];
        if (j >= 0 && j < m) {
            x[j] = b_val[k];
        }
    }

    /* Backward substitution on reach (in decreasing order) */
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];

        /* Find diagonal */
        double diag = 0.0;
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            if (lu->U_rowidx[p] == j) {
                diag = lu->U_values[p];
                break;
            }
        }

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[j] = 0.0;
            continue;
        }

        x[j] /= diag;
        double xj = x[j];

        if (fabs(xj) > RALPH_ZERO_TOL) {
            /* Update predecessors */
            for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
                int i = lu->U_rowidx[p];
                if (i < j) {
                    x[i] -= lu->U_values[p] * xj;
                }
            }
        }
    }

    /* Build sparse output */
    *x_nnz = 0;
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];
        if (fabs(x[j]) > RALPH_ZERO_TOL) {
            x_idx[*x_nnz] = j;
            (*x_nnz)++;
        }
    }
}

/*
 * Hyper-sparse FTRAN: Solve Bx = b where b is very sparse.
 *
 * Returns result in both dense (solution) and sparse (sol_idx, sol_nnz) form.
 * Uses reach computation to minimize work.
 *
 * For a typical LP pivot column with 5-10 nonzeros, this can be 10-100x
 * faster than dense solve.
 */
void lu_ftran_hyper_sparse(const LUFactorization *lu,
                           int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                           double *solution,
                           int *sol_idx, int *sol_nnz) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Threshold: if RHS too dense, fall back to regular sparse solve */
    if (nnz_rhs > m / 8) {
        /* Use existing sparse solve */
        lu_solve_sparse(lu, nnz_rhs, rhs_idx, rhs_val, solution);
        /* Build sparse output by scanning */
        if (sol_idx && sol_nnz) {
            *sol_nnz = 0;
            for (int j = 0; j < m; j++) {
                if (fabs(solution[j]) > RALPH_ZERO_TOL) {
                    sol_idx[(*sol_nnz)++] = j;
                }
            }
        }
        return;
    }

    /* Use pre-allocated workspace (avoid malloc in hot path) */
    /* Cast away const for workspace access - workspace is logically mutable */
    LUFactorization *lu_mut = (LUFactorization*)lu;
    double *work = lu_mut->hs_work1;
    double *work2 = lu_mut->hs_work2;
    int *marked = lu_mut->hs_marked;
    int *temp_idx = lu_mut->hs_idx;
    double *temp_val = lu_mut->hs_val;

    /* Step 1: Apply row permutation to RHS */
    int perm_nnz = 0;
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            temp_idx[perm_nnz] = lu->perm_inv[orig_row];
            temp_val[perm_nnz] = rhs_val[k];
            perm_nnz++;
        }
    }

    /* Step 2: Sparse L solve */
    int L_nnz;
    solve_L_sparse(lu, perm_nnz, temp_idx, temp_val, work, temp_idx, &L_nnz, marked);

    /* Step 3: Sparse U solve */
    /* Gather values for U solve */
    for (int k = 0; k < L_nnz; k++) {
        temp_val[k] = work[temp_idx[k]];
    }

    int U_nnz;
    solve_U_sparse(lu, L_nnz, temp_idx, temp_val, work2, temp_idx, &U_nnz, marked);

    /* Step 4: Apply FT/eta updates */
    if (lu->use_ft_updates && lu->ft_num_updates > 0) {
        apply_ft_spikes_forward(lu, work2);
    } else if (lu->num_eta > 0) {
        apply_eta_forward(lu, work2);
    }

    /* Step 5: Apply column permutation and build output */
    memset(solution, 0, m * sizeof(double));
    if (sol_nnz) *sol_nnz = 0;

    for (int i = 0; i < m; i++) {
        if (fabs(work2[i]) > RALPH_ZERO_TOL) {
            int out_idx = lu->col_perm[i];
            solution[out_idx] = work2[i];
            if (sol_idx && sol_nnz) {
                sol_idx[(*sol_nnz)++] = out_idx;
            }
        }
    }

    /* Clear workspace for next use (only clear used parts) */
    for (int k = 0; k < L_nnz; k++) {
        work[temp_idx[k]] = 0.0;
    }
    for (int i = 0; i < m; i++) {
        work2[i] = 0.0;  /* FT updates may have touched all entries */
    }
}

/*
 * Hyper-sparse BTRAN: Solve B'x = b where b is very sparse.
 *
 * Used for computing dual prices when cost vector is sparse.
 */
void lu_btran_hyper_sparse(const LUFactorization *lu,
                           int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                           double *solution,
                           int *sol_idx, int *sol_nnz) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Threshold for falling back to dense */
    if (nnz_rhs > m / 8) {
        lu_solve_transpose_sparse(lu, nnz_rhs, rhs_idx, rhs_val, solution);
        if (sol_idx && sol_nnz) {
            *sol_nnz = 0;
            for (int j = 0; j < m; j++) {
                if (fabs(solution[j]) > RALPH_ZERO_TOL) {
                    sol_idx[(*sol_nnz)++] = j;
                }
            }
        }
        return;
    }

    /* For transpose solve, the operations are reversed:
     * B' = Q U' L' P
     * B'^{-1} = P' L'^{-1} U'^{-1} Q'
     *
     * 1. Apply Q' (inverse column perm)
     * 2. Apply eta/FT updates in reverse
     * 3. Solve U'^{-1} (forward sub on U')
     * 4. Solve L'^{-1} (backward sub on L')
     * 5. Apply P'
     */

    /* Use pre-allocated workspace - need to cast away const for workspace access */
    LUFactorization *lu_mut = (LUFactorization*)lu;
    double *work = lu_mut->hs_work1;
    double *work2 = lu_mut->hs_work2;

    /* Step 1: Apply inverse column permutation */
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_idx = rhs_idx[k];
        if (orig_idx >= 0 && orig_idx < m) {
            int step_pos = lu->col_perm_inv[orig_idx];
            work[step_pos] = rhs_val[k];
        }
    }

    /* Step 2: Apply updates in reverse */
    if (lu->use_ft_updates && lu->ft_num_updates > 0) {
        apply_ft_spikes_backward(lu, work);
    } else if (lu->num_eta > 0) {
        apply_eta_backward(lu, work);
    }

    /* Step 3 & 4: Solve U' and L' (use dense for now - transpose sparsity is different) */
    solve_Ut(lu, work, work2);
    solve_Lt(lu, work2, solution);

    /* Build sparse output */
    if (sol_idx && sol_nnz) {
        *sol_nnz = 0;
        for (int j = 0; j < m; j++) {
            if (fabs(solution[j]) > RALPH_ZERO_TOL) {
                sol_idx[(*sol_nnz)++] = j;
            }
        }
    }

    /* Clear workspace */
    memset(work, 0, m * sizeof(double));
    memset(work2, 0, m * sizeof(double));
}

/* ============================================================================
 * Basis Updates - Forrest-Tomlin and Eta File Methods
 * ============================================================================ */

/*
 * Forrest-Tomlin update: maintains sparsity better than eta-file.
 *
 * When column k of B is replaced by entering column a_q:
 * 1. Compute spike s = U^{-1} * L^{-1} * P * a_q
 * 2. s[k] is the pivot (must be non-zero)
 * 3. Move column k to the end of the factorization order
 * 4. Store the spike for use in future solves
 *
 * Benefit: Spikes are stored sparsely and don't accumulate fill-in
 * the way eta matrices do.
 */

/* Apply Forrest-Tomlin spikes during forward solve
 * FT spikes are stored with the same format as eta matrices:
 * spike[col] = 1/pivot, spike[i] = -original_spike[i]/pivot for i != col
 *
 * Application is identical to eta-file:
 * x[col] = spike[col] * x_old[col]
 * x[i] += spike[i] * x_old[col] for i != col
 */
static void apply_ft_spikes_forward(const LUFactorization *lu, double *x) {
    for (int k = 0; k < lu->ft_num_updates; k++) {
        int col = lu->ft_spike_col[k];
        int *idx = lu->ft_spike_idx[k];
        double *val = lu->ft_spike_val[k];
        int nnz = lu->ft_spike_nnz[k];

        double xc = x[col];  /* Save original x[col] */

        /* Update all components */
        for (int p = 0; p < nnz; p++) {
            int i = idx[p];
            if (i == col) {
                x[i] = val[p] * xc;
            } else {
                x[i] += val[p] * xc;
            }
        }
    }
}

/* Apply Forrest-Tomlin spikes during backward solve (transpose)
 * For transpose: (E^-1)' * x computes x[col] = spike' * x = sum_i spike[i] * x[i]
 * Applied in reverse order.
 */
static void apply_ft_spikes_backward(const LUFactorization *lu, double *x) {
    for (int k = lu->ft_num_updates - 1; k >= 0; k--) {
        int col = lu->ft_spike_col[k];
        int *idx = lu->ft_spike_idx[k];
        double *val = lu->ft_spike_val[k];
        int nnz = lu->ft_spike_nnz[k];

        /* Compute new x[col] = spike' * x (sparse dot product) */
        double xc = 0.0;
        for (int p = 0; p < nnz; p++) {
            xc += val[p] * x[idx[p]];
        }
        x[col] = xc;
    }
}

/* Update factorization when basis column changes */
int lu_update(LUFactorization *lu, int leaving_pos, const double *entering_col) {
    if (!lu || !entering_col) return -1;
    if (lu->num_updates >= lu->max_updates) return -1;  /* Need refactorization */

    int m = lu->m;

    /* Convert leaving_pos to step coordinates */
    int step_pos = lu->col_perm_inv[leaving_pos];

    /* Solve for spike: s = U^{-1} * L^{-1} * P * entering_col */
    double *work = (double*)malloc(m * sizeof(double));
    double *spike = (double*)malloc(m * sizeof(double));
    if (!work || !spike) {
        free(work);
        free(spike);
        return -1;
    }

    /* Solve L * y = P * entering_col */
    solve_L(lu, entering_col, work);

    /* Solve U * spike = y */
    solve_U(lu, work, spike);

    /* Apply existing updates (FT spikes or eta matrices) */
    if (lu->use_ft_updates && lu->ft_num_updates > 0) {
        apply_ft_spikes_forward(lu, spike);
    } else {
        apply_eta_forward(lu, spike);
    }

    /* Check pivot element (in step coordinates) */
    if (fabs(spike[step_pos]) < RALPH_PIVOT_TOL) {
        free(work);
        free(spike);
        return -1;  /* Singular update */
    }

    /* Normalize spike column and count non-zeros */
    double pivot = spike[step_pos];
    int nnz = 0;
    double max_spike = 0.0;
    for (int i = 0; i < m; i++) {
        if (i == step_pos) {
            spike[i] = 1.0 / pivot;
        } else {
            spike[i] = -spike[i] / pivot;
        }
        double absval = fabs(spike[i]);
        if (absval > max_spike) max_spike = absval;
        if (absval > RALPH_ZERO_TOL) nnz++;
    }

    /* Convert to sparse storage */
    int *indices = (int*)malloc(nnz * sizeof(int));
    double *values = (double*)malloc(nnz * sizeof(double));
    if (!indices || !values) {
        free(work);
        free(spike);
        free(indices);
        free(values);
        return -1;
    }

    int p = 0;
    for (int i = 0; i < m; i++) {
        if (fabs(spike[i]) > RALPH_ZERO_TOL) {
            indices[p] = i;
            values[p] = spike[i];
            p++;
        }
    }

    /* Store as Forrest-Tomlin spike or eta-file update */
    if (lu->use_ft_updates) {
        /* Store as FT spike */
        int k = lu->ft_num_updates;
        lu->ft_spike_col[k] = step_pos;
        lu->ft_spike_idx[k] = indices;
        lu->ft_spike_val[k] = values;
        lu->ft_spike_nnz[k] = nnz;
        lu->ft_num_updates++;
    } else {
        /* Store as eta-file update */
        lu->eta_col[lu->num_eta] = step_pos;
        lu->eta_indices[lu->num_eta] = indices;
        lu->eta_values[lu->num_eta] = values;
        lu->eta_nnz[lu->num_eta] = nnz;
        lu->num_eta++;
    }
    lu->num_updates++;

    /* Track growth factor */
    if (max_spike > lu->growth_factor) {
        lu->growth_factor = max_spike;
    }

    free(work);
    free(spike);
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
