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
    tab->num_aux = num_aux_vars;

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
    tab->pivot_row = (double*)calloc(tab->m, sizeof(double));
    tab->tau_work = (double*)calloc(tab->m, sizeof(double));

    tab->se_weights = (double*)calloc(tab->n, sizeof(double));

    /* Auxiliary variable mapping for cut generation */
    tab->aux_row = (int*)malloc(num_aux_vars * sizeof(int));
    tab->aux_coef = (double*)malloc(num_aux_vars * sizeof(double));

    /* Partial pricing candidate list (hot set) */
    tab->partial_cand_capacity = 100;  /* Fixed size hot set */
    tab->partial_candidates = (int*)malloc(tab->partial_cand_capacity * sizeof(int));
    tab->partial_cand_count = 0;

    if (!tab->c_ext || !tab->lb_ext || !tab->ub_ext ||
        !tab->basis || !tab->nonbasis || !tab->var_status || !tab->basis_pos ||
        !tab->x || !tab->y || !tab->rc ||
        !tab->work1 || !tab->work2 || !tab->work3 || !tab->rhs ||
        !tab->pivot_row || !tab->tau_work || !tab->se_weights ||
        !tab->aux_row || !tab->aux_coef || !tab->partial_candidates) {
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

    /* Compute initial Ax values (x at lower bounds) for each row to decide
     * whether surplus or artificial should be basic for >= constraints */
    double *ax_initial = (double*)calloc(model->num_cons, sizeof(double));
    if (!ax_initial) {
        free(norm_sense);
        free(norm_sign);
        triplets_free(trips);
        tableau_free(tab);
        return NULL;
    }
    for (int j = 0; j < model->num_vars; j++) {
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int row = model->A->rowidx[p];
            double val = model->A->values[p] * norm_sign[row];  /* Apply row normalization */
            ax_initial[row] += val * model->lb[j];  /* x starts at lower bound */
        }
    }

    /* Add auxiliary variables and record their mapping to constraints */
    int aux_idx = model->num_vars;
    int aux_map_idx = 0;  /* Index into aux_row/aux_coef arrays */
    for (int i = 0; i < model->num_cons; i++) {
        if (norm_sense[i] == 'L') {
            /* <= : add slack with coef +1, slack is basic */
            triplets_add(trips, i, aux_idx, 1.0);
            tab->c_ext[aux_idx] = 0.0;
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;

            /* Record mapping: slack for row i with coefficient +1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = 1.0;
            aux_map_idx++;

            aux_idx++;
        } else if (norm_sense[i] == 'G') {
            /* >= : add surplus with coef -1, then artificial with coef +1 */
            double rhs = fabs(model->b[i]);

            /* Check if constraint is already satisfied at initial point (x at lb) */
            int surplus_idx = aux_idx;
            int artificial_idx = aux_idx + 1;

            /* Surplus variable */
            triplets_add(trips, i, surplus_idx, -1.0);
            tab->c_ext[surplus_idx] = 0.0;
            tab->lb_ext[surplus_idx] = 0.0;
            tab->ub_ext[surplus_idx] = RALPH_INFINITY;

            /* Record mapping: surplus for row i with coefficient -1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = -1.0;
            aux_map_idx++;

            /* Artificial variable */
            triplets_add(trips, i, artificial_idx, 1.0);
            tab->c_ext[artificial_idx] = 1e8;  /* Big-M cost */
            tab->lb_ext[artificial_idx] = 0.0;
            tab->ub_ext[artificial_idx] = RALPH_INFINITY;

            /* Record mapping: artificial for row i with coefficient +1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = 1.0;
            aux_map_idx++;

            /* Decide which is basic: if ax_initial >= rhs, surplus can be basic.
             * Otherwise we need the artificial variable. */
            if (ax_initial[i] >= rhs - RALPH_FEAS_TOL) {
                /* Constraint already satisfied, surplus is basic */
                basic_var_for_row[i] = surplus_idx;
            } else {
                /* Need artificial variable to be basic */
                basic_var_for_row[i] = artificial_idx;
            }

            aux_idx += 2;
        } else {
            /* = : add artificial with coef +1 (basic) */
            triplets_add(trips, i, aux_idx, 1.0);
            tab->c_ext[aux_idx] = 1e8;  /* Big-M cost */
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;

            /* Record mapping: artificial for row i with coefficient +1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = 1.0;
            aux_map_idx++;

            aux_idx++;
        }
    }
    free(ax_initial);

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

    /* Compute initial basic variable values: x_B = B^{-1} * (b - N * x_N)
     * For slack/artificial (coeff +1): B^{-1} = 1, so x_B = rhs - sum(A_N * x_N)
     * For surplus (coeff -1): B^{-1} = -1, so x_B = -(rhs - sum(A_N * x_N))
     * General formula: x_B = (rhs - sum(A_N * x_N)) / col_coeff
     */
    for (int i = 0; i < model->num_cons; i++) {
        int bv = basic_var_for_row[i];
        double val = tab->rhs[i];

        /* Subtract A[i,j] * x[j] for nonbasic structural variables j */
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

        /* Get the diagonal coefficient of the basic variable (column of bv in row i) */
        double col_coeff = 0.0;
        for (int p = tab->A_ext->colptr[bv]; p < tab->A_ext->colptr[bv + 1]; p++) {
            if (tab->A_ext->rowidx[p] == i) {
                col_coeff = tab->A_ext->values[p];
                break;
            }
        }

        /* Divide by column coefficient to get correct basic variable value */
        if (fabs(col_coeff) > RALPH_ZERO_TOL) {
            tab->x[bv] = val / col_coeff;
        } else {
            tab->x[bv] = val;  /* Fallback (shouldn't happen) */
        }
    }

    /* Initialize steepest edge / Devex weights
     * For initial basis (typically slack identity), B^{-1} = I, so:
     *   gamma_j = ||B^{-1} * a_j||^2 = ||a_j||^2
     */
    for (int j = 0; j < tab->n; j++) {
        double col_norm_sq = 0.0;
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            col_norm_sq += tab->A_ext->values[p] * tab->A_ext->values[p];
        }
        tab->se_weights[j] = (col_norm_sq > 1.0) ? col_norm_sq : 1.0;
    }
    tab->use_steepest_edge = 1;
    tab->pricing_strategy = 2;  /* Default to Devex */
    tab->devex_refcount = 0;

    /* Initialize lazy reduced cost computation flags */
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;

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
    free(tab->pivot_row);
    free(tab->tau_work);
    free(tab->se_weights);
    free(tab->perturb_backup);
    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);
    free(tab->aux_row);
    free(tab->aux_coef);
    free(tab->partial_candidates);
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
    /* Compute dual values: y = B^{-T} * c_B
     * If c_B is sparse (many slacks with 0 cost), use sparse BTRAN
     */

    /* Count non-zeros in c_B */
    int nnz_cb = 0;
    for (int k = 0; k < tab->m; k++) {
        if (fabs(tab->c_ext[tab->basis[k]]) > RALPH_ZERO_TOL) {
            nnz_cb++;
        }
    }

    /* If c_B is sparse (less than 10% non-zeros), use sparse BTRAN */
    if (nnz_cb < tab->m / 10) {
        int *cb_idx = (int*)malloc(nnz_cb * sizeof(int));
        double *cb_val = (double*)malloc(nnz_cb * sizeof(double));
        if (cb_idx && cb_val) {
            int p = 0;
            for (int k = 0; k < tab->m; k++) {
                double c = tab->c_ext[tab->basis[k]];
                if (fabs(c) > RALPH_ZERO_TOL) {
                    cb_idx[p] = k;
                    cb_val[p] = c;
                    p++;
                }
            }
            lu_solve_transpose_sparse(tab->lu, nnz_cb, cb_idx, cb_val, tab->y);
            free(cb_idx);
            free(cb_val);
        } else {
            free(cb_idx);
            free(cb_val);
            /* Fallback to dense */
            vec_set_zero(tab->work1, tab->m);
            for (int k = 0; k < tab->m; k++) {
                tab->work1[k] = tab->c_ext[tab->basis[k]];
            }
            lu_solve_transpose(tab->lu, tab->work1, tab->y);
        }
    } else {
        /* Dense BTRAN */
        vec_set_zero(tab->work1, tab->m);
        for (int k = 0; k < tab->m; k++) {
            tab->work1[k] = tab->c_ext[tab->basis[k]];
        }
        lu_solve_transpose(tab->lu, tab->work1, tab->y);
    }

    /* Compute reduced costs: rc = c - A' * y
     * Using sparse matrix-transpose-vector multiply: O(nnz) instead of O(n*m) */

    /* First negate y, then compute c + A'*(-y) = c - A'*y */
    for (int i = 0; i < tab->m; i++) {
        tab->work1[i] = -tab->y[i];
    }

    /* rc = c */
    vec_copy_data(tab->rc, tab->c_ext, tab->n);

    /* rc += A' * (-y) = rc - A' * y */
    sparse_matvec_transpose_add(tab->A_ext, tab->work1, tab->rc);

    /* Zero out reduced costs for basic variables */
    for (int k = 0; k < tab->m; k++) {
        tab->rc[tab->basis[k]] = 0.0;
    }

    /* Mark both duals and full rc as valid */
    tab->duals_valid = 1;
    tab->rc_all_valid = 1;

    return 0;
}

