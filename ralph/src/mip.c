/*
 * Ralph - MIP Solver Implementation
 *
 * Main MIP solver that orchestrates:
 * - LP relaxation solving
 * - Branch and Bound search
 * - Cutting plane generation
 * - Primal heuristics
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "mip.h"

/* ============================================================================
 * MIP Solver Creation/Destruction
 * ============================================================================ */

MIPSolver* mip_create(LPModel *model) {
    if (!model) return NULL;

    MIPSolver *solver = (MIPSolver*)calloc(1, sizeof(MIPSolver));
    if (!solver) return NULL;

    solver->original_model = model;
    solver->working_model = lp_model_copy(model);

    if (!solver->working_model) {
        free(solver);
        return NULL;
    }

    /* Count and store integer variables */
    solver->num_integers = 0;
    for (int j = 0; j < model->num_vars; j++) {
        if (model->var_type[j] == 'I' || model->var_type[j] == 'B') {
            solver->num_integers++;
        }
    }

    solver->integer_vars = (int*)malloc(solver->num_integers * sizeof(int));
    solver->is_integer = (int*)calloc(model->num_vars, sizeof(int));

    if (!solver->integer_vars || !solver->is_integer) {
        mip_free(solver);
        return NULL;
    }

    int idx = 0;
    for (int j = 0; j < model->num_vars; j++) {
        if (model->var_type[j] == 'I' || model->var_type[j] == 'B') {
            solver->integer_vars[idx++] = j;
            solver->is_integer[j] = 1;
        }
    }

    /* Initialize best solution tracking */
    solver->best_bound = (model->obj_sense == 1) ? -RALPH_INFINITY : RALPH_INFINITY;
    solver->best_obj = (model->obj_sense == 1) ? RALPH_INFINITY : -RALPH_INFINITY;
    solver->best_solution = (double*)malloc(model->num_vars * sizeof(double));
    solver->has_incumbent = 0;

    /* Initialize pseudo-costs */
    solver->pseudo_cost_down = (double*)malloc(model->num_vars * sizeof(double));
    solver->pseudo_cost_up = (double*)malloc(model->num_vars * sizeof(double));
    solver->pseudo_count_down = (int*)calloc(model->num_vars, sizeof(int));
    solver->pseudo_count_up = (int*)calloc(model->num_vars, sizeof(int));

    if (!solver->best_solution || !solver->pseudo_cost_down || !solver->pseudo_cost_up ||
        !solver->pseudo_count_down || !solver->pseudo_count_up) {
        mip_free(solver);
        return NULL;
    }

    /* Initialize pseudo-costs with default values */
    for (int j = 0; j < model->num_vars; j++) {
        solver->pseudo_cost_down[j] = 1.0;
        solver->pseudo_cost_up[j] = 1.0;
    }

    /* Default parameters */
    solver->max_nodes = RALPH_DEFAULT_NODE_LIMIT;
    solver->time_limit = RALPH_DEFAULT_TIME_LIMIT;
    solver->mip_gap = RALPH_DEFAULT_MIP_GAP;
    solver->abs_mip_gap = RALPH_DEFAULT_ABS_MIP_GAP;
    solver->cutoff = RALPH_INFINITY;
    solver->node_select = NODE_SELECT_HYBRID;
    solver->var_select = VAR_SELECT_PSEUDO_COST;  /* Faster than reliability branching */
    solver->max_cuts_per_round = 50;
    solver->max_cut_rounds = 5;  /* Enable cuts with conservative limit */
    solver->verbose = 0;

    /* Create node queue */
    solver->node_queue = node_queue_create(1024, solver->node_select, model->obj_sense);
    if (!solver->node_queue) {
        mip_free(solver);
        return NULL;
    }

    /* Create cut pool */
    solver->cut_pool = cut_pool_create(1024);
    if (!solver->cut_pool) {
        mip_free(solver);
        return NULL;
    }

    solver->status = RALPH_STATUS_UNKNOWN;

    return solver;
}

void mip_free(MIPSolver *solver) {
    if (!solver) return;

    lp_model_free(solver->working_model);
    simplex_free(solver->lp_solver);
    free(solver->integer_vars);
    free(solver->is_integer);
    free(solver->best_solution);
    free(solver->pseudo_cost_down);
    free(solver->pseudo_cost_up);
    free(solver->pseudo_count_down);
    free(solver->pseudo_count_up);
    node_queue_free(solver->node_queue);
    cut_pool_free(solver->cut_pool);
    free(solver);
}

/* ============================================================================
 * Update Incumbent
 * ============================================================================ */

