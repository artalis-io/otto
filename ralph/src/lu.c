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
#include <limits.h>
#include "lp.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/* Refactorize when spike pool exceeds this percentage of capacity */
#define RALPH_SPIKE_POOL_WARN_PCT 85

/* Forward declarations for reach computation (used by sparse solves) */
static void compute_reach_L(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked);
static void compute_reach_U(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked);

/* ============================================================================
 * LU Factorization Creation/Destruction
 * ============================================================================ */

LUFactorization* lu_create(int m) {
    LUFactorization *lu = (LUFactorization*)calloc(1, sizeof(LUFactorization));
    if (!lu) return NULL;

    lu->m = m;

    /* Refactorization threshold based on problem size.
     * With Forrest-Tomlin updates, spike application cost is O(num_spikes * avg_nnz).
     * Profiling shows: too few updates = excessive refactorization (60% of time),
     * too many updates = excessive spike application.
     * Optimal balance: around 150-200 updates for large problems.
     * Rule: m/5 for small, m/10 for medium, ~150 for large (capped). */
    lu->max_updates = (m < 100) ? 50 : (m < 500) ? m/5 : 200;
    int max_upd = lu->max_updates;

    /* Calculate arena size for fixed-size arrays (with 8-byte alignment padding).
     * Arena contains: permutation arrays, FT column order, spike metadata,
     * eta metadata, and hyper-sparse workspace arrays.
     * 20 arrays total, add 20*8=160 bytes for alignment padding. */
    size_t arena_size =
        /* int arrays of size m: perm, perm_inv, col_perm, col_perm_inv,
         * ft_col_order, ft_col_order_inv, hs_marked, hs_idx, hs_stack (9 arrays) */
        9 * (size_t)m * sizeof(int) +
        /* double arrays of size m: U_diag, hs_work1, hs_work2, hs_val, perm_work (5 arrays) */
        5 * (size_t)m * sizeof(double) +
        /* int arrays of size max_updates: eta_col, eta_nnz,
         * ft_spike_col, ft_spike_nnz, ft_spike_start (5 arrays) */
        5 * (size_t)max_upd * sizeof(int) +
        /* double array of size max_updates: ft_spike_diag (1 array) */
        (size_t)max_upd * sizeof(double) +
        /* Alignment padding */
        160;

    lu->arena = sh_arena_create(arena_size);
    if (!lu->arena) {
        lu_free(lu);
        return NULL;
    }

    /* Allocate permutation arrays from arena */
    lu->perm = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->perm_inv = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->col_perm = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->col_perm_inv = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));

    /* U diagonal cache from arena */
    lu->U_diag = (double*)sh_arena_alloc(lu->arena, m * sizeof(double));

    /* Initialize to identity permutation */
    for (int i = 0; i < m; i++) {
        lu->perm[i] = i;
        lu->perm_inv[i] = i;
        lu->col_perm[i] = i;
        lu->col_perm_inv[i] = i;
    }

    /* Eta file metadata from arena (but eta_indices/values arrays allocated separately) */
    lu->eta_capacity = max_upd;
    lu->num_eta = 0;
    lu->eta_col = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->eta_nnz = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));

    /* eta_indices and eta_values are arrays of pointers - allocated separately
     * because their contents are dynamically allocated during updates */
    lu->eta_indices = (int**)malloc(max_upd * sizeof(int*));
    lu->eta_values = (double**)malloc(max_upd * sizeof(double*));

    if (!lu->eta_indices || !lu->eta_values) {
        lu_free(lu);
        return NULL;
    }

    for (int i = 0; i < max_upd; i++) {
        lu->eta_indices[i] = NULL;
        lu->eta_values[i] = NULL;
        lu->eta_nnz[i] = 0;
    }

    /* Forrest-Tomlin update structures from arena */
    lu->use_ft_updates = 1;
    lu->ft_num_updates = 0;
    lu->ft_col_order = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ft_col_order_inv = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));

    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }

    /* Spike metadata from arena */
    lu->ft_spike_capacity = max_upd;
    lu->ft_spike_col = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->ft_spike_diag = (double*)sh_arena_alloc(lu->arena, max_upd * sizeof(double));
    lu->ft_spike_nnz = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->ft_spike_start = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));

    for (int i = 0; i < max_upd; i++) {
        lu->ft_spike_nnz[i] = 0;
        lu->ft_spike_diag[i] = 0.0;
        lu->ft_spike_start[i] = 0;
    }

    /* Hyper-sparse workspace from arena */
    lu->hs_work1 = (double*)sh_arena_calloc(lu->arena, m, sizeof(double));
    lu->hs_work2 = (double*)sh_arena_calloc(lu->arena, m, sizeof(double));
    lu->hs_marked = (int*)sh_arena_calloc(lu->arena, m, sizeof(int));
    lu->hs_idx = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->hs_val = (double*)sh_arena_alloc(lu->arena, m * sizeof(double));
    lu->hs_stack = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->perm_work = (double*)sh_arena_alloc(lu->arena, m * sizeof(double));

    /* Single check for all arena allocations */
    if (!lu->perm || !lu->perm_inv || !lu->col_perm || !lu->col_perm_inv ||
        !lu->U_diag || !lu->eta_col || !lu->eta_nnz ||
        !lu->ft_col_order || !lu->ft_col_order_inv ||
        !lu->ft_spike_col || !lu->ft_spike_diag || !lu->ft_spike_nnz || !lu->ft_spike_start ||
        !lu->hs_work1 || !lu->hs_work2 || !lu->hs_marked ||
        !lu->hs_idx || !lu->hs_val || !lu->hs_stack || !lu->perm_work) {
        lu_free(lu);
        return NULL;
    }

    /* Contiguous spike pool - allocated separately (large, variable size)
     * Estimate: each spike has ~m/2 non-zeros on average.
     * Use size_t to prevent integer overflow on large problems. */
    {
        size_t pool_size = (size_t)max_upd * ((size_t)m / 2 + 20);
        if (pool_size > (size_t)INT_MAX) {
            pool_size = (size_t)INT_MAX;
        }
        lu->spike_pool_capacity = (int)pool_size;
    }
    lu->spike_pool_idx = (int*)malloc(lu->spike_pool_capacity * sizeof(int));
    lu->spike_pool_val = (double*)malloc(lu->spike_pool_capacity * sizeof(double));
    lu->spike_pool_used = 0;

    if (!lu->spike_pool_idx || !lu->spike_pool_val) {
        lu_free(lu);
        return NULL;
    }

    /* Spike compaction settings */
    lu->ft_compact_interval = 300;
    lu->ft_num_compacted = 0;
    lu->ft_compact_matrix = NULL;  /* Allocated lazily if needed */
    lu->ft_compact_valid = 0;

    /* Initialize condition number tracking */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    lu->cond_estimate = 1.0;
    lu->growth_factor = 1.0;

    /* Pre-allocate dense workspace for fallback factorization (m×m matrix)
     * Allocated separately due to large size O(m²) */
    lu->dense_work = (double*)malloc((size_t)m * (size_t)m * sizeof(double));

    if (!lu->dense_work) {
        lu_free(lu);
        return NULL;
    }

    return lu;
}

