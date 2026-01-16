/*
 * Ralph - Revised Simplex Method Implementation
 *
 * Implements the primal revised simplex algorithm with:
 * - Steepest edge / Devex pricing
 * - Harris ratio test
 * - Bound flipping
 * - Anti-cycling via perturbation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "lp.h"

/* Forward declarations */
int lp_model_finalize(LPModel *model);

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
static int apply_scaling(SimplexSolver *solver) {
    LPModel *model = solver->model;
    if (!model || !model->A) return -1;

    int m = model->num_cons;
    int n = model->num_vars;
    SparseMatrix *A = model->A;

    /* Allocate scaling factors */
    solver->row_scale = (double*)malloc(m * sizeof(double));
    solver->col_scale = (double*)malloc(n * sizeof(double));
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

    /* Compute row max absolute values */
    double *row_max = (double*)calloc(m, sizeof(double));
    if (!row_max) return -1;

    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int i = A->rowidx[p];
            double absval = fabs(A->values[p]);
            if (absval > row_max[i]) row_max[i] = absval;
        }
    }

    /* Compute row scaling factors */
    for (int i = 0; i < m; i++) {
        if (row_max[i] > RALPH_ZERO_TOL) {
            solver->row_scale[i] = 1.0 / sqrt(row_max[i]);
        }
    }
    free(row_max);

    /* Apply row scaling to matrix, then compute column max */
    double *col_max = (double*)calloc(n, sizeof(double));
    if (!col_max) return -1;

    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int i = A->rowidx[p];
            double scaled_val = fabs(A->values[p]) * solver->row_scale[i];
            if (scaled_val > col_max[j]) col_max[j] = scaled_val;
        }
    }

    /* Compute column scaling factors */
    for (int j = 0; j < n; j++) {
        if (col_max[j] > RALPH_ZERO_TOL) {
            solver->col_scale[j] = 1.0 / sqrt(col_max[j]);
        }
    }
    free(col_max);

    /* Apply scaling to matrix A: A_scaled[i,j] = R[i] * A[i,j] * C[j] */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int i = A->rowidx[p];
            A->values[p] *= solver->row_scale[i] * solver->col_scale[j];
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

    /* Scale variable bounds: x_scaled = C^-1 * x, so bounds scale by C */
    /* lb_scaled[j] = lb[j] / C[j], but we store C[j] and apply later */
    /* Actually for bounds: if x = C * x_scaled, then lb <= C * x_scaled <= ub */
    /* So: lb/C <= x_scaled <= ub/C */
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
 * Unscale the solution after solving.
 * x_original = C * x_scaled
 * y_original = R * y_scaled
 * rc_original = C^-1 * rc_scaled
 */
