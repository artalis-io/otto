/*
 * Ralph - Dual Simplex Method Implementation
 *
 * The dual simplex maintains dual feasibility (optimality conditions)
 * while working to achieve primal feasibility. Essential for:
 * - Re-optimization after adding cuts
 * - Re-optimization after fixing variables (in MIP)
 * - Starting from dual feasible basis
 * - Fresh solves on problems where dual start is beneficial
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "lp.h"

/* Forward declarations */
SimplexTableau* tableau_create(LPModel *model);
int tableau_refactorize(SimplexTableau *tab);
int tableau_compute_solution(SimplexTableau *tab);
int tableau_compute_reduced_costs(SimplexTableau *tab);
void tableau_free(SimplexTableau *tab);

/* Bound perturbation for degeneracy prevention (defined below) */
static void apply_bound_perturbation(SimplexTableau *tab);
static void remove_bound_perturbation(SimplexTableau *tab);

/* Forward declaration for dual feasibility function */
static int make_dual_feasible(SimplexTableau *tab, int obj_sense);

/*
 * Extract Farkas ray (certificate of infeasibility) for dual simplex.
 *
 * When dual simplex detects infeasibility (no entering variable found),
 * the current dual values y = c_B' * B^{-1} provide a Farkas certificate:
 *   y'A >= 0 for all columns (at appropriate bounds)
 *   y'b < 0
 *
 * This proves no feasible solution exists via Farkas lemma.
 */
static void extract_farkas_ray_dual(SimplexSolver *solver) {
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

    /* Ensure we have fresh dual values */
    tableau_compute_reduced_costs(tab);

    /* Copy the dual values - these are the Farkas multipliers */
    for (int i = 0; i < m; i++) {
        solver->farkas_ray[i] = tab->y[i];
    }

    solver->farkas_valid = 1;
}

/* ============================================================================
 * Dual Ratio Test
 * ============================================================================ */

/*
 * Harris Ratio Test for Dual Simplex
 *
 * Uses a single-pass algorithm with Harris-style tie-breaking:
 * - Accept any ratio within tolerance of the best found so far
 * - Among near-equal ratios, prefer larger pivot elements
 *
 * Harris tolerance allows slightly suboptimal ratios if they provide
 * numerically more stable pivot elements.
 */
#define HARRIS_TOL 1e-6