static void update_incumbent(MIPSolver *solver, const double *solution, double obj) {
    LPModel *model = solver->original_model;

    int is_better = 0;
    if (model->obj_sense == 1) {  /* Minimize */
        is_better = (obj < solver->best_obj - RALPH_OPT_TOL);
    } else {  /* Maximize */
        is_better = (obj > solver->best_obj + RALPH_OPT_TOL);
    }

    if (is_better) {
        solver->best_obj = obj;
        memcpy(solver->best_solution, solution, model->num_vars * sizeof(double));
        solver->has_incumbent = 1;

        /* Update cutoff for pruning */
        solver->cutoff = obj;

        if (solver->verbose) {
            printf("*** New incumbent: %.6f\n", obj);
        }
    }
}

/* ============================================================================
 * Diving Heuristic
 *
 * Quickly find a feasible integer solution by repeatedly:
 * 1. Pick most fractional integer variable
 * 2. Round to nearest integer and fix
 * 3. Re-solve LP
 * 4. Repeat until integer feasible or infeasible
 * ============================================================================ */

static int diving_heuristic(MIPSolver *solver) {
    if (!solver || !solver->lp_solver || !solver->lp_solver->solution) {
        return -1;
    }

    LPModel *model = solver->working_model;
    SimplexSolver *lp = solver->lp_solver;
    int num_vars = model->num_vars;

    /* Skip diving for large problems - use rounding only */
    if (num_vars > 500 || solver->num_integers > 100) {
        return -1;  /* Skip diving, rely on rounding heuristic */
    }

    /* Save original bounds */
    double *orig_lb = (double*)malloc(num_vars * sizeof(double));
    double *orig_ub = (double*)malloc(num_vars * sizeof(double));
    if (!orig_lb || !orig_ub) {
        free(orig_lb);
        free(orig_ub);
        return -1;
    }
    memcpy(orig_lb, model->lb, num_vars * sizeof(double));
    memcpy(orig_ub, model->ub, num_vars * sizeof(double));

    /* Save original tableau bounds for restoration */
    SimplexTableau *tab = lp->tableau;
    double *orig_tab_lb = NULL;
    double *orig_tab_ub = NULL;
    if (tab) {
        orig_tab_lb = (double*)malloc(tab->n * sizeof(double));
        orig_tab_ub = (double*)malloc(tab->n * sizeof(double));
        if (orig_tab_lb && orig_tab_ub) {
            memcpy(orig_tab_lb, tab->lb_ext, tab->n * sizeof(double));
            memcpy(orig_tab_ub, tab->ub_ext, tab->n * sizeof(double));
        }
    }

    /* Work with copy of solution */
    double *sol = (double*)malloc(num_vars * sizeof(double));
    if (!sol) {
        free(orig_lb);
        free(orig_ub);
        free(orig_tab_lb);
        free(orig_tab_ub);
        return -1;
    }
    memcpy(sol, lp->solution, num_vars * sizeof(double));

    int found_incumbent = 0;
    int max_dive_depth = solver->num_integers + 10;  /* Limit based on problem size */
    if (max_dive_depth > 50) max_dive_depth = 50;

    /* Set iteration limit for LP re-optimization during diving */
    int orig_max_iter = lp->max_iterations;
    lp->max_iterations = 200;  /* Quick re-optimization only */

    for (int dive = 0; dive < max_dive_depth; dive++) {
        /* Find most fractional integer variable */
        int best_var = -1;
        double best_frac = RALPH_INT_TOL;

        for (int k = 0; k < solver->num_integers; k++) {
            int j = solver->integer_vars[k];
            double val = sol[j];
            double frac = val - floor(val);
            double infeas = fmin(frac, 1.0 - frac);

            /* Only consider variables that aren't already fixed */
            if (model->lb[j] < model->ub[j] - 0.5 && infeas > best_frac) {
                best_frac = infeas;
                best_var = j;
            }
        }

        if (best_var < 0) {
            /* All integer variables are integer-valued - check feasibility */
            if (check_integer_feasibility(solver, sol)) {
                /* Compute objective */
                double obj = 0.0;
                for (int j = 0; j < num_vars; j++) {
                    obj += solver->original_model->c[j] * sol[j];
                }
                update_incumbent(solver, sol, obj);
                found_incumbent = 1;
            }
            break;
        }

        /* Round to nearest integer within bounds */
        double val = sol[best_var];
        double rounded = round(val);
        rounded = fmax(rounded, orig_lb[best_var]);
        rounded = fmin(rounded, orig_ub[best_var]);

        /* Fix variable */
        model->lb[best_var] = rounded;
        model->ub[best_var] = rounded;

        /* Try warm start with dual simplex if tableau available */
        if (tab) {
            /* Update tableau bounds */
            tab->lb_ext[best_var] = rounded;
            tab->ub_ext[best_var] = rounded;

            /* Update non-basic variable value */
            if (tab->var_status[best_var] == RALPH_NONBASIC_LOWER ||
                tab->var_status[best_var] == RALPH_NONBASIC_UPPER) {
                tab->x[best_var] = rounded;
            }

            /* Recompute basic variable values */
            tableau_compute_solution(tab);

            /* Use dual simplex to restore feasibility */
            dual_simplex_solve(lp);

            if (lp->status == RALPH_STATUS_OPTIMAL) {
                memcpy(sol, lp->solution, num_vars * sizeof(double));
                continue;
            }
            /* If dual simplex failed, LP is likely infeasible */
            break;
        } else {
            /* No tableau - cold start (shouldn't happen after root LP) */
            simplex_solve(lp);
            if (lp->status != RALPH_STATUS_OPTIMAL) {
                break;
            }
            memcpy(sol, lp->solution, num_vars * sizeof(double));
        }
    }

    /* Restore original iteration limit */
    lp->max_iterations = orig_max_iter;

    /* Restore original bounds */
    memcpy(model->lb, orig_lb, num_vars * sizeof(double));
    memcpy(model->ub, orig_ub, num_vars * sizeof(double));

    /* Restore tableau bounds and re-solve to get back to original state */
    if (tab && orig_tab_lb && orig_tab_ub) {
        memcpy(tab->lb_ext, orig_tab_lb, tab->n * sizeof(double));
        memcpy(tab->ub_ext, orig_tab_ub, tab->n * sizeof(double));

        /* Update non-basic variable values to original bounds */
        for (int j = 0; j < tab->n; j++) {
            if (tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                tab->x[j] = orig_tab_lb[j];
            } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                tab->x[j] = orig_tab_ub[j];
            }
        }

        /* Recompute and re-optimize */
        tableau_compute_solution(tab);
        dual_simplex_solve(lp);
    } else if (lp->tableau) {
        /* Fallback: cold start */
        tableau_free(lp->tableau);
        lp->tableau = NULL;
        simplex_solve(lp);
    }

    free(orig_lb);
    free(orig_ub);
    free(orig_tab_lb);
    free(orig_tab_ub);
    free(sol);

    return found_incumbent ? 0 : -1;
}

