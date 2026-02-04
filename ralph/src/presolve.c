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

    ctx->max_rounds = 10;
    ctx->current_round = 0;

    /* Allocate working arrays */
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

    if (status < 0) {
        /* Problem is infeasible */
        free(result);
        presolve_context_free(ctx);
        return NULL;
    }

    /* Build reduced model */
    result->reduced_model = ctx->working;
    ctx->working = NULL;  /* Transfer ownership */

    /* Preserve original variable types for MIP */
    result->num_orig_vars = model->num_vars;
    result->orig_var_types = (char*)calloc(model->num_vars, sizeof(char));
    if (result->orig_var_types) {
        memcpy(result->orig_var_types, model->var_type, model->num_vars * sizeof(char));
    }

    /* Ensure reduced model has correct variable types */
    if (result->reduced_model && result->reduced_model->var_type) {
        /* The working model was a copy of original, but we need to update
         * var_type array to reflect only non-deleted variables in correct order */
        int new_var = 0;
        for (int j = 0; j < model->num_vars; j++) {
            if (!ctx->col_deleted[j]) {
                result->reduced_model->var_type[new_var] = model->var_type[j];
                new_var++;
            }
        }
        /* Recount integers and binaries */
        result->reduced_model->num_integers = 0;
        result->reduced_model->num_binary = 0;
        for (int j = 0; j < result->reduced_model->num_vars; j++) {
            if (result->reduced_model->var_type[j] == 'I' ||
                result->reduced_model->var_type[j] == 'B') {
                result->reduced_model->num_integers++;
                if (result->reduced_model->var_type[j] == 'B') {
                    result->reduced_model->num_binary++;
                }
            }
        }
    }

    /* Build mappings */
    result->var_map = (int*)calloc(model->num_vars, sizeof(int));
    result->con_map = (int*)calloc(model->num_cons, sizeof(int));
    result->var_map_inv = (int*)calloc(model->num_vars, sizeof(int));
    result->con_map_inv = (int*)calloc(model->num_cons, sizeof(int));

    if (result->var_map && result->con_map) {
        int new_var = 0;
        for (int j = 0; j < model->num_vars; j++) {
            if (!ctx->col_deleted[j]) {
                result->var_map_inv[j] = new_var;
                result->var_map[new_var] = j;
                new_var++;
            } else {
                result->var_map_inv[j] = -1;
            }
        }

        int new_con = 0;
        for (int i = 0; i < model->num_cons; i++) {
            if (!ctx->row_deleted[i]) {
                result->con_map_inv[i] = new_con;
                result->con_map[new_con] = i;
                new_con++;
            } else {
                result->con_map_inv[i] = -1;
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