/* Compute only dual values y = B^{-T} * c_B (for lazy rc computation) */
int tableau_compute_duals(SimplexTableau *tab) {
    /* Count non-zeros in c_B */
    int nnz_cb = 0;
    for (int k = 0; k < tab->m; k++) {
        if (fabs(tab->c_ext[tab->basis[k]]) > RALPH_ZERO_TOL) {
            nnz_cb++;
        }
    }

    /* If c_B is sparse (less than 10% non-zeros), use sparse BTRAN */
    if (nnz_cb < tab->m / 10) {
        int *cb_idx = (int*)malloc(nnz_cb * sizeof(int));
        double *cb_val = (double*)malloc(nnz_cb * sizeof(double));
        if (cb_idx && cb_val) {
            int p = 0;
            for (int k = 0; k < tab->m; k++) {
                double c = tab->c_ext[tab->basis[k]];
                if (fabs(c) > RALPH_ZERO_TOL) {
                    cb_idx[p] = k;
                    cb_val[p] = c;
                    p++;
                }
            }
            lu_solve_transpose_sparse(tab->lu, nnz_cb, cb_idx, cb_val, tab->y);
            free(cb_idx);
            free(cb_val);
        } else {
            free(cb_idx);
            free(cb_val);
            /* Fallback to dense */
            vec_set_zero(tab->work1, tab->m);
            for (int k = 0; k < tab->m; k++) {
                tab->work1[k] = tab->c_ext[tab->basis[k]];
            }
            lu_solve_transpose(tab->lu, tab->work1, tab->y);
        }
    } else {
        /* Dense BTRAN */
        vec_set_zero(tab->work1, tab->m);
        for (int k = 0; k < tab->m; k++) {
            tab->work1[k] = tab->c_ext[tab->basis[k]];
        }
        lu_solve_transpose(tab->lu, tab->work1, tab->y);
    }

    tab->duals_valid = 1;
    tab->rc_all_valid = 0;  /* Full rc[] not computed */

    return 0;
}