/* Select entering variable using Harris dual ratio test */
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

        /* Compute alpha_j = (B^{-1} * a_j)[leaving] = alpha' * a_j using sparse dot */
        double alpha_j = sparse_dot_column(tab->A_ext, j, tab->work2);

        if (fabs(alpha_j) < RALPH_PIVOT_TOL) continue;

        double rc_j = tab->rc[j];
        double ratio = RALPH_INFINITY;

        /* Dual ratio test depends on direction and variable bound status.
         *
         * For internal minimization, dual feasibility requires:
         * - At lower bound: rc >= 0
         * - At upper bound: rc <= 0
         *
         * After pivot, the leaving variable's new rc = -rc_entering / pivot.
         * For dual feasibility at the leaving var's new bound, we need this >= 0
         * (since leaving goes to lower when below its bound).
         *
         * The ratio = -rc_j / alpha_j represents the new rc for the leaving variable.
         * We select the minimum non-negative ratio.
         */
        if (dir > 0) {
            /* Leaving variable needs to increase (currently below lower bound) */
            if (alpha_j < -RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j from lower bound increases x_B[leaving] - good! */
                /* x_j at lower has rc_j >= 0, alpha_j < 0, so -rc_j/alpha_j >= 0 */
                ratio = -rc_j / alpha_j;
            } else if (alpha_j > RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j from upper bound increases x_B[leaving] - good! */
                /* x_j at upper has rc_j <= 0, alpha_j > 0, so -rc_j/alpha_j >= 0 */
                ratio = -rc_j / alpha_j;
            }
        } else {
            /* Leaving variable needs to decrease (currently above upper bound) */
            if (alpha_j > RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j from lower bound decreases x_B[leaving] - good! */
                ratio = -rc_j / alpha_j;
            } else if (alpha_j < -RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j from upper bound decreases x_B[leaving] - good! */
                ratio = -rc_j / alpha_j;
            }
        }

        if (ratio >= -RALPH_OPT_TOL && ratio < *theta) {
            /* Better ratio found */
            *theta = ratio;
            *entering = j;
            best_pivot = fabs(alpha_j);
        } else if (ratio >= -RALPH_OPT_TOL && ratio < *theta + HARRIS_TOL * (1.0 + fabs(*theta))) {
            /* Harris: ratio is within tolerance of best - prefer larger pivot */
            if (fabs(alpha_j) > best_pivot) {
                *entering = j;
                best_pivot = fabs(alpha_j);
                /* Keep *theta as minimum ratio for dual feasibility */
            }
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

    /* Compute entering column in basis representation using sparse solve */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);
    lu_solve_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work3);  /* d = B^{-1} * a_entering */

    double pivot = tab->work3[leaving];

    /* Save rc_entering BEFORE updating reduced costs (needed for bound selection) */
    double rc_entering_orig = tab->rc[entering];

    /* Compute pivot row = e_leaving^T * B^{-1} via single BTRAN
     * This is MUCH more efficient than calling lu_solve_sparse for each column.
     * The pivot row gives us (B^{-1} * a_j)[leaving] for any j via a sparse dot product.
     */
    vec_set_zero(tab->work1, tab->m);
    tab->work1[leaving] = 1.0;
    lu_solve_transpose(tab->lu, tab->work1, tab->work2);  /* work2 = pivot_row */

    /* Update all reduced costs using sparse dot products:
     * rc'[j] = rc[j] - (rc_entering / pivot) * (pivot_row * a_j)
     */
    double rc_factor = tab->rc[entering] / pivot;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) {
            tab->rc[j] = 0.0;
        } else if (j != entering) {
            /* Compute pivot_row * a_j via sparse dot product */
            double dot = sparse_dot_column(tab->A_ext, j, tab->work2);
            tab->rc[j] -= rc_factor * dot;
        }
    }
    tab->rc[leaving_var] = -rc_factor;
    tab->rc[entering] = 0.0;

    /* Determine step size from infeasibility.
     * The basic variable update is: x_B = x_B - delta * d
     * where d = B^{-1} * a_entering.
     * For the leaving var: x_leave_new = x_leave - delta * pivot
     * We want x_leave_new = bound, so delta = (x_leave - bound) / pivot
     */
    double x_leave = tab->x[leaving_var];
    double step;
    if (x_leave < tab->lb_ext[leaving_var]) {
        step = (x_leave - tab->lb_ext[leaving_var]) / pivot;
    } else {
        step = (x_leave - tab->ub_ext[leaving_var]) / pivot;
    }

    /* Update primal solution */
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] -= step * tab->work3[k];
    }

    /* Update entering variable.
     * When entering is at lower bound and alpha < 0 (increases leaving), step > 0
     * When entering is at upper bound and alpha > 0 (increases leaving), step < 0
     * So: x_entering = bound + step (works for both cases)
     */
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        tab->x[entering] = tab->lb_ext[entering] + step;
    } else {
        tab->x[entering] = tab->ub_ext[entering] + step;
    }

    /* Update basis */
    tab->basis[leaving] = entering;
    tab->basis_pos[entering] = leaving;
    tab->basis_pos[leaving_var] = -1;

    tab->var_status[entering] = RALPH_BASIC;

    /* Set leaving variable to appropriate bound for DUAL feasibility.
     * The new reduced cost of leaving is rc_leaving_new = -rc_entering / pivot.
     * For dual feasibility (internal minimization):
     * - If rc_leaving_new >= 0, go to lower bound
     * - If rc_leaving_new < 0, go to upper bound
     * NOTE: Use rc_entering_orig saved before zeroing rc[entering]
     */
    double rc_leaving_new = -rc_entering_orig / pivot;
    if (rc_leaving_new >= -RALPH_OPT_TOL) {
        /* Go to lower bound */
        if (tab->lb_ext[leaving_var] > -RALPH_INFINITY/2) {
            tab->var_status[leaving_var] = RALPH_NONBASIC_LOWER;
            tab->x[leaving_var] = tab->lb_ext[leaving_var];
        } else {
            /* No lower bound - this shouldn't happen if ratio test is correct */
            tab->var_status[leaving_var] = RALPH_NONBASIC_UPPER;
            tab->x[leaving_var] = tab->ub_ext[leaving_var];
        }
    } else {
        /* Go to upper bound */
        if (tab->ub_ext[leaving_var] < RALPH_INFINITY/2) {
            tab->var_status[leaving_var] = RALPH_NONBASIC_UPPER;
            tab->x[leaving_var] = tab->ub_ext[leaving_var];
        } else {
            /* No upper bound - this shouldn't happen if ratio test is correct */
            tab->var_status[leaving_var] = RALPH_NONBASIC_LOWER;
            tab->x[leaving_var] = tab->lb_ext[leaving_var];
        }
    }

    /* Update LU factorization */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    if (lu_update(tab->lu, leaving, tab->work1) != 0) {
        if (tableau_refactorize(tab) != 0) {
            return -1;
        }
    }

    /* Recompute objective value */
    tab->obj_value = 0.0;
    for (int j = 0; j < tab->n; j++) {
        tab->obj_value += tab->c_ext[j] * tab->x[j];
    }

    return 0;
}

