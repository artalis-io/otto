#include <stdlib.h>
#include <math.h>
#include "lp.h"
#include "lp_bfcp_policy.h"
#include "lu_update_backend.h"

static int lu_update_backend_is_schur_compat(const LUFactorization *lu) {
    if (!lu) return 0;
    return lu->update_backend == LU_UPDATE_BACKEND_BG_COMPAT ||
           lu->update_backend == LU_UPDATE_BACKEND_GR_COMPAT;
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
    if (lu_update_backend_is_schur_compat(lu)) {
        return lu->schur_capacity > 0 ? lu->schur_capacity : 0;
    }
    return lu->eta_capacity > 0 ? lu->eta_capacity : 0;
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
    if (lu_update_backend_is_schur_compat(lu)) {
        if (lu->schur_num_updates <= 0) return 0;
        apply_sparse_update_chain_forward(lu->schur_num_updates, lu->schur_col,
                                          lu->schur_indices, lu->schur_values,
                                          lu->schur_nnz, x);
        return 0;
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
    if (lu_update_backend_is_schur_compat(lu)) {
        if (lu->schur_num_updates <= 0) return;
        apply_sparse_update_chain_backward(lu->schur_num_updates, lu->schur_col,
                                           lu->schur_indices, lu->schur_values,
                                           lu->schur_nnz, x);
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

int lu_update_backend_store(LUFactorization *lu,
                            int step_pos,
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

        if (lu_update_backend_is_schur_compat(lu)) {
            reason = store_sparse_update_chain(&lu->schur_num_updates,
                                               lu->schur_capacity,
                                               lu->schur_col,
                                               lu->schur_indices,
                                               lu->schur_values,
                                               lu->schur_nnz,
                                               step_pos,
                                               spike,
                                               m,
                                               total_nnz);
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
        if (reason == LU_FAIL_MAX_UPDATES) {
            lu->last_refactor_trigger_reason = LP_BFCP_REFACTOR_REASON_MAX_UPDATES;
            if (lu->telemetry_enabled) lu->telemetry.update_fail_max_updates++;
        } else if (reason == LU_FAIL_ETA_ALLOC) {
            if (lu->telemetry_enabled) lu->telemetry.update_fail_eta_alloc++;
        }
    }

    return -1;
}