/* Compute single reduced cost rc[j] = c[j] - A[:,j]' * y
 * Requires duals (y) to be valid. Returns the reduced cost.
 * Caches the result in tab->rc[j] for future use and incremental updates. */
static inline double tableau_get_rc(SimplexTableau *tab, int j) {
    /* If full rc[] is valid, just return it */
    if (tab->rc_all_valid) {
        return tab->rc[j];
    }

    /* Compute lazily: rc[j] = c[j] - A[:,j]' * y */
    double rc = tab->c_ext[j] - sparse_dot_column(tab->A_ext, j, tab->y);
    tab->rc[j] = rc;  /* Cache for incremental updates in simplex_pivot */
    return rc;
}

/* Invalidate reduced costs (call after basis change) */
static inline void tableau_invalidate_rc(SimplexTableau *tab) {
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
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
    /* Steepest edge pricing: max |rc_j| / sqrt(gamma_j)
     * Uses exact weights updated with the formula:
     *   gamma_j = ||B^{-1} * a_j||^2
     *
     * Optimization: Compare rc²/weight instead of |rc|/sqrt(weight)
     * to eliminate expensive sqrt() calls. Mathematically equivalent:
     *   |rc|/sqrt(w) > t  ⟺  rc²/w > t²
     */
    double best_ratio_sq = RALPH_OPT_TOL * RALPH_OPT_TOL;  /* Squared threshold */
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        double rc = tab->rc[j];
        double weight = tab->se_weights[j];
        if (weight < 1e-10) weight = 1.0;

        double ratio_sq = 0.0;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        }

        if (ratio_sq > best_ratio_sq) {
            best_ratio_sq = ratio_sq;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;
}

int pricing_devex(SimplexTableau *tab, int *entering) {
    /* Devex pricing: max |rc_j|² / gamma_j
     *
     * Uses approximate steepest edge weights with periodic reset.
     * Reference: Harris, "Pivot Selection Methods of the Devex LP Code", 1973
     */
    double best_ratio = RALPH_OPT_TOL * RALPH_OPT_TOL;  /* Squared tolerance */
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        double rc = tab->rc[j];
        double weight = tab->se_weights[j];
        if (weight < 1.0) weight = 1.0;  /* Devex weights are always >= 1 */

        double ratio = 0.0;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        }

        if (ratio > best_ratio) {
            best_ratio = ratio;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;
}

/* Partial pricing with candidate list (hot set):
 *
 * Instead of scanning all n variables each iteration, we maintain a "hot set" of
 * promising variables. The algorithm:
 * 1. First scan the hot set for eligible variables
 * 2. If no good candidate in hot set, do a partial scan of remaining variables
 * 3. Selected variables are added to the hot set for future iterations
 * 4. Periodically clean the hot set (remove basic variables, refresh)
 *
 * This reduces O(n) to approximately O(hot_set_size + block_size) per iteration.
 */
#define PARTIAL_PRICE_BLOCK 100       /* Variables per partial scan block */
#define PARTIAL_PRICE_THRESHOLD 1e-6  /* Accept if |rc| > threshold */
#define PARTIAL_HOT_ACCEPT 1e-4       /* Accept immediately from hot set if |rc| > this */

/* Check if variable j is eligible for entering */
static inline int is_entering_eligible(SimplexTableau *tab, int j, double *rc_out) {
    if (tab->var_status[j] == RALPH_BASIC) return 0;

    /* Use lazy RC computation - computes on demand if not already cached */
    double rc = tableau_get_rc(tab, j);
    *rc_out = rc;

    if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -PARTIAL_PRICE_THRESHOLD) {
        return 1;
    } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > PARTIAL_PRICE_THRESHOLD) {
        return 1;
    } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > PARTIAL_PRICE_THRESHOLD) {
        return 1;
    }
    return 0;
}

