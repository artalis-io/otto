/*
 * Ralph - Presolve Implementation
 *
 * Preprocessing techniques to reduce problem size and improve numerical stability:
 * - Remove fixed variables
 * - Remove empty rows/columns
 * - Remove singleton rows (forcing constraints)
 * - Bound tightening
 * - Coefficient reduction
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "presolve.h"

/* ============================================================================
 * Presolve Context Creation/Destruction
 * ============================================================================ */

static PresolveContext* presolve_context_create(LPModel *model) {
    PresolveContext *ctx = (PresolveContext*)calloc(1, sizeof(PresolveContext));
    if (!ctx) return NULL;

    ctx->original = model;
    ctx->working = lp_model_copy(model);

    if (!ctx->working) {
        free(ctx);
        return NULL;
    }

    /* Default settings */
    ctx->remove_fixed_vars = 1;
    ctx->remove_empty_rows = 1;
    ctx->remove_empty_cols = 1;
    ctx->remove_singleton_rows = 1;
    ctx->remove_singleton_cols = 1;
    ctx->remove_forcing_cons = 1;
    ctx->bound_tightening = 1;
    ctx->coefficient_reduction = 0;  /* Can be expensive */
    ctx->probing = 0;  /* MIP only */
    ctx->detect_redundant_rows = 1;  /* Critical for equality-heavy problems */

    ctx->max_rounds = 10;
    ctx->current_round = 0;

    /* Redundant row detection stats */
    ctx->redundant_rows_found = 0;
    ctx->matrix_rank = -1;  /* Not computed yet */

    /*
     * Allocate working arrays (batch allocation pattern).
     * All arrays are allocated, then checked together. On failure,
     * free(NULL) is safe, so we can clean up all pointers uniformly.
     */
    ctx->row_deleted = (int*)calloc(model->num_cons, sizeof(int));
    ctx->col_deleted = (int*)calloc(model->num_vars, sizeof(int));
    ctx->row_lb = (double*)calloc(model->num_cons, sizeof(double));
    ctx->row_ub = (double*)calloc(model->num_cons, sizeof(double));

    if (!ctx->row_deleted || !ctx->col_deleted || !ctx->row_lb || !ctx->row_ub) {
        lp_model_free(ctx->working);
        free(ctx->row_deleted);
        free(ctx->col_deleted);
        free(ctx->row_lb);
        free(ctx->row_ub);
        free(ctx);
        return NULL;
    }

    return ctx;
}

static void presolve_context_free(PresolveContext *ctx) {
    if (!ctx) return;

    lp_model_free(ctx->working);
    free(ctx->row_deleted);
    free(ctx->col_deleted);
    free(ctx->row_lb);
    free(ctx->row_ub);
    free(ctx);
}

/* ============================================================================
 * Implied Bounds Computation
 * ============================================================================ */

void presolve_compute_implied_bounds(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int n = model->num_vars;

    /* Allocate dense row buffer once */
    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return;

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        /* Extract row once instead of O(n) element accesses */
        sparse_get_row(model->A, i, row);

        double lb = 0.0;
        double ub = 0.0;

        /* Compute implied bounds: lb <= a'x <= ub based on variable bounds */
        for (int j = 0; j < n; j++) {
            if (ctx->col_deleted[j]) continue;

            double aij = row[j];
            if (fabs(aij) < RALPH_ZERO_TOL) continue;

            if (aij > 0) {
                if (model->lb[j] > -RALPH_INFINITY/2) {
                    lb += aij * model->lb[j];
                } else {
                    lb = -RALPH_INFINITY;
                }
                if (model->ub[j] < RALPH_INFINITY/2) {
                    ub += aij * model->ub[j];
                } else {
                    ub = RALPH_INFINITY;
                }
            } else {
                if (model->ub[j] < RALPH_INFINITY/2) {
                    lb += aij * model->ub[j];
                } else {
                    lb = -RALPH_INFINITY;
                }
                if (model->lb[j] > -RALPH_INFINITY/2) {
                    ub += aij * model->lb[j];
                } else {
                    ub = RALPH_INFINITY;
                }
            }
        }

        ctx->row_lb[i] = lb;
        ctx->row_ub[i] = ub;
    }

    free(row);
}

/* ============================================================================
 * Individual Presolve Operations
 * ============================================================================ */

/* Remove variables fixed by their bounds */
int presolve_remove_fixed_vars(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;

    for (int j = 0; j < model->num_vars; j++) {
        if (ctx->col_deleted[j]) continue;

        if (model->lb[j] >= model->ub[j] - RALPH_ZERO_TOL) {
            /* Variable is fixed */
            double fixed_val = model->lb[j];

            /* Update RHS for constraints involving this variable */
            for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
                int i = model->A->rowidx[p];
                if (!ctx->row_deleted[i]) {
                    model->b[i] -= model->A->values[p] * fixed_val;
                }
            }

            /* Update objective offset */
            model->obj_offset += model->c[j] * fixed_val;

            ctx->col_deleted[j] = 1;
            count++;
        }
    }

    return count;
}

/* Remove rows with no non-zero coefficients */
int presolve_remove_empty_rows(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;
    int n = model->num_vars;

    /* Allocate dense row buffer once */
    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return 0;

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        /* Extract row once */
        sparse_get_row(model->A, i, row);

        /* Count non-zeros in row */
        int nnz = 0;
        for (int j = 0; j < n && nnz == 0; j++) {
            if (!ctx->col_deleted[j] && fabs(row[j]) > RALPH_ZERO_TOL) {
                nnz++;
            }
        }

        if (nnz == 0) {
            /* Empty row */
            double rhs = model->b[i];

            /* Check feasibility */
            if (model->sense[i] == 'L' && rhs < -RALPH_FEAS_TOL) {
                free(row);
                return -1;
            }
            if (model->sense[i] == 'G' && rhs > RALPH_FEAS_TOL) {
                free(row);
                return -1;
            }
            if (model->sense[i] == 'E' && fabs(rhs) > RALPH_FEAS_TOL) {
                free(row);
                return -1;
            }

            ctx->row_deleted[i] = 1;
            count++;
        }
    }

    free(row);
    return count;
}

/* Remove columns with no non-zero coefficients */
int presolve_remove_empty_cols(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;

    for (int j = 0; j < model->num_vars; j++) {
        if (ctx->col_deleted[j]) continue;

        /* Check if column is empty (all rows containing it are deleted) */
        int active_nnz = 0;
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            if (!ctx->row_deleted[model->A->rowidx[p]]) {
                active_nnz++;
            }
        }

        if (active_nnz == 0) {
            /* Column only appears in objective */
            double obj_coef = model->c[j] * model->obj_sense;  /* For minimization */

            /* Set to bound that minimizes objective */
            double fixed_val;
            if (obj_coef > RALPH_ZERO_TOL) {
                /* Positive: minimize by setting to lower bound */
                if (model->lb[j] > -RALPH_INFINITY/2) {
                    fixed_val = model->lb[j];
                } else {
                    return -1;  /* Unbounded */
                }
            } else if (obj_coef < -RALPH_ZERO_TOL) {
                /* Negative: minimize by setting to upper bound */
                if (model->ub[j] < RALPH_INFINITY/2) {
                    fixed_val = model->ub[j];
                } else {
                    return -1;  /* Unbounded */
                }
            } else {
                /* Zero coefficient: set to any bound */
                if (model->lb[j] > -RALPH_INFINITY/2) {
                    fixed_val = model->lb[j];
                } else if (model->ub[j] < RALPH_INFINITY/2) {
                    fixed_val = model->ub[j];
                } else {
                    fixed_val = 0.0;
                }
            }

            model->obj_offset += model->c[j] * fixed_val;
            model->lb[j] = fixed_val;
            model->ub[j] = fixed_val;
            ctx->col_deleted[j] = 1;
            count++;
        }
    }

    return count;
}

