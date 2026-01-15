/*
 * Ralph - Dual Simplex Method Implementation
 *
 * The dual simplex maintains dual feasibility (optimality conditions)
 * while working to achieve primal feasibility. Essential for:
 * - Re-optimization after adding cuts
 * - Re-optimization after fixing variables (in MIP)
 * - Starting from dual feasible basis
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "lp.h"

/* ============================================================================
 * Dual Ratio Test
 * ============================================================================ */

/* Select entering variable using dual ratio test */
int dual_ratio_test(SimplexTableau *tab, int leaving, int *entering, double *theta) {
    int leaving_var = tab->basis[leaving];
    double x_leave = tab->x[leaving_var];

    /* Determine direction based on which bound is violated */
    int dir;  /* +1 if leaving increases, -1 if decreases */
    if (x_leave < tab->lb_ext[leaving_var] - RALPH_FEAS_TOL) {
        dir = 1;  /* Need to increase */
    } else if (x_leave > tab->ub_ext[leaving_var] + RALPH_FEAS_TOL) {
        dir = -1;  /* Need to decrease */
    } else {
        return -1;  /* Not infeasible */
    }

    /* Compute leaving row of basis inverse: e_leaving' * B^{-1} */
    vec_set_zero(tab->work1, tab->m);
    tab->work1[leaving] = 1.0;
    lu_solve_transpose(tab->lu, tab->work1, tab->work2);  /* alpha = B^{-T} * e_leaving */

    /* Find entering variable by dual ratio test */
    *entering = -1;
    *theta = RALPH_INFINITY;
    double best_pivot = 0.0;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        /* Compute alpha_j = (B^{-1} * a_j)[leaving] = alpha' * a_j */
        sparse_get_column(tab->A_ext, j, tab->work1);
        double alpha_j = vec_dot(tab->m, tab->work2, tab->work1);

        if (fabs(alpha_j) < RALPH_PIVOT_TOL) continue;

        double rc_j = tab->rc[j];
        double ratio = RALPH_INFINITY;

        /* Dual ratio test depends on direction and variable bound status */
        if (dir > 0) {
            /* Leaving variable needs to increase */
            /* x_B[leaving] = ... - alpha_j * delta, so need alpha_j > 0 for increase */
            /* Wait, the formula is x_B = B^{-1}b - B^{-1}N x_N */
            /* When x_j increases by delta, x_B[k] changes by -alpha_k[j] * delta */

            if (alpha_j > RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j from lower bound, reduces x_B[leaving] */
                /* We need x_B[leaving] to increase, so this is wrong direction */
            } else if (alpha_j < -RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j increases x_B[leaving] - good! */
                /* Maintain dual feasibility: rc_j >= 0 for x_j at lower bound */
                /* After pivot: rc_j' = rc_j - (rc_leaving / alpha_leaving) * alpha_j */
                /* For j at lower bound, need rc_j' >= 0 */
                ratio = -rc_j / alpha_j;  /* Should be non-negative for valid pivot */
            } else if (alpha_j > RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j from upper bound increases x_B[leaving] - good! */
                ratio = rc_j / alpha_j;
            } else if (alpha_j < -RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Wrong direction */
            }
        } else {
            /* Leaving variable needs to decrease */
            if (alpha_j > RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j decreases x_B[leaving] - good! */
                ratio = -rc_j / alpha_j;
            } else if (alpha_j < -RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j decreases x_B[leaving] - good! */
                ratio = rc_j / alpha_j;
            }
        }

        if (ratio >= -RALPH_OPT_TOL && ratio < *theta) {
            *theta = ratio;
            *entering = j;
            best_pivot = fabs(alpha_j);
        } else if (fabs(ratio - *theta) < RALPH_OPT_TOL && fabs(alpha_j) > best_pivot) {
            /* Tie-breaking: prefer larger pivot */
            *entering = j;
            best_pivot = fabs(alpha_j);
        }
    }

    if (*entering < 0) {
        return -1;  /* Dual infeasible (primal unbounded) */
    }

    return 0;
}

/* ============================================================================
 * Dual Simplex Iteration
 * ============================================================================ */

