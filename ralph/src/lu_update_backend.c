#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "lp_bfcp_policy.h"
#include "lu_update_backend.h"

static int lu_update_backend_is_schur_backend(const LUFactorization *lu) {
    if (!lu) return 0;
    return lu->update_backend == LU_UPDATE_BACKEND_BG_COMPAT ||
           lu->update_backend == LU_UPDATE_BACKEND_GR_COMPAT;
}

static int lu_update_backend_is_bg(const LUFactorization *lu) {
    return lu && lu->update_backend == LU_UPDATE_BACKEND_BG_COMPAT;
}

static int lu_update_backend_is_gr(const LUFactorization *lu) {
    return lu && lu->update_backend == LU_UPDATE_BACKEND_GR_COMPAT;
}

int lu_update_backend_is_ft(const LUFactorization *lu) {
    if (!lu) return 1;
    return lu->update_backend == LU_UPDATE_BACKEND_FT ? 1 : 0;
}

int lu_update_backend_storage_capacity(const LUFactorization *lu) {
    if (!lu) return 0;
    if (lu_update_backend_is_ft(lu)) {
        return lu->ft_spike_capacity > 0 ? lu->ft_spike_capacity : 0;
    }
    if (lu_update_backend_is_schur_backend(lu)) {
        return lu->schur_capacity > 0 ? lu->schur_capacity : 0;
    }
    return lu->eta_capacity > 0 ? lu->eta_capacity : 0;
}

int lu_update_backend_has_updates(const LUFactorization *lu) {
    if (!lu) return 0;
    if (lu_update_backend_is_ft(lu)) {
        return lu->ft_num_updates > 0 ? 1 : 0;
    }
    if (lu_update_backend_is_bg(lu) || lu_update_backend_is_gr(lu)) {
        return lu->schur_num_updates > 0 ? 1 : 0;
    }
    return lu->num_eta > 0 ? 1 : 0;
}

static void lu_mark_bad_input_and_force_refactor(const LUFactorization *lu_const) {
    LUFactorization *lu = (LUFactorization*)lu_const;

    if (!lu) return;
    lu->last_failure_reason = LU_FAIL_BAD_INPUT;
    if (lu->telemetry_enabled) {
        lu->telemetry.update_fail_bad_input++;
    }
    if (lu->max_updates > 0) {
        lu->num_updates = lu->max_updates;
    }
    lu->ft_num_updates = 0;
    lu->spike_pool_used = 0;
    lu->schur_num_updates = 0;
}

static void free_sparse_update_chain(int *count,
                                     int **indices,
                                     double **values,
                                     int *nnz) {
    int n;

    if (!count || !indices || !values) return;
    n = *count;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        SAFE_FREE(indices[i]);
        SAFE_FREE(values[i]);
        if (nnz) nnz[i] = 0;
    }
    *count = 0;
}

void lu_update_backend_reset(LUFactorization *lu) {
    if (!lu) return;

    free_sparse_update_chain(&lu->num_eta, lu->eta_indices, lu->eta_values, lu->eta_nnz);
    free_sparse_update_chain(&lu->schur_num_updates, lu->schur_indices, lu->schur_values, lu->schur_nnz);
    if (lu->schur_k && lu->schur_capacity > 0) {
        memset(lu->schur_k, 0, (size_t)lu->schur_capacity * (size_t)lu->schur_capacity * sizeof(double));
    }
    if (lu->schur_rhs && lu->schur_capacity > 0) {
        memset(lu->schur_rhs, 0, (size_t)lu->schur_capacity * sizeof(double));
    }
    if (lu->schur_piv && lu->schur_capacity > 0) {
        memset(lu->schur_piv, 0, (size_t)lu->schur_capacity * sizeof(int));
    }

    for (int i = 0; i < lu->ft_num_updates; i++) {
        lu->ft_spike_nnz[i] = 0;
        lu->ft_spike_diag[i] = 0.0;
        lu->ft_spike_start[i] = 0;
    }
    lu->ft_num_updates = 0;
    lu->spike_pool_used = 0;
    for (int i = 0; i < lu->m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }
}