/* Handle singleton rows (rows with exactly one non-zero) */
int presolve_singleton_rows(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;
    int n = model->num_vars;

    /* Allocate dense row buffer once */
    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return 0;

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        /* Extract row once */
        sparse_get_row(model->A, i, row);

        /* Find the single non-zero */
        int singleton_col = -1;
        double singleton_val = 0.0;
        int nnz = 0;

        for (int j = 0; j < n; j++) {
            if (ctx->col_deleted[j]) continue;

            if (fabs(row[j]) > RALPH_ZERO_TOL) {
                nnz++;
                singleton_col = j;
                singleton_val = row[j];
                if (nnz > 1) break;
            }
        }

        if (nnz == 1 && singleton_col >= 0) {
            /* Row i is: a_ij * x_j (sense) b_i */
            double rhs = model->b[i];
            double implied_val = rhs / singleton_val;

            if (model->sense[i] == 'E') {
                /* x_j = implied_val */
                model->lb[singleton_col] = fmax(model->lb[singleton_col], implied_val);
                model->ub[singleton_col] = fmin(model->ub[singleton_col], implied_val);
            } else if ((model->sense[i] == 'L' && singleton_val > 0) ||
                       (model->sense[i] == 'G' && singleton_val < 0)) {
                /* x_j <= implied_val */
                model->ub[singleton_col] = fmin(model->ub[singleton_col], implied_val);
            } else {
                /* x_j >= implied_val */
                model->lb[singleton_col] = fmax(model->lb[singleton_col], implied_val);
            }

            /* Check for infeasibility */
            if (model->lb[singleton_col] > model->ub[singleton_col] + RALPH_FEAS_TOL) {
                free(row);
                return -1;  /* Infeasible */
            }

            ctx->row_deleted[i] = 1;
            count++;
        }
    }

    free(row);
    return count;
}

/* Handle singleton columns (columns with exactly one non-zero) */
int presolve_singleton_cols(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;
    int n = model->num_vars;

    /* Allocate dense row buffer once */
    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return 0;

    for (int j = 0; j < n; j++) {
        if (ctx->col_deleted[j]) continue;

        /* Count active non-zeros in column */
        int singleton_row = -1;
        double singleton_val = 0.0;
        int nnz = 0;

        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int i = model->A->rowidx[p];
            if (!ctx->row_deleted[i]) {
                nnz++;
                singleton_row = i;
                singleton_val = model->A->values[p];
                if (nnz > 1) break;
            }
        }

        if (nnz == 1 && singleton_row >= 0) {
            /* Variable appears in only one constraint */
            /* Can substitute and remove (for equality constraints) */
            /* Or determine optimal value based on objective and constraint */

            /* For now, just tighten bounds based on the constraint */
            double rhs = model->b[singleton_row];

            /* Extract row once instead of O(n) element accesses */
            sparse_get_row(model->A, singleton_row, row);

            /* Get contribution from other variables in the row */
            double other_lb = 0.0, other_ub = 0.0;
            for (int jj = 0; jj < n; jj++) {
                if (jj == j || ctx->col_deleted[jj]) continue;

                double aij = row[jj];
                if (fabs(aij) < RALPH_ZERO_TOL) continue;

                if (aij > 0) {
                    if (model->lb[jj] > -RALPH_INFINITY/2) other_lb += aij * model->lb[jj];
                    else other_lb = -RALPH_INFINITY;
                    if (model->ub[jj] < RALPH_INFINITY/2) other_ub += aij * model->ub[jj];
                    else other_ub = RALPH_INFINITY;
                } else {
                    if (model->ub[jj] < RALPH_INFINITY/2) other_lb += aij * model->ub[jj];
                    else other_lb = -RALPH_INFINITY;
                    if (model->lb[jj] > -RALPH_INFINITY/2) other_ub += aij * model->lb[jj];
                    else other_ub = RALPH_INFINITY;
                }
            }

            /* Derive bounds respecting constraint sense */
            char sense = model->sense[singleton_row];

            /* For <= or = constraints: a_ij * x_j <= rhs - other_lb */
            if (sense == 'L' || sense == 'E') {
                if (other_lb > -RALPH_INFINITY/2) {
                    double ax_ub = rhs - other_lb;
                    if (singleton_val > 0) {
                        model->ub[j] = fmin(model->ub[j], ax_ub / singleton_val);
                    } else {
                        model->lb[j] = fmax(model->lb[j], ax_ub / singleton_val);
                    }
                }
            }

            /* For >= or = constraints: a_ij * x_j >= rhs - other_ub */
            if (sense == 'G' || sense == 'E') {
                if (other_ub < RALPH_INFINITY/2) {
                    double ax_lb = rhs - other_ub;
                    if (singleton_val > 0) {
                        model->lb[j] = fmax(model->lb[j], ax_lb / singleton_val);
                    } else {
                        model->ub[j] = fmin(model->ub[j], ax_lb / singleton_val);
                    }
                }
            }

            if (model->lb[j] > model->ub[j] + RALPH_FEAS_TOL) {
                free(row);
                return -1;  /* Infeasible */
            }

            count++;  /* Bound tightened */
        }
    }

    free(row);
    return count;
}

/* Detect and handle forcing constraints */
int presolve_forcing_constraints(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;
    int n = model->num_vars;

    presolve_compute_implied_bounds(ctx);

    /* Allocate dense row buffer for forcing constraint handling */
    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return 0;

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        double rhs = model->b[i];
        double row_lb = ctx->row_lb[i];
        double row_ub = ctx->row_ub[i];

        if (model->sense[i] == 'L') {
            /* a'x <= b */
            if (row_lb > rhs + RALPH_FEAS_TOL) {
                free(row);
                return -1;  /* Infeasible */
            }
            if (row_ub <= rhs + RALPH_FEAS_TOL) {
                /* Constraint is redundant */
                ctx->row_deleted[i] = 1;
                count++;
            } else if (row_lb >= rhs - RALPH_FEAS_TOL) {
                /* Forcing: must be at equality */
                /* All variables at their bounds that achieve row_lb */
                sparse_get_row(model->A, i, row);
                for (int j = 0; j < n; j++) {
                    if (ctx->col_deleted[j]) continue;
                    double aij = row[j];
                    if (fabs(aij) > RALPH_ZERO_TOL) {
                        if (aij > 0) {
                            model->ub[j] = model->lb[j];
                        } else {
                            model->lb[j] = model->ub[j];
                        }
                    }
                }
                ctx->row_deleted[i] = 1;
                count++;
            }
        } else if (model->sense[i] == 'G') {
            /* a'x >= b */
            if (row_ub < rhs - RALPH_FEAS_TOL) {
                free(row);
                return -1;  /* Infeasible */
            }
            if (row_lb >= rhs - RALPH_FEAS_TOL) {
                /* Constraint is redundant */
                ctx->row_deleted[i] = 1;
                count++;
            }
        } else {  /* Equality */
            if (row_lb > rhs + RALPH_FEAS_TOL || row_ub < rhs - RALPH_FEAS_TOL) {
                free(row);
                return -1;  /* Infeasible */
            }
        }
    }

    free(row);
    return count;
}