static void unscale_solution(SimplexSolver *solver) {
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
static void restore_model(SimplexSolver *solver) {
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

/* ============================================================================
 * Simplex Tableau Creation
 * ============================================================================ */

SimplexTableau* tableau_create(LPModel *model) {
    if (!model) return NULL;

    /* Finalize model if not done */
    if (!model->A) {
        if (lp_model_finalize(model) != 0) return NULL;
    }

    SimplexTableau *tab = (SimplexTableau*)calloc(1, sizeof(SimplexTableau));
    if (!tab) return NULL;

    tab->model = model;
    tab->m = model->num_cons;

    /* Normalize constraint senses and RHS signs for each row:
     * - If RHS < 0, multiply entire row by -1 and flip sense (L<->G, E stays E)
     * Then count variables needed:
     * - <= : 1 slack (basic)
     * - >= : 1 surplus + 1 artificial (artificial basic)
     * - =  : 1 artificial (artificial basic)
     */
    char *norm_sense = (char*)malloc(model->num_cons);
    double *norm_sign = (double*)malloc(model->num_cons * sizeof(double));
    if (!norm_sense || !norm_sign) {
        free(norm_sense);
        free(norm_sign);
        tableau_free(tab);
        return NULL;
    }

    int num_aux_vars = 0;  /* Count slack + surplus + artificial */
    for (int i = 0; i < model->num_cons; i++) {
        /* Normalize so RHS >= 0 */
        if (model->b[i] < 0) {
            norm_sign[i] = -1.0;
            if (model->sense[i] == 'L') {
                norm_sense[i] = 'G';  /* <= with negative RHS becomes >= */
            } else if (model->sense[i] == 'G') {
                norm_sense[i] = 'L';  /* >= with negative RHS becomes <= */
            } else {
                norm_sense[i] = 'E';  /* = stays = */
            }
        } else {
            norm_sign[i] = 1.0;
            norm_sense[i] = model->sense[i];
        }

        /* Count auxiliary variables needed */
        if (norm_sense[i] == 'L') {
            num_aux_vars += 1;  /* slack only */
        } else if (norm_sense[i] == 'G') {
            num_aux_vars += 2;  /* surplus + artificial */
        } else {
            num_aux_vars += 1;  /* artificial only */
        }
    }

    tab->n = model->num_vars + num_aux_vars;

    /* Allocate extended arrays */
    tab->c_ext = (double*)calloc(tab->n, sizeof(double));
    tab->lb_ext = (double*)calloc(tab->n, sizeof(double));
    tab->ub_ext = (double*)calloc(tab->n, sizeof(double));

    tab->basis = (int*)malloc(tab->m * sizeof(int));
    tab->nonbasis = (int*)malloc((tab->n - tab->m) * sizeof(int));
    tab->var_status = (VarStatus*)malloc(tab->n * sizeof(VarStatus));
    tab->basis_pos = (int*)malloc(tab->n * sizeof(int));

    tab->x = (double*)calloc(tab->n, sizeof(double));
    tab->y = (double*)calloc(tab->m, sizeof(double));
    tab->rc = (double*)calloc(tab->n, sizeof(double));

    tab->work1 = (double*)calloc(tab->m, sizeof(double));
    tab->work2 = (double*)calloc(tab->m, sizeof(double));
    tab->work3 = (double*)calloc(tab->n, sizeof(double));
    tab->rhs = (double*)calloc(tab->m, sizeof(double));

    tab->se_weights = (double*)calloc(tab->n, sizeof(double));

    if (!tab->c_ext || !tab->lb_ext || !tab->ub_ext ||
        !tab->basis || !tab->nonbasis || !tab->var_status || !tab->basis_pos ||
        !tab->x || !tab->y || !tab->rc ||
        !tab->work1 || !tab->work2 || !tab->work3 || !tab->rhs ||
        !tab->se_weights) {
        free(norm_sense);
        free(norm_sign);
        tableau_free(tab);
        return NULL;
    }

    /* Copy structural variable data */
    for (int j = 0; j < model->num_vars; j++) {
        tab->c_ext[j] = model->c[j] * model->obj_sense;  /* Convert to minimization */
        tab->lb_ext[j] = model->lb[j];
        tab->ub_ext[j] = model->ub[j];
    }

    /* Build extended constraint matrix with slacks/surplus/artificial */
    SparseTriplets *trips = triplets_create(tab->m, tab->n,
                                            model->A->nnz + num_aux_vars);
    if (!trips) {
        free(norm_sense);
        free(norm_sign);
        tableau_free(tab);
        return NULL;
    }

    /* Copy original matrix (with row sign normalization) */
    for (int j = 0; j < model->num_vars; j++) {
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int row = model->A->rowidx[p];
            double val = model->A->values[p] * norm_sign[row];
            triplets_add(trips, row, j, val);
        }
    }

    /* Track which auxiliary variable is basic for each row */
    int *basic_var_for_row = (int*)malloc(model->num_cons * sizeof(int));
    if (!basic_var_for_row) {
        free(norm_sense);
        free(norm_sign);
        triplets_free(trips);
        tableau_free(tab);
        return NULL;
    }

    /* Add auxiliary variables */
    int aux_idx = model->num_vars;
    for (int i = 0; i < model->num_cons; i++) {
        if (norm_sense[i] == 'L') {
            /* <= : add slack with coef +1, slack is basic */
            triplets_add(trips, i, aux_idx, 1.0);
            tab->c_ext[aux_idx] = 0.0;
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;
            aux_idx++;
        } else if (norm_sense[i] == 'G') {
            /* >= : add surplus with coef -1, then artificial with coef +1 */
            /* Surplus variable (not basic) */
            triplets_add(trips, i, aux_idx, -1.0);
            tab->c_ext[aux_idx] = 0.0;
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            aux_idx++;
            /* Artificial variable (basic) */
            triplets_add(trips, i, aux_idx, 1.0);
            tab->c_ext[aux_idx] = 1e8;  /* Big-M cost */
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;
            aux_idx++;
        } else {
            /* = : add artificial with coef +1 (basic) */
            triplets_add(trips, i, aux_idx, 1.0);
            tab->c_ext[aux_idx] = 1e8;  /* Big-M cost */
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;
            aux_idx++;
        }
    }

    tab->A_ext = triplets_to_csc(trips);
    triplets_free(trips);

    if (!tab->A_ext) {
        free(norm_sense);
        free(norm_sign);
        free(basic_var_for_row);
        tableau_free(tab);
        return NULL;
    }

    /* Set up RHS (with sign normalization) */
    for (int i = 0; i < model->num_cons; i++) {
        tab->rhs[i] = fabs(model->b[i]);  /* Already normalized to be non-negative */
    }

    /* Initialize basis using basic_var_for_row */
    /* First set all variables as nonbasic at their lower bounds */
    for (int j = 0; j < tab->n; j++) {
        tab->basis_pos[j] = -1;
        tab->var_status[j] = RALPH_NONBASIC_LOWER;
        tab->x[j] = tab->lb_ext[j];
    }

    /* Mark basic variables */
    for (int i = 0; i < model->num_cons; i++) {
        int bv = basic_var_for_row[i];
        tab->basis[i] = bv;
        tab->basis_pos[bv] = i;
        tab->var_status[bv] = RALPH_BASIC;
    }

    /* Compute initial basic variable values: x_B = b - A_N * x_N
     * Since initial basis is slacks/artificials with B = I, we have x_B directly.
     * But we need to subtract the contribution from nonbasic vars at non-zero bounds.
     */
    for (int i = 0; i < model->num_cons; i++) {
        int bv = basic_var_for_row[i];
        double val = tab->rhs[i];

        /* Subtract A[i,j] * x[j] for nonbasic variables j */
        for (int j = 0; j < model->num_vars; j++) {
            if (tab->var_status[j] != RALPH_BASIC && fabs(tab->x[j]) > RALPH_ZERO_TOL) {
                /* Get A[i,j] from sparse matrix */
                for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                    if (tab->A_ext->rowidx[p] == i) {
                        val -= tab->A_ext->values[p] * tab->x[j];
                        break;
                    }
                }
            }
        }
        tab->x[bv] = val;
    }

    /* Initialize steepest edge weights */
    for (int j = 0; j < tab->n; j++) {
        tab->se_weights[j] = 1.0;
    }
    tab->use_steepest_edge = 1;

    /* Create LU factorization */
    tab->lu = lu_create(tab->m);
    if (!tab->lu) {
        free(norm_sense);
        free(norm_sign);
        free(basic_var_for_row);
        tableau_free(tab);
        return NULL;
    }

    free(norm_sense);
    free(norm_sign);
    free(basic_var_for_row);
    return tab;
}

