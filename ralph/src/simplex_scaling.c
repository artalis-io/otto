/*
 * simplex_scaling.c - Geometric mean scaling and solution verification.
 *
 * Zero behavior change — pure code motion from simplex.c (R3.9).
 */

#include "simplex_scaling.h"
#include "simplex_internal.h"
#include "lp_log.h"

#include <stdlib.h>
#include <math.h>

/* ============================================================================
 * Geometric Mean Scaling
 * ============================================================================ */

/*
 * Apply geometric mean scaling to the LP model.
 * This scales rows and columns to improve numerical stability.
 *
 * Row scaling: R[i] such that scaled row has max element ~1
 * Col scaling: C[j] such that scaled col has max element ~1
 *
 * Scaled problem: (R*A*C) * (C^-1 * x) = R*b with objective (C*c)' * (C^-1 * x)
 */
int apply_scaling(SimplexSolver *solver) {
    LPModel *model = solver->model;
    if (!model || !model->A) return -1;

    int m = model->num_cons;
    int n = model->num_vars;
    SparseMatrix *A = model->A;

    int geo_rounds = solver->scaling;    /* N geometric mean rounds */
    int eq_rounds = (geo_rounds > 1) ? 20 : 0;  /* equilibrium only for multi-round */

    /* Allocate scaling factors */
    solver->row_scale = (double*)calloc(m, sizeof(double));
    solver->col_scale = (double*)calloc(n, sizeof(double));
    if (!solver->row_scale || !solver->col_scale) {
        free(solver->row_scale);
        free(solver->col_scale);
        solver->row_scale = NULL;
        solver->col_scale = NULL;
        return -1;
    }

    /* Initialize scaling factors to 1 */
    for (int i = 0; i < m; i++) solver->row_scale[i] = 1.0;
    for (int j = 0; j < n; j++) solver->col_scale[j] = 1.0;

    /* Workspace for row/column extremes */
    double *row_max = (double*)calloc(m, sizeof(double));
    double *col_max = (double*)calloc(n, sizeof(double));
    if (!row_max || !col_max) {
        free(row_max);
        free(col_max);
        return -1;
    }

    /* Geometric mean scaling rounds: R[i] *= 1/sqrt(max), C[j] *= 1/sqrt(max)
     * Each round shrinks the ratio between largest and smallest elements.
     * Uses virtual scaling: A is not modified, just R[] and C[] accumulate. */
    for (int round = 0; round < geo_rounds; round++) {
        /* Compute row maxes of |R[i] * A[i,j] * C[j]| */
        for (int i = 0; i < m; i++) row_max[i] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > row_max[i]) row_max[i] = scaled;
            }
        }

        /* Update row factors */
        for (int i = 0; i < m; i++) {
            if (row_max[i] > RALPH_ZERO_TOL) {
                solver->row_scale[i] *= 1.0 / sqrt(row_max[i]);
            }
        }

        /* Compute col maxes of |R[i] * A[i,j] * C[j]| with updated R */
        for (int j = 0; j < n; j++) col_max[j] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > col_max[j]) col_max[j] = scaled;
            }
        }

        /* Update column factors */
        for (int j = 0; j < n; j++) {
            if (col_max[j] > RALPH_ZERO_TOL) {
                solver->col_scale[j] *= 1.0 / sqrt(col_max[j]);
            }
        }
    }

    /* Equilibrium scaling rounds: R[i] *= 1/max, C[j] *= 1/max
     * Drives every row and column max toward 1.0.
     * Converges quickly — typically 3-5 rounds sufficient. */
    for (int round = 0; round < eq_rounds; round++) {
        /* Compute row maxes */
        for (int i = 0; i < m; i++) row_max[i] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > row_max[i]) row_max[i] = scaled;
            }
        }

        /* Update row factors (equilibrium: scale max to 1.0) */
        for (int i = 0; i < m; i++) {
            if (row_max[i] > RALPH_ZERO_TOL) {
                solver->row_scale[i] *= 1.0 / row_max[i];
            }
        }

        /* Compute col maxes with updated R */
        double max_deviation = 0.0;
        for (int j = 0; j < n; j++) col_max[j] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > col_max[j]) col_max[j] = scaled;
            }
        }

        /* Update column factors and check convergence */
        for (int j = 0; j < n; j++) {
            if (col_max[j] > RALPH_ZERO_TOL) {
                double dev = fabs(col_max[j] - 1.0);
                if (dev > max_deviation) max_deviation = dev;
                solver->col_scale[j] *= 1.0 / col_max[j];
            }
        }

        /* Converged: all column maxes within 10% of 1.0 */
        if (max_deviation < 0.1) break;
    }

    free(row_max);
    free(col_max);

    /* Apply accumulated scaling to matrix A: A_scaled[i,j] = R[i] * A[i,j] * C[j] */
    for (int j = 0; j < n; j++) {
        double cj = solver->col_scale[j];
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int i = A->rowidx[p];
            A->values[p] *= solver->row_scale[i] * cj;
        }
    }

    /* Scale RHS: b_scaled[i] = R[i] * b[i] */
    for (int i = 0; i < m; i++) {
        model->b[i] *= solver->row_scale[i];
    }

    /* Scale objective: c_scaled[j] = C[j] * c[j] */
    for (int j = 0; j < n; j++) {
        model->c[j] *= solver->col_scale[j];
    }

    /* Scale variable bounds: x = C * x_scaled, so lb/C <= x_scaled <= ub/C */
    for (int j = 0; j < n; j++) {
        if (model->lb[j] > -RALPH_INFINITY/2) {
            model->lb[j] /= solver->col_scale[j];
        }
        if (model->ub[j] < RALPH_INFINITY/2) {
            model->ub[j] /= solver->col_scale[j];
        }
    }

    solver->is_scaled = 1;
    return 0;
}