void lu_free(LUFactorization *lu) {
    if (!lu) return;

    /* Free L/U storage (allocated during factorization, not in arena) */
    SAFE_FREE(lu->L_colptr);
    SAFE_FREE(lu->L_rowidx);
    SAFE_FREE(lu->L_values);
    SAFE_FREE(lu->U_colptr);
    SAFE_FREE(lu->U_rowidx);
    SAFE_FREE(lu->U_values);

    /* Free eta file contents (dynamically allocated during updates) */
    if (lu->eta_indices) {
        for (int i = 0; i < lu->eta_capacity; i++) {
            SAFE_FREE(lu->eta_indices[i]);
        }
        SAFE_FREE(lu->eta_indices);
    }
    if (lu->eta_values) {
        for (int i = 0; i < lu->eta_capacity; i++) {
            SAFE_FREE(lu->eta_values[i]);
        }
        SAFE_FREE(lu->eta_values);
    }

    /* Free compact matrix (allocated lazily, not in arena) */
    SAFE_FREE(lu->ft_compact_matrix);

    /* Free spike pool (large variable-size arrays, not in arena) */
    SAFE_FREE(lu->spike_pool_idx);
    SAFE_FREE(lu->spike_pool_val);

    /* Free dense workspace (O(m²), not in arena) */
    SAFE_FREE(lu->dense_work);

    /* Free arena (frees all fixed-size arrays in one call:
     * perm, perm_inv, col_perm, col_perm_inv, U_diag,
     * eta_col, eta_nnz, ft_col_order, ft_col_order_inv,
     * ft_spike_col, ft_spike_diag, ft_spike_nnz, ft_spike_start,
     * hs_work1, hs_work2, hs_marked, hs_idx, hs_val, hs_stack, perm_work) */
    sh_arena_free(lu->arena);
    lu->arena = NULL;

    /* NULL out arena-allocated pointers for safety */
    lu->perm = NULL;
    lu->perm_inv = NULL;
    lu->col_perm = NULL;
    lu->col_perm_inv = NULL;
    lu->U_diag = NULL;
    lu->eta_col = NULL;
    lu->eta_nnz = NULL;
    lu->ft_col_order = NULL;
    lu->ft_col_order_inv = NULL;
    lu->ft_spike_col = NULL;
    lu->ft_spike_diag = NULL;
    lu->ft_spike_nnz = NULL;
    lu->ft_spike_start = NULL;
    lu->hs_work1 = NULL;
    lu->hs_work2 = NULL;
    lu->hs_marked = NULL;
    lu->hs_idx = NULL;
    lu->hs_val = NULL;
    lu->hs_stack = NULL;
    lu->perm_work = NULL;

    free(lu);
}

