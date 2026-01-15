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
    ctx->row_lb = (double*)malloc(model->num_cons * sizeof(double));
    ctx->row_ub = (double*)malloc(model->num_cons * sizeof(double));

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

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        double lb = 0.0;
        double ub = 0.0;

        /* Compute implied bounds: lb <= a'x <= ub based on variable bounds */
        for (int j = 0; j < model->num_vars; j++) {
            if (ctx->col_deleted[j]) continue;

            double aij = sparse_get_element(model->A, i, j);
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

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        /* Count non-zeros in row */
        int nnz = 0;
        for (int j = 0; j < model->num_vars && nnz == 0; j++) {
            if (!ctx->col_deleted[j]) {
                if (fabs(sparse_get_element(model->A, i, j)) > RALPH_ZERO_TOL) {
                    nnz++;
                }
            }
        }

        if (nnz == 0) {
            /* Empty row */
            double rhs = model->b[i];

            /* Check feasibility */
            if (model->sense[i] == 'L' && rhs < -RALPH_FEAS_TOL) {
                /* 0 <= negative: infeasible */
                return -1;
            }
            if (model->sense[i] == 'G' && rhs > RALPH_FEAS_TOL) {
                /* 0 >= positive: infeasible */
                return -1;
            }
            if (model->sense[i] == 'E' && fabs(rhs) > RALPH_FEAS_TOL) {
                /* 0 = nonzero: infeasible */
                return -1;
            }

            ctx->row_deleted[i] = 1;
            count++;
        }
    }

    return count;
}

/* Remove columns with no non-zero coefficients */
int presolve_remove_empty_cols(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;

    for (int j = 0; j < model->num_vars; j++) {
        if (ctx->col_deleted[j]) continue;

        /* Check if column is empty */
        int nnz = model->A->colptr[j + 1] - model->A->colptr[j];

        /* Also check if all rows containing this column are deleted */
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

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        /* Find the single non-zero */
        int singleton_col = -1;
        double singleton_val = 0.0;
        int nnz = 0;

        for (int j = 0; j < model->num_vars; j++) {
            if (ctx->col_deleted[j]) continue;

            double aij = sparse_get_element(model->A, i, j);
            if (fabs(aij) > RALPH_ZERO_TOL) {
                nnz++;
                singleton_col = j;
                singleton_val = aij;
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
                return -1;  /* Infeasible */
            }

            ctx->row_deleted[i] = 1;
            count++;
        }
    }

    return count;
}

/* Handle singleton columns (columns with exactly one non-zero) */
int presolve_singleton_cols(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;

    for (int j = 0; j < model->num_vars; j++) {
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

            /* Get contribution from other variables in the row */
            double other_lb = 0.0, other_ub = 0.0;
            for (int jj = 0; jj < model->num_vars; jj++) {
                if (jj == j || ctx->col_deleted[jj]) continue;

                double aij = sparse_get_element(model->A, singleton_row, jj);
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

            /* Implied bounds on singleton_val * x_j */
            double ax_lb = rhs - other_ub;
            double ax_ub = rhs - other_lb;

            /* Convert to bounds on x_j */
            if (singleton_val > 0) {
                if (ax_lb > -RALPH_INFINITY/2) {
                    model->lb[j] = fmax(model->lb[j], ax_lb / singleton_val);
                }
                if (ax_ub < RALPH_INFINITY/2) {
                    model->ub[j] = fmin(model->ub[j], ax_ub / singleton_val);
                }
            } else {
                if (ax_ub < RALPH_INFINITY/2) {
                    model->lb[j] = fmax(model->lb[j], ax_ub / singleton_val);
                }
                if (ax_lb > -RALPH_INFINITY/2) {
                    model->ub[j] = fmin(model->ub[j], ax_lb / singleton_val);
                }
            }

            if (model->lb[j] > model->ub[j] + RALPH_FEAS_TOL) {
                return -1;  /* Infeasible */
            }

            count++;  /* Bound tightened */
        }
    }

    return count;
}