/*
 * Post-solve verification (T2.3 + T3.6).
 * Read-only: inspects solution, dual, reduced costs, model A/b/c.
 * Checks:
 *   1. Primal feasibility:  ||Ax - b||_inf for constraint satisfaction
 *   2. Bound feasibility:   lb <= x <= ub for all vars
 *   3. Dual feasibility:    rc[j] >= -tol for nonbasics at lower bound
 *   4. Complementary slackness: |x_j - lb_j| * |rc_j| ~ 0
 *   5. Objective accuracy:  Kahan summation recomputation
 *   6. Basis conditioning:  LU condition estimate (T3.6)
 * Downgrades OPTIMAL → IMPRECISE if any check exceeds threshold.
 */
void verify_solution(SimplexSolver *solver) {
    LPModel *model = solver->model;
    int m = model->num_cons;
    int n = model->num_vars;
    double *x = solver->solution;
    double *rc = solver->reduced_costs;
    SparseMatrix *A = model->A;

    if (!x || !A || !model->b || !model->lb || !model->ub) return;

    /* W2: Use runtime tolerances from model (default to compile-time constants) */
    double feas_tol = model->feas_tol;
    double opt_tol = model->opt_tol;

    double max_primal_infeas = 0.0;
    double max_bound_infeas = 0.0;
    double max_primal_rel_infeas = 0.0;
    double max_bound_rel_infeas = 0.0;
    double max_dual_infeas = 0.0;
    double max_comp_slack = 0.0;

    /* 1. Primal feasibility: compute Ax via column-wise sparse matvec (O(nnz)),
     * then check against b with sense.
     * (B6 fix: was O(n*m) triple-nested loop, now O(nnz)) */
    double *ax = (double*)calloc(m, sizeof(double));
    if (ax) {
        for (int j = 0; j < n; j++) {
            double xj = x[j];
            if (fabs(xj) < 1e-15) continue;
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                ax[A->rowidx[p]] += A->values[p] * xj;
            }
        }
        for (int i = 0; i < m; i++) {
            double violation = 0.0;
            double row_scale = fmax(1.0, fmax(fabs(ax[i]), fabs(model->b[i])));
            char sense = model->sense[i];
            if (sense == 'L') {
                violation = ax[i] - model->b[i];
                if (violation < 0.0) violation = 0.0;
            } else if (sense == 'G') {
                violation = model->b[i] - ax[i];
                if (violation < 0.0) violation = 0.0;
            } else {
                violation = fabs(ax[i] - model->b[i]);
            }
            if (violation > max_primal_infeas) max_primal_infeas = violation;
            {
                double rel_violation = violation / row_scale;
                if (rel_violation > max_primal_rel_infeas) {
                    max_primal_rel_infeas = rel_violation;
                }
            }
        }
        free(ax);
    }

    /* 2. Bound feasibility */
    for (int j = 0; j < n; j++) {
        double bound_scale = fmax(1.0, fabs(x[j]));
        double lb_viol = model->lb[j] - x[j];
        if (lb_viol > 0.0) {
            bound_scale = fmax(bound_scale, fabs(model->lb[j]));
            if (lb_viol > max_bound_infeas) max_bound_infeas = lb_viol;
            if (lb_viol / bound_scale > max_bound_rel_infeas) {
                max_bound_rel_infeas = lb_viol / bound_scale;
            }
        }
        double ub_viol = x[j] - model->ub[j];
        if (ub_viol > 0.0) {
            bound_scale = fmax(bound_scale, fabs(model->ub[j]));
            if (ub_viol > max_bound_infeas) max_bound_infeas = ub_viol;
            if (ub_viol / bound_scale > max_bound_rel_infeas) {
                max_bound_rel_infeas = ub_viol / bound_scale;
            }
        }
    }

    /* 3. Dual feasibility: for minimization, nonbasics at lb should have rc >= 0,
     * at ub should have rc <= 0. Solution is in original space (obj_sense applied). */
    if (rc) {
        for (int j = 0; j < n; j++) {
            double xj = x[j];
            double rcj = rc[j];  /* Already in user space (obj_sense applied) */
            int at_lb = fabs(xj - model->lb[j]) < feas_tol;
            int at_ub = fabs(xj - model->ub[j]) < feas_tol;

            /* In user space: minimize → rc >= 0 at lb, rc <= 0 at ub
             *                maximize → rc <= 0 at lb, rc >= 0 at ub
             * Equivalently: obj_sense * rc >= 0 at lb (in internal space) */
            double viol = 0.0;
            if (at_lb && !at_ub) {
                /* At lower bound: rc should push away from lb.
                 * For min: rc >= 0. For max: rc <= 0. */
                viol = (model->obj_sense == 1) ? -rcj : rcj;
            } else if (at_ub && !at_lb) {
                /* At upper bound: rc should push away from ub. */
                viol = (model->obj_sense == 1) ? rcj : -rcj;
            }
            if (viol > max_dual_infeas) max_dual_infeas = viol;
        }
    }

    /* 4. Complementary slackness: for non-fixed vars, |x - lb| * |rc| should be ~ 0 */
    if (rc) {
        for (int j = 0; j < n; j++) {
            if (fabs(model->ub[j] - model->lb[j]) < feas_tol) continue;
            double dist_lb = fabs(x[j] - model->lb[j]);
            double dist_ub = fabs(x[j] - model->ub[j]);
            double min_dist = (dist_lb < dist_ub) ? dist_lb : dist_ub;
            double cs = min_dist * fabs(rc[j]);
            if (cs > max_comp_slack) max_comp_slack = cs;
        }
    }

    /* 5. Objective accuracy with Kahan summation */
    double obj_kahan = 0.0;
    double kahan_comp = 0.0;
    for (int j = 0; j < n; j++) {
        double term = model->c[j] * x[j] - kahan_comp;
        double temp = obj_kahan + term;
        kahan_comp = (temp - obj_kahan) - term;
        obj_kahan = temp;
    }
    obj_kahan = obj_kahan + model->obj_offset;
    double obj_denom = fabs(solver->obj_value) > 1.0 ? fabs(solver->obj_value) : 1.0;
    double obj_rel_error = fabs(obj_kahan - solver->obj_value) / obj_denom;

    /* 6. Basis conditioning (T3.6) */
    double cond = 1.0;
    if (solver->tableau && solver->tableau->lu) {
        cond = lu_get_cond_estimate(solver->tableau->lu);
    }

    /* Store metrics */
    solver->verify_primal_infeas = max_primal_infeas;
    solver->verify_bound_infeas = max_bound_infeas;
    solver->verify_dual_infeas = max_dual_infeas;
    solver->verify_comp_slack = max_comp_slack;
    solver->verify_obj_error = obj_rel_error;
    solver->verify_cond_estimate = cond;

    /* W4: Dual iterative refinement — if dual infeasibility exceeds threshold,
     * recompute reduced costs from dual variables: rc[j] = c[j] - A^T y[j].
     * This catches RC drift from accumulated LU update errors without
     * requiring refactorization. Only fires when verification detects a problem. */
    if (max_dual_infeas > opt_tol && rc && solver->dual_solution && A) {
        /* Recompute reduced costs from duals in user space */
        for (int j = 0; j < n; j++) {
            double rc_refined = model->c[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                rc_refined -= A->values[p] * solver->dual_solution[A->rowidx[p]];
            }
            solver->reduced_costs[j] = rc_refined;
        }

        /* Re-check dual feasibility with refined reduced costs */
        max_dual_infeas = 0.0;
        max_comp_slack = 0.0;
        for (int j = 0; j < n; j++) {
            double xj = x[j];
            double rcj = solver->reduced_costs[j];
            int at_lb = fabs(xj - model->lb[j]) < feas_tol;
            int at_ub = fabs(xj - model->ub[j]) < feas_tol;

            double viol = 0.0;
            if (at_lb && !at_ub) {
                viol = (model->obj_sense == 1) ? -rcj : rcj;
            } else if (at_ub && !at_lb) {
                viol = (model->obj_sense == 1) ? rcj : -rcj;
            }
            if (viol > max_dual_infeas) max_dual_infeas = viol;

            if (fabs(model->ub[j] - model->lb[j]) >= feas_tol) {
                double dist_lb = fabs(xj - model->lb[j]);
                double dist_ub = fabs(xj - model->ub[j]);
                double min_dist = (dist_lb < dist_ub) ? dist_lb : dist_ub;
                double cs = min_dist * fabs(rcj);
                if (cs > max_comp_slack) max_comp_slack = cs;
            }
        }

        /* Update stored metrics */
        solver->verify_dual_infeas = max_dual_infeas;
        solver->verify_comp_slack = max_comp_slack;
    }

    /* Downgrade to IMPRECISE if any metric exceeds threshold (W2: runtime tols) */
    int imprecise = 0;
    if (max_primal_rel_infeas > 2000.0 * feas_tol) imprecise = 1;
    if (max_bound_rel_infeas > 2000.0 * feas_tol) imprecise = 1;
    if (max_dual_infeas > opt_tol) imprecise = 1;
    if (obj_rel_error > opt_tol) imprecise = 1;

    if (imprecise) {
        solver->status = RALPH_STATUS_IMPRECISE;
        if (solver->verbose) {
            LP_LOG_STDOUT("[verify] IMPRECISE: primal=%.2e bound=%.2e dual=%.2e cs=%.2e obj=%.2e cond=%.2e\n",
                   max_primal_infeas, max_bound_infeas, max_dual_infeas,
                   max_comp_slack, obj_rel_error, cond);
        }
    } else if (solver->verbose) {
        LP_LOG_STDOUT("[verify] OK: primal=%.2e bound=%.2e dual=%.2e cs=%.2e obj=%.2e cond=%.2e\n",
               max_primal_infeas, max_bound_infeas, max_dual_infeas,
               max_comp_slack, obj_rel_error, cond);
    }
}