/* External sparse factorization (from lu_sparse.c) */
int lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B);
int lu_factorize_sparse_efficient(LUFactorization *lu, const SparseMatrix *B);

/* ============================================================================
 * Main LU Factorization Entry Point
 * ============================================================================ */

/* Main factorization entry point.
 *
 * Uses the efficient sparse implementation with AMD ordering for fill-in
 * reduction. Falls back to dense if sparse fails.
 */
int lu_factorize(LUFactorization *lu, const SparseMatrix *B) {
    /* Try efficient sparse factorization first */
    int result = lu_factorize_sparse_efficient(lu, B);
    if (result == 0) return 0;

    /* Fall back to dense */
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

    /* Use pre-allocated dense workspace (m×m matrix) */
    double *A = lu->dense_work;
    if (!A) return -1;

    /* Zero the workspace */
    memset(A, 0, (size_t)m * (size_t)m * sizeof(double));

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

    /* Clear Forrest-Tomlin spikes - contiguous pool storage, just reset counters */
    for (int i = 0; i < lu->ft_num_updates; i++) {
        lu->ft_spike_nnz[i] = 0;
        lu->ft_spike_diag[i] = 0.0;
        lu->ft_spike_start[i] = 0;
    }
    lu->ft_num_updates = 0;
    lu->ft_num_compacted = 0;
    lu->ft_compact_valid = 0;
    lu->spike_pool_used = 0;  /* Reset contiguous pool */
    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }

    lu->num_updates = 0;

    /* Extract U diagonals and compute condition number estimate */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    for (int j = 0; j < m; j++) {
        lu->U_diag[j] = 0.0;  /* Default in case not found */
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            if (lu->U_rowidx[p] == j) {
                double val = lu->U_values[p];
                lu->U_diag[j] = val;
                double absval = fabs(val);
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

    /* Note: A is pre-allocated lu->dense_work, no free needed */
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
    const double *U_diag = lu->U_diag;

    vec_copy_data(x, b, m);

    /* Backward substitution - use cached diagonals for speed */
    for (int j = m - 1; j >= 0; j--) {
        double diag = U_diag[j];

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[j] = 0.0;  /* Effectively zero row */
            continue;
        }

        x[j] /= diag;
        double xj = x[j];

        /* Update remaining elements (off-diagonal) */
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
    const double *U_diag = lu->U_diag;

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

        double diag = U_diag[i];

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
     * Threshold: use sparse path for RHS with <= 25% density (m/4)
     */
    if (nnz_rhs > m / 4) {
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

    /* Sparse path: Clear work2 for sparse solve (work already cleared) */
    memset(work2, 0, m * sizeof(double));

    /* Build permuted RHS and track nonzero indices */
    int *perm_rhs_idx = (int*)lu->perm_work;  /* Reuse perm_work as int array */
    int perm_rhs_nnz = 0;
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            int perm_row = lu->perm_inv[orig_row];
            work[perm_row] = rhs_val[k];
            perm_rhs_idx[perm_rhs_nnz++] = perm_row;
        }
    }

    /* Compute reach through L - only these indices need processing */
    int *reach = lu->hs_idx;
    int reach_nnz;
    compute_reach_L(lu, perm_rhs_nnz, perm_rhs_idx, reach, &reach_nnz, lu->hs_marked);

    /* Forward solve L: only process indices in reach (sorted ascending) */
    for (int i = 0; i < reach_nnz; i++) {
        int j = reach[i];
        double xj = work[j];
        if (fabs(xj) < RALPH_ZERO_TOL) continue;

        /* Update rows below j where L[i,j] != 0 */
        for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
            work[lu->L_rowidx[p]] -= lu->L_values[p] * xj;
        }
    }

    /* Backward solve U - use reach-based solve for U as well */
    /* After L solve, nonzeros are at reach indices; use these for U reach */
    int *u_reach = (int*)lu->perm_work;  /* Reuse for U reach */
    int u_reach_nnz;
    compute_reach_U(lu, reach_nnz, reach, u_reach, &u_reach_nnz, lu->hs_marked);

    /* Backward solve U: only process indices in u_reach (sorted descending) */
    for (int i = 0; i < u_reach_nnz; i++) {
        int j = u_reach[i];

        /* Use cached diagonal for speed (avoids O(nnz_col) search) */
        double diag = lu->U_diag[j];

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            work2[j] = 0.0;
            continue;
        }

        work2[j] = work[j] / diag;
        double xj = work2[j];

        if (fabs(xj) > RALPH_ZERO_TOL) {
            /* Update predecessors */
            for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
                int r = lu->U_rowidx[p];
                if (r < j) {
                    work[r] -= lu->U_values[p] * xj;
                }
            }
        }
    }

    /* Clear work for non-reached indices to avoid stale values */
    for (int i = 0; i < reach_nnz; i++) {
        work[reach[i]] = 0.0;
    }

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
     * Threshold: use sparse path for RHS with <= 25% density (m/4)
     */
    if (nnz_rhs > m / 4) {
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
 *
 * OPTIMIZED: Collects visited indices during DFS, then sorts only those
 * instead of scanning all m indices. This makes the function O(reach) instead of O(m).
 */
static void compute_reach_L(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked) {
    int m = lu->m;
    *reach_nnz = 0;

    /* For lower triangular L, topological order is ascending indices.
     * Strategy: mark reachable indices via DFS, then scan 0..m-1 to collect.
     * This avoids sorting since we collect in ascending order naturally.
     */

    /* Track min/max reached for efficient scan bounds */
    int min_reached = m, max_reached = -1;

    /* Stack-based DFS to mark all reachable indices */
    int *stack = lu->hs_stack;  /* Dedicated stack workspace */
    int stack_top;

    for (int k = 0; k < nnz_rhs; k++) {
        int start = rhs_idx[k];
        if (start < 0 || start >= m || marked[start]) continue;

        stack_top = 0;
        stack[stack_top++] = start;
        marked[start] = 1;
        if (start < min_reached) min_reached = start;
        if (start > max_reached) max_reached = start;

        while (stack_top > 0) {
            int j = stack[--stack_top];

            /* Visit all unvisited children (L[i,j] != 0 means i > j) */
            for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
                int i = lu->L_rowidx[p];
                if (marked[i] == 0) {
                    marked[i] = 1;
                    stack[stack_top++] = i;
                    if (i > max_reached) max_reached = i;
                }
            }
        }
    }

    /* Collect marked indices in ascending order (no sorting needed) */
    for (int j = min_reached; j <= max_reached; j++) {
        if (marked[j]) {
            reach_out[(*reach_nnz)++] = j;
            marked[j] = 0;  /* Clear as we go */
        }
    }
}