void tableau_free(SimplexTableau *tab) {
    if (!tab) return;

    sparse_free(tab->A_ext);
    free(tab->c_ext);
    free(tab->lb_ext);
    free(tab->ub_ext);
    free(tab->basis);
    free(tab->nonbasis);
    free(tab->var_status);
    free(tab->basis_pos);
    free(tab->x);
    free(tab->y);
    free(tab->rc);
    free(tab->work1);
    free(tab->work2);
    free(tab->work3);
    free(tab->rhs);
    free(tab->se_weights);
    lu_free(tab->lu);
    free(tab);
}

/* ============================================================================
 * Basis Management
 * ============================================================================ */

/* Initialize a crash basis (slack variables basic) */
static int initialize_slack_basis(SimplexTableau *tab) {
    int m = tab->m;
    int n = tab->n;
    int num_struct = tab->model->num_vars;

    /* Put slacks in basis first */
    int basis_idx = 0;
    int nonbasis_idx = 0;

    for (int j = 0; j < n; j++) {
        tab->basis_pos[j] = -1;
    }

    /* Slacks are basic */
    for (int j = num_struct; j < n && basis_idx < m; j++) {
        tab->basis[basis_idx] = j;
        tab->var_status[j] = RALPH_BASIC;
        tab->basis_pos[j] = basis_idx;
        basis_idx++;
    }

    /* Structural variables are non-basic */
    for (int j = 0; j < num_struct; j++) {
        if (tab->lb_ext[j] > -RALPH_INFINITY/2) {
            tab->var_status[j] = RALPH_NONBASIC_LOWER;
            tab->x[j] = tab->lb_ext[j];
        } else if (tab->ub_ext[j] < RALPH_INFINITY/2) {
            tab->var_status[j] = RALPH_NONBASIC_UPPER;
            tab->x[j] = tab->ub_ext[j];
        } else {
            tab->var_status[j] = RALPH_NONBASIC_FREE;
            tab->x[j] = 0.0;
        }
        tab->nonbasis[nonbasis_idx++] = j;
    }

    /* Need to add artificial variables for equality constraints */
    /* For now, use Big-M or two-phase if slack basis insufficient */

    return 0;
}

/* Build basis matrix from current basis */
static SparseMatrix* build_basis_matrix(SimplexTableau *tab) {
    return sparse_get_columns(tab->A_ext, tab->m, tab->basis);
}

int tableau_refactorize(SimplexTableau *tab) {
    SparseMatrix *B = build_basis_matrix(tab);
    if (!B) return -1;

    int status = lu_factorize(tab->lu, B);
    sparse_free(B);

    return status;
}

/* ============================================================================
 * Solution Computation
 * ============================================================================ */