/* ============================================================================
 * Dual Simplex Algorithm
 * ============================================================================ */

int dual_simplex_solve(SimplexSolver *solver) {
    if (!solver) return -1;

    /* If no tableau exists, we need to initialize with primal simplex first.
     * This creates the tableau, runs Phase 1 to find feasibility, then we
     * can switch to dual for any subsequent re-optimizations.
     */
    if (!solver->tableau) {
        /* Use primal simplex for full initialization */
        return simplex_solve(solver);
    }

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
        /* Try to achieve dual feasibility by flipping non-basic variables */
        int changes = make_dual_feasible(tab, solver->model->obj_sense);

        if (changes > 0) {
            /* Recompute solution and reduced costs after flipping */
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);

            /* Re-check dual feasibility */
            dual_feasible = 1;
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

            if (solver->verbose && dual_feasible) {
                printf("[dual_simplex] Achieved dual feasibility after %d bound flips\n", changes);
            }
        }

        if (!dual_feasible) {
            /* Still not dual feasible - fall back to primal */
            return simplex_solve(solver);
        }
    }

    /* Apply bound perturbation for cycling prevention */
    apply_bound_perturbation(tab);

    /* Degeneracy and stalling detection for cycling prevention */
    int degenerate_count = 0;
    const int DEGEN_PERTURB_THRESHOLD = 15;

    /* Stalling detection: track if objective doesn't change */
    double last_obj = tab->obj_value;
    int stall_count = 0;
    const int STALL_THRESHOLD = 50;  /* Iterations without progress */
    int perturb_attempts = 0;
    const int MAX_PERTURB_ATTEMPTS = 3;

    /* Track iterations with dual violations to detect lack of progress */
    int no_progress_count = 0;

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
            /* Primal feasible - optimal! Remove perturbation and finalize */
            remove_bound_perturbation(tab);
            tableau_compute_solution(tab);
            solver->status = RALPH_STATUS_OPTIMAL;
            solver->obj_value = tab->obj_value * solver->model->obj_sense;
            return 0;
        }

        /* Dual ratio test to find entering variable */
        int entering;
        double theta;

        if (dual_ratio_test(tab, leaving, &entering, &theta) != 0) {
            /* No valid entering variable - infeasible */
            remove_bound_perturbation(tab);
            extract_farkas_ray_dual(solver);
            solver->status = RALPH_STATUS_INFEASIBLE;
            return 0;
        }

        /* Perform dual pivot */
        if (dual_simplex_pivot(tab, entering, leaving, theta) != 0) {
            /* Pivot failed, try refactorization */
            if (tableau_refactorize(tab) != 0) {
                remove_bound_perturbation(tab);
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
        }

        /* Detect degenerate pivots (theta ≈ 0 means no dual objective change) */
        if (fabs(theta) < RALPH_FEAS_TOL) {
            degenerate_count++;
            if (degenerate_count >= DEGEN_PERTURB_THRESHOLD) {
                /* Re-apply perturbation to break cycling */
                apply_bound_perturbation(tab);
                degenerate_count = 0;
            }
        } else {
            degenerate_count = 0;
        }

        /* Stalling detection: objective not improving significantly
         * Use generous tolerance to detect "effectively stalled" states */
        double obj_tol = 1e-4 * (1.0 + fabs(last_obj));
        double obj_change = fabs(tab->obj_value - last_obj);
        if (obj_change < obj_tol) {
            stall_count++;
            if (stall_count >= STALL_THRESHOLD) {
                perturb_attempts++;
                if (perturb_attempts <= MAX_PERTURB_ATTEMPTS) {
                    /* Re-apply perturbation to break stalling */
                    remove_bound_perturbation(tab);
                    apply_bound_perturbation(tab);
                    stall_count = 0;
                    if (solver->verbose) {
                        printf("Dual iter %d: stalled (Δobj=%.2e), re-perturbing (attempt %d)\n",
                               iter, obj_change, perturb_attempts);
                    }
                } else {
                    /* Too many perturbation attempts - give up on dual */
                    if (solver->verbose) {
                        printf("Dual iter %d: stalled after %d perturb attempts, falling back\n",
                               iter, perturb_attempts);
                    }
                    remove_bound_perturbation(tab);
                    tableau_free(solver->tableau);
                    solver->tableau = NULL;
                    return simplex_solve(solver);
                }
            }
        } else {
            stall_count = 0;
            last_obj = tab->obj_value;
        }

        /* Check for dual feasibility violations */
        int dual_violations = 0;
        for (int j = 0; j < tab->n; j++) {
            if (tab->var_status[j] == RALPH_BASIC) continue;
            double rc = tab->rc[j];
            if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
                dual_violations++;
            }
            if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
                dual_violations++;
            }
        }

        /* Fall back to primal if too many dual violations */
        if (dual_violations > tab->n / 5) {
            remove_bound_perturbation(tab);
            tableau_free(solver->tableau);
            solver->tableau = NULL;
            return simplex_solve(solver);
        }

        /* Track if dual violations are improving.
         * If we consistently have dual violations without reducing them, fall back. */
        if (dual_violations > 0) {
            no_progress_count++;
            /* If we've had 200+ iterations with persistent dual violations, give up */
            if (no_progress_count >= 200) {
                if (solver->verbose) {
                    printf("Dual iter %d: persistent %d violations, falling back\n",
                           iter, dual_violations);
                }
                remove_bound_perturbation(tab);
                tableau_free(solver->tableau);
                solver->tableau = NULL;
                return simplex_solve(solver);
            }
        } else {
            no_progress_count = 0;  /* Reset when we have no violations */
        }

        /* Fall back early if taking too many iterations (5x problem size) */
        if (iter > 5 * tab->m && iter % 100 == 0) {
            if (solver->verbose) {
                printf("Dual iter %d: too many iterations, falling back to primal\n", iter);
            }
            remove_bound_perturbation(tab);
            tableau_free(solver->tableau);
            solver->tableau = NULL;
            return simplex_solve(solver);
        }

        /* Numerical stability: refactorize and recompute RCs periodically */
        int need_refactor = lu_needs_refactorization(tab->lu) ||
                           (iter > 0 && iter % 50 == 0);

        if (need_refactor) {
            if (tableau_refactorize(tab) != 0) {
                remove_bound_perturbation(tab);
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        } else if (iter > 0 && iter % 20 == 0) {
            /* Periodic RC recomputation without full refactorization */
            tableau_compute_reduced_costs(tab);
        }

        if (solver->verbose && iter % 100 == 0) {
            printf("Dual iter %d: infeas = %.6e, obj = %.6f, dual_viol=%d\n",
                   iter, max_infeas, tab->obj_value, dual_violations);
        }
    }

    remove_bound_perturbation(tab);
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