static void apply_sparse_update_chain_forward(int count,
                                              const int *col,
                                              int *const *indices,
                                              double *const *values,
                                              const int *nnz,
                                              double *x) {
    for (int k = 0; k < count; k++) {
        int pivot_col = col[k];
        int *rows = indices[k];
        double *vals = values[k];
        int entries = nnz[k];
        double xc = x[pivot_col];

        for (int p = 0; p < entries; p++) {
            int i = rows[p];
            if (i == pivot_col) {
                x[i] = vals[p] * xc;
            } else {
                x[i] += vals[p] * xc;
            }
        }
    }
}

static void apply_sparse_update_chain_backward(int count,
                                               const int *col,
                                               int *const *indices,
                                               double *const *values,
                                               const int *nnz,
                                               double *x) {
    for (int k = count - 1; k >= 0; k--) {
        int pivot_col = col[k];
        int *rows = indices[k];
        double *vals = values[k];
        int entries = nnz[k];
        double xc = 0.0;

        for (int p = 0; p < entries; p++) {
            xc += vals[p] * x[rows[p]];
        }
        x[pivot_col] = xc;
    }
}

static double sparse_column_get_entry(const int *indices,
                                      const double *values,
                                      int nnz,
                                      int row) {
    for (int p = 0; p < nnz; p++) {
        if (indices[p] == row) return values[p];
    }
    return 0.0;
}

static void sparse_column_axpy(const int *indices,
                               const double *values,
                               int nnz,
                               double alpha,
                               double *x) {
    for (int p = 0; p < nnz; p++) {
        x[indices[p]] += alpha * values[p];
    }
}

static void bg_build_m_times_unit_column(const LUFactorization *lu,
                                         int step_pos,
                                         double *out) {
    int k = lu->schur_num_updates;
    int m = lu->m;

    memset(out, 0, (size_t)m * sizeof(double));
    if (step_pos >= 0 && step_pos < m) out[step_pos] = 1.0;
    for (int t = 0; t < k; t++) {
        if (lu->schur_col[t] != step_pos) continue;
        sparse_column_axpy(lu->schur_indices[t], lu->schur_values[t],
                           lu->schur_nnz[t], 1.0, out);
    }
}

static int bg_solve_dense(int n, double *A, int *piv, double *rhs) {
    for (int i = 0; i < n; i++) piv[i] = i;

    for (int col = 0; col < n; col++) {
        int piv_row = col;
        double max_abs = fabs(A[(size_t)col * n + col]);
        for (int row = col + 1; row < n; row++) {
            double absval = fabs(A[(size_t)row * n + col]);
            if (absval > max_abs) {
                max_abs = absval;
                piv_row = row;
            }
        }
        if (max_abs < RALPH_PIVOT_TOL) return -1;
        if (piv_row != col) {
            for (int j = 0; j < n; j++) {
                double tmp = A[(size_t)col * n + j];
                A[(size_t)col * n + j] = A[(size_t)piv_row * n + j];
                A[(size_t)piv_row * n + j] = tmp;
            }
            {
                double tmp_rhs = rhs[col];
                rhs[col] = rhs[piv_row];
                rhs[piv_row] = tmp_rhs;
            }
            {
                int tmp_p = piv[col];
                piv[col] = piv[piv_row];
                piv[piv_row] = tmp_p;
            }
        }
        for (int row = col + 1; row < n; row++) {
            double factor = A[(size_t)row * n + col] / A[(size_t)col * n + col];
            A[(size_t)row * n + col] = factor;
            for (int j = col + 1; j < n; j++) {
                A[(size_t)row * n + j] -= factor * A[(size_t)col * n + j];
            }
            rhs[row] -= factor * rhs[col];
        }
    }

    for (int row = n - 1; row >= 0; row--) {
        double sum = rhs[row];
        for (int j = row + 1; j < n; j++) {
            sum -= A[(size_t)row * n + j] * rhs[j];
        }
        rhs[row] = sum / A[(size_t)row * n + row];
    }
    return 0;
}