/* Add variable to hot set if not already present and not full */
static inline void add_to_hot_set(SimplexTableau *tab, int var) {
    /* Check if already in hot set */
    for (int i = 0; i < tab->partial_cand_count; i++) {
        if (tab->partial_candidates[i] == var) return;
    }
    /* Add if space available */
    if (tab->partial_cand_count < tab->partial_cand_capacity) {
        tab->partial_candidates[tab->partial_cand_count++] = var;
    }
}

int pricing_partial(SimplexTableau *tab, int *entering) {
    *entering = -1;

    /* Ensure duals are valid for lazy RC computation */
    if (!tab->duals_valid) {
        tableau_compute_duals(tab);
    }

    int n = tab->n;
    double best_rc_val = 0.0;
    int best_var = -1;

    /* Phase 1: Scan hot set first (fast path) */
    int write_idx = 0;
    for (int i = 0; i < tab->partial_cand_count; i++) {
        int j = tab->partial_candidates[i];

        /* Skip and remove basic variables from hot set */
        if (tab->var_status[j] == RALPH_BASIC) continue;

        /* Keep this variable in the compacted hot set */
        tab->partial_candidates[write_idx++] = j;

        double rc;
        if (is_entering_eligible(tab, j, &rc)) {
            double rc_abs = fabs(rc);

            /* Accept immediately if reduced cost is very attractive */
            if (rc_abs > PARTIAL_HOT_ACCEPT) {
                *entering = j;
                tab->partial_cand_count = write_idx;
                return 0;
            }

            /* Track best candidate seen */
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = j;
            }
        }
    }
    /* Update hot set count after compaction */
    tab->partial_cand_count = write_idx;

    /* Phase 2: Partial scan from current position */
    int start = tab->partial_price_pos;
    int scanned = 0;

    for (int i = 0; i < n && scanned < PARTIAL_PRICE_BLOCK; i++) {
        int j = (start + i) % n;
        if (tab->var_status[j] == RALPH_BASIC) continue;

        scanned++;
        double rc;
        if (is_entering_eligible(tab, j, &rc)) {
            double rc_abs = fabs(rc);
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = j;
            }
        }
    }

    /* Update scan position for next call (round-robin) */
    tab->partial_price_pos = (start + PARTIAL_PRICE_BLOCK) % n;

    /* Use best variable found (if any) */
    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    /* Phase 3: Full scan if partial scan found nothing (rare) */
    for (int i = 0; i < n; i++) {
        double rc;
        if (is_entering_eligible(tab, i, &rc)) {
            double rc_abs = fabs(rc);
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = i;
            }
        }
    }

    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    return 1;  /* Optimal - no eligible variable found */
}

/* ============================================================================
 * Ratio Test (Leaving Variable Selection)
 * ============================================================================ */