/*
 * Compute reach of sparse RHS through upper triangular U using DFS.
 * Returns indices in reverse topological order (decreasing for upper triangular).
 *
 * OPTIMIZED: Uses mark + descending scan instead of collect + sort.
 * For upper triangular U, topological order is descending indices.
 * Strategy: mark reachable via DFS, then scan max..min to collect in order.
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

    /* Track min/max reached for efficient scan bounds */
    int min_reached = m, max_reached = -1;

    int *stack = lu->hs_stack;  /* Dedicated stack workspace */
    int stack_top;

    for (int k = 0; k < nnz_rhs; k++) {
        int start = rhs_idx[k];
        if (start < 0 || start >= m || marked[start]) continue;

        stack_top = 0;
        stack[stack_top++] = start;
        marked[start] = 1;
        if (start < min_reached) min_reached = start;
        if (start > max_reached) max_reached = start;

        while (stack_top > 0) {
            int j = stack[--stack_top];

            /* Visit all unvisited predecessors (row i < j where U[i,j] != 0) */
            for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
                int i = lu->U_rowidx[p];
                if (i < j && marked[i] == 0) {
                    marked[i] = 1;
                    stack[stack_top++] = i;
                    if (i < min_reached) min_reached = i;
                }
            }
        }
    }

    /* Collect marked indices in DESCENDING order (no sorting needed) */
    for (int j = max_reached; j >= min_reached; j--) {
        if (marked[j]) {
            reach_out[(*reach_nnz)++] = j;
            marked[j] = 0;  /* Clear as we go */
        }
    }
}