static int bg_apply_forward(const LUFactorization *lu, double *x) {
    int k = lu->schur_num_updates;
    int cap = lu->schur_capacity;

    if (k <= 0) return 0;
    if (!lu->schur_k || !lu->schur_k_work || !lu->schur_rhs || !lu->schur_piv) {
        return -1;
    }

    for (int i = 0; i < k; i++) {
        lu->schur_rhs[i] = x[lu->schur_col[i]];
    }
    for (int row = 0; row < k; row++) {
        memcpy(lu->schur_k_work + (size_t)row * k,
               lu->schur_k + (size_t)row * cap,
               (size_t)k * sizeof(double));
    }
    if (bg_solve_dense(k, lu->schur_k_work, lu->schur_piv, lu->schur_rhs) != 0) {
        lu_mark_bad_input_and_force_refactor(lu);
        return -1;
    }
    for (int i = 0; i < k; i++) {
        double alpha = -lu->schur_rhs[i];
        if (fabs(alpha) <= RALPH_ZERO_TOL) continue;
        sparse_column_axpy(lu->schur_indices[i], lu->schur_values[i],
                           lu->schur_nnz[i], alpha, x);
    }
    return 0;
}

static int bg_apply_backward(const LUFactorization *lu, double *x) {
    int k = lu->schur_num_updates;
    int cap = lu->schur_capacity;

    if (k <= 0) return 0;
    if (!lu->schur_k || !lu->schur_k_work || !lu->schur_rhs || !lu->schur_piv) {
        return -1;
    }

    for (int i = 0; i < k; i++) {
        lu->schur_rhs[i] = 0.0;
        for (int p = 0; p < lu->schur_nnz[i]; p++) {
            lu->schur_rhs[i] += lu->schur_values[i][p] * x[lu->schur_indices[i][p]];
        }
    }
    for (int row = 0; row < k; row++) {
        for (int col = 0; col < k; col++) {
            lu->schur_k_work[(size_t)row * k + col] = lu->schur_k[(size_t)col * cap + row];
        }
    }
    if (bg_solve_dense(k, lu->schur_k_work, lu->schur_piv, lu->schur_rhs) != 0) {
        lu_mark_bad_input_and_force_refactor(lu);
        return -1;
    }
    for (int i = 0; i < k; i++) {
        x[lu->schur_col[i]] -= lu->schur_rhs[i];
    }
    return 0;
}

static int gr_solve_dense(int n, double *A, double *rhs) {
    for (int col = 0; col < n; col++) {
        for (int row = n - 1; row > col; row--) {
            double a = A[(size_t)(row - 1) * n + col];
            double b = A[(size_t)row * n + col];
            if (fabs(b) <= RALPH_ZERO_TOL) continue;

            double r = hypot(a, b);
            if (r < RALPH_PIVOT_TOL) return -1;
            double c = a / r;
            double s = -b / r;

            for (int j = col; j < n; j++) {
                double t0 = c * A[(size_t)(row - 1) * n + j] - s * A[(size_t)row * n + j];
                double t1 = s * A[(size_t)(row - 1) * n + j] + c * A[(size_t)row * n + j];
                A[(size_t)(row - 1) * n + j] = t0;
                A[(size_t)row * n + j] = t1;
            }
            {
                double t0 = c * rhs[row - 1] - s * rhs[row];
                double t1 = s * rhs[row - 1] + c * rhs[row];
                rhs[row - 1] = t0;
                rhs[row] = t1;
            }
        }
        if (fabs(A[(size_t)col * n + col]) < RALPH_PIVOT_TOL) return -1;
    }

    for (int row = n - 1; row >= 0; row--) {
        double sum = rhs[row];
        for (int j = row + 1; j < n; j++) {
            sum -= A[(size_t)row * n + j] * rhs[j];
        }
        rhs[row] = sum / A[(size_t)row * n + row];
    }
    return 0;
}

static int gr_apply_forward(const LUFactorization *lu, double *x) {
    int k = lu->schur_num_updates;
    int cap = lu->schur_capacity;

    if (k <= 0) return 0;
    if (!lu->schur_k || !lu->schur_k_work || !lu->schur_rhs) {
        return -1;
    }

    for (int i = 0; i < k; i++) {
        lu->schur_rhs[i] = x[lu->schur_col[i]];
    }
    for (int row = 0; row < k; row++) {
        memcpy(lu->schur_k_work + (size_t)row * k,
               lu->schur_k + (size_t)row * cap,
               (size_t)k * sizeof(double));
    }
    if (gr_solve_dense(k, lu->schur_k_work, lu->schur_rhs) != 0) {
        lu_mark_bad_input_and_force_refactor(lu);
        return -1;
    }
    for (int i = 0; i < k; i++) {
        double alpha = -lu->schur_rhs[i];
        if (fabs(alpha) <= RALPH_ZERO_TOL) continue;
        sparse_column_axpy(lu->schur_indices[i], lu->schur_values[i],
                           lu->schur_nnz[i], alpha, x);
    }
    return 0;
}