/* Bland's ratio test: among ties, choose smallest index leaving variable */
int ratio_test_bland(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    /* Use hyper-sparse FTRAN for better performance on sparse columns */
    lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);

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
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    /* Use hyper-sparse FTRAN for better performance on sparse columns */
    lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);

    double dir = 1.0;  /* Direction of movement */
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    /* Single-pass Harris ratio test (merged from two passes)
     *
     * Harris ratio test allows small infeasibility (FEAS_TOL) when computing
     * theta_max, then selects among candidates within that tolerance.
     *
     * Tie-breaking strategy:
     * 1. Prefer non-degenerate pivots (ratio > tolerance)
     * 2. Among degenerate ties, prefer larger pivot for numerical stability
     */
    double theta_max = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_is_degen = 1;
    *leaving = -1;
    *theta = RALPH_INFINITY;

    /* Check bound on entering variable first (contributes to theta_max) */
    double enter_range = tab->ub_ext[entering] - tab->lb_ext[entering];
    if (enter_range < RALPH_INFINITY/2) {
        theta_max = enter_range;
    }

    /* Single pass: compute theta_max and select best leaving simultaneously */
    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        if (fabs(dk) < RALPH_PIVOT_TOL) continue;  /* Skip tiny pivots */

        int j = tab->basis[k];
        double xj = tab->x[j];

        double ratio_harris;  /* Ratio with Harris tolerance */
        double ratio_exact;   /* Exact ratio for selection */

        if (dk > 0) {
            /* Variable will decrease toward lower bound */
            double slack = xj - tab->lb_ext[j];
            ratio_harris = (slack + RALPH_FEAS_TOL) / dk;
            ratio_exact = slack / dk;
        } else {
            /* Variable will increase toward upper bound (dk < 0) */
            double slack = tab->ub_ext[j] - xj;
            ratio_harris = (slack + RALPH_FEAS_TOL) / (-dk);
            ratio_exact = slack / (-dk);
        }

        /* Update theta_max */
        if (ratio_harris < theta_max) {
            theta_max = ratio_harris;
        }

        /* Check if this is a valid candidate (within current theta_max + tolerance) */
        if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
            int is_degen = (ratio_exact < 1e-8);
            double pivot_size = fabs(dk);

            /* Selection criteria */
            int select = 0;
            if (*leaving < 0) {
                select = 1;  /* First candidate */
            } else if (!is_degen && best_is_degen) {
                select = 1;  /* Prefer non-degenerate */
            } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                select = 1;  /* Significantly larger pivot */
            }

            if (select) {
                best_pivot = pivot_size;
                best_is_degen = is_degen;
                *leaving = k;
                *theta = ratio_exact > 0 ? ratio_exact : 0;
            }
        }
    }

    /* After the pass, invalidate selection if it's no longer within theta_max
     * (theta_max may have decreased after we selected the candidate) */
    if (*leaving >= 0 && *theta > theta_max + RALPH_FEAS_TOL) {
        /* Re-scan for valid candidates - this is rare */
        best_pivot = 0.0;
        best_is_degen = 1;
        *leaving = -1;
        *theta = RALPH_INFINITY;

        for (int k = 0; k < tab->m; k++) {
            double dk = tab->work2[k] * dir;
            if (fabs(dk) < RALPH_PIVOT_TOL) continue;

            int j = tab->basis[k];
            double xj = tab->x[j];
            double ratio_exact;

            if (dk > 0) {
                ratio_exact = (xj - tab->lb_ext[j]) / dk;
            } else {
                ratio_exact = (tab->ub_ext[j] - xj) / (-dk);
            }

            if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
                int is_degen = (ratio_exact < 1e-8);
                double pivot_size = fabs(dk);

                int select = 0;
                if (*leaving < 0) {
                    select = 1;
                } else if (!is_degen && best_is_degen) {
                    select = 1;
                } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                    select = 1;
                }

                if (select) {
                    best_pivot = pivot_size;
                    best_is_degen = is_degen;
                    *leaving = k;
                    *theta = ratio_exact > 0 ? ratio_exact : 0;
                }
            }
        }
    }

    /* Check for unbounded */
    if (theta_max >= RALPH_INFINITY/2) {
        *theta = RALPH_INFINITY;
        return -1;  /* Unbounded */
    }

    /* Check if entering variable hits its bound (bound flip) */
    if (enter_range <= theta_max && enter_range < RALPH_INFINITY/2) {
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

    /* Compute pivot row and steepest edge update data BEFORE LU update (using old basis) */
    double pivot = tab->work2[leaving_pos];
    double pivot_sq = pivot * pivot;

    /* Compute exact entering column weight: gamma_e = ||d_entering||^2 = ||work2||^2 */
    double gamma_e = 0.0;
    for (int k = 0; k < tab->m; k++) {
        gamma_e += tab->work2[k] * tab->work2[k];
    }
    if (gamma_e < 1.0) gamma_e = 1.0;

    /* Always compute pivot row for incremental reduced cost updates
     * pivot_row = e_r^T * B^{-1}
     * This is also used for steepest edge weight updates
     * Use pre-allocated workspace to avoid malloc in hot path
     */
    double *pivot_row = tab->pivot_row;
    double *tau_helper = tab->tau_work;

    /* Use sparse BTRAN since e_leaving has only 1 non-zero */
    int rhs_idx = leaving_pos;
    double rhs_val = 1.0;
    lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, pivot_row);

    /* For steepest edge weight updates: compute tau_helper = B^{-T} * d_entering
     * This enables exact weight updates using: gamma_j' = gamma_j - 2*(alpha_j/p)*tau_j + (alpha_j/p)^2*gamma_e
     *
     * Note: We use exact SE weights for both SE and Devex pricing because the
     * Devex approximation (max formula) leads to worse pivot selection and
     * significantly more iterations, which outweighs the BTRAN savings.
     */
    int use_true_se = tab->use_steepest_edge;
    if (use_true_se && fabs(pivot_sq) > RALPH_ZERO_TOL) {
        lu_solve_transpose(tab->lu, tab->work2, tau_helper);
    }

    /* Update LU factorization */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    if (lu_update(tab->lu, leaving_pos, tab->work1) != 0) {
        /* Update failed, refactorize */
        if (tableau_refactorize(tab) != 0) {
            return -1;
        }
    }

    /* Update steepest edge pricing weights
     *
     * True Steepest Edge (exact formula):
     *   gamma_j_new = gamma_j - 2*(alpha_j/pivot)*tau_j + (alpha_j/pivot)^2 * gamma_e
     * where:
     *   alpha_j = pivot_row * a_j (pivot row entry)
     *   tau_j = d_j' * d_entering = a_j' * (B^{-T} * d_entering)
     *   gamma_e = ||d_entering||^2 (entering column norm squared)
     *
     * Devex approximation (simpler but much faster):
     *   gamma_j = max(gamma_j, (alpha_j^2 * gamma_e) / pivot^2)
     * This doesn't need tau_helper, making it O(n) instead of O(n*m).
     */
    int skip_se_update = 0;  /* Flag to skip SE update after reset */
    if (tab->use_steepest_edge) {
        tab->devex_refcount++;
        double pivot_inv_sq = 1.0 / (pivot * pivot);

        /* Weight for leaving variable (now nonbasic): gamma_e / pivot^2 */
        double leaving_weight = gamma_e * pivot_inv_sq;
        if (leaving_weight < 1.0) leaving_weight = 1.0;
        if (leaving_weight > 1e8) leaving_weight = 1e8;
        tab->se_weights[leaving] = leaving_weight;

        /* Periodic reference reset: recalculate weights from column norms */
        if (tab->devex_refcount >= 2 * tab->n) {
            for (int j = 0; j < tab->n; j++) {
                double col_norm_sq = 0.0;
                for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                    col_norm_sq += tab->A_ext->values[p] * tab->A_ext->values[p];
                }
                tab->se_weights[j] = (col_norm_sq > 1.0) ? col_norm_sq : 1.0;
            }
            tab->devex_refcount = 0;
            skip_se_update = 1;  /* Don't overwrite fresh reset values */
        }
    }

    /* Update reduced costs and weights in a single merged loop.
     *
     * In lazy RC mode (rc_all_valid == 0), skip the O(n) incremental update
     * and just invalidate duals. This saves O(n * avg_col_nnz) per iteration
     * at the cost of O(m²) BTRAN to recompute duals next iteration.
     * For large n with partial pricing (examining ~200-500 vars), this is faster.
     */
    if (!tab->rc_all_valid) {
        /* Lazy RC mode: skip incremental updates, invalidate duals */
        tab->duals_valid = 0;
        tab->rc[entering] = 0.0;  /* Basic variables have rc = 0 */
        return 0;
    }

    if (fabs(pivot) > RALPH_PIVOT_TOL) {
        double rc_enter = tab->rc[entering];
        double rc_ratio = rc_enter / pivot;
        double pivot_inv = 1.0 / pivot;
        int do_se_update = tab->use_steepest_edge && !skip_se_update;

        /* For all non-basic variables, update reduced costs and optionally weights */
        for (int j = 0; j < tab->n; j++) {
            if (tab->var_status[j] == RALPH_BASIC) continue;
            if (j == entering) continue;

            /* Compute alpha_j = pivot_row * a_j */
            double alpha_j = 0.0;
            for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                alpha_j += pivot_row[tab->A_ext->rowidx[p]] * tab->A_ext->values[p];
            }

            /* Update reduced cost */
            tab->rc[j] -= rc_ratio * alpha_j;

            /* Update steepest edge weights if enabled */
            if (do_se_update && j != leaving) {
                /* True Steepest Edge: exact formula using tau_helper */
                double tau_j = sparse_dot_column(tab->A_ext, j, tau_helper);
                double alpha_ratio = alpha_j * pivot_inv;
                double new_weight = tab->se_weights[j]
                                  - 2.0 * alpha_ratio * tau_j
                                  + alpha_ratio * alpha_ratio * gamma_e;
                if (new_weight < 1.0) new_weight = 1.0;
                if (new_weight > 1e8) new_weight = 1e8;
                tab->se_weights[j] = new_weight;
            }
        }

        /* Reduced cost for entering variable (now basic) is 0 */
        tab->rc[entering] = 0.0;

        /* Reduced cost for leaving variable (now non-basic) */
        tab->rc[leaving] = -rc_enter / pivot;
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
    solver->pricing_strategy = 2;  /* Devex pricing (better than Dantzig) */
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
    free(solver->farkas_ray);
    free(solver);
}

