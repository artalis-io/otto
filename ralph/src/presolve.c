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

/* Forward declarations */
static int postsolve_push(PresolveResult *result, PostsolveOp op);

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
    ctx->probing = 0;               /* MIP only */
    ctx->detect_redundant_rows = 1; /* Only removes redundant equality constraints */

    ctx->technique_mask = 0xFFFF;  /* All techniques enabled by default */

    ctx->max_rounds = 20;  /* Multiple rounds for fixed-point convergence (GLOP uses 20) */
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
 * Row Activity Bounds (shared primitive)
 * ============================================================================ */

void compute_row_bounds(const double *row, int n,
                        const double *var_lb, const double *var_ub,
                        const int *col_deleted, RowBounds *out) {
    out->lb = 0.0;
    out->ub = 0.0;
    out->abs_sum = 0.0;
    out->lb_finite = 1;
    out->ub_finite = 1;

    for (int j = 0; j < n; j++) {
        if (col_deleted && col_deleted[j]) continue;
        double aij = row[j];
        if (fabs(aij) < RALPH_ZERO_TOL) continue;

        if (aij > 0) {
            if (var_lb[j] <= -RALPH_INFINITY/2) {
                out->lb_finite = 0;
            } else {
                double contrib = aij * var_lb[j];
                out->lb += contrib;
                out->abs_sum += fabs(contrib);
            }
            if (var_ub[j] >= RALPH_INFINITY/2) {
                out->ub_finite = 0;
            } else {
                double contrib = aij * var_ub[j];
                out->ub += contrib;
            }
        } else {
            if (var_ub[j] >= RALPH_INFINITY/2) {
                out->lb_finite = 0;
            } else {
                double contrib = aij * var_ub[j];
                out->lb += contrib;
                out->abs_sum += fabs(contrib);
            }
            if (var_lb[j] <= -RALPH_INFINITY/2) {
                out->ub_finite = 0;
            } else {
                double contrib = aij * var_lb[j];
                out->ub += contrib;
            }
        }
    }

    /* Map non-finite accumulations to infinity sentinels */
    if (!out->lb_finite) out->lb = -RALPH_INFINITY;
    if (!out->ub_finite) out->ub = RALPH_INFINITY;
}

/* ============================================================================
 * Implied Bounds Computation
 * ============================================================================ */