static int gr_apply_backward(const LUFactorization *lu, double *x) {
    int k = lu->schur_num_updates;
    int cap = lu->schur_capacity;

    if (k <= 0) return 0;
    if (!lu->schur_k || !lu->schur_k_work || !lu->schur_rhs) {
        return -1;
    }

    for (int i = 0; i < k; i++) {
        lu->schur_rhs[i] = 0.0;
        for (int p = 0; p < lu->schur_nnz[i]; p++) {
            lu->schur_rhs[i] += lu->schur_values[i][p] * x[lu->schur_indices[i][p]];
        }
    }
    for (int row = 0; row < k; row++) {
        for (int col = 0; col < k; col++) {
            lu->schur_k_work[(size_t)row * k + col] = lu->schur_k[(size_t)col * cap + row];
        }
    }
    if (gr_solve_dense(k, lu->schur_k_work, lu->schur_rhs) != 0) {
        lu_mark_bad_input_and_force_refactor(lu);
        return -1;
    }
    for (int i = 0; i < k; i++) {
        x[lu->schur_col[i]] -= lu->schur_rhs[i];
    }
    return 0;
}

static inline int apply_single_ft_spike_forward(const int col,
                                                const double diag,
                                                const int *idx,
                                                const double *val,
                                                const int nnz,
                                                const int m,
                                                double *x) {
    if ((unsigned)col >= (unsigned)m) return -1;

    double xc = x[col];
    if (fabs(xc) < RALPH_ZERO_TOL) return 0;

    x[col] = diag * xc;

    /* Keep the previous FT kernel order intact; changing floating-point
     * accumulation order here measurably perturbs degenerate NETLIB paths. */
    int p = 0;
    int nnz4 = nnz & ~3;
    for (; p < nnz4; p += 4) {
        int r0 = idx[p];
        int r1 = idx[p + 1];
        int r2 = idx[p + 2];
        int r3 = idx[p + 3];

        if ((unsigned)r0 >= (unsigned)m || (unsigned)r1 >= (unsigned)m ||
            (unsigned)r2 >= (unsigned)m || (unsigned)r3 >= (unsigned)m) {
            return -1;
        }
        x[r0] += val[p] * xc;
        x[r1] += val[p + 1] * xc;
        x[r2] += val[p + 2] * xc;
        x[r3] += val[p + 3] * xc;
    }
    for (; p < nnz; p++) {
        int r = idx[p];
        if ((unsigned)r >= (unsigned)m) return -1;
        x[r] += val[p] * xc;
    }
    return 0;
}

static int apply_ft_spikes_forward(const LUFactorization *lu, double *x) {
    const int n = lu->ft_num_updates;
    const int m = lu->m;
    const int used = lu->spike_pool_used;
    const int *cols = lu->ft_spike_col;
    const double *diags = lu->ft_spike_diag;
    const int *starts = lu->ft_spike_start;
    const int *nnzs = lu->ft_spike_nnz;
    const int *pool_idx = lu->spike_pool_idx;
    const double *pool_val = lu->spike_pool_val;
    int invalid = 0;
    int k = 0;

    if (n == 0) return 0;

    for (; k + 1 < n; k += 2) {
        int start0 = starts[k];
        int start1 = starts[k + 1];
        int nnz0 = nnzs[k];
        int nnz1 = nnzs[k + 1];

        if (start0 < 0 || start1 < 0 ||
            nnz0 < 0 || nnz1 < 0 ||
            start0 > used || start1 > used ||
            nnz0 > used - start0 || nnz1 > used - start1) {
            invalid = 1;
            continue;
        }

        if (apply_single_ft_spike_forward(cols[k], diags[k],
                                          pool_idx + start0, pool_val + start0,
                                          nnz0, m, x) != 0 ||
            apply_single_ft_spike_forward(cols[k + 1], diags[k + 1],
                                          pool_idx + start1, pool_val + start1,
                                          nnz1, m, x) != 0) {
            invalid = 1;
            continue;
        }
    }

    if (k < n) {
        int start = starts[k];
        int nnz = nnzs[k];

        if (start < 0 || nnz < 0 || start > used || nnz > used - start) {
            invalid = 1;
        } else if (apply_single_ft_spike_forward(cols[k], diags[k],
                                                 pool_idx + start, pool_val + start,
                                                 nnz, m, x) != 0) {
            invalid = 1;
        }
    }

    if (invalid) {
        lu_mark_bad_input_and_force_refactor(lu);
        return -1;
    }
    return 0;
}