int tableau_compute_solution(SimplexTableau *tab) {
    /* Compute x_B = B^{-1} * (b - N*x_N) */

    /* First compute b - N*x_N */
    vec_copy_data(tab->work1, tab->rhs, tab->m);

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] != RALPH_BASIC) {
            double xj = tab->x[j];
            if (fabs(xj) > RALPH_ZERO_TOL) {
                sparse_axpy_column(tab->A_ext, j, -xj, tab->work1);
            }
        }
    }

    /* Save original RHS for iterative refinement */
    double *orig_rhs = (double*)malloc(tab->m * sizeof(double));
    if (orig_rhs) {
        vec_copy_data(orig_rhs, tab->work1, tab->m);
    }

    /* Solve B * x_B = work1 */
    lu_solve(tab->lu, tab->work1, tab->work2);

    /* Update basic variable values */
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] = tab->work2[k];
    }

    /* Iterative refinement: check residual and correct if needed */
    if (orig_rhs) {
        /* Compute residual: r = b - B*x_B */
        /* work3 will hold B*x_B */
        vec_set_zero(tab->work3, tab->m);
        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            sparse_axpy_column(tab->A_ext, j, tab->x[j], tab->work3);
        }

        /* work1 = original_rhs - B*x_B = residual */
        double max_residual = 0.0;
        for (int i = 0; i < tab->m; i++) {
            tab->work1[i] = orig_rhs[i] - tab->work3[i];
            double absval = fabs(tab->work1[i]);
            if (absval > max_residual) max_residual = absval;
        }

        /* If residual is large, do iterative refinement */
        int max_refine_iters = 5;
        for (int refine_iter = 0; refine_iter < max_refine_iters && max_residual > RALPH_FEAS_TOL; refine_iter++) {
            /* Solve B * correction = residual */
            lu_solve(tab->lu, tab->work1, tab->work2);

            /* Update solution: x_B += correction */
            for (int k = 0; k < tab->m; k++) {
                tab->x[tab->basis[k]] += tab->work2[k];
            }

            /* Recompute residual */
            vec_set_zero(tab->work3, tab->m);
            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                sparse_axpy_column(tab->A_ext, j, tab->x[j], tab->work3);
            }
            max_residual = 0.0;
            for (int i = 0; i < tab->m; i++) {
                tab->work1[i] = orig_rhs[i] - tab->work3[i];
                double absval = fabs(tab->work1[i]);
                if (absval > max_residual) max_residual = absval;
            }
        }

        free(orig_rhs);
    }

    /* Compute objective value */
    tab->obj_value = 0.0;
    for (int j = 0; j < tab->n; j++) {
        tab->obj_value += tab->c_ext[j] * tab->x[j];
    }

    return 0;
}

int tableau_compute_reduced_costs(SimplexTableau *tab) {
    /* Compute dual values: y = B^{-T} * c_B */
    vec_set_zero(tab->work1, tab->m);
    for (int k = 0; k < tab->m; k++) {
        tab->work1[k] = tab->c_ext[tab->basis[k]];
    }

    lu_solve_transpose(tab->lu, tab->work1, tab->y);

    /* Compute reduced costs: rc_j = c_j - y' * A_j */
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) {
            tab->rc[j] = 0.0;
        } else {
            sparse_get_column(tab->A_ext, j, tab->work1);
            tab->rc[j] = tab->c_ext[j] - vec_dot(tab->m, tab->y, tab->work1);
        }
    }

    return 0;
}

/* ============================================================================
 * Pricing (Entering Variable Selection)
 * ============================================================================ */

int pricing_dantzig(SimplexTableau *tab, int *entering) {
    /* Standard Dantzig pricing: most negative reduced cost */
    double best_rc = -RALPH_OPT_TOL;
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        double rc = tab->rc[j];

        /* Check if this variable can improve */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < best_rc) {
            best_rc = rc;
            *entering = j;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && -rc < best_rc) {
            best_rc = -rc;
            *entering = j;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > -best_rc) {
            best_rc = -fabs(rc);
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;  /* 1 = optimal */
}

/* Bland's rule pricing: choose smallest index among eligible variables.
 * Used as fallback when cycling is detected. */
int pricing_bland(SimplexTableau *tab, int *entering) {
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        double rc = tab->rc[j];

        /* Check if this variable can improve */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            *entering = j;
            return 0;  /* Return first eligible */
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
    }

    return 1;  /* 1 = optimal */
}

int pricing_steepest_edge(SimplexTableau *tab, int *entering) {
    /* Steepest edge pricing: max |rc_j| / sqrt(gamma_j) */
    double best_ratio = RALPH_OPT_TOL;
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        double rc = tab->rc[j];
        double weight = tab->se_weights[j];
        if (weight < 1e-10) weight = 1.0;

        double ratio = 0.0;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            ratio = (-rc) / sqrt(weight);
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            ratio = rc / sqrt(weight);
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            ratio = fabs(rc) / sqrt(weight);
        }

        if (ratio > best_ratio) {
            best_ratio = ratio;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;
}

/* ============================================================================
 * Ratio Test (Leaving Variable Selection)
 * ============================================================================ */