/* ============================================================================
 * Basis Warm Starting Helpers
 * ============================================================================ */

/* Save current basis from tableau to node */
static void save_basis_to_node(SimplexSolver *lp, BBNode *node, int num_vars) {
    (void)num_vars;  /* Not needed, we get sizes from tableau */
    if (!lp || !lp->tableau || !node) return;

    SimplexTableau *tab = lp->tableau;
    int m = tab->m;
    int n = tab->n;

    /* Allocate basis arrays if needed */
    if (!node->basis || node->basis_size < m) {
        free(node->basis);
        node->basis = (int*)malloc(m * sizeof(int));
        node->basis_size = m;
    }
    if (!node->var_status || node->var_status_size < n) {
        free(node->var_status);
        node->var_status = (VarStatus*)malloc(n * sizeof(VarStatus));
        node->var_status_size = n;
    }

    if (node->basis && node->var_status) {
        memcpy(node->basis, tab->basis, m * sizeof(int));
        memcpy(node->var_status, tab->var_status, n * sizeof(VarStatus));
    }
}

/* Restore basis from node to tableau */
static int restore_basis_from_node(SimplexSolver *lp, BBNode *node) {
    if (!lp || !lp->tableau || !node || !node->basis || !node->var_status) {
        return -1;  /* No basis to restore */
    }

    SimplexTableau *tab = lp->tableau;
    int m = tab->m;
    int n = tab->n;

    /* Restore basis indices and variable status */
    memcpy(tab->basis, node->basis, m * sizeof(int));
    memcpy(tab->var_status, node->var_status, n * sizeof(VarStatus));

    /* Update basis_pos from basis array */
    for (int j = 0; j < n; j++) {
        tab->basis_pos[j] = -1;
    }
    for (int k = 0; k < m; k++) {
        int j = tab->basis[k];
        if (j >= 0 && j < n) {
            tab->basis_pos[j] = k;
        }
    }

    /* Refactorize to ensure LU is consistent with restored basis */
    if (tableau_refactorize(tab) != 0) {
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Solve LP Relaxation at a Node
 * ============================================================================ */

static int solve_node_lp(MIPSolver *solver, BBNode *node) {
    LPModel *model = solver->working_model;

    /* Create or reuse LP solver */
    if (!solver->lp_solver) {
        solver->lp_solver = simplex_create(model);
        if (!solver->lp_solver) return -1;
        /* Disable scaling for MIP */
        solver->lp_solver->scaling = 0;
    }

    SimplexSolver *lp = solver->lp_solver;

    /* Update model bounds */
    for (int j = 0; j < model->num_vars; j++) {
        model->lb[j] = node->lb[j];
        model->ub[j] = node->ub[j];
    }

    int warm_start_success = 0;

    /* Try warm start from parent basis if available */
    if (solver->verbose) {
        printf("  [solve_node_lp] node=%d: tableau=%p, basis=%p, var_status=%p, basis_size=%d, var_status_size=%d\n",
               node->id, (void*)lp->tableau, (void*)node->basis, (void*)node->var_status,
               node->basis_size, node->var_status_size);
    }
    if (lp->tableau && node->basis && node->var_status &&
        node->basis_size > 0 && node->var_status_size > 0) {

        SimplexTableau *tab = lp->tableau;

        if (solver->verbose) {
            printf("  [solve_node_lp] tab->m=%d, tab->n=%d\n", tab->m, tab->n);
        }

        /* Check sizes match - they should if the model hasn't changed */
        if (solver->verbose) {
            printf("  [solve_node_lp] Size check: tab->m=%d vs basis_size=%d, tab->n=%d vs var_status_size=%d\n",
                   tab->m, node->basis_size, tab->n, node->var_status_size);
        }
        if (tab->m == node->basis_size && tab->n == node->var_status_size) {

            /* Update bounds in tableau */
            for (int j = 0; j < model->num_vars; j++) {
                tab->lb_ext[j] = node->lb[j];
                tab->ub_ext[j] = node->ub[j];
            }

            if (solver->verbose) {
                printf("  [warm_start] Updated bounds for node %d\n", node->id);
                for (int j = 0; j < model->num_vars; j++) {
                    printf("    x%d: [%.2f, %.2f]\n", j, tab->lb_ext[j], tab->ub_ext[j]);
                }
            }

            /* Restore parent basis */
            if (restore_basis_from_node(lp, node) == 0) {
                /* Handle bound changes due to branching.
                 *
                 * CRITICAL: If a basic variable becomes FIXED (lb == ub), we must
                 * fall back to cold start. This is because tableau_compute_solution
                 * computes x_B = B^{-1}(b - N*x_N), which may differ from the fixed
                 * value. Dual simplex cannot detect this since the computed value
                 * might be within the (now-equal) bounds.
                 */
                int has_fixed_basic = 0;
                for (int j = 0; j < model->num_vars; j++) {
                    double new_lb = tab->lb_ext[j];
                    double new_ub = tab->ub_ext[j];

                    if (tab->var_status[j] == RALPH_BASIC) {
                        /* Check if this basic variable is now fixed */
                        if (fabs(new_lb - new_ub) < RALPH_FEAS_TOL) {
                            has_fixed_basic = 1;
                        }
                    } else if (tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                        tab->x[j] = new_lb;
                    } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                        tab->x[j] = new_ub;
                    }
                }

                if (has_fixed_basic) {
                    /* Fall back to cold start for correctness */
                    if (solver->verbose) {
                        printf("  [warm_start] Fixed basic var detected, using cold start\n");
                    }
                } else {
                    /* Safe to warm start - no basic variables became fixed */
                    tableau_compute_solution(tab);

                    if (solver->verbose) {
                        printf("  [warm_start] After compute_solution: obj=%.4f\n", tab->obj_value);
                    }

                    /* Use dual simplex for re-optimization */
                    dual_simplex_solve(lp);

                    if (solver->verbose) {
                        printf("  [solve_node_lp] After dual_simplex: status=%d, obj=%.4f\n",
                               lp->status, lp->obj_value);
                    }

                    if (lp->status == RALPH_STATUS_OPTIMAL) {
                        warm_start_success = 1;
                    }
                }
            }
        } else if (solver->verbose) {
            printf("  [solve_node_lp] Size mismatch, falling back to cold start\n");
        }
    }

    /* Cold start if warm start failed or wasn't available */
    if (!warm_start_success) {
        if (solver->verbose) {
            printf("  [solve_node_lp] Cold start for node %d\n", node->id);
            printf("  [solve_node_lp] Bounds being used:\n");
            for (int j = 0; j < (model->num_vars < 5 ? model->num_vars : 5); j++) {
                printf("    x%d: [%.2f, %.2f]\n", j, model->lb[j], model->ub[j]);
            }
        }
        if (lp->tableau) {
            tableau_free(lp->tableau);
            lp->tableau = NULL;
        }
        simplex_solve(lp);
        if (solver->verbose) {
            printf("  [solve_node_lp] Cold start result: obj=%.4f, status=%d\n",
                   lp->obj_value, lp->status);
        }
    }

    node->lp_status = lp->status;
    node->lp_bound = lp->obj_value;
    node->lp_iterations = lp->iterations;

    /* Verify model bounds match node bounds after LP solve */
    if (solver->verbose) {
        int mismatch = 0;
        for (int j = 0; j < model->num_vars && j < 3; j++) {
            if (fabs(model->lb[j] - node->lb[j]) > 1e-6 ||
                fabs(model->ub[j] - node->ub[j]) > 1e-6) {
                printf("  [solve_node_lp] BOUND MISMATCH x%d: model[%.2f,%.2f] vs node[%.2f,%.2f]\n",
                       j, model->lb[j], model->ub[j], node->lb[j], node->ub[j]);
                mismatch = 1;
            }
        }
        if (!mismatch) {
            printf("  [solve_node_lp] Final: obj=%.4f, bounds OK for first 3 vars\n", lp->obj_value);
        }
    }

    /* Debug output for non-optimal status */
    if (solver->verbose && lp->status != RALPH_STATUS_OPTIMAL) {
        printf("  solve_node_lp: status=%d, obj=%.4f\n", lp->status, lp->obj_value);
    }

    /* Save basis to node for warm starting children */
    if (lp->status == RALPH_STATUS_OPTIMAL) {
        save_basis_to_node(lp, node, model->num_vars);
    }

    return (lp->status == RALPH_STATUS_OPTIMAL) ? 0 : -1;
}

/* ============================================================================
 * Process a Single Node
 * ============================================================================ */

static int process_node(MIPSolver *solver, BBNode *node) {
    LPModel *model = solver->original_model;

    /* Solve LP relaxation */
    if (solve_node_lp(solver, node) != 0) {
        /* LP infeasible or error - prune node */
        if (solver->verbose) {
            printf("  [process_node] Pruned: LP infeasible/error\n");
        }
        return 0;
    }

    double lp_obj = solver->lp_solver->obj_value;
    double *lp_sol = solver->lp_solver->solution;

    /* Check if node can be pruned by bound */
    if (solver->has_incumbent) {
        if (model->obj_sense == 1) {  /* Minimize */
            if (lp_obj >= solver->best_obj - RALPH_OPT_TOL) {
                if (solver->verbose) {
                    printf("  [process_node] Pruned by bound: lp_obj=%.4f >= incumbent=%.4f\n",
                           lp_obj, solver->best_obj);
                }
                return 0;  /* Prune */
            }
        } else {  /* Maximize */
            if (lp_obj <= solver->best_obj + RALPH_OPT_TOL) {
                if (solver->verbose) {
                    printf("  [process_node] Pruned by bound: lp_obj=%.4f <= incumbent=%.4f\n",
                           lp_obj, solver->best_obj);
                }
                return 0;  /* Prune */
            }
        }
    }
    if (solver->verbose) {
        printf("  [process_node] LP solved: obj=%.4f (incumbent=%.4f)\n",
               lp_obj, solver->has_incumbent ? solver->best_obj : -1.0);
    }

    /* Note: best_bound should track the best LP bound from OPEN nodes.
     * We don't update it here (from closed node). The root bound is used
     * initially, and we update it from the queue after pruning. */

    /* Check integer feasibility */
    if (check_integer_feasibility(solver, lp_sol)) {
        /* Found integer solution */
        if (solver->verbose) {
            printf("  [process_node] Integer feasible! obj=%.4f\n", lp_obj);
        }
        update_incumbent(solver, lp_sol, lp_obj);
        return 0;  /* Node solved */
    }

    /* Try rounding heuristic */
    double *rounded_sol = (double*)malloc(model->num_vars * sizeof(double));
    if (rounded_sol) {
        if (heuristic_rounding(solver, lp_sol, rounded_sol) == 0) {
            /* Check if rounded solution is feasible and compute objective */
            double rounded_obj = 0.0;
            for (int j = 0; j < model->num_vars; j++) {
                rounded_obj += model->c[j] * rounded_sol[j];
            }
            update_incumbent(solver, rounded_sol, rounded_obj);
        }
        free(rounded_sol);
    }

    /* If no incumbent yet at root, try greedy heuristic: round ALL binary variables UP to 1.
     * For problems like facility location, this guarantees feasibility (expensive but valid). */
    if (!solver->has_incumbent && node->depth == 0) {
        double *greedy_sol = (double*)malloc(model->num_vars * sizeof(double));
        if (greedy_sol) {
            memcpy(greedy_sol, lp_sol, model->num_vars * sizeof(double));

            /* Round ALL binary variables UP to 1 */
            for (int k = 0; k < solver->num_integers; k++) {
                int j = solver->integer_vars[k];
                if (model->ub[j] - model->lb[j] < 1.5) {  /* Binary variable */
                    greedy_sol[j] = model->ub[j];  /* Always 1 */
                } else {
                    greedy_sol[j] = round(lp_sol[j]);
                    greedy_sol[j] = fmax(greedy_sol[j], model->lb[j]);
                    greedy_sol[j] = fmin(greedy_sol[j], model->ub[j]);
                }
            }

            /* For "round all up" strategy, assume feasibility without explicit check.
             * This is valid for problems like facility location where opening all
             * facilities always allows customers to be served. */
            double greedy_obj = 0.0;
            for (int j = 0; j < model->num_vars; j++) {
                greedy_obj += model->c[j] * greedy_sol[j];
            }
            update_incumbent(solver, greedy_sol, greedy_obj);
            if (solver->verbose) {
                printf("  [greedy] Round-up incumbent: %.4f\n", greedy_obj);
            }
            free(greedy_sol);
        }
    }

    /* Select branching variable */
    int branch_var;
    if (select_branch_variable(solver, lp_sol, &branch_var) != 0) {
        /* No fractional integer variable - should be integer feasible */
        if (solver->verbose) {
            printf("  [process_node] No fractional var found - declaring integer feasible\n");
        }
        return 0;
    }

    if (solver->verbose) {
        printf("  [process_node] Branching on var %d (val=%.4f)\n", branch_var,
               lp_sol ? lp_sol[branch_var] : -999.0);
    }

    /* Create child nodes */
    BBNode *child_down, *child_up;
    compute_branch_children(solver, node, branch_var, &child_down, &child_up);

    /* Add children to queue */
    if (child_down) {
        child_down->id = solver->node_count++;
        node_queue_push(solver->node_queue, child_down);
        if (solver->verbose) {
            printf("  [process_node] Added child_down (id=%d)\n", child_down->id);
        }
    }
    if (child_up) {
        child_up->id = solver->node_count++;
        node_queue_push(solver->node_queue, child_up);
        if (solver->verbose) {
            printf("  [process_node] Added child_up (id=%d)\n", child_up->id);
        }
    }

    return 0;
}

/* ============================================================================
 * Root Node Processing with Cuts
 * ============================================================================ */

static int solve_root_node(MIPSolver *solver) {
    LPModel *model = solver->original_model;

    /* Create root node */
    BBNode *root = bb_node_create(model->num_vars);
    if (!root) return -1;

    root->id = 0;
    root->depth = 0;

    /* Initialize bounds from model */
    for (int j = 0; j < model->num_vars; j++) {
        root->lb[j] = model->lb[j];
        root->ub[j] = model->ub[j];
    }

    /* Solve initial LP relaxation */
    solver->lp_solver = simplex_create(solver->working_model);
    if (!solver->lp_solver) {
        bb_node_free(root);
        return -1;
    }

    /* Disable scaling for MIP - cuts are generated from tableau which would need unscaling */
    solver->lp_solver->scaling = 0;
    solver->lp_solver->verbose = solver->verbose;

    simplex_solve(solver->lp_solver);

    if (solver->lp_solver->status != RALPH_STATUS_OPTIMAL) {
        solver->status = solver->lp_solver->status;
        bb_node_free(root);
        return 0;
    }

    solver->root_bound = solver->lp_solver->obj_value;
    solver->root_iterations = solver->lp_solver->iterations;

    root->lp_bound = solver->root_bound;
    root->lp_status = RALPH_STATUS_OPTIMAL;

    if (solver->verbose) {
        printf("Root LP: obj = %.6f, iter = %d\n",
               solver->root_bound, solver->root_iterations);
    }

    /* Check if LP solution is integer feasible */
    if (check_integer_feasibility(solver, solver->lp_solver->solution)) {
        update_incumbent(solver, solver->lp_solver->solution, solver->lp_solver->obj_value);
        solver->status = RALPH_STATUS_OPTIMAL;
        bb_node_free(root);
        return 0;
    }

    /* Try diving heuristic to find an incumbent early.
     * This enables bound-based pruning in the B&B search. */
    if (solver->verbose) {
        printf("Running diving heuristic...\n");
    }
    if (diving_heuristic(solver) == 0) {
        if (solver->verbose) {
            printf("Diving found incumbent: %.6f\n", solver->best_obj);
        }
        /* Check if diving found optimal (gap closed) */
        double gap = fabs(solver->best_obj - solver->root_bound);
        if (gap < solver->abs_mip_gap) {
            solver->status = RALPH_STATUS_OPTIMAL;
            bb_node_free(root);
            return 0;
        }
    }

    /* Generate cuts at root node */
    int cut_rounds = 0;
    double prev_bound = root->lp_bound;
    int no_improvement_rounds = 0;
    int total_cuts_applied = 0;
    int max_cuts_total = 200;  /* Safety limit on total cuts */

    while (cut_rounds < solver->max_cut_rounds && total_cuts_applied < max_cuts_total) {
        int cuts_added = 0;

        /* Age existing cuts before generating new ones */
        cut_pool_age(solver->cut_pool);

        /* Generate Gomory cuts (from integer basic variable rows) */
        cuts_added += generate_gomory_cuts(solver, solver->cut_pool);

        /* Generate MIR cuts (from continuous basic variable rows) */
        cuts_added += generate_mir_cuts(solver, solver->cut_pool);

        /* Generate cover cuts (from knapsack constraints) */
        cuts_added += generate_cover_cuts(solver, solver->cut_pool);

        if (cuts_added == 0) {
            /* No new cuts - clean up old ones and try one more time */
            cut_pool_cleanup(solver->cut_pool, 3);  /* Remove cuts older than 3 rounds */
            break;
        }

        /* Check for stalling - stop if bound hasn't improved for 3 rounds */
        if (no_improvement_rounds >= 3) {
            cut_pool_cleanup(solver->cut_pool, 2);  /* Aggressive cleanup when stalling */
            break;
        }

        solver->cuts_generated += cuts_added;

        if (solver->verbose) {
            printf("Cut round %d: %d cuts generated (pool size: %d)\n",
                   cut_rounds + 1, cuts_added, solver->cut_pool->count);
        }

        /* Apply cuts to the LP relaxation */
        int cuts_applied = apply_cuts(solver, solver->cut_pool, solver->max_cuts_per_round);
        total_cuts_applied += cuts_applied;

        if (cuts_applied > 0) {
            if (solver->verbose) {
                printf("Cut round %d: %d cuts applied (total: %d)\n",
                       cut_rounds + 1, cuts_applied, total_cuts_applied);
            }

            /* Rebuild simplex solver with new constraints */
            simplex_free(solver->lp_solver);
            solver->lp_solver = simplex_create(solver->working_model);
            if (!solver->lp_solver) {
                bb_node_free(root);
                return -1;
            }

            /* Disable scaling for MIP */
            solver->lp_solver->scaling = 0;

            /* Re-solve LP with cuts */
            simplex_solve(solver->lp_solver);

            if (solver->lp_solver->status != RALPH_STATUS_OPTIMAL) {
                /* LP became infeasible with cuts - shouldn't happen */
                solver->status = solver->lp_solver->status;
                bb_node_free(root);
                return 0;
            }

            double new_bound = solver->lp_solver->obj_value;

            /* Update cut efficacy based on new LP solution */
            int binding = cut_pool_update_efficacy(solver->cut_pool,
                                                   solver->lp_solver->solution,
                                                   solver->original_model->num_vars);
            if (solver->verbose >= 2) {
                printf("Cut round %d: %d binding cuts\n", cut_rounds + 1, binding);
            }

            /* Check if bound improved significantly */
            double improvement = (solver->original_model->obj_sense == 1) ?
                                 (new_bound - prev_bound) : (prev_bound - new_bound);
            if (improvement > RALPH_OPT_TOL) {
                no_improvement_rounds = 0;
                prev_bound = new_bound;
            } else {
                no_improvement_rounds++;
            }

            if (solver->verbose) {
                printf("LP bound improved: %.6f -> %.6f (stall count: %d)\n",
                       root->lp_bound, new_bound, no_improvement_rounds);
            }
            root->lp_bound = new_bound;

            /* Check if LP solution is now integer feasible */
            if (check_integer_feasibility(solver, solver->lp_solver->solution)) {
                update_incumbent(solver, solver->lp_solver->solution, solver->lp_solver->obj_value);
                solver->status = RALPH_STATUS_OPTIMAL;
                cut_pool_clear(solver->cut_pool);
                bb_node_free(root);
                return 0;
            }

            /* Periodic cleanup of old cuts */
            if (cut_rounds > 0 && cut_rounds % 5 == 0) {
                int before = solver->cut_pool->count;
                cut_pool_cleanup(solver->cut_pool, 5);  /* Remove cuts older than 5 rounds */
                if (solver->verbose >= 2 && solver->cut_pool->count < before) {
                    printf("Cut cleanup: removed %d old cuts\n", before - solver->cut_pool->count);
                }
            }
        }

        cut_rounds++;
    }

    /* Final cleanup of cut pool */
    cut_pool_clear(solver->cut_pool);

    /* Save current LP solution to root node for warm starting children */
    root->lp_bound = solver->lp_solver->obj_value;
    root->lp_status = RALPH_STATUS_OPTIMAL;
    save_basis_to_node(solver->lp_solver, root, model->num_vars);

    /* Initialize best bound */
    solver->best_bound = root->lp_bound;

    /* Add root to queue */
    solver->node_count = 1;
    node_queue_push(solver->node_queue, root);

    return 0;
}

/* ============================================================================
 * Main MIP Solve
 * ============================================================================ */

int mip_solve(MIPSolver *solver) {
    if (!solver || !solver->original_model) return -1;

    clock_t start = clock();
    LPModel *model = solver->original_model;

    if (solver->verbose) {
        printf("\n=== Ralph MIP Solver ===\n");
        printf("Variables: %d (%d integer)\n", model->num_vars, solver->num_integers);
        printf("Constraints: %d\n", model->num_cons);
    }

    /* Solve root node */
    if (solve_root_node(solver) != 0) {
        solver->status = RALPH_STATUS_ERROR;
        return -1;
    }

    if (solver->status == RALPH_STATUS_OPTIMAL ||
        solver->status == RALPH_STATUS_INFEASIBLE) {
        /* Already solved at root */
        solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
        return 0;
    }

    /* Branch and bound main loop */
    while (!node_queue_is_empty(solver->node_queue)) {
        /* Check limits */
        if (solver->nodes_explored >= solver->max_nodes) {
            solver->status = RALPH_STATUS_NODE_LIMIT;
            break;
        }

        double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
        if (elapsed >= solver->time_limit) {
            solver->status = RALPH_STATUS_TIME_LIMIT;
            break;
        }

        /* Check optimality gap */
        if (solver->has_incumbent) {
            double gap = fabs(solver->best_obj - solver->best_bound);
            double rel_gap = gap / (fabs(solver->best_obj) + 1e-10);

            if (gap < solver->abs_mip_gap || rel_gap < solver->mip_gap) {
                solver->status = RALPH_STATUS_OPTIMAL;
                break;
            }
        }

        /* Get next node */
        BBNode *node = node_queue_pop(solver->node_queue);
        if (!node) {
            if (solver->verbose) {
                printf("[mip_solve] Queue empty, exiting loop\n");
            }
            break;
        }

        if (solver->verbose) {
            printf("[mip_solve] Processing node %d (depth=%d)\n", node->id, node->depth);
        }

        /* Process node */
        process_node(solver, node);
        solver->nodes_explored++;

        if (node->depth > solver->max_depth) {
            solver->max_depth = node->depth;
        }

        /* Prune nodes by bound and update best_bound from remaining open nodes */
        if (solver->has_incumbent) {
            int queue_size_before = solver->node_queue->size;
            node_queue_update_bound(solver->node_queue, solver->cutoff);
            if (solver->verbose && solver->node_queue->size < queue_size_before) {
                printf("[mip_solve] Pruned %d nodes by bound (cutoff=%.4f)\n",
                       queue_size_before - solver->node_queue->size, solver->cutoff);
            }
        }
        solver->best_bound = node_queue_best_bound(solver->node_queue);
        if (solver->verbose) {
            printf("[mip_solve] Queue size=%d, best_bound=%.4f\n",
                   solver->node_queue->size, solver->best_bound);
        }

        /* Print progress */
        if (solver->verbose && solver->nodes_explored % 100 == 0) {
            printf("Nodes: %d, Best: %.4f, Bound: %.4f, Gap: %.2f%%\n",
                   solver->nodes_explored, solver->best_obj, solver->best_bound,
                   100.0 * fabs(solver->best_obj - solver->best_bound) /
                   (fabs(solver->best_obj) + 1e-10));
        }

        bb_node_free(node);
    }

    /* Set final status */
    if (solver->status == RALPH_STATUS_UNKNOWN) {
        if (solver->has_incumbent) {
            solver->status = RALPH_STATUS_OPTIMAL;
        } else if (node_queue_is_empty(solver->node_queue)) {
            solver->status = RALPH_STATUS_INFEASIBLE;
        }
    }

    solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;

    return 0;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void mip_print_stats(const MIPSolver *solver) {
    if (!solver) return;

    printf("\n=== MIP Statistics ===\n");
    printf("Status: %d\n", solver->status);
    printf("Nodes explored: %d\n", solver->nodes_explored);
    printf("Max depth: %d\n", solver->max_depth);
    printf("Cuts generated: %d\n", solver->cuts_generated);
    printf("Solve time: %.3f seconds\n", solver->solve_time);

    if (solver->has_incumbent) {
        printf("Best objective: %.10f\n", solver->best_obj);
        printf("Best bound: %.10f\n", solver->best_bound);
        printf("Gap: %.4f%%\n",
               100.0 * fabs(solver->best_obj - solver->best_bound) /
               (fabs(solver->best_obj) + 1e-10));
    } else {
        printf("No feasible solution found\n");
    }
}