static inline void apply_single_ft_spike_backward(const int col,
                                                  const double diag,
                                                  const int *idx,
                                                  const double *val,
                                                  const int nnz,
                                                  const int m,
                                                  double *x) {
    if ((unsigned)col >= (unsigned)m) return;

    double xc = diag * x[col];

    int p = 0;
    int nnz8 = nnz & ~7;
    for (; p < nnz8; p += 8) {
        int r0 = idx[p];
        int r1 = idx[p + 1];
        int r2 = idx[p + 2];
        int r3 = idx[p + 3];
        int r4 = idx[p + 4];
        int r5 = idx[p + 5];
        int r6 = idx[p + 6];
        int r7 = idx[p + 7];
        if ((unsigned)r0 >= (unsigned)m || (unsigned)r1 >= (unsigned)m ||
            (unsigned)r2 >= (unsigned)m || (unsigned)r3 >= (unsigned)m ||
            (unsigned)r4 >= (unsigned)m || (unsigned)r5 >= (unsigned)m ||
            (unsigned)r6 >= (unsigned)m || (unsigned)r7 >= (unsigned)m) {
            continue;
        }
        xc += val[p] * x[r0];
        xc += val[p + 1] * x[r1];
        xc += val[p + 2] * x[r2];
        xc += val[p + 3] * x[r3];
        xc += val[p + 4] * x[r4];
        xc += val[p + 5] * x[r5];
        xc += val[p + 6] * x[r6];
        xc += val[p + 7] * x[r7];
    }
    for (; p < nnz; p++) {
        int r = idx[p];
        if ((unsigned)r >= (unsigned)m) continue;
        xc += val[p] * x[r];
    }
    x[col] = xc;
}

static void apply_ft_spikes_backward(const LUFactorization *lu, double *x) {
    const int n = lu->ft_num_updates;
    const int m = lu->m;
    const int used = lu->spike_pool_used;
    const int *cols = lu->ft_spike_col;
    const double *diags = lu->ft_spike_diag;
    const int *starts = lu->ft_spike_start;
    const int *nnzs = lu->ft_spike_nnz;
    const int *pool_idx = lu->spike_pool_idx;
    const double *pool_val = lu->spike_pool_val;
    int k = n - 1;

    if (n == 0) return;

    for (; k > 0; k -= 2) {
        int start0 = starts[k];
        int start1 = starts[k - 1];
        int nnz0 = nnzs[k];
        int nnz1 = nnzs[k - 1];

        if (start0 < 0 || start1 < 0 ||
            nnz0 < 0 || nnz1 < 0 ||
            start0 > used || start1 > used ||
            nnz0 > used - start0 || nnz1 > used - start1) {
            continue;
        }
        apply_single_ft_spike_backward(cols[k], diags[k],
                                       pool_idx + start0, pool_val + start0, nnz0, m, x);
        apply_single_ft_spike_backward(cols[k - 1], diags[k - 1],
                                       pool_idx + start1, pool_val + start1, nnz1, m, x);
    }

    if (k == 0) {
        int start = starts[0];
        int nnz = nnzs[0];
        if (start < 0 || nnz < 0 || start > used || nnz > used - start) return;
        apply_single_ft_spike_backward(cols[0], diags[0],
                                       pool_idx + start, pool_val + start, nnz, m, x);
    }
}