/* Bland's ratio test: among ties, choose smallest index leaving variable */
int ratio_test_bland(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    lu_solve(tab->lu, tab->work1, tab->work2);  /* d = B^{-1} * a_entering */

    double dir = 1.0;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    *leaving = -1;
    *theta = RALPH_INFINITY;
    int leaving_var = tab->n;  /* Track actual variable index for Bland's tie-breaking */

    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        int j = tab->basis[k];
        double xj = tab->x[j];

        double ratio = RALPH_INFINITY;
        if (dk > RALPH_PIVOT_TOL) {
            ratio = (xj - tab->lb_ext[j]) / dk;
        } else if (dk < -RALPH_PIVOT_TOL) {
            ratio = (tab->ub_ext[j] - xj) / (-dk);
        }

        if (ratio < *theta - RALPH_FEAS_TOL) {
            *theta = ratio;
            *leaving = k;
            leaving_var = j;
        } else if (fabs(ratio - *theta) <= RALPH_FEAS_TOL && j < leaving_var) {
            /* Bland's rule: among ties, choose smallest variable index */
            *leaving = k;
            leaving_var = j;
        }
    }

    /* Check bound flip */
    double enter_range = tab->ub_ext[entering] - tab->lb_ext[entering];
    if (enter_range < *theta && enter_range < RALPH_INFINITY/2) {
        *theta = enter_range;
        *leaving = -2;
    }

    if (*theta >= RALPH_INFINITY/2) {
        return -1;  /* Unbounded */
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

int ratio_test_harris(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    lu_solve(tab->lu, tab->work1, tab->work2);  /* d = B^{-1} * a_entering */

    double dir = 1.0;  /* Direction of movement */
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    /* Harris ratio test with tolerance */
    double theta_max = RALPH_INFINITY;
    *leaving = -1;

    /* Phase 1: Find theta_max allowing small infeasibility */
    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        int j = tab->basis[k];
        double xj = tab->x[j];

        if (dk > RALPH_PIVOT_TOL) {
            /* Variable will decrease */
            double bound = tab->lb_ext[j];
            double ratio = (xj - bound + RALPH_FEAS_TOL) / dk;
            if (ratio < theta_max) theta_max = ratio;
        } else if (dk < -RALPH_PIVOT_TOL) {
            /* Variable will increase */
            double bound = tab->ub_ext[j];
            double ratio = (bound - xj + RALPH_FEAS_TOL) / (-dk);
            if (ratio < theta_max) theta_max = ratio;
        }
    }

    /* Check bound on entering variable */
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        double range = tab->ub_ext[entering] - tab->lb_ext[entering];
        if (range < theta_max) theta_max = range;
    } else if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        double range = tab->ub_ext[entering] - tab->lb_ext[entering];
        if (range < theta_max) theta_max = range;
    }

    if (theta_max >= RALPH_INFINITY/2) {
        *theta = RALPH_INFINITY;
        return -1;  /* Unbounded */
    }

    /* Phase 2: Among those within theta_max, pick largest pivot */
    double best_pivot = 0.0;

    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        int j = tab->basis[k];
        double xj = tab->x[j];

        double ratio = RALPH_INFINITY;
        if (dk > RALPH_PIVOT_TOL) {
            ratio = (xj - tab->lb_ext[j]) / dk;
        } else if (dk < -RALPH_PIVOT_TOL) {
            ratio = (tab->ub_ext[j] - xj) / (-dk);
        }

        if (ratio <= theta_max + RALPH_FEAS_TOL) {
            if (fabs(dk) > best_pivot) {
                best_pivot = fabs(dk);
                *leaving = k;
                *theta = ratio > 0 ? ratio : 0;
            }
        }
    }

    /* Check if entering variable hits its bound (bound flip) */
    double enter_range = tab->ub_ext[entering] - tab->lb_ext[entering];
    if (enter_range <= theta_max && enter_range < RALPH_INFINITY/2) {
        /* Bound flip might be better */
        if (*leaving < 0 || enter_range < *theta) {
            *theta = enter_range;
            *leaving = -2;  /* Special flag for bound flip */
        }
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

/* ============================================================================
 * Simplex Iteration
 * ============================================================================ */

static int simplex_pivot(SimplexTableau *tab, int entering, int leaving_pos, double theta) {
    double dir = (tab->var_status[entering] == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;

    /* Update entering variable */
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        tab->x[entering] += theta;
    } else {
        tab->x[entering] -= theta;
    }

    /* Update basic variables */
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] -= theta * dir * tab->work2[k];
    }

    if (leaving_pos == -2) {
        /* Bound flip: entering variable goes to opposite bound */
        if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
            tab->var_status[entering] = RALPH_NONBASIC_UPPER;
            tab->x[entering] = tab->ub_ext[entering];
        } else {
            tab->var_status[entering] = RALPH_NONBASIC_LOWER;
            tab->x[entering] = tab->lb_ext[entering];
        }
        return 0;
    }

    /* Normal pivot: swap entering and leaving */
    int leaving = tab->basis[leaving_pos];

    /* Update basis */
    tab->basis[leaving_pos] = entering;
    tab->basis_pos[entering] = leaving_pos;
    tab->basis_pos[leaving] = -1;

    /* Update variable status */
    tab->var_status[entering] = RALPH_BASIC;

    /* Leaving goes to appropriate bound */
    if (tab->work2[leaving_pos] * dir > 0) {
        tab->var_status[leaving] = RALPH_NONBASIC_LOWER;
        tab->x[leaving] = tab->lb_ext[leaving];
    } else {
        tab->var_status[leaving] = RALPH_NONBASIC_UPPER;
        tab->x[leaving] = tab->ub_ext[leaving];
    }

    /* Update LU factorization */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    if (lu_update(tab->lu, leaving_pos, tab->work1) != 0) {
        /* Update failed, refactorize */
        if (tableau_refactorize(tab) != 0) {
            return -1;
        }
    }

    /* Update Devex pricing weights
     * Devex is an approximation to steepest edge that only requires
     * the pivot column in basis representation (already in work2).
     *
     * Reference: Harris, "Pivot Selection Methods of the Devex LP Code", 1973
     *
     * For each nonbasic variable j:
     *   gamma_j = max(gamma_j, (alpha_j² * gamma_e) / pivot²)
     * where alpha_j is the j-th component of B^{-1} * a_j
     *
     * We approximate alpha_j using the pivot row of the tableau.
     */
    if (tab->use_steepest_edge) {
        double pivot = tab->work2[leaving_pos];
        double pivot_sq = pivot * pivot;
        double gamma_e = tab->se_weights[entering];

        if (fabs(pivot_sq) > RALPH_ZERO_TOL) {
            /* Compute pivot row for Devex update: row = e_r^T * B^{-1} * A
             * This is the leaving_pos-th row of B^{-1} * A
             * We only need it for nonbasic variables
             */
            vec_set_zero(tab->work1, tab->m);
            tab->work1[leaving_pos] = 1.0;
            lu_solve_transpose(tab->lu, tab->work1, tab->work3);  /* work3 = (B^{-T} * e_r) */

            for (int j = 0; j < tab->n; j++) {
                if (j != entering && tab->var_status[j] != RALPH_BASIC) {
                    /* Compute alpha_j = pivot_row * a_j */
                    double alpha_j = 0.0;
                    for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                        alpha_j += tab->work3[tab->A_ext->rowidx[p]] * tab->A_ext->values[p];
                    }

                    /* Devex update: only increase weights */
                    double new_weight = (alpha_j * alpha_j * gamma_e) / pivot_sq;
                    if (new_weight > tab->se_weights[j]) {
                        tab->se_weights[j] = new_weight;
                    }
                }
            }
        }

        /* Weight for leaving variable */
        tab->se_weights[leaving] = fabs(pivot_sq) > RALPH_ZERO_TOL ?
                                   gamma_e / pivot_sq : 1.0;
    }

    return 0;
}