/*
 * Extract Farkas ray (certificate of infeasibility)
 *
 * When the LP is infeasible, the dual values y from Phase 1 satisfy:
 *   y'A >= 0 for all columns (adjusted for constraint sense)
 *   y'b < 0
 *
 * This proves no feasible solution exists via Farkas lemma.
 * The ray is stored in solver->farkas_ray for retrieval via API.
 */
static void extract_farkas_ray(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;
    int m = tab->m;

    /* Allocate if needed */
    if (!solver->farkas_ray) {
        solver->farkas_ray = (double*)malloc(m * sizeof(double));
    }
    if (!solver->farkas_ray) {
        solver->farkas_valid = 0;
        return;
    }

    /* The dual values y = c_B' * B^{-1} from Phase 1 give the Farkas ray.
     * At infeasibility detection, tab->y contains these values.
     * We need to compute them fresh using the current basis. */

    /* Compute y = c_B' * B^{-1} via BTRAN
     * For Phase 1 infeasibility, we use the direction of the infeasible row */

    /* Get the dual values from the tableau */
    tableau_compute_reduced_costs(tab);

    /* Copy the dual values - these are the Farkas multipliers */
    for (int i = 0; i < m; i++) {
        solver->farkas_ray[i] = tab->y[i];
    }

    solver->farkas_valid = 1;
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

            double alpha = sparse_dot_column(tab->A_ext, jj, tab->work2);

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
            extract_farkas_ray(solver);
            solver->status = RALPH_STATUS_INFEASIBLE;
            return -1;
        }

        /* Perform pivot using sparse solve */
        int col_nnz;
        const int *col_idx;
        const double *col_val;
        sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);
        lu_solve_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2);

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