int lu_update_backend_apply_forward(const LUFactorization *lu, double *x) {
    if (!lu || !x) return -1;
    if (lu_update_backend_is_ft(lu)) {
        if (lu->ft_num_updates <= 0) return 0;
        return apply_ft_spikes_forward(lu, x);
    }
    if (lu_update_backend_is_bg(lu)) {
        return bg_apply_forward(lu, x);
    }
    if (lu_update_backend_is_gr(lu)) {
        return gr_apply_forward(lu, x);
    }
    if (lu->num_eta <= 0) return 0;
    apply_sparse_update_chain_forward(lu->num_eta, lu->eta_col,
                                      lu->eta_indices, lu->eta_values,
                                      lu->eta_nnz, x);
    return 0;
}

void lu_update_backend_apply_backward(const LUFactorization *lu, double *x) {
    if (!lu || !x) return;
    if (lu_update_backend_is_ft(lu)) {
        if (lu->ft_num_updates <= 0) return;
        apply_ft_spikes_backward(lu, x);
        return;
    }
    if (lu_update_backend_is_bg(lu)) {
        (void)bg_apply_backward(lu, x);
        return;
    }
    if (lu_update_backend_is_gr(lu)) {
        (void)gr_apply_backward(lu, x);
        return;
    }
    if (lu->num_eta <= 0) return;
    apply_sparse_update_chain_backward(lu->num_eta, lu->eta_col,
                                       lu->eta_indices, lu->eta_values,
                                       lu->eta_nnz, x);
}

static void lu_note_update_path_telemetry(LUFactorization *lu) {
    if (!lu || !lu->telemetry_enabled) return;

    switch ((LUUpdateBackend)lu->update_backend) {
        case LU_UPDATE_BACKEND_BG_COMPAT:
            lu->telemetry.update_path_bg_compat++;
            break;
        case LU_UPDATE_BACKEND_GR_COMPAT:
            lu->telemetry.update_path_gr_compat++;
            break;
        case LU_UPDATE_BACKEND_FT:
        default:
            if (lu_update_backend_is_ft(lu)) lu->telemetry.update_path_ft++;
            else lu->telemetry.update_path_eta++;
            break;
    }
}

static int store_sparse_update_chain(int *count,
                                     int capacity,
                                     int *col,
                                     int **indices_arr,
                                     double **values_arr,
                                     int *nnz_arr,
                                     int step_pos,
                                     const double *spike,
                                     int m,
                                     int total_nnz) {
    int *indices;
    double *values;
    int slot;
    int p = 0;

    if (*count < 0 || *count >= capacity) {
        return LU_FAIL_MAX_UPDATES;
    }

    indices = (int*)calloc((size_t)total_nnz, sizeof(int));
    values = (double*)calloc((size_t)total_nnz, sizeof(double));
    if (!indices || !values) {
        free(indices);
        free(values);
        return LU_FAIL_ETA_ALLOC;
    }

    for (int i = 0; i < m; i++) {
        if (fabs(spike[i]) > RALPH_ZERO_TOL) {
            indices[p] = i;
            values[p] = spike[i];
            p++;
        }
    }

    slot = *count;
    col[slot] = step_pos;
    indices_arr[slot] = indices;
    values_arr[slot] = values;
    nnz_arr[slot] = total_nnz;
    *count = slot + 1;
    return LU_FAIL_NONE;
}