/* Tighten variable bounds using constraint information */
int presolve_bound_tightening(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;
    int n = model->num_vars;

    /* Allocate dense row buffer once */
    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return 0;

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        double rhs = model->b[i];

        /* Extract row once (O(nnz) instead of O(n²) element accesses) */
        sparse_get_row(model->A, i, row);

        /* First pass: compute total row_lb and row_ub */
        double row_lb = 0.0, row_ub = 0.0;
        int row_lb_finite = 1, row_ub_finite = 1;

        for (int j = 0; j < n; j++) {
            if (ctx->col_deleted[j]) continue;
            double aij = row[j];
            if (fabs(aij) < RALPH_ZERO_TOL) continue;

            if (aij > 0) {
                if (model->lb[j] <= -RALPH_INFINITY/2) row_lb_finite = 0;
                else row_lb += aij * model->lb[j];
                if (model->ub[j] >= RALPH_INFINITY/2) row_ub_finite = 0;
                else row_ub += aij * model->ub[j];
            } else {
                if (model->ub[j] >= RALPH_INFINITY/2) row_lb_finite = 0;
                else row_lb += aij * model->ub[j];
                if (model->lb[j] <= -RALPH_INFINITY/2) row_ub_finite = 0;
                else row_ub += aij * model->lb[j];
            }
        }

        /* Second pass: derive bounds for each variable */
        for (int j = 0; j < n; j++) {
            if (ctx->col_deleted[j]) continue;
            double aij = row[j];
            if (fabs(aij) < RALPH_ZERO_TOL) continue;

            /* Compute contribution of variable j to row bounds */
            double j_contrib_lb, j_contrib_ub;
            int j_lb_finite = 1, j_ub_finite = 1;

            if (aij > 0) {
                if (model->lb[j] <= -RALPH_INFINITY/2) j_lb_finite = 0;
                else j_contrib_lb = aij * model->lb[j];
                if (model->ub[j] >= RALPH_INFINITY/2) j_ub_finite = 0;
                else j_contrib_ub = aij * model->ub[j];
            } else {
                if (model->ub[j] >= RALPH_INFINITY/2) j_lb_finite = 0;
                else j_contrib_lb = aij * model->ub[j];
                if (model->lb[j] <= -RALPH_INFINITY/2) j_ub_finite = 0;
                else j_contrib_ub = aij * model->lb[j];
            }

            /* other_lb = row_lb - j_contrib_lb (if both finite) */
            /* other_ub = row_ub - j_contrib_ub (if both finite) */
            double other_lb = row_lb_finite && j_lb_finite ? row_lb - j_contrib_lb : -RALPH_INFINITY;
            double other_ub = row_ub_finite && j_ub_finite ? row_ub - j_contrib_ub : RALPH_INFINITY;

            /* Derive bounds on a_ij * x_j */
            double new_lb = model->lb[j];
            double new_ub = model->ub[j];

            if (model->sense[i] == 'L' || model->sense[i] == 'E') {
                /* a_ij * x_j <= rhs - other_lb */
                if (other_lb > -RALPH_INFINITY/2) {
                    double bound = (rhs - other_lb) / aij;
                    if (aij > 0) {
                        new_ub = fmin(new_ub, bound);
                    } else {
                        new_lb = fmax(new_lb, bound);
                    }
                }
            }

            if (model->sense[i] == 'G' || model->sense[i] == 'E') {
                /* a_ij * x_j >= rhs - other_ub */
                if (other_ub < RALPH_INFINITY/2) {
                    double bound = (rhs - other_ub) / aij;
                    if (aij > 0) {
                        new_lb = fmax(new_lb, bound);
                    } else {
                        new_ub = fmin(new_ub, bound);
                    }
                }
            }

            /* Check for improvement */
            if (new_lb > model->lb[j] + RALPH_ZERO_TOL) {
                model->lb[j] = new_lb;
                count++;
            }
            if (new_ub < model->ub[j] - RALPH_ZERO_TOL) {
                model->ub[j] = new_ub;
                count++;
            }

            /* Check feasibility */
            if (model->lb[j] > model->ub[j] + RALPH_FEAS_TOL) {
                free(row);
                return -1;
            }
        }
    }

    free(row);
    return count;
}

/* ============================================================================
 * Redundant Row Detection via Gaussian Elimination
 * ============================================================================ */

/*
 * Detect and remove linearly dependent (redundant) rows.
 *
 * Algorithm:
 * 1. Build dense augmented matrix [A | b] from active rows/columns
 * 2. Perform Gaussian elimination with partial pivoting
 * 3. Rows that reduce to all-zeros in A are redundant
 * 4. If b[row] != 0 for a zero row -> infeasible
 *
 * This is critical for problems like beaconfd which have 140 equalities
 * out of 173 constraints - some are linear combinations of others.
 */