/*
 * Sparse forward solve: Lx = b where b and x are sparse.
 *
 * NOTE: b_idx and x_idx MUST NOT alias. The caller must ensure this.
 *
 * Input:
 *   nnz_b, b_idx, b_val: sparse RHS (in permuted coordinates)
 *   x: dense output vector (will be cleared for reach indices)
 *   marked: workspace of size m
 *
 * Output:
 *   x: solution values at reach indices
 *   x_idx, x_nnz: sparse representation of result
 *   reach_nnz_out: total reach size (for cleanup)
 */
static void solve_L_sparse(const LUFactorization *lu,
                           int nnz_b, const int *b_idx, const double *b_val,
                           double *x,
                           int *x_idx, int *x_nnz,
                           int *marked,
                           int *reach_nnz_out) {
    int m = lu->m;

    /* Compute reach - stored in x_idx */
    int *reach = x_idx;
    int reach_nnz;
    compute_reach_L(lu, nnz_b, b_idx, reach, &reach_nnz, marked);

    /* Clear x for all reach indices (critical for correctness!) */
    for (int k = 0; k < reach_nnz; k++) {
        x[reach[k]] = 0.0;
    }

    /* Initialize x with RHS values (b_idx must not alias x_idx!) */
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

    /* Return reach size for caller to use for cleanup */
    if (reach_nnz_out) *reach_nnz_out = reach_nnz;

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
 *
 * NOTE: b_idx and x_idx MUST NOT alias. The caller must ensure this.
 */
static void solve_U_sparse(const LUFactorization *lu,
                           int nnz_b, const int *b_idx, const double *b_val,
                           double *x,
                           int *x_idx, int *x_nnz,
                           int *marked,
                           int *reach_nnz_out) {
    int m = lu->m;

    /* Compute reach */
    int *reach = x_idx;
    int reach_nnz;
    compute_reach_U(lu, nnz_b, b_idx, reach, &reach_nnz, marked);

    /* Clear x for all reach indices (critical for correctness!) */
    for (int k = 0; k < reach_nnz; k++) {
        x[reach[k]] = 0.0;
    }

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

        /* Use cached diagonal for speed (avoids O(nnz_col) search) */
        double diag = lu->U_diag[j];

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

    /* Return reach size for caller to use for cleanup */
    if (reach_nnz_out) *reach_nnz_out = reach_nnz;

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
    int *perm_rhs_idx = lu_mut->hs_idx;   /* Permuted RHS indices (input to L solve) */
    int *L_out_idx = (int*)lu_mut->perm_work;  /* L solve output indices (separate!) */
    double *temp_val = lu_mut->hs_val;

    /* Clear work2 - may have stale values from lu_update or previous calls */
    memset(work2, 0, m * sizeof(double));

    /* Step 1: Apply row permutation to RHS */
    int perm_nnz = 0;
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            perm_rhs_idx[perm_nnz] = lu->perm_inv[orig_row];
            temp_val[perm_nnz] = rhs_val[k];
            perm_nnz++;
        }
    }

    /* Step 2: Sparse L solve
     * Input: perm_rhs_idx, temp_val (no aliasing with L_out_idx) */
    int L_nnz, L_reach_nnz;
    solve_L_sparse(lu, perm_nnz, perm_rhs_idx, temp_val, work, L_out_idx, &L_nnz, marked, &L_reach_nnz);

    /* Step 3: Sparse U solve */
    /* Gather values for U solve - L_out_idx has L solve nonzero indices */
    for (int k = 0; k < L_nnz; k++) {
        temp_val[k] = work[L_out_idx[k]];
    }

    /* U solve: L_out_idx is INPUT, reuse perm_rhs_idx as OUTPUT (safe now) */
    int U_nnz, U_reach_nnz;
    solve_U_sparse(lu, L_nnz, L_out_idx, temp_val, work2, perm_rhs_idx, &U_nnz, marked, &U_reach_nnz);

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

    /* Clear workspace - FT updates may touch all entries so full clear needed */
    for (int i = 0; i < m; i++) {
        work2[i] = 0.0;
    }
    /* work is cleared by next solve_L_sparse call, no action needed here */
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