/* ============================================================================
 * True Dual Phase 1 - Initialize for Dual Simplex from Scratch
 * ============================================================================ */

/*
 * Initialize tableau for dual simplex by achieving dual feasibility.
 *
 * The reduced costs stored in tab->rc are for the INTERNAL minimization problem.
 * For dual feasibility of the internal minimization:
 *   - Variables at lower bound need rc >= 0
 *   - Variables at upper bound need rc <= 0
 *
 * Strategy: Flip non-basic variables to the bound that satisfies dual feasibility.
 * After flipping, basic variable values are recomputed and may become infeasible,
 * which dual Phase 2 will fix.
 */
static int make_dual_feasible(SimplexTableau *tab, int obj_sense) {
    (void)obj_sense;  /* Not needed - rc is already for internal minimization */

    /* Compute reduced costs with current basis */
    tableau_compute_reduced_costs(tab);

    int changes = 0;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        double rc = tab->rc[j];
        double lb = tab->lb_ext[j];
        double ub = tab->ub_ext[j];

        /*
         * For internal minimization:
         * - At lower bound: need rc >= 0 (else variable wants to increase)
         * - At upper bound: need rc <= 0 (else variable wants to decrease)
         */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            /* Dual infeasible at lower bound, try to flip to upper */
            if (ub < RALPH_INFINITY/2) {
                tab->x[j] = ub;
                tab->var_status[j] = RALPH_NONBASIC_UPPER;
                changes++;
            }
            /* else: can't flip, will need Phase 1 pivots to fix */
        }
        else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            /* Dual infeasible at upper bound, try to flip to lower */
            if (lb > -RALPH_INFINITY/2) {
                tab->x[j] = lb;
                tab->var_status[j] = RALPH_NONBASIC_LOWER;
                changes++;
            }
            /* else: can't flip, will need Phase 1 pivots to fix */
        }
    }

    return changes;
}