int presolve_detect_redundant_rows(PresolveContext *ctx) {
    if (!ctx || !ctx->working) return 0;

    LPModel *model = ctx->working;
    int m_orig = model->num_cons;
    int n_orig = model->num_vars;

    /* Count active rows and columns */
    int m_active = 0, n_active = 0;
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i]) m_active++;
    }
    for (int j = 0; j < n_orig; j++) {
        if (!ctx->col_deleted[j]) n_active++;
    }

    if (m_active == 0 || n_active == 0) {
        ctx->matrix_rank = 0;
        return 0;
    }

    /* Build mapping from active indices to dense indices */
    int *row_to_dense = (int *)malloc((size_t)m_orig * sizeof(int));
    int *col_to_dense = (int *)malloc((size_t)n_orig * sizeof(int));
    int *dense_to_row = (int *)malloc((size_t)m_active * sizeof(int));

    if (!row_to_dense || !col_to_dense || !dense_to_row) {
        free(row_to_dense);
        free(col_to_dense);
        free(dense_to_row);
        return 0;
    }

    int dense_row = 0;
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i]) {
            row_to_dense[i] = dense_row;
            dense_to_row[dense_row] = i;
            dense_row++;
        } else {
            row_to_dense[i] = -1;
        }
    }

    int dense_col = 0;
    for (int j = 0; j < n_orig; j++) {
        if (!ctx->col_deleted[j]) {
            col_to_dense[j] = dense_col;
            dense_col++;
        } else {
            col_to_dense[j] = -1;
        }
    }

    /* Allocate dense augmented matrix [A | b] in column-major order
     * Size: m_active rows x (n_active + 1) columns */
    size_t aug_cols = (size_t)n_active + 1;
    double *A = (double *)calloc((size_t)m_active * aug_cols, sizeof(double));
    int *pivot_col = (int *)malloc((size_t)m_active * sizeof(int));

    if (!A || !pivot_col) {
        free(row_to_dense);
        free(col_to_dense);
        free(dense_to_row);
        free(A);
        free(pivot_col);
        return 0;
    }

    /* Fill the dense matrix from sparse CSC */
    for (int j = 0; j < n_orig; j++) {
        if (ctx->col_deleted[j]) continue;
        int dc = col_to_dense[j];

        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int i = model->A->rowidx[p];
            if (ctx->row_deleted[i]) continue;
            int dr = row_to_dense[i];
            A[dr + (size_t)dc * m_active] = model->A->values[p];
        }
    }

    /* Fill the RHS column (last column of augmented matrix) */
    for (int i = 0; i < m_orig; i++) {
        if (ctx->row_deleted[i]) continue;
        int dr = row_to_dense[i];
        A[dr + (size_t)n_active * m_active] = model->b[i];
    }

    /* Gaussian elimination with partial pivoting */
    int rank = 0;
    int min_dim = (m_active < n_active) ? m_active : n_active;

    for (int k = 0; k < min_dim; k++) {
        pivot_col[k] = -1;
    }

    int col = 0;  /* Current column being processed */
    for (int k = 0; k < min_dim && col < n_active; ) {
        /* Find pivot: largest absolute value in column 'col' from row k onwards */
        int best_row = -1;
        double best_val = RALPH_PIVOT_TOL;

        for (int i = k; i < m_active; i++) {
            double val = fabs(A[i + (size_t)col * m_active]);
            if (val > best_val) {
                best_val = val;
                best_row = i;
            }
        }

        if (best_row < 0) {
            /* No pivot in this column, try next column */
            col++;
            continue;
        }

        /* Swap rows k and best_row */
        if (best_row != k) {
            for (int j = col; j <= n_active; j++) {  /* Include RHS column */
                double tmp = A[k + (size_t)j * m_active];
                A[k + (size_t)j * m_active] = A[best_row + (size_t)j * m_active];
                A[best_row + (size_t)j * m_active] = tmp;
            }
            /* Swap in dense_to_row mapping too */
            int tmp_idx = dense_to_row[k];
            dense_to_row[k] = dense_to_row[best_row];
            dense_to_row[best_row] = tmp_idx;
        }

        /* Eliminate below pivot */
        double pivot = A[k + (size_t)col * m_active];
        for (int i = k + 1; i < m_active; i++) {
            double factor = A[i + (size_t)col * m_active] / pivot;
            if (fabs(factor) < RALPH_ZERO_TOL) continue;

            A[i + (size_t)col * m_active] = 0.0;  /* Exact zero */
            for (int j = col + 1; j <= n_active; j++) {  /* Include RHS */
                A[i + (size_t)j * m_active] -= factor * A[k + (size_t)j * m_active];
            }
        }

        pivot_col[k] = col;
        rank++;
        k++;
        col++;
    }

    ctx->matrix_rank = rank;

    /* Check rows rank..m_active-1 for redundancy/infeasibility */
    int count = 0;
    int infeasible = 0;

    for (int k = rank; k < m_active; k++) {
        /* Row k should be all zeros in A part */
        int is_zero_row = 1;
        for (int j = 0; j < n_active; j++) {
            if (fabs(A[k + (size_t)j * m_active]) > RALPH_ZERO_TOL) {
                is_zero_row = 0;
                break;
            }
        }

        if (is_zero_row) {
            /* Check RHS */
            double rhs = A[k + (size_t)n_active * m_active];
            int orig_row = dense_to_row[k];
            char sense = model->sense[orig_row];

            if (sense == 'E') {
                /* Equality: 0 = rhs must have rhs = 0 */
                if (fabs(rhs) > RALPH_FEAS_TOL) {
                    infeasible = 1;
                    break;
                }
            } else if (sense == 'L') {
                /* 0 <= rhs: satisfied if rhs >= 0 */
                if (rhs < -RALPH_FEAS_TOL) {
                    infeasible = 1;
                    break;
                }
            } else if (sense == 'G') {
                /* 0 >= rhs: satisfied if rhs <= 0 */
                if (rhs > RALPH_FEAS_TOL) {
                    infeasible = 1;
                    break;
                }
            }

            /* Row is redundant - mark for deletion */
            ctx->row_deleted[orig_row] = 1;
            count++;
        }
    }

    ctx->redundant_rows_found = count;

    /* Cleanup */
    free(row_to_dense);
    free(col_to_dense);
    free(dense_to_row);
    free(A);
    free(pivot_col);

    if (infeasible) {
        return -1;  /* Inconsistent system */
    }

    return count;
}

/* ============================================================================
 * MIP-Specific Presolve: Probing
 * ============================================================================ */

/*
 * Probing: Fix binary variables by checking if fixing to 0 or 1 leads to
 * infeasibility. Also tighten bounds by computing implied bounds when
 * a binary is set to each value.
 */