/*
 * Compact multiple FT spikes into a dense matrix for faster application.
 * The product M = E_N * ... * E_1 is computed and stored.
 * This is O(N * m) but applying M is just O(m²) instead of O(N * nnz).
 */
static void compact_ft_spikes(LUFactorization *lu, int start, int end) {
    int m = lu->m;

    /* Allocate compact matrix if needed (stored row-major for cache efficiency) */
    if (!lu->ft_compact_matrix) {
        lu->ft_compact_matrix = (double*)malloc(m * m * sizeof(double));
        if (!lu->ft_compact_matrix) return;
    }

    double *M = lu->ft_compact_matrix;

    /* Initialize to identity */
    memset(M, 0, m * m * sizeof(double));
    for (int i = 0; i < m; i++) {
        M[i * m + i] = 1.0;
    }

    /* Apply spikes to M: M = E_k * M for k = start..end-1
     * E_k * M:
     *   row[col] = diag * row[col]
     *   row[i] = row[i] + spike[i] * row[col]  (for i in off-diag)
     * IMPORTANT: Must use original row[col] for off-diag updates!
     */
    double *row_copy = lu->perm_work;  /* Temporary for row copy */

    for (int k = start; k < end; k++) {
        int col = lu->ft_spike_col[k];
        double diag = lu->ft_spike_diag[k];
        int spike_start = lu->ft_spike_start[k];
        int nnz = lu->ft_spike_nnz[k];

        double *row_col = &M[col * m];

        /* Save original row[col] for off-diagonal updates */
        memcpy(row_copy, row_col, m * sizeof(double));

        /* Scale row[col] by diag */
        for (int j = 0; j < m; j++) {
            row_col[j] *= diag;
        }

        /* Update other affected rows using ORIGINAL row[col] values */
        for (int p = 0; p < nnz; p++) {
            int i = lu->spike_pool_idx[spike_start + p];
            double v = lu->spike_pool_val[spike_start + p];
            double *row_i = &M[i * m];
            for (int j = 0; j < m; j++) {
                row_i[j] += v * row_copy[j];
            }
        }
    }

    lu->ft_num_compacted = end;
    lu->ft_compact_valid = 1;
}

/* Apply compacted matrix M to x: x = M * x */
static void apply_compacted_matrix(const LUFactorization *lu, double *x) {
    int m = lu->m;
    const double *M = lu->ft_compact_matrix;
    double *work = lu->perm_work;  /* Temporary storage */

    /* Dense matrix-vector multiply with SIMD */
    for (int i = 0; i < m; i++) {
        double sum = 0.0;
        const double *row = &M[i * m];
        #pragma omp simd reduction(+:sum)
        for (int j = 0; j < m; j++) {
            sum += row[j] * x[j];
        }
        work[i] = sum;
    }

    /* Copy result back */
    memcpy(x, work, m * sizeof(double));
}

/* Apply Forrest-Tomlin spikes during forward solve
 * FT spikes are stored with the same format as eta matrices:
 * spike[col] = 1/pivot, spike[i] = -original_spike[i]/pivot for i != col
 *
 * Application is identical to eta-file:
 * x[col] = spike[col] * x_old[col]
 * x[i] += spike[i] * x_old[col] for i != col
 */