static int dual_simplex_pivot(SimplexTableau *tab, int entering, int leaving, double theta) {
    int leaving_var = tab->basis[leaving];

    /* Compute entering column in basis representation */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    lu_solve(tab->lu, tab->work1, tab->work3);  /* d = B^{-1} * a_entering */

    double pivot = tab->work3[leaving];

    /* Update reduced costs */
    double rc_leaving = tab->rc[entering] / pivot;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) {
            tab->rc[j] = 0.0;
        } else if (j != entering) {
            sparse_get_column(tab->A_ext, j, tab->work1);
            lu_solve(tab->lu, tab->work1, tab->work2);
            tab->rc[j] -= rc_leaving * tab->work2[leaving];
        }
    }
    tab->rc[leaving_var] = -rc_leaving;
    tab->rc[entering] = 0.0;

    /* Determine step size from infeasibility */
    double x_leave = tab->x[leaving_var];
    double step;
    if (x_leave < tab->lb_ext[leaving_var]) {
        step = (tab->lb_ext[leaving_var] - x_leave) / pivot;
    } else {
        step = (tab->ub_ext[leaving_var] - x_leave) / pivot;
    }

    /* Update primal solution */
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] -= step * tab->work3[k];
    }

    /* Update entering variable */
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        tab->x[entering] = tab->lb_ext[entering] + step;
    } else {
        tab->x[entering] = tab->ub_ext[entering] - step;
    }

    /* Update basis */
    tab->basis[leaving] = entering;
    tab->basis_pos[entering] = leaving;
    tab->basis_pos[leaving_var] = -1;

    tab->var_status[entering] = RALPH_BASIC;

    /* Set leaving variable to appropriate bound */
    if (x_leave < tab->lb_ext[leaving_var]) {
        tab->var_status[leaving_var] = RALPH_NONBASIC_LOWER;
        tab->x[leaving_var] = tab->lb_ext[leaving_var];
    } else {
        tab->var_status[leaving_var] = RALPH_NONBASIC_UPPER;
        tab->x[leaving_var] = tab->ub_ext[leaving_var];
    }

    /* Update LU factorization */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    if (lu_update(tab->lu, leaving, tab->work1) != 0) {
        if (tableau_refactorize(tab) != 0) {
            return -1;
        }
    }

    return 0;
}

/* ============================================================================
 * Dual Simplex Algorithm
 * ============================================================================ */

int dual_simplex_solve(SimplexSolver *solver) {
    if (!solver || !solver->tableau) return -1;

    SimplexTableau *tab = solver->tableau;

    /* Ensure we have reduced costs */
    tableau_compute_reduced_costs(tab);

    /* Check dual feasibility */
    int dual_feasible = 1;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && tab->rc[j] < -RALPH_OPT_TOL) {
            dual_feasible = 0;
            break;
        }
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER && tab->rc[j] > RALPH_OPT_TOL) {
            dual_feasible = 0;
            break;
        }
    }

    if (!dual_feasible) {
        /* Need to achieve dual feasibility first - use primal */
        return simplex_solve(solver);
    }

    /* Main dual simplex loop */
    for (int iter = 0; iter < solver->max_iterations; iter++) {
        solver->iterations = iter;

        /* Compute primal solution */
        tableau_compute_solution(tab);

        /* Find most infeasible basic variable (leaving) */
        int leaving = -1;
        double max_infeas = RALPH_FEAS_TOL;

        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            double infeas = 0.0;

            if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL) {
                infeas = tab->lb_ext[j] - tab->x[j];
            } else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
                infeas = tab->x[j] - tab->ub_ext[j];
            }

            if (infeas > max_infeas) {
                max_infeas = infeas;
                leaving = k;
            }
        }

        if (leaving < 0) {
            /* Primal feasible - optimal */
            solver->status = RALPH_STATUS_OPTIMAL;
            solver->obj_value = tab->obj_value * solver->model->obj_sense;
            return 0;
        }

        /* Dual ratio test to find entering variable */
        int entering;
        double theta;

        if (dual_ratio_test(tab, leaving, &entering, &theta) != 0) {
            /* No valid entering variable - infeasible */
            solver->status = RALPH_STATUS_INFEASIBLE;
            return 0;
        }

        /* Perform dual pivot */
        if (dual_simplex_pivot(tab, entering, leaving, theta) != 0) {
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }

        /* Refactorize if needed */
        if (lu_needs_refactorization(tab->lu)) {
            if (tableau_refactorize(tab) != 0) {
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            tableau_compute_reduced_costs(tab);
        }

        if (solver->verbose && iter % 100 == 0) {
            printf("Dual iter %d: infeas = %.6e, obj = %.6f\n",
                   iter, max_infeas, tab->obj_value);
        }
    }

    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    return -1;
}

/* ============================================================================
 * Warm Start with Dual Simplex
 * ============================================================================ */

/* Re-optimize after changing bounds (useful for branch and bound) */
int dual_simplex_reoptimize(SimplexSolver *solver, int var, double new_lb, double new_ub) {
    if (!solver || !solver->tableau) return -1;

    SimplexTableau *tab = solver->tableau;

    /* Update bounds */
    double old_lb = tab->lb_ext[var];
    double old_ub = tab->ub_ext[var];

    tab->lb_ext[var] = new_lb;
    tab->ub_ext[var] = new_ub;

    /* If variable is basic, might become infeasible */
    if (tab->var_status[var] == RALPH_BASIC) {
        /* Need to recompute solution */
        tableau_compute_solution(tab);
    } else {
        /* Non-basic: push to bound if necessary */
        if (tab->var_status[var] == RALPH_NONBASIC_LOWER && tab->x[var] < new_lb) {
            tab->x[var] = new_lb;
        } else if (tab->var_status[var] == RALPH_NONBASIC_UPPER && tab->x[var] > new_ub) {
            tab->x[var] = new_ub;
        } else if (tab->x[var] < new_lb) {
            tab->x[var] = new_lb;
            tab->var_status[var] = RALPH_NONBASIC_LOWER;
        } else if (tab->x[var] > new_ub) {
            tab->x[var] = new_ub;
            tab->var_status[var] = RALPH_NONBASIC_UPPER;
        }

        /* Recompute solution to update basic variables */
        tableau_compute_solution(tab);
    }

    /* Run dual simplex to restore optimality */
    return dual_simplex_solve(solver);
}