int presolve_probing(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int n = model->num_vars;
    int count = 0;

    /* Allocate working arrays for implied bounds */
    double *implied_lb0 = (double*)calloc(n, sizeof(double));
    double *implied_ub0 = (double*)calloc(n, sizeof(double));
    double *implied_lb1 = (double*)calloc(n, sizeof(double));
    double *implied_ub1 = (double*)calloc(n, sizeof(double));
    double *row = (double*)calloc(n, sizeof(double));

    if (!implied_lb0 || !implied_ub0 || !implied_lb1 || !implied_ub1 || !row) {
        free(implied_lb0);
        free(implied_ub0);
        free(implied_lb1);
        free(implied_ub1);
        free(row);
        return 0;
    }

    /* Limit probing iterations to avoid expensive O(n*m*n) worst case */
    int max_probe_vars = 100;
    int probed = 0;

    for (int j = 0; j < n && probed < max_probe_vars; j++) {
        if (ctx->col_deleted[j]) continue;

        /* Only probe binary variables */
        if (model->var_type[j] != 'B') continue;
        if (model->lb[j] > 0.5 || model->ub[j] < 0.5) continue;  /* Already fixed */

        probed++;

        /* Initialize implied bounds for both settings */
        for (int k = 0; k < n; k++) {
            implied_lb0[k] = model->lb[k];
            implied_ub0[k] = model->ub[k];
            implied_lb1[k] = model->lb[k];
            implied_ub1[k] = model->ub[k];
        }

        /* Try x_j = 0 */
        implied_lb0[j] = 0.0;
        implied_ub0[j] = 0.0;
        int infeas_0 = 0;

        /* Propagate bounds when x_j = 0 */
        for (int i = 0; i < model->num_cons && !infeas_0; i++) {
            if (ctx->row_deleted[i]) continue;

            sparse_get_row(model->A, i, row);
            double a_j = row[j];
            if (fabs(a_j) < RALPH_ZERO_TOL) continue;

            double rhs = model->b[i];

            /* Compute row activity bounds with x_j = 0 */
            double row_lb = 0.0, row_ub = 0.0;
            int row_lb_finite = 1, row_ub_finite = 1;

            for (int k = 0; k < n; k++) {
                if (ctx->col_deleted[k]) continue;
                double a_k = row[k];
                if (fabs(a_k) < RALPH_ZERO_TOL) continue;

                double lb_k = (k == j) ? 0.0 : implied_lb0[k];
                double ub_k = (k == j) ? 0.0 : implied_ub0[k];

                if (a_k > 0) {
                    if (lb_k <= -RALPH_INFINITY/2) row_lb_finite = 0;
                    else row_lb += a_k * lb_k;
                    if (ub_k >= RALPH_INFINITY/2) row_ub_finite = 0;
                    else row_ub += a_k * ub_k;
                } else {
                    if (ub_k >= RALPH_INFINITY/2) row_lb_finite = 0;
                    else row_lb += a_k * ub_k;
                    if (lb_k <= -RALPH_INFINITY/2) row_ub_finite = 0;
                    else row_ub += a_k * lb_k;
                }
            }

            /* Check feasibility */
            if (model->sense[i] == 'L') {
                if (row_lb_finite && row_lb > rhs + RALPH_FEAS_TOL) infeas_0 = 1;
            } else if (model->sense[i] == 'G') {
                if (row_ub_finite && row_ub < rhs - RALPH_FEAS_TOL) infeas_0 = 1;
            } else {  /* 'E' */
                if (row_lb_finite && row_lb > rhs + RALPH_FEAS_TOL) infeas_0 = 1;
                if (row_ub_finite && row_ub < rhs - RALPH_FEAS_TOL) infeas_0 = 1;
            }
        }

        /* Try x_j = 1 */
        implied_lb1[j] = 1.0;
        implied_ub1[j] = 1.0;
        int infeas_1 = 0;

        /* Propagate bounds when x_j = 1 */
        for (int i = 0; i < model->num_cons && !infeas_1; i++) {
            if (ctx->row_deleted[i]) continue;

            sparse_get_row(model->A, i, row);
            double a_j = row[j];
            if (fabs(a_j) < RALPH_ZERO_TOL) continue;

            double rhs = model->b[i];

            /* Compute row activity bounds with x_j = 1 */
            double row_lb = 0.0, row_ub = 0.0;
            int row_lb_finite = 1, row_ub_finite = 1;

            for (int k = 0; k < n; k++) {
                if (ctx->col_deleted[k]) continue;
                double a_k = row[k];
                if (fabs(a_k) < RALPH_ZERO_TOL) continue;

                double lb_k = (k == j) ? 1.0 : implied_lb1[k];
                double ub_k = (k == j) ? 1.0 : implied_ub1[k];

                if (a_k > 0) {
                    if (lb_k <= -RALPH_INFINITY/2) row_lb_finite = 0;
                    else row_lb += a_k * lb_k;
                    if (ub_k >= RALPH_INFINITY/2) row_ub_finite = 0;
                    else row_ub += a_k * ub_k;
                } else {
                    if (ub_k >= RALPH_INFINITY/2) row_lb_finite = 0;
                    else row_lb += a_k * ub_k;
                    if (lb_k <= -RALPH_INFINITY/2) row_ub_finite = 0;
                    else row_ub += a_k * lb_k;
                }
            }

            /* Check feasibility */
            if (model->sense[i] == 'L') {
                if (row_lb_finite && row_lb > rhs + RALPH_FEAS_TOL) infeas_1 = 1;
            } else if (model->sense[i] == 'G') {
                if (row_ub_finite && row_ub < rhs - RALPH_FEAS_TOL) infeas_1 = 1;
            } else {  /* 'E' */
                if (row_lb_finite && row_lb > rhs + RALPH_FEAS_TOL) infeas_1 = 1;
                if (row_ub_finite && row_ub < rhs - RALPH_FEAS_TOL) infeas_1 = 1;
            }
        }

        /* Analyze probing results */
        if (infeas_0 && infeas_1) {
            /* Both settings infeasible - problem is infeasible */
            free(implied_lb0);
            free(implied_ub0);
            free(implied_lb1);
            free(implied_ub1);
            free(row);
            return -1;  /* Infeasible */
        } else if (infeas_0) {
            /* x_j = 0 infeasible -> fix x_j = 1 */
            model->lb[j] = 1.0;
            model->ub[j] = 1.0;
            count++;
        } else if (infeas_1) {
            /* x_j = 1 infeasible -> fix x_j = 0 */
            model->lb[j] = 0.0;
            model->ub[j] = 0.0;
            count++;
        }
        /* If neither infeasible, we could derive tighter bounds on other variables
         * by taking the intersection, but we skip this for simplicity */
    }

    free(implied_lb0);
    free(implied_ub0);
    free(implied_lb1);
    free(implied_ub1);
    free(row);
    return count;
}

/* ============================================================================
 * Set Covering/Partitioning Specific Presolve
 * ============================================================================ */

/*
 * Check if model has SCP structure (binary vars, 0-1 coefficients).
 * Quick check - doesn't verify all constraints, just samples.
 */
static int is_scp_structure(const LPModel *model) {
    if (!model || !model->A) return 0;

    /* Check some variables are binary */
    int has_binary = 0;
    for (int j = 0; j < model->num_vars && !has_binary; j++) {
        if (model->var_type[j] == 'B' ||
            (model->var_type[j] == 'I' &&
             fabs(model->lb[j]) < RALPH_ZERO_TOL &&
             fabs(model->ub[j] - 1.0) < RALPH_ZERO_TOL)) {
            has_binary = 1;
        }
    }
    return has_binary;
}

/*
 * Essential set detection for SCP.
 *
 * If an element is covered by only one active set, that set must be selected.
 */
int presolve_scp_essential_sets(PresolveContext *ctx) {
    if (!ctx || !ctx->working) return 0;

    LPModel *model = ctx->working;
    if (!is_scp_structure(model)) return 0;

    int m = model->num_cons;
    int n = model->num_vars;
    int count = 0;

    /* For each row (element), count active columns (sets) covering it */
    for (int i = 0; i < m; i++) {
        if (ctx->row_deleted[i]) continue;

        /* Only apply to covering (>=) or partitioning (=) constraints */
        if (model->sense[i] != 'G' && model->sense[i] != 'E') continue;

        /* Count active sets covering this element and track the single one */
        int covering_count = 0;
        int single_set = -1;

        /* Iterate through columns to find which cover this row */
        for (int j = 0; j < n; j++) {
            if (ctx->col_deleted[j]) continue;

            /* Check if column j covers row i */
            for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
                if (model->A->rowidx[p] == i && fabs(model->A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                    covering_count++;
                    single_set = j;
                    break;
                }
            }
        }

        /* If exactly one active set covers this element, fix it to 1 */
        if (covering_count == 1 && single_set >= 0) {
            /* Check if already fixed */
            if (model->lb[single_set] >= 1.0 - RALPH_ZERO_TOL) continue;

            /* Fix set to 1 */
            model->lb[single_set] = 1.0;
            model->ub[single_set] = 1.0;

            /* Update RHS for all constraints this set covers */
            for (int p = model->A->colptr[single_set]; p < model->A->colptr[single_set + 1]; p++) {
                int row = model->A->rowidx[p];
                if (!ctx->row_deleted[row]) {
                    model->b[row] -= model->A->values[p];  /* Subtract 1 from RHS */
                }
            }

            /* Update objective */
            model->obj_offset += model->c[single_set];

            /* Mark column as deleted (fixed to 1) */
            ctx->col_deleted[single_set] = 1;
            count++;

            /* Check if any constraint became infeasible */
            for (int p = model->A->colptr[single_set]; p < model->A->colptr[single_set + 1]; p++) {
                int row = model->A->rowidx[p];
                if (ctx->row_deleted[row]) continue;

                double rhs = model->b[row];
                if (model->sense[row] == 'G' && rhs < -RALPH_FEAS_TOL) {
                    /* Can't satisfy >= constraint */
                    /* Actually this shouldn't happen if RHS was 1 */
                }
                if (model->sense[row] == 'E' && rhs < -RALPH_FEAS_TOL) {
                    return -1;  /* Infeasible - over-covered */
                }

                /* If RHS <= 0 for covering constraint, it's satisfied - remove */
                if (model->sense[row] == 'G' && rhs <= RALPH_ZERO_TOL) {
                    ctx->row_deleted[row] = 1;
                }
                /* If RHS = 0 for partitioning, constraint is satisfied - remove */
                if (model->sense[row] == 'E' && fabs(rhs) < RALPH_ZERO_TOL) {
                    ctx->row_deleted[row] = 1;
                }
            }
        }

        /* Check if element has no covering sets - infeasible */
        if (covering_count == 0 && model->b[i] > RALPH_ZERO_TOL) {
            if (model->sense[i] == 'G' || model->sense[i] == 'E') {
                return -1;  /* Cannot cover this element */
            }
        }
    }

    return count;
}