/*
 * Bound Perturbation for Degeneracy Prevention
 *
 * Adds small perturbations to upper bounds to break degeneracy and prevent
 * cycling. Uses pseudo-random perturbations based on variable index to ensure
 * reproducibility. Perturbations are removed before returning the final solution.
 *
 * Uses per-tableau storage in work4 array instead of static storage to be
 * safe for concurrent use and multiple tableaux.
 */
#define PERTURB_BASE 1e-4  /* Larger perturbation to break cycles more aggressively */
#define PERTURB_MULT 7  /* Prime for pseudo-randomness */

static void apply_bound_perturbation(SimplexTableau *tab) {
    /* Allocate backup storage if needed */
    if (!tab->perturb_backup) {
        tab->perturb_backup = (double*)malloc(tab->n * sizeof(double));
        if (!tab->perturb_backup) return;
    }

    for (int j = 0; j < tab->n; j++) {
        tab->perturb_backup[j] = tab->ub_ext[j];

        /* Only perturb finite upper bounds */
        if (tab->ub_ext[j] < RALPH_INFINITY / 2) {
            /* Pseudo-random perturbation: eps * (1 + (j*7) mod 13) */
            double eps = PERTURB_BASE * (1.0 + fabs(tab->ub_ext[j]));
            tab->ub_ext[j] += eps * (1.0 + (j * PERTURB_MULT) % 13);
        }
    }
}