/* ============================================================================
 * Bound Perturbation for Degeneracy Prevention (Primal Simplex)
 * ============================================================================
 *
 * Adds small perturbations to bounds to break degeneracy and prevent cycling.
 * This is proactive (applied at start) vs reactive (Bland's rule after cycling).
 *
 * The perturbation scheme:
 * - Perturb lower bounds down by small epsilon
 * - Perturb upper bounds up by small epsilon
 * - Use pseudo-random scaling based on variable index for reproducibility
 */
#define PRIMAL_PERTURB_BASE 1e-6
#define PRIMAL_PERTURB_MULT 7

static void primal_apply_perturbation(SimplexTableau *tab) {
    int n = tab->n;

    /* Free any existing perturbation state */
    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);

    /* Save original bounds */
    tab->primal_saved_lb = (double*)malloc(n * sizeof(double));
    tab->primal_saved_ub = (double*)malloc(n * sizeof(double));
    if (!tab->primal_saved_lb || !tab->primal_saved_ub) {
        free(tab->primal_saved_lb);
        free(tab->primal_saved_ub);
        tab->primal_saved_lb = tab->primal_saved_ub = NULL;
        tab->primal_perturb_active = 0;
        return;
    }

    for (int j = 0; j < n; j++) {
        tab->primal_saved_lb[j] = tab->lb_ext[j];
        tab->primal_saved_ub[j] = tab->ub_ext[j];
    }

    /* Apply perturbations */
    for (int j = 0; j < n; j++) {
        /* Pseudo-random perturbation factor */
        double factor = 1.0 + (j * PRIMAL_PERTURB_MULT) % 13;

        /* Perturb finite lower bounds down */
        if (tab->lb_ext[j] > -RALPH_INFINITY / 2) {
            double eps = PRIMAL_PERTURB_BASE * factor * (1.0 + fabs(tab->lb_ext[j]));
            tab->lb_ext[j] -= eps;
        }

        /* Perturb finite upper bounds up */
        if (tab->ub_ext[j] < RALPH_INFINITY / 2) {
            double eps = PRIMAL_PERTURB_BASE * factor * (1.0 + fabs(tab->ub_ext[j]));
            tab->ub_ext[j] += eps;
        }
    }

    tab->primal_perturb_active = 1;
}

static void primal_remove_perturbation(SimplexTableau *tab) {
    if (!tab->primal_perturb_active || !tab->primal_saved_lb || !tab->primal_saved_ub) {
        return;
    }

    /* Restore original bounds */
    for (int j = 0; j < tab->n; j++) {
        tab->lb_ext[j] = tab->primal_saved_lb[j];
        tab->ub_ext[j] = tab->primal_saved_ub[j];
    }

    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);
    tab->primal_saved_lb = tab->primal_saved_ub = NULL;
    tab->primal_perturb_active = 0;
}