/*
 * Row dominance reduction for SCP.
 *
 * For covering constraints (>=): row i dominates row j if every set
 * covering row i also covers row j, and RHS[i] >= RHS[j].
 * The dominated row i can be removed (it's implied by row j).
 */
int presolve_scp_row_dominance(PresolveContext *ctx) {
    if (!ctx || !ctx->working) return 0;

    LPModel *model = ctx->working;
    if (!is_scp_structure(model)) return 0;

    int m = model->num_cons;
    int n = model->num_vars;
    int count = 0;

    /* Build row coverage sets for efficient comparison */
    /* For each row, store a bitmask or list of covering columns */

    /* Allocate coverage arrays: coverage[i] = list of active columns covering row i */
    int **row_coverage = (int **)calloc(m, sizeof(int *));
    int *row_coverage_count = (int *)calloc(m, sizeof(int));
    if (!row_coverage || !row_coverage_count) {
        free(row_coverage);
        free(row_coverage_count);
        return 0;
    }

    /* First pass: count coverage per row */
    for (int j = 0; j < n; j++) {
        if (ctx->col_deleted[j]) continue;
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int i = model->A->rowidx[p];
            if (!ctx->row_deleted[i] && fabs(model->A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                row_coverage_count[i]++;
            }
        }
    }

    /* Allocate coverage lists */
    for (int i = 0; i < m; i++) {
        if (row_coverage_count[i] > 0) {
            row_coverage[i] = (int *)malloc(row_coverage_count[i] * sizeof(int));
            if (!row_coverage[i]) {
                /* Cleanup on failure */
                for (int k = 0; k < i; k++) free(row_coverage[k]);
                free(row_coverage);
                free(row_coverage_count);
                return 0;
            }
        }
        row_coverage_count[i] = 0;  /* Reset for second pass */
    }

    /* Second pass: fill coverage lists */
    for (int j = 0; j < n; j++) {
        if (ctx->col_deleted[j]) continue;
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int i = model->A->rowidx[p];
            if (!ctx->row_deleted[i] && fabs(model->A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                row_coverage[i][row_coverage_count[i]++] = j;
            }
        }
    }

    /* Check dominance: row i dominated by row j if coverage[i] superset of coverage[j] */
    /* Only for covering constraints (>=) */
    for (int i = 0; i < m; i++) {
        if (ctx->row_deleted[i]) continue;
        if (model->sense[i] != 'G') continue;  /* Only covering constraints */

        for (int j = 0; j < m; j++) {
            if (i == j || ctx->row_deleted[j]) continue;
            if (model->sense[j] != 'G') continue;

            /* Check if row i is dominated by row j */
            /* i dominated if: coverage[j] subset of coverage[i] AND RHS[j] >= RHS[i] */
            if (model->b[j] < model->b[i] - RALPH_ZERO_TOL) continue;

            /* Check subset: every column covering j must also cover i */
            int is_dominated = 1;
            for (int k = 0; k < row_coverage_count[j] && is_dominated; k++) {
                int col = row_coverage[j][k];
                /* Check if col covers row i */
                int found = 0;
                for (int l = 0; l < row_coverage_count[i] && !found; l++) {
                    if (row_coverage[i][l] == col) found = 1;
                }
                if (!found) is_dominated = 0;
            }

            if (is_dominated && row_coverage_count[j] > 0) {
                /* Row i is dominated by row j - remove row i */
                ctx->row_deleted[i] = 1;
                count++;
                break;  /* Move to next row i */
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < m; i++) free(row_coverage[i]);
    free(row_coverage);
    free(row_coverage_count);

    return count;
}

/*
 * Column dominance reduction for SCP.
 *
 * Column j dominates column k if:
 *   - Set j covers everything set k covers (A[*,j] >= A[*,k])
 *   - Cost c[j] <= c[k]
 *
 * The dominated column k can be fixed to 0.
 */
int presolve_scp_column_dominance(PresolveContext *ctx) {
    if (!ctx || !ctx->working) return 0;

    LPModel *model = ctx->working;
    if (!is_scp_structure(model)) return 0;

    int n = model->num_vars;
    int count = 0;

    /* Build column coverage as sorted arrays for efficient comparison */
    int **col_coverage = (int **)calloc(n, sizeof(int *));
    int *col_coverage_count = (int *)calloc(n, sizeof(int));
    if (!col_coverage || !col_coverage_count) {
        free(col_coverage);
        free(col_coverage_count);
        return 0;
    }

    /* Build coverage lists from sparse matrix (already column-major) */
    for (int j = 0; j < n; j++) {
        if (ctx->col_deleted[j]) continue;

        int nnz = 0;
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            if (!ctx->row_deleted[model->A->rowidx[p]] &&
                fabs(model->A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                nnz++;
            }
        }

        if (nnz > 0) {
            col_coverage[j] = (int *)malloc(nnz * sizeof(int));
            if (!col_coverage[j]) {
                for (int k = 0; k < j; k++) free(col_coverage[k]);
                free(col_coverage);
                free(col_coverage_count);
                return 0;
            }

            int idx = 0;
            for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
                int row = model->A->rowidx[p];
                if (!ctx->row_deleted[row] && fabs(model->A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                    col_coverage[j][idx++] = row;
                }
            }
            col_coverage_count[j] = nnz;
        }
    }

    /* Check dominance: column j dominates column k if coverage[j] superset of coverage[k] */
    for (int k = 0; k < n; k++) {
        if (ctx->col_deleted[k]) continue;
        if (col_coverage_count[k] == 0) continue;

        /* Only consider binary variables */
        if (model->var_type[k] != 'B' &&
            !(model->var_type[k] == 'I' && fabs(model->lb[k]) < RALPH_ZERO_TOL &&
              model->ub[k] >= 1.0 - RALPH_ZERO_TOL)) {
            continue;
        }

        double cost_k = model->c[k] * model->obj_sense;  /* Adjusted for min */

        for (int j = 0; j < n; j++) {
            if (j == k || ctx->col_deleted[j]) continue;
            if (col_coverage_count[j] < col_coverage_count[k]) continue;  /* j can't dominate k */

            double cost_j = model->c[j] * model->obj_sense;
            if (cost_j > cost_k + RALPH_ZERO_TOL) continue;  /* j not cheaper */

            /* Check if j covers everything k covers */
            int dominates = 1;
            int j_idx = 0;
            for (int k_idx = 0; k_idx < col_coverage_count[k] && dominates; k_idx++) {
                int row_k = col_coverage[k][k_idx];
                /* Find row_k in col_coverage[j] (both sorted by row index) */
                while (j_idx < col_coverage_count[j] && col_coverage[j][j_idx] < row_k) {
                    j_idx++;
                }
                if (j_idx >= col_coverage_count[j] || col_coverage[j][j_idx] != row_k) {
                    dominates = 0;
                }
            }

            if (dominates) {
                /* Column k is dominated by column j - fix k to 0 */
                model->lb[k] = 0.0;
                model->ub[k] = 0.0;
                ctx->col_deleted[k] = 1;
                count++;
                break;  /* Move to next k */
            }
        }
    }

    /* Cleanup */
    for (int j = 0; j < n; j++) free(col_coverage[j]);
    free(col_coverage);
    free(col_coverage_count);

    return count;
}

/*
 * Combined SCP presolve pass.
 */
int presolve_scp(PresolveContext *ctx) {
    if (!ctx) return 0;

    int total = 0;
    int changed = 1;
    int max_rounds = 10;
    int round = 0;

    while (changed && round < max_rounds) {
        changed = 0;
        round++;

        /* Essential sets first (most effective) */
        int n = presolve_scp_essential_sets(ctx);
        if (n < 0) return -1;
        changed += n;
        total += n;

        /* Row dominance */
        n = presolve_scp_row_dominance(ctx);
        if (n < 0) return -1;
        changed += n;
        total += n;

        /* Column dominance */
        n = presolve_scp_column_dominance(ctx);
        if (n < 0) return -1;
        changed += n;
        total += n;
    }

    return total;
}

/* ============================================================================
 * Build Reduced Model
 * ============================================================================ */

/*
 * Build a reduced LPModel by physically removing deleted rows and columns.
 *
 * This is necessary because the working model still contains all original
 * rows/columns - only marked as deleted in arrays. The simplex solver needs
 * a properly sized model to work correctly.
 *
 * Parameters:
 *   ctx     - Presolve context with deletion flags
 *   var_map - Output: mapping from reduced var idx to original var idx
 *   con_map - Output: mapping from reduced con idx to original con idx
 *
 * Returns:
 *   New LPModel with only active rows/columns, or NULL on error.
 */
static LPModel* build_reduced_model(PresolveContext *ctx,
                                    int *var_map, int *con_map) {
    if (!ctx || !ctx->working) return NULL;

    LPModel *orig = ctx->working;
    int n_orig = orig->num_vars;
    int m_orig = orig->num_cons;

    /* Count active rows and columns */
    int n_new = 0, m_new = 0;
    for (int j = 0; j < n_orig; j++) {
        if (!ctx->col_deleted[j]) n_new++;
    }
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i]) m_new++;
    }

    /* If no reduction, just return a copy of the working model */
    if (n_new == n_orig && m_new == m_orig) {
        return lp_model_copy(orig);
    }

    /* Build reverse mapping: original idx -> new idx */
    int *col_map_inv = (int *)malloc((size_t)n_orig * sizeof(int));
    int *row_map_inv = (int *)malloc((size_t)m_orig * sizeof(int));

    if (!col_map_inv || !row_map_inv) {
        free(col_map_inv);
        free(row_map_inv);
        return NULL;
    }

    int new_col = 0;
    for (int j = 0; j < n_orig; j++) {
        if (!ctx->col_deleted[j]) {
            col_map_inv[j] = new_col;
            if (var_map) var_map[new_col] = j;
            new_col++;
        } else {
            col_map_inv[j] = -1;
        }
    }

    int new_row = 0;
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i]) {
            row_map_inv[i] = new_row;
            if (con_map) con_map[new_row] = i;
            new_row++;
        } else {
            row_map_inv[i] = -1;
        }
    }

    /* Create new model */
    LPModel *reduced = lp_model_create();
    if (!reduced) {
        free(col_map_inv);
        free(row_map_inv);
        return NULL;
    }

    /* Copy problem metadata */
    reduced->obj_sense = orig->obj_sense;
    reduced->obj_offset = orig->obj_offset;
    if (orig->name) {
        reduced->name = strdup(orig->name);
    }

    /* Allocate arrays for new dimensions */
    reduced->num_vars = n_new;
    reduced->num_cons = m_new;

    reduced->c = (double *)calloc((size_t)n_new, sizeof(double));
    reduced->lb = (double *)malloc((size_t)n_new * sizeof(double));
    reduced->ub = (double *)malloc((size_t)n_new * sizeof(double));
    reduced->var_type = (char *)malloc((size_t)n_new * sizeof(char));
    reduced->b = (double *)malloc((size_t)m_new * sizeof(double));
    reduced->sense = (char *)malloc((size_t)m_new * sizeof(char));

    if (!reduced->c || !reduced->lb || !reduced->ub ||
        !reduced->var_type || !reduced->b || !reduced->sense) {
        free(col_map_inv);
        free(row_map_inv);
        lp_model_free(reduced);
        return NULL;
    }

    /* Copy variable data */
    new_col = 0;
    reduced->num_integers = 0;
    reduced->num_binary = 0;
    for (int j = 0; j < n_orig; j++) {
        if (!ctx->col_deleted[j]) {
            reduced->c[new_col] = orig->c[j];
            reduced->lb[new_col] = orig->lb[j];
            reduced->ub[new_col] = orig->ub[j];
            reduced->var_type[new_col] = orig->var_type[j];
            if (orig->var_type[j] == 'I' || orig->var_type[j] == 'B') {
                reduced->num_integers++;
                if (orig->var_type[j] == 'B') reduced->num_binary++;
            }
            new_col++;
        }
    }

    /* Copy constraint data */
    new_row = 0;
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i]) {
            reduced->b[new_row] = orig->b[i];
            reduced->sense[new_row] = orig->sense[i];
            new_row++;
        }
    }

    /* Build new sparse matrix: count non-zeros per column first */
    int *new_colptr = (int *)calloc((size_t)(n_new + 1), sizeof(int));
    if (!new_colptr) {
        free(col_map_inv);
        free(row_map_inv);
        lp_model_free(reduced);
        return NULL;
    }

    /* Count nnz per column */
    int total_nnz = 0;

    /* Check if original matrix exists and has data */
    if (!orig->A || !orig->A->colptr || !orig->A->rowidx || !orig->A->values) {
        fprintf(stderr, "build_reduced_model: orig->A is NULL or incomplete!\n");
        fprintf(stderr, "  orig->A=%p, num_elements=%d\n",
                (void*)orig->A, orig->num_elements);
        /* Fall back to empty matrix */
    } else {
        for (int j = 0; j < n_orig; j++) {
            if (ctx->col_deleted[j]) continue;
            int new_j = col_map_inv[j];

            for (int p = orig->A->colptr[j]; p < orig->A->colptr[j + 1]; p++) {
                int i = orig->A->rowidx[p];
                if (!ctx->row_deleted[i]) {
                    new_colptr[new_j + 1]++;
                    total_nnz++;
                }
            }
        }
    }

    /* Convert counts to offsets */
    for (int j = 0; j < n_new; j++) {
        new_colptr[j + 1] += new_colptr[j];
    }

    /* Allocate sparse arrays */
    int *new_rowidx = (int *)malloc((size_t)total_nnz * sizeof(int));
    double *new_values = (double *)malloc((size_t)total_nnz * sizeof(double));
    int *insert_pos = (int *)malloc((size_t)n_new * sizeof(int));

    if (!new_rowidx || !new_values || !insert_pos) {
        free(col_map_inv);
        free(row_map_inv);
        free(new_colptr);
        free(new_rowidx);
        free(new_values);
        free(insert_pos);
        lp_model_free(reduced);
        return NULL;
    }

    /* Initialize insert positions */
    for (int j = 0; j < n_new; j++) {
        insert_pos[j] = new_colptr[j];
    }

    /* Copy non-zero entries */
    for (int j = 0; j < n_orig; j++) {
        if (ctx->col_deleted[j]) continue;
        int new_j = col_map_inv[j];

        for (int p = orig->A->colptr[j]; p < orig->A->colptr[j + 1]; p++) {
            int i = orig->A->rowidx[p];
            if (!ctx->row_deleted[i]) {
                int new_i = row_map_inv[i];
                int pos = insert_pos[new_j]++;
                new_rowidx[pos] = new_i;
                new_values[pos] = orig->A->values[p];
            }
        }
    }

    /* Create sparse matrix */
    reduced->A = sparse_create(m_new, n_new, total_nnz);
    if (!reduced->A) {
        free(col_map_inv);
        free(row_map_inv);
        free(new_colptr);
        free(new_rowidx);
        free(new_values);
        free(insert_pos);
        lp_model_free(reduced);
        return NULL;
    }

    /* Copy sparse data */
    memcpy(reduced->A->colptr, new_colptr, (size_t)(n_new + 1) * sizeof(int));
    memcpy(reduced->A->rowidx, new_rowidx, (size_t)total_nnz * sizeof(int));
    memcpy(reduced->A->values, new_values, (size_t)total_nnz * sizeof(double));
    reduced->A->nnz = total_nnz;  /* CRITICAL: set nnz in sparse matrix */
    reduced->num_elements = total_nnz;

    /* Cleanup temporaries */
    free(col_map_inv);
    free(row_map_inv);
    free(new_colptr);
    free(new_rowidx);
    free(new_values);
    free(insert_pos);

    return reduced;
}