/* ============================================================================
 * Primal Simplex Algorithm
 * ============================================================================ */

SimplexSolver* simplex_create(LPModel *model) {
    if (!model) return NULL;

    SimplexSolver *solver = (SimplexSolver*)calloc(1, sizeof(SimplexSolver));
    if (!solver) return NULL;

    solver->model = model;
    solver->status = RALPH_STATUS_UNKNOWN;

    /* Default parameters */
    solver->max_iterations = RALPH_DEFAULT_MAX_ITER;
    solver->time_limit = RALPH_DEFAULT_TIME_LIMIT;
    solver->presolve = 1;  /* Enable presolve for performance */
    solver->scaling = 1;   /* Enable scaling for numerical stability */
    solver->pricing_strategy = 1;  /* Steepest edge */
    solver->verbose = 0;
    solver->is_scaled = 0;

    return solver;
}

void simplex_free(SimplexSolver *solver) {
    if (!solver) return;

    tableau_free(solver->tableau);
    free(solver->solution);
    free(solver->dual_solution);
    free(solver->reduced_costs);
    free(solver->row_scale);
    free(solver->col_scale);
    free(solver);
}

/* Phase 1: Find initial basic feasible solution */
static int simplex_phase1(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;

    /* Check if current basis is feasible */
    tableau_compute_solution(tab);

    int infeasible = 0;
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL ||
            tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
            infeasible = 1;
            break;
        }
    }

    if (!infeasible) {
        return 0;  /* Already feasible */
    }

    /* Need to run Phase 1 with artificial variables */
    /* For simplicity, use Big-M method */
    double BIG_M = 1e8;

    /* Add artificial variables for rows with negative RHS */
    /* or use dual simplex to restore feasibility */

    /* Simplified: try to fix by pushing variables to bounds */
    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tableau_compute_solution(tab);

        int most_infeas_k = -1;
        double max_infeas = RALPH_FEAS_TOL;

        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            double infeas = 0.0;

            if (tab->x[j] < tab->lb_ext[j]) {
                infeas = tab->lb_ext[j] - tab->x[j];
            } else if (tab->x[j] > tab->ub_ext[j]) {
                infeas = tab->x[j] - tab->ub_ext[j];
            }

            if (infeas > max_infeas) {
                max_infeas = infeas;
                most_infeas_k = k;
            }
        }

        if (most_infeas_k < 0) {
            return 0;  /* Feasible */
        }

        /* Try dual pivot */
        int leaving = most_infeas_k;
        int j_leave = tab->basis[leaving];

        /* Find entering variable by dual ratio test */
        /* Compute row of tableau */
        vec_set_zero(tab->work1, tab->m);
        tab->work1[leaving] = 1.0;
        lu_solve_transpose(tab->lu, tab->work1, tab->work2);

        int entering = -1;
        double best_ratio = RALPH_INFINITY;
        int dir = (tab->x[j_leave] < tab->lb_ext[j_leave]) ? 1 : -1;

        for (int jj = 0; jj < tab->n; jj++) {
            if (tab->var_status[jj] == RALPH_BASIC) continue;

            sparse_get_column(tab->A_ext, jj, tab->work1);
            double alpha = vec_dot(tab->m, tab->work2, tab->work1);

            if (fabs(alpha) < RALPH_PIVOT_TOL) continue;

            double rc = tab->rc[jj];
            double ratio = RALPH_INFINITY;

            if (dir > 0 && alpha > RALPH_PIVOT_TOL &&
                tab->var_status[jj] == RALPH_NONBASIC_LOWER) {
                ratio = -rc / alpha;
            } else if (dir > 0 && alpha < -RALPH_PIVOT_TOL &&
                       tab->var_status[jj] == RALPH_NONBASIC_UPPER) {
                ratio = rc / (-alpha);
            } else if (dir < 0 && alpha < -RALPH_PIVOT_TOL &&
                       tab->var_status[jj] == RALPH_NONBASIC_LOWER) {
                ratio = -rc / (-alpha);
            } else if (dir < 0 && alpha > RALPH_PIVOT_TOL &&
                       tab->var_status[jj] == RALPH_NONBASIC_UPPER) {
                ratio = rc / alpha;
            }

            if (ratio >= 0 && ratio < best_ratio) {
                best_ratio = ratio;
                entering = jj;
            }
        }

        if (entering < 0) {
            solver->status = RALPH_STATUS_INFEASIBLE;
            return -1;
        }

        /* Perform pivot */
        sparse_get_column(tab->A_ext, entering, tab->work1);
        lu_solve(tab->lu, tab->work1, tab->work2);

        double theta = max_infeas / fabs(tab->work2[leaving]);
        simplex_pivot(tab, entering, leaving, theta);

        if (lu_needs_refactorization(tab->lu)) {
            tableau_refactorize(tab);
        }

        tableau_compute_reduced_costs(tab);
    }

    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    return -1;
}