/* Phase 2: Optimize */
static int simplex_phase2(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;

    tab->phase = 2;

    /* Note: Bound perturbation is now applied adaptively when degeneracy detected,
     * rather than proactively at start. See cycling detection below.
     */
    int perturbation_active = 0;

    /* Compute initial solution for Phase 2 */
    tableau_compute_solution(tab);

    /* Cycling detection: track consecutive degenerate pivots */
    int degenerate_count = 0;
    int non_degen_streak = 0;
    const int DEGEN_THRESHOLD = 50;  /* Switch to Bland's rule after this many */
    const int NON_DEGEN_THRESHOLD = 100;  /* Non-degenerate pivots to turn Bland off */
    int use_bland = 0;

    /* Compute initial reduced costs.
     * For partial pricing, use lazy mode (duals only) for efficiency. */
    if (solver->pricing_strategy == 3) {
        tableau_compute_duals(tab);  /* Lazy mode: duals only */
    } else {
        tableau_compute_reduced_costs(tab);
    }

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tab->iterations = iter;

        /* Reduced costs are updated incrementally in simplex_pivot().
         * Full recomputation only needed:
         * - After refactorization (for numerical stability)
         * - Periodically to correct drift
         */

        /* Pricing: select entering variable */
        int entering;
        int price_status;

        if (use_bland) {
            /* Use Bland's rule to prevent cycling */
            price_status = pricing_bland(tab, &entering);
        } else if (solver->pricing_strategy == 0) {
            price_status = pricing_dantzig(tab, &entering);
        } else if (solver->pricing_strategy == 1) {
            price_status = pricing_steepest_edge(tab, &entering);
        } else if (solver->pricing_strategy == 3) {
            price_status = pricing_partial(tab, &entering);
        } else {
            price_status = pricing_devex(tab, &entering);
        }

        if (price_status != 0) {
            /* Optimal - remove perturbation and finalize */
            primal_remove_perturbation(tab);
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
            /* No leaving variable found. This could mean:
             * 1. The problem is unbounded (rare in practice)
             * 2. Numerical issues with Big-M artificial variables
             *
             * If there are artificial variables (Big-M cost) that are still
             * at non-zero values, the problem is actually INFEASIBLE, not UNBOUNDED.
             */
            primal_remove_perturbation(tab);

            /* Check for non-zero artificial variables */
            int num_struct = solver->model->num_vars;
            double artificial_sum = 0.0;
            for (int j = num_struct; j < tab->n; j++) {
                if (tab->c_ext[j] > 1e6 && fabs(tab->x[j]) > RALPH_FEAS_TOL) {
                    artificial_sum += fabs(tab->x[j]);
                }
            }

            if (artificial_sum > RALPH_FEAS_TOL) {
                /* Non-zero artificials mean the original problem is infeasible */
                extract_farkas_ray(solver);
                solver->status = RALPH_STATUS_INFEASIBLE;
            } else {
                /* No artificials - truly unbounded */
                solver->status = RALPH_STATUS_UNBOUNDED;
            }
            solver->iterations = iter;
            return -1;
        }

        /* Track degenerate/near-degenerate pivots for cycling prevention
         *
         * Strategy:
         * 1. After 30 degenerate pivots: apply bound perturbation
         * 2. After 100 more degenerate pivots: switch to Bland's rule
         * 3. After 100 non-degenerate pivots: reset and try faster methods
         */
        const double NEAR_DEGEN_TOL = 1e-3;
        const int PERTURB_THRESHOLD = 30;

        if (theta < NEAR_DEGEN_TOL) {
            degenerate_count++;
            non_degen_streak = 0;

            /* First try perturbation */
            if (degenerate_count >= PERTURB_THRESHOLD && !perturbation_active && !use_bland) {
                primal_apply_perturbation(tab);
                perturbation_active = 1;
                if (solver->verbose) {
                    printf("Iter %d: Applying perturbation due to degeneracy\n", iter);
                }
            }

            /* If still cycling after perturbation, use Bland's rule */
            if (degenerate_count >= DEGEN_THRESHOLD && !use_bland) {
                use_bland = 1;
                if (solver->verbose) {
                    printf("Iter %d: Switching to Bland's rule due to potential cycling\n", iter);
                }
            }
        } else {
            /* Only reset after many consecutive non-degenerate pivots */
            non_degen_streak++;
            degenerate_count = 0;
            if (use_bland && non_degen_streak >= NON_DEGEN_THRESHOLD) {
                use_bland = 0;
                non_degen_streak = 0;
                if (solver->verbose) {
                    printf("Iter %d: Turning off Bland's rule after %d non-degenerate pivots\n",
                           iter, NON_DEGEN_THRESHOLD);
                }
            }
        }

        /* Perform pivot */
        if (simplex_pivot(tab, entering, leaving, theta) != 0) {
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }

        /* Refactorize if needed */
        if (lu_needs_refactorization(tab->lu)) {
            if (tableau_refactorize(tab) != 0) {
                primal_remove_perturbation(tab);
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            /* After refactorization, recompute solution to eliminate drift */
            tableau_compute_solution(tab);
            /* For partial pricing, use lazy RC computation (duals only).
             * For other strategies, compute full RC for incremental updates. */
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);  /* Lazy mode: duals only */
            } else {
                tableau_compute_reduced_costs(tab);  /* Full RC for incremental updates */
            }
        }

        /* Periodically recompute solution and reduced costs to correct numerical drift */
        if (iter > 0 && iter % 200 == 0) {
            tableau_compute_solution(tab);
            /* For partial pricing, use lazy RC. For others, full RC. */
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);  /* Lazy mode */
            } else {
                tableau_compute_reduced_costs(tab);  /* Full recomputation */
            }
            if (solver->verbose) {
                int leave_var = (leaving >= 0) ? tab->basis[leaving] : leaving;
                printf("Iter %d: obj = %.6f, enter=%d, leave=%d, theta=%.2e, rc=%.2e\n",
                       iter, tab->obj_value, entering, leave_var, theta, tab->rc[entering]);
            }
        }
    }

    primal_remove_perturbation(tab);
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

    /* Only use steepest edge weights for strategies that need them (1=SE, 2=Devex) */
    tab->use_steepest_edge = (solver->pricing_strategy == 1 || solver->pricing_strategy == 2);
    tab->pricing_strategy = solver->pricing_strategy;

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

        /* Compute true objective from original variables only.
         * This is more reliable than subtracting artificial contributions
         * from tab->obj_value, which can have numerical issues. */
        double true_obj = 0.0;
        for (int j = 0; j < num_struct; j++) {
            true_obj += tab->c_ext[j] * tab->x[j];
        }

        /* Check if any artificial variables (Big-M cost) have significant non-zero values.
         * This indicates the original problem is infeasible. */
        for (int j = num_struct; j < tab->n; j++) {
            if (tab->c_ext[j] > 1e6 && fabs(tab->x[j]) > RALPH_FEAS_TOL) {
                artificial_contrib += tab->c_ext[j] * tab->x[j];
                artificial_count++;
            }
        }

        if (artificial_count > 0 && artificial_contrib > 1e-2) {
            /* Significant positive artificial contribution means infeasible */
            extract_farkas_ray(solver);
            solver->status = RALPH_STATUS_INFEASIBLE;
            return 0;
        }

        /* Use true objective computed from original variables */
        solver->obj_value = true_obj * solver->model->obj_sense;
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