/* ============================================================================
 * Main Presolve Interface
 * ============================================================================ */

PresolveResult* presolve(LPModel *model) {
    if (!model) return NULL;

    PresolveContext *ctx = presolve_context_create(model);
    if (!ctx) return NULL;

    /* Enable probing for MIP (models with binary variables) */
    if (model->num_binary > 0) {
        ctx->probing = 1;
    }

    PresolveResult *result = (PresolveResult*)calloc(1, sizeof(PresolveResult));
    if (!result) {
        presolve_context_free(ctx);
        return NULL;
    }

    /* Main presolve loop */
    int changed = 1;
    int status = 0;

    while (changed && ctx->current_round < ctx->max_rounds && status >= 0) {
        changed = 0;
        ctx->current_round++;

        if (ctx->remove_fixed_vars) {
            int n = presolve_remove_fixed_vars(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;
        }

        if (ctx->remove_empty_rows) {
            int n = presolve_remove_empty_rows(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->cons_removed += n;
        }

        if (ctx->remove_empty_cols) {
            int n = presolve_remove_empty_cols(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;
        }

        if (ctx->remove_singleton_rows) {
            int n = presolve_singleton_rows(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->cons_removed += n;
        }

        if (ctx->remove_singleton_cols) {
            int n = presolve_singleton_cols(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->bounds_tightened += n;
        }

        if (ctx->remove_forcing_cons) {
            int n = presolve_forcing_constraints(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->cons_removed += n;
        }

        if (ctx->bound_tightening) {
            int n = presolve_bound_tightening(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->bounds_tightened += n;
        }

        /* MIP-specific: probing for binary variables */
        if (ctx->probing && model->num_binary > 0) {
            int n = presolve_probing(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;  /* Probing fixes variables */
        }
    }

    /* Redundant row detection via rank computation.
     * This is expensive O(m*n*min(m,n)) so we do it once AFTER other
     * reductions have stabilized. Critical for equality-heavy problems
     * like beaconfd (140 equalities out of 173 constraints). */
    if (status >= 0 && ctx->detect_redundant_rows) {
        int n = presolve_detect_redundant_rows(ctx);
        if (n < 0) {
            status = -1;  /* Inconsistent system detected */
        } else {
            result->cons_removed += n;
        }
    }

    if (status < 0) {
        /* Problem is infeasible */
        free(result);
        presolve_context_free(ctx);
        return NULL;
    }

    /* Preserve original variable types for MIP */
    result->num_orig_vars = model->num_vars;
    result->orig_var_types = (char*)calloc(model->num_vars, sizeof(char));
    if (result->orig_var_types) {
        memcpy(result->orig_var_types, model->var_type, model->num_vars * sizeof(char));
    }

    /* Count active dimensions for mapping arrays */
    int n_new = 0, m_new = 0;
    for (int j = 0; j < model->num_vars; j++) {
        if (!ctx->col_deleted[j]) n_new++;
    }
    for (int i = 0; i < model->num_cons; i++) {
        if (!ctx->row_deleted[i]) m_new++;
    }

    /* Build mappings (allocated to reduced size, not original size) */
    result->var_map = (int*)calloc(n_new > 0 ? n_new : 1, sizeof(int));
    result->con_map = (int*)calloc(m_new > 0 ? m_new : 1, sizeof(int));
    result->var_map_inv = (int*)calloc(model->num_vars, sizeof(int));
    result->con_map_inv = (int*)calloc(model->num_cons, sizeof(int));

    /* Build reduced model with mappings */
    result->reduced_model = build_reduced_model(ctx, result->var_map, result->con_map);

    if (!result->reduced_model) {
        presolve_free(result);
        presolve_context_free(ctx);
        return NULL;
    }

    /* Build inverse mappings */
    if (result->var_map_inv) {
        for (int j = 0; j < model->num_vars; j++) {
            result->var_map_inv[j] = -1;  /* Default: deleted */
        }
        for (int j = 0; j < n_new; j++) {
            int orig_j = result->var_map[j];
            if (orig_j >= 0 && orig_j < model->num_vars) {
                result->var_map_inv[orig_j] = j;
            }
        }
    }

    if (result->con_map_inv) {
        for (int i = 0; i < model->num_cons; i++) {
            result->con_map_inv[i] = -1;  /* Default: deleted */
        }
        for (int i = 0; i < m_new; i++) {
            int orig_i = result->con_map[i];
            if (orig_i >= 0 && orig_i < model->num_cons) {
                result->con_map_inv[orig_i] = i;
            }
        }
    }

    presolve_context_free(ctx);
    return result;
}

void presolve_free(PresolveResult *result) {
    if (!result) return;

    lp_model_free(result->reduced_model);
    result->reduced_model = NULL;
    SAFE_FREE(result->fixed_vars);
    SAFE_FREE(result->fixed_values);
    SAFE_FREE(result->removed_cons);
    SAFE_FREE(result->var_map);
    SAFE_FREE(result->con_map);
    SAFE_FREE(result->var_map_inv);
    SAFE_FREE(result->con_map_inv);
    SAFE_FREE(result->orig_var_types);
    SAFE_FREE(result->bound_change_vars);
    SAFE_FREE(result->old_lb);
    SAFE_FREE(result->old_ub);
    free(result);
}

/* Recover original solution from presolved solution */
int postsolve(const PresolveResult *result, const double *reduced_solution,
              double *original_solution) {
    if (!result || !reduced_solution || !original_solution) return -1;

    /* Initialize with fixed values */
    for (int j = 0; j < result->num_fixed_vars; j++) {
        if (result->fixed_vars && result->fixed_values) {
            original_solution[result->fixed_vars[j]] = result->fixed_values[j];
        }
    }

    /* Copy solution values using mapping */
    if (result->var_map && result->reduced_model) {
        int num_reduced_vars = result->reduced_model->num_vars;
        for (int j = 0; j < num_reduced_vars; j++) {
            int orig_idx = result->var_map[j];
            if (orig_idx >= 0) {
                original_solution[orig_idx] = reduced_solution[j];
            }
        }
    }

    return 0;
}