/* Phase 2: Optimize */
static int simplex_phase2(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;

    tab->phase = 2;

    /* Compute initial solution for Phase 2 */
    tableau_compute_solution(tab);

    /* Cycling detection: track consecutive degenerate pivots */
    int degenerate_count = 0;
    const int DEGEN_THRESHOLD = 50;  /* Switch to Bland's rule after this many */
    int use_bland = 0;

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tab->iterations = iter;

        /* Compute reduced costs */
        tableau_compute_reduced_costs(tab);

        /* Pricing: select entering variable */
        int entering;
        int price_status;

        if (use_bland) {
            /* Use Bland's rule to prevent cycling */
            price_status = pricing_bland(tab, &entering);
        } else if (solver->pricing_strategy == 0) {
            price_status = pricing_dantzig(tab, &entering);
        } else {
            price_status = pricing_steepest_edge(tab, &entering);
        }

        if (price_status != 0) {
            /* Optimal */
            solver->status = RALPH_STATUS_OPTIMAL;
            solver->iterations = iter;
            solver->degenerate_pivots = degenerate_count;
            tableau_compute_solution(tab);

            solver->obj_value = tab->obj_value * solver->model->obj_sense;
            return 0;
        }

        /* Ratio test: select leaving variable */
        int leaving;
        double theta;
        int ratio_status;

        if (use_bland) {
            ratio_status = ratio_test_bland(tab, entering, &leaving, &theta);
        } else {
            ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
        }

        if (ratio_status != 0) {
            /* Unbounded */
            solver->status = RALPH_STATUS_UNBOUNDED;
            solver->iterations = iter;
            return -1;
        }

        /* Track degenerate pivots for cycling detection */
        if (theta < RALPH_FEAS_TOL) {
            degenerate_count++;
            if (degenerate_count >= DEGEN_THRESHOLD && !use_bland) {
                use_bland = 1;
                if (solver->verbose) {
                    printf("Iter %d: Switching to Bland's rule due to potential cycling\n", iter);
                }
            }
        } else {
            /* Reset counter on non-degenerate pivot */
            degenerate_count = 0;
            use_bland = 0;
        }

        /* Perform pivot */
        if (simplex_pivot(tab, entering, leaving, theta) != 0) {
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }

        /* Refactorize if needed */
        if (lu_needs_refactorization(tab->lu)) {
            if (tableau_refactorize(tab) != 0) {
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            /* After refactorization, recompute solution to eliminate drift */
            tableau_compute_solution(tab);
        }

        /* Periodically recompute solution to correct numerical drift */
        if (iter > 0 && iter % 50 == 0) {
            tableau_compute_solution(tab);
            if (solver->verbose) {
                printf("Iter %d: obj = %.6f\n", iter, tab->obj_value);
            }
        }
    }

    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    solver->iterations = solver->max_iterations;
    return -1;
}