static void apply_ft_spikes_forward(const LUFactorization *lu, double *x) {
    const int n = lu->ft_num_updates;

    /* If we have compacted spikes, apply the compacted matrix first */
    if (lu->ft_compact_valid && lu->ft_num_compacted > 0) {
        apply_compacted_matrix(lu, x);

        /* Apply only the non-compacted spikes from contiguous pool */
        const int start_spike = lu->ft_num_compacted;
        const int *cols = lu->ft_spike_col;
        const double *diags = lu->ft_spike_diag;
        const int *starts = lu->ft_spike_start;
        const int *nnzs = lu->ft_spike_nnz;
        const int *pool_idx = lu->spike_pool_idx;
        const double *pool_val = lu->spike_pool_val;

        for (int k = start_spike; k < n; k++) {
            int col = cols[k];
            double xc = x[col];
            if (fabs(xc) < RALPH_ZERO_TOL) continue;
            x[col] = diags[k] * xc;
            int start = starts[k];
            int nnz = nnzs[k];
            const int *idx = pool_idx + start;
            const double *val = pool_val + start;
            for (int p = 0; p < nnz; p++) {
                x[idx[p]] += val[p] * xc;
            }
        }
        return;
    }

    /* No compaction - apply all spikes individually using contiguous pool */
    const int *cols = lu->ft_spike_col;
    const double *diags = lu->ft_spike_diag;
    const int *starts = lu->ft_spike_start;
    const int *nnzs = lu->ft_spike_nnz;
    const int *pool_idx = lu->spike_pool_idx;
    const double *pool_val = lu->spike_pool_val;

    for (int k = 0; k < n; k++) {
        int col = cols[k];
        double xc = x[col];  /* Save original x[col] */

        /* Skip if xc is zero - no update needed */
        if (fabs(xc) < RALPH_ZERO_TOL) continue;

        /* Update diagonal (branchless) */
        x[col] = diags[k] * xc;

        /* Update off-diagonal entries from contiguous pool
         * Use local pointers for better cache access */
        int start = starts[k];
        int nnz = nnzs[k];
        const int *idx = pool_idx + start;
        const double *val = pool_val + start;

        for (int p = 0; p < nnz; p++) {
            x[idx[p]] += val[p] * xc;
        }
    }
}

/* Apply compacted matrix transpose M' to x: x = M' * x */
static void apply_compacted_matrix_transpose(const LUFactorization *lu, double *x) {
    int m = lu->m;
    const double *M = lu->ft_compact_matrix;
    double *work = lu->perm_work;

    /* Dense matrix-vector multiply with M' (column-major access of row-major M)
     * Note: Strided access (stride=m) is less SIMD-friendly but still benefits */
    for (int j = 0; j < m; j++) {
        double sum = 0.0;
        #pragma omp simd reduction(+:sum)
        for (int i = 0; i < m; i++) {
            sum += M[i * m + j] * x[i];  /* M'[j,i] = M[i,j] */
        }
        work[j] = sum;
    }

    memcpy(x, work, m * sizeof(double));
}

/* Apply Forrest-Tomlin spikes during backward solve (transpose)
 * For transpose: (E^-1)' * x computes x[col] = spike' * x = sum_i spike[i] * x[i]
 * Applied in reverse order.
 */
static void apply_ft_spikes_backward(const LUFactorization *lu, double *x) {
    const int n = lu->ft_num_updates;
    const int *cols = lu->ft_spike_col;
    const double *diags = lu->ft_spike_diag;
    const int *starts = lu->ft_spike_start;
    const int *nnzs = lu->ft_spike_nnz;
    const int *pool_idx = lu->spike_pool_idx;
    const double *pool_val = lu->spike_pool_val;

    /* Apply non-compacted spikes first (in reverse order) */
    int start_spike = lu->ft_compact_valid ? lu->ft_num_compacted : 0;

    for (int k = n - 1; k >= start_spike; k--) {
        int col = cols[k];
        int start = starts[k];
        int nnz = nnzs[k];

        /* Compute new x[col] = diag * x[col] + sum(off_diag * x)
         * SIMD reduction on the sparse dot product */
        double xc = diags[k] * x[col];
        const int *idx = pool_idx + start;
        const double *val = pool_val + start;
        #pragma omp simd reduction(+:xc)
        for (int p = 0; p < nnz; p++) {
            xc += val[p] * x[idx[p]];
        }
        x[col] = xc;
    }

    /* Then apply compacted matrix transpose if available */
    if (lu->ft_compact_valid && lu->ft_num_compacted > 0) {
        apply_compacted_matrix_transpose(lu, x);
    }
}