static int store_schur_update(LUFactorization *lu,
                              int step_pos,
                              const double *base_spike) {
    int k = lu->schur_num_updates;
    int m = lu->m;
    double *u_dense;
    int total_nnz = 0;
    int reason;

    if (!lu->schur_k || !lu->schur_k_work || !lu->schur_rhs || !lu->schur_piv) {
        return LU_FAIL_BAD_INPUT;
    }
    if (k < 0 || k >= lu->schur_capacity) {
        return LU_FAIL_MAX_UPDATES;
    }

    u_dense = lu->hs_work1;
    memcpy(u_dense, base_spike, (size_t)m * sizeof(double));
    bg_build_m_times_unit_column(lu, step_pos, lu->hs_work2);
    for (int i = 0; i < m; i++) {
        u_dense[i] -= lu->hs_work2[i];
        if (fabs(u_dense[i]) > RALPH_ZERO_TOL) total_nnz++;
    }

    reason = store_sparse_update_chain(&lu->schur_num_updates,
                                       lu->schur_capacity,
                                       lu->schur_col,
                                       lu->schur_indices,
                                       lu->schur_values,
                                       lu->schur_nnz,
                                       step_pos,
                                       u_dense,
                                       m,
                                       total_nnz);
    if (reason != LU_FAIL_NONE) {
        return reason;
    }

    for (int row = 0; row < k; row++) {
        lu->schur_k[(size_t)row * lu->schur_capacity + k] =
            u_dense[lu->schur_col[row]];
    }
    for (int col = 0; col < k; col++) {
        lu->schur_k[(size_t)k * lu->schur_capacity + col] =
            sparse_column_get_entry(lu->schur_indices[col],
                                    lu->schur_values[col],
                                    lu->schur_nnz[col],
                                    step_pos);
    }
    lu->schur_k[(size_t)k * lu->schur_capacity + k] = 1.0 + u_dense[step_pos];
    return LU_FAIL_NONE;
}

int lu_update_backend_store(LUFactorization *lu,
                            int step_pos,
                            const double *base_spike,
                            const double *spike,
                            int off_diag_nnz) {
    int m;

    if (!lu || !spike) return -1;

    lu_note_update_path_telemetry(lu);
    m = lu->m;

    if (lu_update_backend_is_ft(lu)) {
        int k;
        int p = 0;

        if (lu->ft_num_updates < 0 || lu->ft_num_updates >= lu->ft_spike_capacity) {
            lu->last_refactor_trigger_reason = LP_BFCP_REFACTOR_REASON_MAX_UPDATES;
            lu->last_failure_reason = LU_FAIL_MAX_UPDATES;
            if (lu->telemetry_enabled) lu->telemetry.update_fail_max_updates++;
            return -1;
        }
        if (lu->spike_pool_used + off_diag_nnz > lu->spike_pool_capacity) {
            lu->last_failure_reason = LU_FAIL_SPIKE_POOL_FULL;
            if (lu->telemetry_enabled) lu->telemetry.update_fail_spike_pool_full++;
            return -1;
        }

        k = lu->ft_num_updates;
        lu->ft_spike_col[k] = step_pos;
        lu->ft_spike_diag[k] = spike[step_pos];
        lu->ft_spike_start[k] = lu->spike_pool_used;
        lu->ft_spike_nnz[k] = off_diag_nnz;

        for (int i = 0; i < m; i++) {
            if (i != step_pos && fabs(spike[i]) > RALPH_ZERO_TOL) {
                lu->spike_pool_idx[lu->spike_pool_used + p] = i;
                lu->spike_pool_val[lu->spike_pool_used + p] = spike[i];
                p++;
            }
        }
        lu->spike_pool_used += off_diag_nnz;
        lu->ft_num_updates++;
        return 0;
    }

    {
        int reason;
        int total_nnz = off_diag_nnz + 1;

        if (lu_update_backend_is_bg(lu) || lu_update_backend_is_gr(lu)) {
            if (!base_spike) {
                reason = LU_FAIL_BAD_INPUT;
            } else {
                reason = store_schur_update(lu, step_pos, base_spike);
            }
        } else {
            reason = store_sparse_update_chain(&lu->num_eta,
                                               lu->eta_capacity,
                                               lu->eta_col,
                                               lu->eta_indices,
                                               lu->eta_values,
                                               lu->eta_nnz,
                                               step_pos,
                                               spike,
                                               m,
                                               total_nnz);
        }

        if (reason == LU_FAIL_NONE) return 0;

        lu->last_failure_reason = reason;
        if (reason == LU_FAIL_BAD_INPUT) {
            if (lu->telemetry_enabled) lu->telemetry.update_fail_bad_input++;
        } else if (reason == LU_FAIL_MAX_UPDATES) {
            lu->last_refactor_trigger_reason = LP_BFCP_REFACTOR_REASON_MAX_UPDATES;
            if (lu->telemetry_enabled) lu->telemetry.update_fail_max_updates++;
        } else if (reason == LU_FAIL_ETA_ALLOC) {
            if (lu->telemetry_enabled) lu->telemetry.update_fail_eta_alloc++;
        }
    }

    return -1;
}