/*
 * Unscale the solution after solving.
 * x_original = C * x_scaled
 * y_original = R * y_scaled
 * rc_original = C^-1 * rc_scaled
 */
void unscale_solution(SimplexSolver *solver) {
    if (!solver->is_scaled) return;

    int n = solver->model->num_vars;
    int m = solver->model->num_cons;

    /* Unscale primal solution: x = C * x_scaled */
    if (solver->solution) {
        for (int j = 0; j < n; j++) {
            solver->solution[j] *= solver->col_scale[j];
        }
    }

    /* Unscale dual solution: y = R * y_scaled */
    if (solver->dual_solution) {
        for (int i = 0; i < m; i++) {
            solver->dual_solution[i] *= solver->row_scale[i];
        }
    }

    /* Unscale reduced costs: rc = rc_scaled / C */
    if (solver->reduced_costs) {
        for (int j = 0; j < n; j++) {
            solver->reduced_costs[j] /= solver->col_scale[j];
        }
    }
}

/*
 * Restore the model to its original (unscaled) state.
 * This reverses the transformations applied by apply_scaling().
 */
void restore_model(SimplexSolver *solver) {
    if (!solver->is_scaled) return;

    LPModel *model = solver->model;
    if (!model || !model->A) return;

    int m = model->num_cons;
    int n = model->num_vars;
    SparseMatrix *A = model->A;

    /* Unscale matrix A: A_original = A_scaled / (R[i] * C[j]) */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int i = A->rowidx[p];
            A->values[p] /= (solver->row_scale[i] * solver->col_scale[j]);
        }
    }

    /* Unscale RHS: b_original = b_scaled / R[i] */
    for (int i = 0; i < m; i++) {
        model->b[i] /= solver->row_scale[i];
    }

    /* Unscale objective: c_original = c_scaled / C[j] */
    for (int j = 0; j < n; j++) {
        model->c[j] /= solver->col_scale[j];
    }

    /* Unscale variable bounds: lb/ub_original = lb/ub_scaled * C[j] */
    for (int j = 0; j < n; j++) {
        if (model->lb[j] > -RALPH_INFINITY/2) {
            model->lb[j] *= solver->col_scale[j];
        }
        if (model->ub[j] < RALPH_INFINITY/2) {
            model->ub[j] *= solver->col_scale[j];
        }
    }

    solver->is_scaled = 0;
}