/* Update factorization when basis column changes */
int lu_update(LUFactorization *lu, int leaving_pos, const double *entering_col) {
    if (!lu || !entering_col) return -1;
    if (lu->num_updates >= lu->max_updates) return -1;  /* Need refactorization */

    int m = lu->m;

    /* Convert leaving_pos to step coordinates */
    int step_pos = lu->col_perm_inv[leaving_pos];

    /* Solve for spike: s = U^{-1} * L^{-1} * P * entering_col
     * Use pre-allocated workspaces to avoid malloc in hot path */
    double *work = lu->hs_work1;
    double *spike = lu->hs_work2;

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
        return -1;  /* Singular update */
    }

    /* Normalize spike column and count OFF-DIAGONAL non-zeros */
    double pivot = spike[step_pos];
    double diag_val = 1.0 / pivot;
    int off_diag_nnz = 0;
    double max_spike = fabs(diag_val);

    for (int i = 0; i < m; i++) {
        if (i != step_pos) {
            spike[i] = -spike[i] / pivot;
            double absval = fabs(spike[i]);
            if (absval > max_spike) max_spike = absval;
            if (absval > RALPH_ZERO_TOL) off_diag_nnz++;
        }
    }
    spike[step_pos] = diag_val;  /* For eta-file compatibility */

    /* Store as Forrest-Tomlin spike or eta-file update */
    if (lu->use_ft_updates) {
        /* Check if pool has room for this spike */
        if (lu->spike_pool_used + off_diag_nnz > lu->spike_pool_capacity) {
            return -1;  /* Pool full - need refactorization */
        }

        /* Store FT spike with contiguous pool storage */
        int k = lu->ft_num_updates;
        lu->ft_spike_col[k] = step_pos;
        lu->ft_spike_diag[k] = diag_val;
        lu->ft_spike_start[k] = lu->spike_pool_used;
        lu->ft_spike_nnz[k] = off_diag_nnz;

        /* Copy off-diagonal entries to contiguous pool */
        if (off_diag_nnz > 0) {
            int p = 0;
            for (int i = 0; i < m; i++) {
                if (i != step_pos && fabs(spike[i]) > RALPH_ZERO_TOL) {
                    lu->spike_pool_idx[lu->spike_pool_used + p] = i;
                    lu->spike_pool_val[lu->spike_pool_used + p] = spike[i];
                    p++;
                }
            }
            lu->spike_pool_used += off_diag_nnz;
        }

        lu->ft_num_updates++;
    } else {
        /* Store as eta-file update (includes diagonal) - still uses malloc */
        int total_nnz = off_diag_nnz + 1;  /* +1 for diagonal */
        int *indices = (int*)malloc(total_nnz * sizeof(int));
        double *values = (double*)malloc(total_nnz * sizeof(double));
        if (!indices || !values) {
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

        lu->eta_col[lu->num_eta] = step_pos;
        lu->eta_indices[lu->num_eta] = indices;
        lu->eta_values[lu->num_eta] = values;
        lu->eta_nnz[lu->num_eta] = total_nnz;
        lu->num_eta++;
    }
    lu->num_updates++;

    /* Track growth factor */
    if (max_spike > lu->growth_factor) {
        lu->growth_factor = max_spike;
    }

    /* Trigger spike compaction if interval reached and using FT updates */
    if (lu->use_ft_updates && lu->ft_compact_interval > 0) {
        int uncompacted = lu->ft_num_updates - lu->ft_num_compacted;
        if (uncompacted >= lu->ft_compact_interval) {
            compact_ft_spikes(lu, 0, lu->ft_num_updates);
        }
    }

    return 0;
}

int lu_needs_refactorization(const LUFactorization *lu) {
    if (!lu) return 0;

    /* Refactorize if max updates reached */
    if (lu->num_updates >= lu->max_updates) return 1;

    /* Refactorize early if condition has degraded significantly */
    if (lu->growth_factor > 1e6) return 1;

    /* Refactorize early if spike pool is nearly full
     * This prevents update failures when spike density is higher than expected */
    if (lu->use_ft_updates && lu->spike_pool_capacity > 0) {
        if (lu->spike_pool_used > lu->spike_pool_capacity * RALPH_SPIKE_POOL_WARN_PCT / 100) return 1;
    }

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