/* Detect and handle forcing constraints */
int presolve_forcing_constraints(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;

    presolve_compute_implied_bounds(ctx);

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        double rhs = model->b[i];
        double row_lb = ctx->row_lb[i];
        double row_ub = ctx->row_ub[i];

        if (model->sense[i] == 'L') {
            /* a'x <= b */
            if (row_lb > rhs + RALPH_FEAS_TOL) {
                return -1;  /* Infeasible */
            }
            if (row_ub <= rhs + RALPH_FEAS_TOL) {
                /* Constraint is redundant */
                ctx->row_deleted[i] = 1;
                count++;
            } else if (row_lb >= rhs - RALPH_FEAS_TOL) {
                /* Forcing: must be at equality */
                /* All variables at their bounds that achieve row_lb */
                for (int j = 0; j < model->num_vars; j++) {
                    if (ctx->col_deleted[j]) continue;
                    double aij = sparse_get_element(model->A, i, j);
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
                return -1;  /* Infeasible */
            }
            if (row_lb >= rhs - RALPH_FEAS_TOL) {
                /* Constraint is redundant */
                ctx->row_deleted[i] = 1;
                count++;
            }
        } else {  /* Equality */
            if (row_lb > rhs + RALPH_FEAS_TOL || row_ub < rhs - RALPH_FEAS_TOL) {
                return -1;  /* Infeasible */
            }
        }
    }

    return count;
}

/* Tighten variable bounds using constraint information */
int presolve_bound_tightening(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int count = 0;

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        double rhs = model->b[i];

        for (int j = 0; j < model->num_vars; j++) {
            if (ctx->col_deleted[j]) continue;

            double aij = sparse_get_element(model->A, i, j);
            if (fabs(aij) < RALPH_ZERO_TOL) continue;

            /* Compute contribution from other variables */
            double other_lb = 0.0, other_ub = 0.0;
            int valid = 1;

            for (int jj = 0; jj < model->num_vars; jj++) {
                if (jj == j || ctx->col_deleted[jj]) continue;

                double aik = sparse_get_element(model->A, i, jj);
                if (fabs(aik) < RALPH_ZERO_TOL) continue;

                if (aik > 0) {
                    if (model->lb[jj] <= -RALPH_INFINITY/2) { other_lb = -RALPH_INFINITY; }
                    else { other_lb += aik * model->lb[jj]; }
                    if (model->ub[jj] >= RALPH_INFINITY/2) { other_ub = RALPH_INFINITY; }
                    else { other_ub += aik * model->ub[jj]; }
                } else {
                    if (model->ub[jj] >= RALPH_INFINITY/2) { other_lb = -RALPH_INFINITY; }
                    else { other_lb += aik * model->ub[jj]; }
                    if (model->lb[jj] <= -RALPH_INFINITY/2) { other_ub = RALPH_INFINITY; }
                    else { other_ub += aik * model->lb[jj]; }
                }
            }

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
                return -1;
            }
        }
    }

    return count;
}

/* ============================================================================
 * Main Presolve Interface
 * ============================================================================ */

PresolveResult* presolve(LPModel *model) {
    if (!model) return NULL;

    PresolveContext *ctx = presolve_context_create(model);
    if (!ctx) return NULL;

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

    /* Build mappings */
    result->var_map = (int*)malloc(model->num_vars * sizeof(int));
    result->con_map = (int*)malloc(model->num_cons * sizeof(int));
    result->var_map_inv = (int*)malloc(model->num_vars * sizeof(int));
    result->con_map_inv = (int*)malloc(model->num_cons * sizeof(int));

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
    free(result->fixed_vars);
    free(result->fixed_values);
    free(result->removed_cons);
    free(result->var_map);
    free(result->con_map);
    free(result->var_map_inv);
    free(result->con_map_inv);
    free(result->bound_change_vars);
    free(result->old_lb);
    free(result->old_ub);
    free(result);
}

/* Recover original solution from presolved solution */
int postsolve(const PresolveResult *result, const double *reduced_solution,
              double *original_solution) {
    if (!result || !reduced_solution || !original_solution) return -1;

    /* Initialize with fixed values */
    for (int j = 0; j < result->num_fixed_vars; j++) {
        original_solution[result->fixed_vars[j]] = result->fixed_values[j];
    }

    /* Copy solution values using mapping */
    int num_reduced = 0;
    for (int j = 0; result->var_map && result->var_map[j] >= 0; j++) {
        original_solution[result->var_map[j]] = reduced_solution[j];
        num_reduced++;
    }

    return 0;
}