static void remove_bound_perturbation(SimplexTableau *tab) {
    if (!tab->perturb_backup) return;

    for (int j = 0; j < tab->n; j++) {
        tab->ub_ext[j] = tab->perturb_backup[j];

        /* Snap non-basic variables at upper bound to original bound */
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            tab->x[j] = tab->perturb_backup[j];
        }
    }
}

/*
 * Solve LP using dual simplex from scratch.
 *
 * This is the proper dual Phase 1 approach:
 * 1. Create tableau with slack basis
 * 2. Adjust non-basic variables to achieve dual feasibility
 * 3. Recompute basic variable values (may be primal infeasible)
 * 4. Run dual simplex to achieve primal feasibility
 */
int dual_simplex_solve_from_scratch(SimplexSolver *solver) {
    if (!solver || !solver->model) return -1;

    clock_t start = clock();

    if (solver->verbose) {
        printf("[dual_simplex] Starting from scratch...\n");
    }

    /* Create tableau if needed */
    if (!solver->tableau) {
        solver->tableau = tableau_create(solver->model);
        if (!solver->tableau) {
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }
    }

    SimplexTableau *tab = solver->tableau;

    /* Factorize initial basis (slacks) */
    if (tableau_refactorize(tab) != 0) {
        solver->status = RALPH_STATUS_ERROR;
        return -1;
    }

    /* Apply bound perturbation for degeneracy prevention */
    apply_bound_perturbation(tab);

    /* Compute initial solution and reduced costs */
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    if (solver->verbose) {
        printf("[dual_simplex] Initial: obj=%.2f\n", tab->obj_value);
    }

    /* Adjust variable bounds to achieve dual feasibility */
    int changes = make_dual_feasible(tab, solver->model->obj_sense);

    if (solver->verbose) {
        printf("[dual_simplex] Moved %d variables for dual feasibility\n", changes);
    }

    /* Recompute basic variable values after bound changes */
    if (changes > 0) {
        tableau_compute_solution(tab);
        tableau_compute_reduced_costs(tab);
    }

    /* Check if we achieved dual feasibility */
    int dual_infeasible = 0;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;
        double rc = tab->rc[j];
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            dual_infeasible = 1;
            break;
        }
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            dual_infeasible = 1;
            break;
        }
    }

    if (dual_infeasible) {
        /* Bound flipping wasn't enough - fall back to primal simplex */
        if (solver->verbose) {
            printf("[dual_simplex] Could not achieve dual feasibility, falling back to primal\n");
        }
        return simplex_solve(solver);
    }

    /* Check if we're already primal feasible */
    int primal_infeasible = 0;
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL ||
            tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
            primal_infeasible = 1;
            break;
        }
    }

    if (!primal_infeasible) {
        /* Already optimal! Remove perturbation and finalize */
        remove_bound_perturbation(tab);
        tableau_compute_solution(tab);

        solver->status = RALPH_STATUS_OPTIMAL;
        solver->obj_value = tab->obj_value * solver->model->obj_sense;
        solver->iterations = 0;

        /* Copy solution */
        int n_orig = solver->model->num_vars;
        if (!solver->solution) {
            solver->solution = (double*)malloc(n_orig * sizeof(double));
        }
        if (solver->solution) {
            for (int j = 0; j < n_orig; j++) {
                solver->solution[j] = tab->x[j];
            }
        }

        solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
        return 0;
    }

    if (solver->verbose) {
        printf("[dual_simplex] Running dual Phase 2 to achieve primal feasibility...\n");
    }

    /* Degeneracy tracking for cycling prevention.
     * In dual simplex, a degenerate pivot has theta ≈ 0 (no dual objective change).
     * Too many consecutive degenerate pivots suggests cycling.
     */
    int degenerate_count = 0;
    const int DEGEN_PERTURB_THRESHOLD = 15;  /* Re-perturb after this many degenerate pivots */

    /* Run dual simplex Phase 2 */
    for (int iter = 0; iter < solver->max_iterations; iter++) {
        solver->iterations = iter;

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
            /* Primal feasible - optimal! Remove perturbation and finalize */
            remove_bound_perturbation(tab);
            tableau_compute_solution(tab);

            solver->status = RALPH_STATUS_OPTIMAL;
            solver->obj_value = tab->obj_value * solver->model->obj_sense;

            /* Copy solution */
            int n_orig = solver->model->num_vars;
            if (!solver->solution) {
                solver->solution = (double*)malloc(n_orig * sizeof(double));
            }
            if (solver->solution) {
                for (int j = 0; j < n_orig; j++) {
                    solver->solution[j] = tab->x[j];
                }
            }

            solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            return 0;
        }

        /* Dual ratio test */
        int entering;
        double theta;

        if (dual_ratio_test(tab, leaving, &entering, &theta) != 0 || entering < 0) {
            /* No valid entering variable - problem is infeasible */
            extract_farkas_ray_dual(solver);
            solver->status = RALPH_STATUS_INFEASIBLE;
            solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            return 0;
        }

        /* Perform dual pivot */
        if (dual_simplex_pivot(tab, entering, leaving, theta) != 0) {
            /* Pivot failed, try refactorization */
            if (tableau_refactorize(tab) != 0) {
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
        }

        /* Detect degenerate pivots (theta ≈ 0 means no dual objective change).
         * Too many consecutive degenerate pivots indicates potential cycling.
         */
        if (fabs(theta) < RALPH_FEAS_TOL) {
            degenerate_count++;
            if (degenerate_count >= DEGEN_PERTURB_THRESHOLD) {
                /* Re-apply perturbation to break cycling */
                apply_bound_perturbation(tab);
                degenerate_count = 0;
                if (solver->verbose) {
                    printf("[dual_simplex] Iter %d: Re-applying perturbation due to degeneracy\n", iter);
                }
            }
        } else {
            degenerate_count = 0;
        }

        /* Check for dual infeasibility */
        int dual_violations = 0;
        for (int j = 0; j < tab->n; j++) {
            if (tab->var_status[j] == RALPH_BASIC) continue;
            double rc = tab->rc[j];
            if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
                dual_violations++;
            }
            if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
                dual_violations++;
            }
        }

        /* If too many dual violations, fall back to primal (with higher threshold) */
        if (dual_violations > tab->n / 5) {  /* 20% threshold (was 10%) */
            if (solver->verbose) {
                printf("[dual_simplex] Too many dual violations (%d), falling back to primal\n",
                       dual_violations);
            }
            /* Reset tableau and use primal simplex */
            tableau_free(solver->tableau);
            solver->tableau = NULL;
            return simplex_solve(solver);
        }

        /* Fall back early if taking too many iterations (10x the problem size) */
        if (iter > 10 * tab->m && iter % 100 == 0) {
            if (solver->verbose) {
                printf("[dual_simplex] Too many iterations (%d), falling back to primal\n", iter);
            }
            tableau_free(solver->tableau);
            solver->tableau = NULL;
            return simplex_solve(solver);
        }

        /* More aggressive numerical stability:
         * - Refactorize when LU needs it or every 50 iterations
         * - Recompute reduced costs every 20 iterations (even without refactorization)
         *   to catch drift before it accumulates to problematic levels */
        int need_refactor = lu_needs_refactorization(tab->lu) ||
                           (iter > 0 && iter % 50 == 0);

        if (need_refactor) {
            if (tableau_refactorize(tab) != 0) {
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        } else if (iter > 0 && iter % 20 == 0) {
            /* Periodic RC recomputation without full refactorization */
            tableau_compute_reduced_costs(tab);
        }

        if (solver->verbose && iter % 50 == 0) {
            printf("Dual iter %d: infeas=%.2e, obj=%.2f, dual_viol=%d\n",
                   iter, max_infeas, tab->obj_value * solver->model->obj_sense, dual_violations);
        }
    }

    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    return -1;
}