void presolve_compute_implied_bounds(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int n = model->num_vars;

    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return;

    for (int i = 0; i < model->num_cons; i++) {
        if (ctx->row_deleted[i]) continue;

        sparse_get_row(model->A, i, row);

        RowBounds rb;
        compute_row_bounds(row, n, model->lb, model->ub, ctx->col_deleted, &rb);

        ctx->row_lb[i] = rb.lb;
        ctx->row_ub[i] = rb.ub;
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
            /* Row i is: a_ij * x_j (sense) b_i
             * For inequality constraints: derive implied bound, tighten, delete.
             * For equality constraints: only delete if already redundant
             * (equalities are better handled by Gaussian elimination to
             * avoid creating ill-conditioned reduced models). */
            double rhs = model->b[i];
            double implied_val = rhs / singleton_val;
            int j = singleton_col;
            double lb = model->lb[j];
            double ub = model->ub[j];
            int can_delete = 0;

            if (model->sense[i] == 'E') {
                /* Conservative for equality: only delete if bounds force the value */
                if (fabs(lb - implied_val) <= RALPH_FEAS_TOL &&
                    fabs(ub - implied_val) <= RALPH_FEAS_TOL) {
                    can_delete = 1;
                }
            } else if ((model->sense[i] == 'L' && singleton_val > 0) ||
                       (model->sense[i] == 'G' && singleton_val < 0)) {
                /* Implies x_j <= implied_val */
                if (ub <= implied_val + RALPH_FEAS_TOL) {
                    can_delete = 1;  /* Already redundant */
                } else if (fabs(singleton_val) >= RALPH_PIVOT_TOL &&
                           implied_val >= lb - RALPH_FEAS_TOL) {
                    /* Tighten upper bound and delete */
                    model->ub[j] = implied_val;
                    can_delete = 1;
                }
            } else {
                /* Implies x_j >= implied_val */
                if (lb >= implied_val - RALPH_FEAS_TOL) {
                    can_delete = 1;  /* Already redundant */
                } else if (fabs(singleton_val) >= RALPH_PIVOT_TOL &&
                           implied_val <= ub + RALPH_FEAS_TOL) {
                    /* Tighten lower bound and delete */
                    model->lb[j] = implied_val;
                    can_delete = 1;
                }
            }

            if (can_delete) {
                ctx->row_deleted[i] = 1;
                count++;
            }
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

            /* Derive bounds respecting constraint sense.
             * Use relative tolerance to avoid accumulating numerical errors. */
            char sense = model->sense[singleton_row];
            double lb_tol = fmax(RALPH_FEAS_TOL, RALPH_FEAS_TOL * fabs(model->lb[j]));
            double ub_tol = fmax(RALPH_FEAS_TOL, RALPH_FEAS_TOL * fabs(model->ub[j]));

            /* For <= or = constraints: a_ij * x_j <= rhs - other_lb */
            if (sense == 'L' || sense == 'E') {
                if (other_lb > -RALPH_INFINITY/2) {
                    double ax_ub = rhs - other_lb;
                    double new_bound = ax_ub / singleton_val;
                    if (singleton_val > 0) {
                        if (new_bound < model->ub[j] - ub_tol)
                            model->ub[j] = new_bound;
                    } else {
                        if (new_bound > model->lb[j] + lb_tol)
                            model->lb[j] = new_bound;
                    }
                }
            }

            /* For >= or = constraints: a_ij * x_j >= rhs - other_ub */
            if (sense == 'G' || sense == 'E') {
                if (other_ub < RALPH_INFINITY/2) {
                    double ax_lb = rhs - other_ub;
                    double new_bound = ax_lb / singleton_val;
                    if (singleton_val > 0) {
                        if (new_bound > model->lb[j] + lb_tol)
                            model->lb[j] = new_bound;
                    } else {
                        if (new_bound < model->ub[j] - ub_tol)
                            model->ub[j] = new_bound;
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

/* ============================================================================
 * Doubleton Equality Elimination
 * ============================================================================ */

/*
 * For equality constraints with exactly 2 non-zeros:
 *   a_j * x_j + a_k * x_k = b_i
 * Solve for one variable (the "eliminated" one):
 *   x_j = (b_i - a_k * x_k) / a_j = offset + factor * x_k
 * where offset = b_i / a_j, factor = -a_k / a_j
 *
 * Substitute into all other constraints and the objective, then remove
 * row i and column j. Record substitution for postsolve.
 *
 * Choice of which variable to eliminate:
 *   - Prefer the one with the larger absolute coefficient (pivot stability)
 *   - Never eliminate integer/binary variables
 *   - Prefer eliminating free variables (no bounds)
 */
int presolve_doubleton_equality(PresolveContext *ctx, PresolveResult *result) {
    LPModel *model = ctx->working;
    int n = model->num_vars;
    int m = model->num_cons;
    int count = 0;

    /* Allocate dense row buffer */
    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return 0;

    for (int i = 0; i < m; i++) {
        if (ctx->row_deleted[i]) continue;
        if (model->sense[i] != 'E') continue;  /* Only equality constraints */

        /* Extract row and find exactly 2 non-zeros */
        sparse_get_row(model->A, i, row);
        int col1 = -1, col2 = -1;
        double val1 = 0.0, val2 = 0.0;
        int nnz = 0;

        for (int j = 0; j < n; j++) {
            if (ctx->col_deleted[j]) continue;
            if (fabs(row[j]) > RALPH_ZERO_TOL) {
                nnz++;
                if (nnz == 1) { col1 = j; val1 = row[j]; }
                else if (nnz == 2) { col2 = j; val2 = row[j]; }
                else break;
            }
        }
        if (nnz != 2) continue;

        double rhs = model->b[i];

        /* Decide which variable to eliminate.
         * Prefer: (1) continuous over integer, (2) larger coefficient */
        int elim, remain;
        double a_elim, a_remain;

        int col1_integer = (model->var_type[col1] == 'I' || model->var_type[col1] == 'B');
        int col2_integer = (model->var_type[col2] == 'I' || model->var_type[col2] == 'B');

        /* Never eliminate integer/binary variables */
        if (col1_integer && col2_integer) continue;

        if (col1_integer) {
            /* Must eliminate col2 */
            elim = col2; a_elim = val2;
            remain = col1; a_remain = val1;
        } else if (col2_integer) {
            /* Must eliminate col1 */
            elim = col1; a_elim = val1;
            remain = col2; a_remain = val2;
        } else {
            /* Both continuous: eliminate the one with larger coefficient */
            if (fabs(val1) >= fabs(val2)) {
                elim = col1; a_elim = val1;
                remain = col2; a_remain = val2;
            } else {
                elim = col2; a_elim = val2;
                remain = col1; a_remain = val1;
            }
        }

        /* Skip if pivot coefficient is too small */
        if (fabs(a_elim) < RALPH_PIVOT_TOL) continue;

        /* x_elim = (rhs - a_remain * x_remain) / a_elim
         *        = rhs/a_elim + (-a_remain/a_elim) * x_remain
         *        = offset + factor * x_remain */
        double offset = rhs / a_elim;
        double factor = -a_remain / a_elim;

        /* Check that eliminated variable's bounds are satisfied:
         * lb_elim <= offset + factor * x_remain <= ub_elim
         * This imposes additional bounds on x_remain.
         * We propagate these bounds now. */
        double lb_e = model->lb[elim];
        double ub_e = model->ub[elim];
        int bounds_ok = 1;

        if (lb_e > -RALPH_INFINITY/2) {
            /* offset + factor * x_remain >= lb_e */
            if (fabs(factor) > RALPH_ZERO_TOL) {
                double bound = (lb_e - offset) / factor;
                if (factor > 0) {
                    /* x_remain >= bound */
                    if (bound > model->ub[remain] + RALPH_FEAS_TOL) { bounds_ok = 0; }
                    else if (bound > model->lb[remain] + RALPH_FEAS_TOL) {
                        model->lb[remain] = bound;
                    }
                } else {
                    /* x_remain <= bound */
                    if (bound < model->lb[remain] - RALPH_FEAS_TOL) { bounds_ok = 0; }
                    else if (bound < model->ub[remain] - RALPH_FEAS_TOL) {
                        model->ub[remain] = bound;
                    }
                }
            } else {
                /* factor ≈ 0: x_elim ≈ offset, check directly */
                if (offset < lb_e - RALPH_FEAS_TOL) { bounds_ok = 0; }
            }
        }
        if (ub_e < RALPH_INFINITY/2 && bounds_ok) {
            /* offset + factor * x_remain <= ub_e */
            if (fabs(factor) > RALPH_ZERO_TOL) {
                double bound = (ub_e - offset) / factor;
                if (factor > 0) {
                    /* x_remain <= bound */
                    if (bound < model->lb[remain] - RALPH_FEAS_TOL) { bounds_ok = 0; }
                    else if (bound < model->ub[remain] - RALPH_FEAS_TOL) {
                        model->ub[remain] = bound;
                    }
                } else {
                    /* x_remain >= bound */
                    if (bound > model->ub[remain] + RALPH_FEAS_TOL) { bounds_ok = 0; }
                    else if (bound > model->lb[remain] + RALPH_FEAS_TOL) {
                        model->lb[remain] = bound;
                    }
                }
            } else {
                if (offset > ub_e + RALPH_FEAS_TOL) { bounds_ok = 0; }
            }
        }
        if (!bounds_ok) continue;  /* Skip — bound propagation failed */

        /* Pre-check: verify x_remain exists in every active row that
         * contains x_elim. CSC doesn't support insertion, so we must
         * skip if any row lacks x_remain (fill-in would require rebuild). */
        int can_substitute = 1;
        for (int p = model->A->colptr[elim]; p < model->A->colptr[elim + 1]; p++) {
            int k = model->A->rowidx[p];
            if (ctx->row_deleted[k] || k == i) continue;
            if (fabs(model->A->values[p]) < RALPH_ZERO_TOL) continue;

            /* Check if x_remain has an entry in row k */
            int found = 0;
            for (int q = model->A->colptr[remain]; q < model->A->colptr[remain + 1]; q++) {
                if (model->A->rowidx[q] == k) { found = 1; break; }
            }
            if (!found) { can_substitute = 0; break; }
        }
        if (!can_substitute) continue;

        /* Record substitution for postsolve */
        PostsolveOp op = {
            .type = POSTSOLVE_SUBSTITUTION,
            .var = elim,
            .var2 = remain,
            .value = offset,
            .factor = factor,
        };
        if (postsolve_push(result, op) < 0) continue;

        /* Substitute x_elim into objective:
         * c_elim * x_elim = c_elim * (offset + factor * x_remain) */
        model->obj_offset += model->c[elim] * offset;
        model->c[remain] += model->c[elim] * factor;
        model->c[elim] = 0.0;

        /* Substitute into all other constraints containing x_elim.
         * For each constraint row_k with coefficient a_ke for x_elim:
         *   a_ke * x_elim = a_ke * (offset + factor * x_remain)
         * Replace: a_ke → 0, a_kr += a_ke * factor, b_k -= a_ke * offset */
        for (int p = model->A->colptr[elim]; p < model->A->colptr[elim + 1]; p++) {
            int k = model->A->rowidx[p];
            if (ctx->row_deleted[k] || k == i) continue;

            double a_ke = model->A->values[p];
            if (fabs(a_ke) < RALPH_ZERO_TOL) continue;

            /* Update RHS */
            model->b[k] -= a_ke * offset;

            /* Update coefficient of x_remain in row k */
            for (int q = model->A->colptr[remain]; q < model->A->colptr[remain + 1]; q++) {
                if (model->A->rowidx[q] == k) {
                    model->A->values[q] += a_ke * factor;
                    break;
                }
            }
        }

        /* Zero out the eliminated variable's column entries */
        for (int p = model->A->colptr[elim]; p < model->A->colptr[elim + 1]; p++) {
            model->A->values[p] = 0.0;
        }

        /* Mark row and column as deleted */
        ctx->row_deleted[i] = 1;
        ctx->col_deleted[elim] = 1;
        count++;
    }

    free(row);
    return count;
}

/* ============================================================================
 * Implied Free Variable Detection
 * ============================================================================ */

/*
 * A variable x_j is "implied free" if its explicit bounds [lb_j, ub_j] are
 * never binding — the constraints alone restrict x_j to within [lb_j, ub_j].
 *
 * For each constraint i containing x_j with coefficient a_ij:
 *   Compute the implied range of x_j from that constraint given the bounds
 *   of all other variables. The intersection of all implied ranges gives
 *   the tightest constraint-implied bounds on x_j.
 *
 * If this intersection contains [lb_j, ub_j], the bounds are redundant.
 * Removing them simplifies the simplex (free variables don't need bound
 * flipping) and enables more doubleton equality eliminations.
 */
int presolve_implied_free(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int n = model->num_vars;
    int m = model->num_cons;
    int count = 0;

    double *row = (double*)calloc(n, sizeof(double));
    if (!row) return 0;

    for (int j = 0; j < n; j++) {
        if (ctx->col_deleted[j]) continue;

        /* Skip already-free variables */
        if (model->lb[j] <= -RALPH_INFINITY/2 && model->ub[j] >= RALPH_INFINITY/2) continue;

        /* Skip integer/binary (bounds are essential for integrality) */
        if (model->var_type[j] == 'I' || model->var_type[j] == 'B') continue;

        /* Compute tightest implied bounds on x_j from all constraints */
        double implied_lb = -RALPH_INFINITY;
        double implied_ub = RALPH_INFINITY;
        int bounded = 1;

        for (int i = 0; i < m && bounded; i++) {
            if (ctx->row_deleted[i]) continue;

            /* Get coefficient of x_j in row i from column storage */
            double a_ij = 0.0;
            for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
                if (model->A->rowidx[p] == i) {
                    a_ij = model->A->values[p];
                    break;
                }
            }
            if (fabs(a_ij) < RALPH_ZERO_TOL) continue;

            /* Extract full row for computing other variables' contributions */
            sparse_get_row(model->A, i, row);

            /* Compute sum of other variables at their bounds */
            double other_lb = 0.0, other_ub = 0.0;
            int other_lb_finite = 1, other_ub_finite = 1;

            for (int k = 0; k < n; k++) {
                if (k == j || ctx->col_deleted[k]) continue;
                double a_ik = row[k];
                if (fabs(a_ik) < RALPH_ZERO_TOL) continue;

                if (a_ik > 0) {
                    if (model->lb[k] <= -RALPH_INFINITY/2) other_lb_finite = 0;
                    else other_lb += a_ik * model->lb[k];
                    if (model->ub[k] >= RALPH_INFINITY/2) other_ub_finite = 0;
                    else other_ub += a_ik * model->ub[k];
                } else {
                    if (model->ub[k] >= RALPH_INFINITY/2) other_lb_finite = 0;
                    else other_lb += a_ik * model->ub[k];
                    if (model->lb[k] <= -RALPH_INFINITY/2) other_ub_finite = 0;
                    else other_ub += a_ik * model->lb[k];
                }
            }

            double rhs = model->b[i];

            /* Derive bounds on x_j from this constraint:
             * For <= : a_ij * x_j + other <= rhs
             * For >= : a_ij * x_j + other >= rhs
             * For =  : a_ij * x_j + other = rhs → both */
            if (model->sense[i] == 'L' || model->sense[i] == 'E') {
                /* a_ij * x_j <= rhs - other_lb */
                if (other_lb_finite) {
                    double bound = (rhs - other_lb) / a_ij;
                    if (a_ij > 0) {
                        /* x_j <= bound */
                        if (bound < implied_ub) implied_ub = bound;
                    } else {
                        /* x_j >= bound */
                        if (bound > implied_lb) implied_lb = bound;
                    }
                }
            }
            if (model->sense[i] == 'G' || model->sense[i] == 'E') {
                /* a_ij * x_j >= rhs - other_ub */
                if (other_ub_finite) {
                    double bound = (rhs - other_ub) / a_ij;
                    if (a_ij > 0) {
                        /* x_j >= bound */
                        if (bound > implied_lb) implied_lb = bound;
                    } else {
                        /* x_j <= bound */
                        if (bound < implied_ub) implied_ub = bound;
                    }
                }
            }

            /* If implied bounds already narrower than explicit, stop early */
            if (implied_lb > implied_ub + RALPH_FEAS_TOL) {
                bounded = 0;
            }
        }

        if (!bounded) continue;

        /* Check if explicit bounds are implied (redundant).
         * lb is redundant if constraints already force x_j >= lb_j
         * (i.e., implied_lb >= lb_j).
         * ub is redundant if constraints already force x_j <= ub_j
         * (i.e., implied_ub <= ub_j). */
        double margin = RALPH_FEAS_TOL;
        int lb_implied = (model->lb[j] <= -RALPH_INFINITY/2) ||
                         (implied_lb >= model->lb[j] - margin);
        int ub_implied = (model->ub[j] >= RALPH_INFINITY/2) ||
                         (implied_ub <= model->ub[j] + margin);

        if (lb_implied && ub_implied) {
            /* Both bounds are implied by constraints — tighten to implied range.
             * We use the finite implied bounds rather than ±RALPH_INFINITY to avoid
             * breaking the Big-M method in the simplex (which can't handle ±1e30). */
            int changed = 0;
            if (implied_lb > -RALPH_INFINITY/2 && implied_lb < model->ub[j]) {
                if (implied_lb > model->lb[j] + RALPH_FEAS_TOL) {
                    model->lb[j] = implied_lb;
                    changed = 1;
                }
            }
            if (implied_ub < RALPH_INFINITY/2 && implied_ub > model->lb[j]) {
                if (implied_ub < model->ub[j] - RALPH_FEAS_TOL) {
                    model->ub[j] = implied_ub;
                    changed = 1;
                }
            }
            if (changed) count++;
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

        /* First pass: compute total row activity bounds */
        RowBounds rb;
        compute_row_bounds(row, n, model->lb, model->ub, ctx->col_deleted, &rb);
        double row_lb = rb.lb, row_ub = rb.ub, abs_sum = rb.abs_sum;
        int row_lb_finite = rb.lb_finite, row_ub_finite = rb.ub_finite;

        /* Second pass: derive bounds for each variable */
        for (int j = 0; j < n; j++) {
            if (ctx->col_deleted[j]) continue;
            double aij = row[j];
            if (fabs(aij) < RALPH_ZERO_TOL) continue;

            /* Compute contribution of variable j to row bounds */
            double j_contrib_lb = 0.0, j_contrib_ub = 0.0;
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

            /* CANCELLATION CHECK: Skip this variable if its contribution is a significant
             * fraction of the total. When we compute other_lb = row_lb - j_contrib_lb,
             * catastrophic cancellation occurs if j_contrib_lb ≈ row_lb.
             * Use conservative threshold: skip if |j_contrib| > 0.1 * abs_sum */
            if (abs_sum > RALPH_ZERO_TOL && fabs(j_contrib_lb) > 0.1 * abs_sum) {
                continue;  /* Cancellation risk - skip */
            }

            /* other_lb = row_lb - j_contrib_lb (if both finite) */
            /* other_ub = row_ub - j_contrib_ub (if both finite) */
            double other_lb = row_lb_finite && j_lb_finite ? row_lb - j_contrib_lb : -RALPH_INFINITY;
            double other_ub = row_ub_finite && j_ub_finite ? row_ub - j_contrib_ub : RALPH_INFINITY;

            /* Compute safety margin based on numerical uncertainty.
             * The error in other_lb is roughly eps * abs_sum where eps is machine epsilon.
             * We use a conservative multiplier (1e-8) to account for accumulated errors. */
            double eps_factor = 1e-8;
            double safety_margin = eps_factor * fmax(abs_sum, fmax(fabs(rhs), 1.0));

            double new_lb = model->lb[j];
            double new_ub = model->ub[j];

            if (model->sense[i] == 'L' || model->sense[i] == 'E') {
                /* a_ij * x_j <= rhs - other_lb */
                if (other_lb > -RALPH_INFINITY/2) {
                    double bound = (rhs - other_lb) / aij;
                    /* Add safety margin in the conservative direction */
                    if (aij > 0) {
                        new_ub = fmin(new_ub, bound + safety_margin / fabs(aij));
                    } else {
                        new_lb = fmax(new_lb, bound - safety_margin / fabs(aij));
                    }
                }
            }

            if (model->sense[i] == 'G' || model->sense[i] == 'E') {
                /* a_ij * x_j >= rhs - other_ub */
                if (other_ub < RALPH_INFINITY/2) {
                    double bound = (rhs - other_ub) / aij;
                    /* Add safety margin in the conservative direction */
                    if (aij > 0) {
                        new_lb = fmax(new_lb, bound - safety_margin / fabs(aij));
                    } else {
                        new_ub = fmin(new_ub, bound + safety_margin / fabs(aij));
                    }
                }
            }

            /* Only accept bounds that represent SIGNIFICANT improvement.
             * Use 1% relative tolerance to avoid accumulating tiny changes. */
            double rel_tol = 0.01;  /* 1% relative improvement required */
            double abs_tol = 1e-4;  /* Absolute minimum improvement */

            double curr_lb = model->lb[j];
            double curr_ub = model->ub[j];
            double range = curr_ub - curr_lb;

            /* For lb improvement: new_lb must be significantly higher than curr_lb */
            double lb_threshold = fmax(abs_tol, rel_tol * fmax(fabs(curr_lb), range));
            /* For ub improvement: new_ub must be significantly lower than curr_ub */
            double ub_threshold = fmax(abs_tol, rel_tol * fmax(fabs(curr_ub), range));

            /* Only tighten lb if improvement is significant AND won't cause infeasibility */
            if (new_lb > curr_lb + lb_threshold && new_lb < curr_ub - abs_tol) {
                model->lb[j] = new_lb;
                count++;
            }

            /* Only tighten ub if improvement is significant AND won't cause infeasibility */
            if (new_ub < curr_ub - ub_threshold && new_ub > curr_lb + abs_tol) {
                model->ub[j] = new_ub;
                count++;
            }

            /* Sanity check - should never happen with our safeguards */
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
 * Proportional Row Detection
 * ============================================================================ */

/*
 * Detect and remove proportional (parallel) rows.
 *
 * Two rows i and j are proportional if row_i = k * row_j for some scalar k.
 * For <= constraints: keep the tighter one.
 * For = constraints: check RHS consistency (else infeasible).
 *
 * Algorithm: For each pair of rows with the same sparsity pattern,
 * check if coefficients are proportional.
 */
int presolve_proportional_rows(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int m = model->num_cons;
    int n = model->num_vars;
    int count = 0;

    /* Allocate two dense row buffers */
    double *row_i = (double*)calloc(n, sizeof(double));
    double *row_j = (double*)calloc(n, sizeof(double));
    if (!row_i || !row_j) {
        free(row_i);
        free(row_j);
        return 0;
    }

    for (int i = 0; i < m; i++) {
        if (ctx->row_deleted[i]) continue;

        sparse_get_row(model->A, i, row_i);

        /* Find first non-zero for normalization */
        int first_nz_i = -1;
        for (int k = 0; k < n; k++) {
            if (!ctx->col_deleted[k] && fabs(row_i[k]) > RALPH_ZERO_TOL) {
                first_nz_i = k;
                break;
            }
        }
        if (first_nz_i < 0) continue;  /* Empty row handled elsewhere */

        for (int j = i + 1; j < m; j++) {
            if (ctx->row_deleted[j]) continue;

            sparse_get_row(model->A, j, row_j);

            /* Check first non-zero of row j */
            double val_i = row_i[first_nz_i];
            double val_j = row_j[first_nz_i];
            if (fabs(val_j) < RALPH_ZERO_TOL) continue;  /* Different sparsity */

            double ratio = val_i / val_j;

            /* Check proportionality: row_i[k] == ratio * row_j[k] for all k */
            int proportional = 1;
            for (int k = 0; k < n && proportional; k++) {
                if (ctx->col_deleted[k]) continue;
                double diff = row_i[k] - ratio * row_j[k];
                double scale = fmax(fabs(row_i[k]), fabs(row_j[k]));
                double tol = fmax(RALPH_FEAS_TOL, RALPH_FEAS_TOL * scale);
                if (fabs(diff) > tol) proportional = 0;
            }
            if (!proportional) continue;

            /* Rows i and j are proportional: row_i = ratio * row_j
             * Normalize both: row_i (sense_i) rhs_i  and  ratio*row_j (sense_j) ratio*rhs_j */
            double rhs_i = model->b[i];
            double rhs_j_scaled = ratio * model->b[j];

            if (model->sense[i] == 'E' && model->sense[j] == 'E') {
                /* Both equalities: must have same RHS (after scaling) */
                if (fabs(rhs_i - rhs_j_scaled) > RALPH_FEAS_TOL * fmax(1.0, fabs(rhs_i))) {
                    free(row_i);
                    free(row_j);
                    return -1;  /* Infeasible: inconsistent equalities */
                }
                /* Remove duplicate */
                ctx->row_deleted[j] = 1;
                count++;
            } else if (model->sense[i] == 'L' && model->sense[j] == 'L') {
                /* Both <=: keep the tighter one */
                if (ratio > 0) {
                    /* Same direction: row_i <= rhs_i, row_j <= rhs_j
                     * After scaling: row_i <= rhs_i and row_i <= ratio*rhs_j
                     * Keep the one with smaller RHS */
                    if (rhs_i <= rhs_j_scaled + RALPH_FEAS_TOL) {
                        ctx->row_deleted[j] = 1;  /* i is tighter */
                    } else {
                        ctx->row_deleted[i] = 1;  /* j is tighter */
                    }
                    count++;
                } else {
                    /* Opposite direction after scaling — not truly parallel for <= */
                    /* ratio < 0 means row_i = ratio*row_j with sign flip.
                     * row_j <= rhs_j becomes -row_j >= -rhs_j, i.e., (row_i/ratio) >= -rhs_j
                     * This gives us a bound pair, not a redundancy. Skip. */
                }
            } else if (model->sense[i] == 'G' && model->sense[j] == 'G') {
                /* Both >=: keep the tighter one */
                if (ratio > 0) {
                    if (rhs_i >= rhs_j_scaled - RALPH_FEAS_TOL) {
                        ctx->row_deleted[j] = 1;  /* i is tighter */
                    } else {
                        ctx->row_deleted[i] = 1;  /* j is tighter */
                    }
                    count++;
                }
            }
            /* Mixed sense (L/G, L/E, G/E): more complex, skip for now */

            if (ctx->row_deleted[i]) break;  /* Row i was removed, move on */
        }
    }

    free(row_i);
    free(row_j);
    return count;
}

/* ============================================================================
 * Proportional Column Detection
 * ============================================================================ */

/*
 * Detect proportional (parallel) columns for continuous variables.
 *
 * Two columns j and k are proportional if A[*,j] = r * A[*,k] for some r > 0.
 * If c[j]/r >= c[k] (cost per unit of j is no better than k), then j is
 * dominated: we can substitute x_j out (set to its bound that helps the
 * objective most) and keep only x_k.
 *
 * For minimization with positive ratio r > 0:
 *   If c[j] >= r * c[k], then column k dominates j.
 *   Fix x_j at its lower bound (for positive obj coeff) or upper bound.
 */
int presolve_proportional_cols(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int n = model->num_vars;
    int count = 0;

    for (int j = 0; j < n; j++) {
        if (ctx->col_deleted[j]) continue;
        /* Only continuous variables */
        if (model->var_type[j] == 'I' || model->var_type[j] == 'B') continue;

        /* Get column j non-zeros */
        int nnz_j = 0;
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            if (!ctx->row_deleted[model->A->rowidx[p]] &&
                fabs(model->A->values[p]) > RALPH_ZERO_TOL) {
                nnz_j++;
            }
        }
        if (nnz_j == 0) continue;

        for (int k = j + 1; k < n; k++) {
            if (ctx->col_deleted[k]) continue;
            if (model->var_type[k] == 'I' || model->var_type[k] == 'B') continue;

            /* Quick check: same number of active non-zeros */
            int nnz_k = 0;
            for (int p = model->A->colptr[k]; p < model->A->colptr[k + 1]; p++) {
                if (!ctx->row_deleted[model->A->rowidx[p]] &&
                    fabs(model->A->values[p]) > RALPH_ZERO_TOL) {
                    nnz_k++;
                }
            }
            if (nnz_k != nnz_j) continue;

            /* Check proportionality by walking both columns */
            double ratio = 0.0;
            int proportional = 1;
            int pj = model->A->colptr[j];
            int pk = model->A->colptr[k];
            int pj_end = model->A->colptr[j + 1];
            int pk_end = model->A->colptr[k + 1];

            while (pj < pj_end && pk < pk_end && proportional) {
                /* Skip deleted rows */
                while (pj < pj_end && (ctx->row_deleted[model->A->rowidx[pj]] ||
                       fabs(model->A->values[pj]) < RALPH_ZERO_TOL)) pj++;
                while (pk < pk_end && (ctx->row_deleted[model->A->rowidx[pk]] ||
                       fabs(model->A->values[pk]) < RALPH_ZERO_TOL)) pk++;

                if (pj >= pj_end && pk >= pk_end) break;
                if (pj >= pj_end || pk >= pk_end) { proportional = 0; break; }

                int rj = model->A->rowidx[pj];
                int rk = model->A->rowidx[pk];
                if (rj != rk) { proportional = 0; break; }

                double vj = model->A->values[pj];
                double vk = model->A->values[pk];

                if (ratio == 0.0) {
                    ratio = vj / vk;
                } else {
                    double expected = ratio * vk;
                    double tol = fmax(RALPH_FEAS_TOL, RALPH_FEAS_TOL * fabs(expected));
                    if (fabs(vj - expected) > tol) proportional = 0;
                }
                pj++; pk++;
            }
            /* Check remaining entries */
            while (pj < pj_end && proportional) {
                if (!ctx->row_deleted[model->A->rowidx[pj]] &&
                    fabs(model->A->values[pj]) > RALPH_ZERO_TOL) proportional = 0;
                pj++;
            }
            while (pk < pk_end && proportional) {
                if (!ctx->row_deleted[model->A->rowidx[pk]] &&
                    fabs(model->A->values[pk]) > RALPH_ZERO_TOL) proportional = 0;
                pk++;
            }
            if (!proportional || ratio == 0.0) continue;

            /* Only handle positive ratio (same-direction columns).
             * Negative ratio means opposite constraint contributions — skip. */
            if (ratio < 0.0) continue;

            /* Columns j and k are proportional: A[*,j] = ratio * A[*,k]
             * For the internal minimizer: effective cost of j per unit of
             * constraint contribution is c[j]*obj_sense vs ratio*c[k]*obj_sense.
             * If cj_eff >= ck_eff, then j is dominated by k. */
            double cj_eff = model->c[j] * model->obj_sense;
            double ck_eff = model->c[k] * model->obj_sense * ratio;

            int dominated = -1;  /* Which column to fix */
            if (cj_eff >= ck_eff - RALPH_FEAS_TOL) {
                dominated = j;  /* j is dominated by k */
            } else {
                dominated = k;  /* k is dominated by j */
            }

            if (dominated >= 0) {
                /* Fix dominated variable at lower bound.
                 * The non-dominated column can substitute more efficiently. */
                if (model->lb[dominated] <= -RALPH_INFINITY/2) continue;

                int non_dom = (dominated == j) ? k : j;
                double dom_range = model->ub[dominated] - model->lb[dominated];

                /* When the dominated var has range and the non-dominated var has
                 * a finite upper bound, fixing at lb shrinks the feasible region.
                 * The correct fix is bound expansion (ub += ratio * range) plus
                 * a custom postsolve op.  For now, skip these cases. */
                if (dom_range > RALPH_ZERO_TOL &&
                    model->ub[non_dom] < RALPH_INFINITY / 2) {
                    continue;
                }

                double fixed_val = model->lb[dominated];
                model->lb[dominated] = fixed_val;
                model->ub[dominated] = fixed_val;
                count++;

                if (dominated == j) break;  /* j will be fixed next round */
            }
        }
    }

    return count;
}

/* ============================================================================
 * Shift Variable Bounds
 * ============================================================================ */

/*
 * Shift variables so that lower bound is zero: x' = x - lb.
 *
 * This simplifies the simplex (fewer bound flips) and can help with
 * numerical conditioning. Only shifts continuous variables with finite,
 * non-zero lower bounds.
 *
 * After shift:
 *   - lb' = 0, ub' = ub - lb
 *   - Constraint coefficients unchanged
 *   - RHS: b_i -= a_ij * lb_j for each constraint
 *   - Objective offset: obj_offset += c_j * lb_j
 *
 * Postsolve: x_j = x'_j + lb_j (original lower bound)
 */
int presolve_shift_bounds(PresolveContext *ctx, PresolveResult *result) {
    LPModel *model = ctx->working;
    int n = model->num_vars;
    int count = 0;

    for (int j = 0; j < n; j++) {
        if (ctx->col_deleted[j]) continue;

        double lb = model->lb[j];

        /* Only shift if lb is finite and non-zero */
        if (fabs(lb) < RALPH_ZERO_TOL) continue;
        if (lb <= -RALPH_INFINITY/2) continue;

        /* Don't shift integer/binary variables (changes integrality) */
        if (model->var_type[j] == 'I' || model->var_type[j] == 'B') continue;

        /* Record shift for postsolve: x_orig = x_shifted + lb */
        if (result) {
            PostsolveOp op = {
                .type = POSTSOLVE_SHIFT,
                .var = j,
                .var2 = -1,
                .value = lb,
                .factor = 0.0,
            };
            if (postsolve_push(result, op) < 0) continue;
        }

        /* Update RHS for all constraints */
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int i = model->A->rowidx[p];
            if (!ctx->row_deleted[i]) {
                model->b[i] -= model->A->values[p] * lb;
            }
        }

        /* Update objective offset */
        model->obj_offset += model->c[j] * lb;

        /* Shift bounds */
        model->ub[j] -= lb;
        model->lb[j] = 0.0;
        if (model->var_shifted && j < model->var_shifted_capacity) {
            model->var_shifted[j] = 1;
        }
        if (model->var_shift && j < model->var_shift_capacity) {
            model->var_shift[j] += lb;
        }
        count++;
    }

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

    /* Count active EQUALITY rows and active columns.
     * Only equality rows participate in redundancy detection.
     * An equality row is redundant only if it's a linear combination of
     * OTHER equality rows. Mixing inequalities is wrong — an equality
     * in the span of inequalities is NOT redundant (inequalities only
     * imply one direction, not the equality). */
    int m_eq = 0, n_active = 0;
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i] && model->sense[i] == 'E') m_eq++;
    }
    for (int j = 0; j < n_orig; j++) {
        if (!ctx->col_deleted[j]) n_active++;
    }

    if (m_eq == 0 || n_active == 0) {
        ctx->matrix_rank = 0;
        return 0;
    }

    /* Build mapping from active equality indices to dense indices */
    int *row_to_dense = (int *)malloc((size_t)m_orig * sizeof(int));
    int *col_to_dense = (int *)malloc((size_t)n_orig * sizeof(int));
    int *dense_to_row = (int *)malloc((size_t)m_eq * sizeof(int));

    if (!row_to_dense || !col_to_dense || !dense_to_row) {
        free(row_to_dense);
        free(col_to_dense);
        free(dense_to_row);
        return 0;
    }

    int dense_row = 0;
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i] && model->sense[i] == 'E') {
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
     * Size: m_eq rows x (n_active + 1) columns */
    size_t aug_cols = (size_t)n_active + 1;
    double *A = (double *)calloc((size_t)m_eq * aug_cols, sizeof(double));
    int *pivot_col = (int *)malloc((size_t)m_eq * sizeof(int));

    if (!A || !pivot_col) {
        free(row_to_dense);
        free(col_to_dense);
        free(dense_to_row);
        free(A);
        free(pivot_col);
        return 0;
    }

    /* Fill the dense matrix from sparse CSC (only equality rows) */
    for (int j = 0; j < n_orig; j++) {
        if (ctx->col_deleted[j]) continue;
        int dc = col_to_dense[j];

        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int i = model->A->rowidx[p];
            int dr = row_to_dense[i];  /* -1 for non-equality or deleted rows */
            if (dr < 0) continue;
            A[dr + (size_t)dc * m_eq] = model->A->values[p];
        }
    }

    /* Fill the RHS column (last column of augmented matrix) */
    for (int i = 0; i < m_orig; i++) {
        int dr = row_to_dense[i];
        if (dr < 0) continue;
        A[dr + (size_t)n_active * m_eq] = model->b[i];
    }

    /* Compute matrix infinity norm for relative pivot threshold.
     * Using absolute thresholds (like 1e-6) causes false rank deficiency
     * on ill-conditioned matrices where valid pivots are small. */
    double anorm = 0.0;
    for (int i = 0; i < m_eq; i++) {
        double row_sum = 0.0;
        for (int j = 0; j < n_active; j++) {
            row_sum += fabs(A[i + (size_t)j * m_eq]);
        }
        if (row_sum > anorm) anorm = row_sum;
    }
    /* Pivot threshold: relative to matrix norm, scaled by dimension.
     * This accounts for O(n) growth in Gaussian elimination. */
    int max_dim = (m_eq > n_active) ? m_eq : n_active;
    double pivot_tol = anorm * max_dim * 1e-13;
    if (pivot_tol < 1e-15) pivot_tol = 1e-15;  /* Floor for zero matrices */

    /* Gaussian elimination with partial pivoting */
    int rank = 0;
    int min_dim = (m_eq < n_active) ? m_eq : n_active;

    for (int k = 0; k < min_dim; k++) {
        pivot_col[k] = -1;
    }

    int col = 0;  /* Current column being processed */
    for (int k = 0; k < min_dim && col < n_active; ) {
        /* Find pivot: largest absolute value in column 'col' from row k onwards */
        int best_row = -1;
        double best_val = pivot_tol;

        for (int i = k; i < m_eq; i++) {
            double val = fabs(A[i + (size_t)col * m_eq]);
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
                double tmp = A[k + (size_t)j * m_eq];
                A[k + (size_t)j * m_eq] = A[best_row + (size_t)j * m_eq];
                A[best_row + (size_t)j * m_eq] = tmp;
            }
            /* Swap in dense_to_row mapping too */
            int tmp_idx = dense_to_row[k];
            dense_to_row[k] = dense_to_row[best_row];
            dense_to_row[best_row] = tmp_idx;
        }

        /* Eliminate below pivot */
        double pivot = A[k + (size_t)col * m_eq];
        for (int i = k + 1; i < m_eq; i++) {
            double factor = A[i + (size_t)col * m_eq] / pivot;
            if (fabs(factor) < RALPH_ZERO_TOL) continue;

            A[i + (size_t)col * m_eq] = 0.0;  /* Exact zero */
            for (int j = col + 1; j <= n_active; j++) {  /* Include RHS */
                A[i + (size_t)j * m_eq] -= factor * A[k + (size_t)j * m_eq];
            }
        }

        pivot_col[k] = col;
        rank++;
        k++;
        col++;
    }

    ctx->matrix_rank = rank;

    /* Check rows rank..m_eq-1 for redundancy/infeasibility */
    int count = 0;
    int infeasible = 0;

    /* Zero-row tolerance: scale with matrix norm and dimension.
     * After elimination, residuals from rounding are O(anorm * n * eps). */
    double zero_tol = anorm * max_dim * 1e-12;
    if (zero_tol < RALPH_ZERO_TOL) zero_tol = RALPH_ZERO_TOL;

    for (int k = rank; k < m_eq; k++) {
        /* Row k should be all zeros in A part (all rows are equalities) */
        int is_zero_row = 1;
        for (int j = 0; j < n_active; j++) {
            if (fabs(A[k + (size_t)j * m_eq]) > zero_tol) {
                is_zero_row = 0;
                break;
            }
        }

        if (is_zero_row) {
            double rhs_val = A[k + (size_t)n_active * m_eq];
            int orig_row = dense_to_row[k];
            /* Equality: 0x = rhs must have rhs = 0 for redundancy */
            if (fabs(rhs_val) > zero_tol) {
                infeasible = 1;
                break;
            }
            /* Equality row is redundant - mark for deletion */
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
 * MIP-Specific Presolve: Probing with Implication Propagation
 * ============================================================================ */

/*
 * Probing with implication propagation.
 *
 * For each binary variable x_j:
 * 1. Fix x_j = 0, run presolve_bound_tightening (multi-pass)
 * 2. Fix x_j = 1, run presolve_bound_tightening (multi-pass)
 * 3. If one setting is infeasible, fix x_j to the other
 * 4. If both feasible, intersect implied bounds: any bound that is tighter
 *    in BOTH probes is globally valid and can be applied permanently
 *
 * Reuses the existing presolve_bound_tightening rather than reimplementing
 * constraint-based bound propagation.  Probing is the orchestrator; bound
 * tightening is the shared primitive.
 */
int presolve_probing(PresolveContext *ctx) {
    LPModel *model = ctx->working;
    int n = model->num_vars;
    int count = 0;

    /* Save original bounds — restored after each probe */
    double *saved_lb = (double*)malloc(n * sizeof(double));
    double *saved_ub = (double*)malloc(n * sizeof(double));
    /* Capture implied bounds after each probe */
    double *implied_lb0 = (double*)malloc(n * sizeof(double));
    double *implied_ub0 = (double*)malloc(n * sizeof(double));

    if (!saved_lb || !saved_ub || !implied_lb0 || !implied_ub0) {
        free(saved_lb);
        free(saved_ub);
        free(implied_lb0);
        free(implied_ub0);
        return 0;
    }

    memcpy(saved_lb, model->lb, n * sizeof(double));
    memcpy(saved_ub, model->ub, n * sizeof(double));

    /* Limit probing to avoid expensive O(n * passes * m * n) worst case */
    int max_probe_vars = 100;
    int max_propagation_passes = 3;
    int probed = 0;

    for (int j = 0; j < n && probed < max_probe_vars; j++) {
        if (ctx->col_deleted[j]) continue;

        /* Only probe binary variables */
        if (model->var_type[j] != 'B') continue;
        if (saved_lb[j] > 0.5 || saved_ub[j] < 0.5) continue;  /* Already fixed */

        probed++;

        /* --- Probe x_j = 0 --- */
        memcpy(model->lb, saved_lb, n * sizeof(double));
        memcpy(model->ub, saved_ub, n * sizeof(double));
        model->lb[j] = 0.0;
        model->ub[j] = 0.0;

        int infeas_0 = 0;
        for (int pass = 0; pass < max_propagation_passes; pass++) {
            int r = presolve_bound_tightening(ctx);
            if (r < 0) { infeas_0 = 1; break; }
            if (r == 0) break;  /* Converged */
        }

        /* Capture probe-0 implied bounds */
        memcpy(implied_lb0, model->lb, n * sizeof(double));
        memcpy(implied_ub0, model->ub, n * sizeof(double));

        /* --- Probe x_j = 1 --- */
        memcpy(model->lb, saved_lb, n * sizeof(double));
        memcpy(model->ub, saved_ub, n * sizeof(double));
        model->lb[j] = 1.0;
        model->ub[j] = 1.0;

        int infeas_1 = 0;
        for (int pass = 0; pass < max_propagation_passes; pass++) {
            int r = presolve_bound_tightening(ctx);
            if (r < 0) { infeas_1 = 1; break; }
            if (r == 0) break;  /* Converged */
        }
        /* model->lb/ub now hold probe-1 implied bounds.
         * implied_lb0/ub0 hold probe-0 implied bounds.
         * Analyze results and apply before restoring. */

        if (infeas_0 && infeas_1) {
            memcpy(model->lb, saved_lb, n * sizeof(double));
            memcpy(model->ub, saved_ub, n * sizeof(double));
            free(saved_lb);
            free(saved_ub);
            free(implied_lb0);
            free(implied_ub0);
            return -1;
        }

        if (infeas_0) {
            /* x_j = 0 infeasible → fix x_j = 1 */
            memcpy(model->lb, saved_lb, n * sizeof(double));
            memcpy(model->ub, saved_ub, n * sizeof(double));
            model->lb[j] = 1.0;
            model->ub[j] = 1.0;
            saved_lb[j] = 1.0;
            saved_ub[j] = 1.0;
            count++;
        } else if (infeas_1) {
            /* x_j = 1 infeasible → fix x_j = 0 */
            memcpy(model->lb, saved_lb, n * sizeof(double));
            memcpy(model->ub, saved_ub, n * sizeof(double));
            model->lb[j] = 0.0;
            model->ub[j] = 0.0;
            saved_lb[j] = 0.0;
            saved_ub[j] = 0.0;
            count++;
        } else {
            /* Both feasible: intersect implied bounds.
             * x_j ∈ {0,1}, so a bound valid in BOTH probes is globally valid.
             * Take the weaker (more conservative) of the two:
             *   global lb = min(lb_probe0, lb_probe1)
             *   global ub = max(ub_probe0, ub_probe1)
             *
             * model->lb/ub still hold probe-1 bounds; read before restoring. */
            for (int k = 0; k < n; k++) {
                if (ctx->col_deleted[k] || k == j) continue;

                double new_lb = fmin(implied_lb0[k], model->lb[k]);
                double new_ub = fmax(implied_ub0[k], model->ub[k]);

                /* Apply to saved_lb/ub so subsequent probes see the tightening */
                if (new_lb > saved_lb[k] + RALPH_FEAS_TOL) {
                    saved_lb[k] = new_lb;
                    count++;
                }
                if (new_ub < saved_ub[k] - RALPH_FEAS_TOL) {
                    saved_ub[k] = new_ub;
                    count++;
                }

                /* Snap to fixed if bounds converged */
                if (saved_lb[k] > saved_ub[k] - RALPH_FEAS_TOL) {
                    double avg = (saved_lb[k] + saved_ub[k]) / 2.0;
                    saved_lb[k] = avg;
                    saved_ub[k] = avg;
                }
            }

            /* Restore from (possibly tightened) saved bounds */
            memcpy(model->lb, saved_lb, n * sizeof(double));
            memcpy(model->ub, saved_ub, n * sizeof(double));
        }
    }

    free(saved_lb);
    free(saved_ub);
    free(implied_lb0);
    free(implied_ub0);
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
        /* Still need to populate identity mappings */
        if (var_map) {
            for (int j = 0; j < n_orig; j++) var_map[j] = j;
        }
        if (con_map) {
            for (int i = 0; i < m_orig; i++) con_map[i] = i;
        }
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
    reduced->var_shifted = (unsigned char *)calloc((size_t)n_new, sizeof(unsigned char));
    reduced->var_shift = (double *)calloc((size_t)n_new, sizeof(double));
    reduced->var_type = (char *)malloc((size_t)n_new * sizeof(char));
    reduced->b = (double *)malloc((size_t)m_new * sizeof(double));
    reduced->sense = (char *)malloc((size_t)m_new * sizeof(char));
    reduced->con_origin = (int *)malloc((size_t)m_new * sizeof(int));

    if (!reduced->c || !reduced->lb || !reduced->ub || !reduced->var_shifted ||
        !reduced->var_shift ||
        !reduced->var_type || !reduced->b || !reduced->sense || !reduced->con_origin) {
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
            reduced->var_shifted[new_col] =
                (orig->var_shifted && j < orig->var_shifted_capacity) ? orig->var_shifted[j] : 0;
            reduced->var_shift[new_col] =
                (orig->var_shift && j < orig->var_shift_capacity) ? orig->var_shift[j] : 0.0;
            reduced->var_type[new_col] = orig->var_type[j];
            if (orig->var_type[j] == 'I' || orig->var_type[j] == 'B') {
                reduced->num_integers++;
                if (orig->var_type[j] == 'B') reduced->num_binary++;
            }
            new_col++;
        }
    }
    reduced->var_shifted_capacity = n_new;
    reduced->var_shift_capacity = n_new;

    /* Copy constraint data */
    new_row = 0;
    for (int i = 0; i < m_orig; i++) {
        if (!ctx->row_deleted[i]) {
            reduced->b[new_row] = orig->b[i];
            reduced->sense[new_row] = orig->sense[i];
            reduced->con_origin[new_row] = (orig->con_origin) ? orig->con_origin[i] : i;
            new_row++;
        }
    }
    reduced->con_origin_capacity = m_new;

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
    return presolve_with_mask(model, PRESOLVE_ALL);
}

PresolveResult* presolve_with_mask(LPModel *model, unsigned int technique_mask) {
    if (!model) return NULL;

    PresolveContext *ctx = presolve_context_create(model);
    if (!ctx) return NULL;

    ctx->technique_mask = technique_mask;

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

    unsigned int mask = ctx->technique_mask;

    while (changed && ctx->current_round < ctx->max_rounds && status >= 0) {
        changed = 0;
        ctx->current_round++;

        if (ctx->remove_fixed_vars && (mask & PRESOLVE_FIXED_VARS)) {
            int n = presolve_remove_fixed_vars(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;
        }

        if (ctx->remove_empty_rows && (mask & PRESOLVE_EMPTY_ROWS)) {
            int n = presolve_remove_empty_rows(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->cons_removed += n;
        }

        if (ctx->remove_empty_cols && (mask & PRESOLVE_EMPTY_COLS)) {
            int n = presolve_remove_empty_cols(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;
        }

        if (ctx->remove_singleton_rows && (mask & PRESOLVE_SINGLETON_ROWS)) {
            int n = presolve_singleton_rows(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->cons_removed += n;
        }

        if (ctx->remove_singleton_cols && (mask & PRESOLVE_SINGLETON_COLS)) {
            int n = presolve_singleton_cols(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->bounds_tightened += n;
        }

        if (mask & PRESOLVE_IMPLIED_FREE) {
            int n = presolve_implied_free(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->bounds_tightened += n;
        }

        if (mask & PRESOLVE_DOUBLETON_EQ) {
            int n = presolve_doubleton_equality(ctx, result);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;
            result->cons_removed += n;
        }

        if (ctx->remove_forcing_cons && (mask & PRESOLVE_FORCING)) {
            int n = presolve_forcing_constraints(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->cons_removed += n;
        }

        if (ctx->bound_tightening && (mask & PRESOLVE_BOUND_TIGHTENING)) {
            int n = presolve_bound_tightening(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->bounds_tightened += n;
        }

        if (mask & PRESOLVE_PROPORTIONAL_ROWS) {
            int n = presolve_proportional_rows(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->cons_removed += n;
        }

        if (mask & PRESOLVE_PROPORTIONAL_COLS) {
            int n = presolve_proportional_cols(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;
        }

        if (ctx->probing && model->num_binary > 0 && (mask & PRESOLVE_PROBING)) {
            int n = presolve_probing(ctx);
            if (n < 0) { status = -1; break; }
            changed += n;
            result->vars_removed += n;
        }
    }
    result->rounds = ctx->current_round;

    /* Shift variable bounds (one-time, after main loop stabilizes) */
    if (status >= 0 && (mask & PRESOLVE_SHIFT_BOUNDS)) {
        int n = presolve_shift_bounds(ctx, result);
        result->bounds_tightened += n;
    }

    /* Redundant row detection via rank computation */
    if (status >= 0 && ctx->detect_redundant_rows && (mask & PRESOLVE_REDUNDANT_ROWS)) {
        int n = presolve_detect_redundant_rows(ctx);
        if (n < 0) {
            status = -1;
        } else {
            result->cons_removed += n;
            result->matrix_rank = ctx->matrix_rank;
            result->redundant_rows_found = ctx->redundant_rows_found;
        }
    }

    if (status < 0) {
        /* Problem is infeasible */
        free(result);
        presolve_context_free(ctx);
        return NULL;
    }

    /* Record all fixed (deleted) variables for postsolve recovery.
     * Variables get fixed by remove_fixed_vars, remove_empty_cols,
     * proportional_cols (via remove_fixed_vars), etc. None of these
     * push postsolve ops, so we do a sweep here to ensure every fixed
     * variable's value is recoverable during postsolve.
     * Push POSTSOLVE_FIXED_VAR for each col_deleted variable with lb==ub.
     * These must be pushed AFTER shift_bounds so they're replayed BEFORE
     * shifts in the LIFO postsolve order.
     * IMPORTANT: Use ctx->working (not parameter 'model') since presolve
     * operations modify the working copy's bounds. */
    {
        LPModel *working = ctx->working;
        for (int j = 0; j < working->num_vars; j++) {
            if (ctx->col_deleted[j]) {
                if (fabs(working->lb[j] - working->ub[j]) < RALPH_ZERO_TOL) {
                    PostsolveOp op = {
                        .type = POSTSOLVE_FIXED_VAR,
                        .var = j,
                        .var2 = -1,
                        .value = working->lb[j],
                        .factor = 0.0,
                    };
                    if (postsolve_push(result, op) < 0) continue;
                }
            }
        }
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

/* Push an operation onto the postsolve stack */
static int postsolve_push(PresolveResult *result, PostsolveOp op) {
    if (result->num_postsolve_ops >= result->postsolve_capacity) {
        int new_cap = result->postsolve_capacity == 0 ? 32 : result->postsolve_capacity * 2;
        PostsolveOp *new_stack = (PostsolveOp*)realloc(
            result->postsolve_stack, (size_t)new_cap * sizeof(PostsolveOp));
        if (!new_stack) return -1;
        result->postsolve_stack = new_stack;
        result->postsolve_capacity = new_cap;
    }
    result->postsolve_stack[result->num_postsolve_ops++] = op;
    return 0;
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
    SAFE_FREE(result->postsolve_stack);
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

    /* Replay postsolve stack in reverse (LIFO) order */
    for (int k = result->num_postsolve_ops - 1; k >= 0; k--) {
        const PostsolveOp *op = &result->postsolve_stack[k];
        switch (op->type) {
            case POSTSOLVE_FIXED_VAR:
                original_solution[op->var] = op->value;
                break;
            case POSTSOLVE_SUBSTITUTION:
                /* x_elim = offset + factor * x_remain */
                original_solution[op->var] =
                    op->value + op->factor * original_solution[op->var2];
                break;
            case POSTSOLVE_SHIFT:
                /* x_orig = x_shifted + shift_amount */
                original_solution[op->var] += op->value;
                break;
        }
    }

    return 0;
}