int simplex_solve(SimplexSolver *solver) {
    if (!solver || !solver->model) return -1;

    clock_t start = clock();

    if (solver->verbose) printf("[simplex_solve] Starting...\n");

    /* Finalize model if needed (required before scaling) */
    if (!solver->model->A) {
        if (solver->verbose) printf("[simplex_solve] Finalizing model...\n");
        if (lp_model_finalize(solver->model) != 0) {
            if (solver->verbose) printf("[simplex_solve] ERROR: lp_model_finalize failed\n");
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }
    }

    if (solver->verbose) {
        printf("[simplex_solve] Model: %d vars, %d cons, %d nnz\n",
               solver->model->num_vars, solver->model->num_cons,
               solver->model->A ? solver->model->A->nnz : 0);
    }

    /* Apply scaling if enabled */
    if (solver->scaling) {
        if (apply_scaling(solver) != 0) {
            /* Scaling failed, continue without scaling */
            solver->is_scaled = 0;
        }
    }

    /* Create tableau */
    if (solver->verbose) printf("[simplex_solve] Creating tableau...\n");
    solver->tableau = tableau_create(solver->model);
    if (!solver->tableau) {
        solver->status = RALPH_STATUS_ERROR;
        return -1;
    }

    SimplexTableau *tab = solver->tableau;
    if (solver->verbose) {
        printf("[simplex_solve] Tableau: n=%d (extended), m=%d\n", tab->n, tab->m);
    }

    /* Basis is already initialized in tableau_create with proper slack/artificial vars */

    /* Factorize initial basis */
    if (solver->verbose) printf("[simplex_solve] Factorizing initial basis...\n");
    if (tableau_refactorize(tab) != 0) {
        solver->status = RALPH_STATUS_ERROR;
        return -1;
    }
    if (solver->verbose) printf("[simplex_solve] Initial factorization OK\n");

    /* Phase 1: Find feasible solution */
    if (solver->verbose) printf("[simplex_solve] Starting Phase 1...\n");
    if (simplex_phase1(solver) != 0) {
        if (solver->status == RALPH_STATUS_INFEASIBLE) {
            if (solver->verbose) printf("[simplex_solve] Phase 1: INFEASIBLE\n");
            return 0;  /* Infeasible is a valid result */
        }
        return -1;
    }
    if (solver->verbose) printf("[simplex_solve] Phase 1 complete\n");

    /* Phase 2: Optimize */
    int status = simplex_phase2(solver);

    solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;

    /* Check for infeasibility: if any artificial variable (Big-M cost) is non-zero,
     * the original problem is infeasible */
    if (solver->status == RALPH_STATUS_OPTIMAL) {
        int num_struct = solver->model->num_vars;
        double artificial_contrib = 0.0;
        int artificial_count = 0;

        /* Check all variables, not just basic ones */
        for (int j = num_struct; j < tab->n; j++) {
            if (tab->c_ext[j] > 1e6 && fabs(tab->x[j]) > RALPH_FEAS_TOL) {
                artificial_contrib += tab->c_ext[j] * tab->x[j];
                artificial_count++;
                if (solver->verbose) {
                    fprintf(stderr, "[DEBUG] Artificial var %d still active: x=%.2e, cost=%.0f, contrib=%.2f\n",
                            j, tab->x[j], tab->c_ext[j], tab->c_ext[j] * tab->x[j]);
                }
            }
        }

        if (artificial_count > 0) {
            if (solver->verbose) {
                fprintf(stderr, "[DEBUG] %d artificial vars active, total contribution=%.2f\n",
                        artificial_count, artificial_contrib);
            }
            if (artificial_contrib > 1e-2) {  /* Significant artificial contribution */
                solver->status = RALPH_STATUS_INFEASIBLE;
                return 0;
            }
            /* Subtract artificial contribution from objective for reporting */
            solver->obj_value = (tab->obj_value - artificial_contrib) * solver->model->obj_sense;
        }
    }

    /* Copy solution */
    if (solver->status == RALPH_STATUS_OPTIMAL) {
        solver->solution = (double*)malloc(solver->model->num_vars * sizeof(double));
        solver->dual_solution = (double*)malloc(solver->model->num_cons * sizeof(double));
        solver->reduced_costs = (double*)malloc(solver->model->num_vars * sizeof(double));

        if (solver->solution && solver->dual_solution && solver->reduced_costs) {
            for (int j = 0; j < solver->model->num_vars; j++) {
                solver->solution[j] = tab->x[j];
                solver->reduced_costs[j] = tab->rc[j] * solver->model->obj_sense;
            }
            for (int i = 0; i < solver->model->num_cons; i++) {
                solver->dual_solution[i] = tab->y[i] * solver->model->obj_sense;
            }
        }

        /* Unscale solution if scaling was applied */
        unscale_solution(solver);
    }

    /* Restore original model if scaling was applied */
    restore_model(solver);

    return status;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

void lp_print_stats(const SimplexSolver *solver) {
    if (!solver) return;

    printf("\n=== Simplex Statistics ===\n");
    printf("Status: %d\n", solver->status);
    printf("Iterations: %d\n", solver->iterations);
    printf("Solve time: %.3f seconds\n", solver->solve_time);

    if (solver->status == RALPH_STATUS_OPTIMAL) {
        printf("Objective: %.10f\n", solver->obj_value);
    }
}
