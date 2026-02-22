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
#include "mip_lp_adapter.h"
#include "lp_log.h"

/* Apply P5/P6 feature flags and iteration budget from MIP solver to LP sub-solver.
 * MIP LP solves should NEVER run for 1M+ iterations — if v2 cycles for >2000
 * iterations on a node LP, something is wrong and cold-start fallback handles it. */
static void mip_apply_dual_flags(MIPSolver *solver) {
    if (!solver->lp_solver) return;
    solver->lp_solver->telemetry_enabled = solver->telemetry ? 1 : 0;
    if (solver->dual_bound_flip >= 0)
        solver->lp_solver->use_dual_bound_flip = solver->dual_bound_flip;
    if (solver->dual_steepest_edge >= 0)
        solver->lp_solver->use_dual_steepest_edge = solver->dual_steepest_edge;
    /* Cap iteration limit for MIP LP solves.  The old dual_reopt had a 500-pivot
     * budget; if dual v2 hasn't converged in 500 iterations, fall back to primal.
     * This prevents stall-detection-fooling cycling from burning minutes. */
    if (solver->lp_solver->max_iterations > 500)
        solver->lp_solver->max_iterations = 500;
    /* T2.1: Propagate supernodal LU flag to LP solver */
    solver->lp_solver->lu_supernode = solver->lu_supernode;
}

/* ============================================================================
 * MIP Solver Creation/Destruction
 * ============================================================================ */

MIPSolver* mip_create(LPModel *model, int detect_special, int pool_capacity) {
    if (!model) return NULL;
    if (pool_capacity <= 0) pool_capacity = 1024;  /* Default */

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

    solver->integer_vars = (int*)calloc(solver->num_integers, sizeof(int));
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
    solver->best_solution = (double*)calloc(model->num_vars, sizeof(double));
    solver->has_incumbent = 0;
    solver->mip_start = NULL;
    solver->mip_start_mask = NULL;
    solver->mip_start_n = 0;
    solver->mip_start_nnz = 0;
    solver->mip_start_repair_mode = (int)RALPH_MIP_START_REPAIR_STRICT;
    solver->mip_start_attempted = 0;
    solver->mip_start_accepted = 0;

    /* Initialize pseudo-costs */
    solver->pseudo_cost_down = (double*)calloc(model->num_vars, sizeof(double));
    solver->pseudo_cost_up = (double*)calloc(model->num_vars, sizeof(double));
    solver->pseudo_count_down = (int*)calloc(model->num_vars, sizeof(int));
    solver->pseudo_count_up = (int*)calloc(model->num_vars, sizeof(int));

    if (!solver->best_solution || !solver->pseudo_cost_down || !solver->pseudo_cost_up ||
        !solver->pseudo_count_down || !solver->pseudo_count_up) {
        mip_free(solver);
        return NULL;
    }

    /* Initialize pseudo-costs from objective coefficients.
     * fmax(|c_j|, 1.0) gives a reasonable prior: variables with large
     * objective impact get higher pseudo-costs, biasing branching toward them. */
    for (int j = 0; j < model->num_vars; j++) {
        double init = fmax(fabs(model->c[j]), 1.0);
        solver->pseudo_cost_down[j] = init;
        solver->pseudo_cost_up[j] = init;
    }

    /* Default parameters */
    solver->max_nodes = RALPH_DEFAULT_NODE_LIMIT;
    solver->time_limit = RALPH_DEFAULT_TIME_LIMIT;
    solver->mip_gap = RALPH_DEFAULT_MIP_GAP;
    solver->abs_mip_gap = RALPH_DEFAULT_ABS_MIP_GAP;
    solver->cutoff = RALPH_INFINITY;
    solver->node_select = NODE_SELECT_HYBRID;
    solver->var_select = VAR_SELECT_RELIABILITY;  /* Bootstraps pseudocosts via strong branching */
    solver->max_cuts_per_round = 50;
    solver->max_cut_rounds = 5;  /* Enable cuts with conservative limit */
    solver->verbose = 0;
    solver->telemetry = 1;
    solver->dual_bound_flip = -1;    /* use default */
    solver->dual_steepest_edge = -1; /* use default */

    /* Create node queue */
    solver->node_queue = node_queue_create(1024, solver->node_select, model->obj_sense);
    if (!solver->node_queue) {
        mip_free(solver);
        return NULL;
    }

    /* Create node pool for efficient B&B node allocation
     * Benefits: reduces malloc overhead, improves cache locality
     * Falls back to malloc when pool is exhausted */
    solver->node_pool = bb_node_pool_create(pool_capacity, model->num_vars);
    /* Note: pool is optional - NULL pool falls back to individual allocs */

    /* Create cut pool */
    solver->cut_pool = cut_pool_create(1024);
    if (!solver->cut_pool) {
        mip_free(solver);
        return NULL;
    }

    solver->status = RALPH_STATUS_UNKNOWN;
    solver->last_solved_node_id = -1;
    solver->node_basis_warm_attempts = 0;
    solver->node_basis_warm_applied = 0;
    solver->node_basis_warm_rejected = 0;
    solver->node_basis_staged = 0;
    solver->node_basis_stage_cooldown = 0;

    /* Try to detect LAP structure for specialized solving */
    solver->use_lap_solver = 0;
    solver->lap_sig = NULL;
    solver->lap_nodes_solved = 0;
    solver->simplex_nodes_solved = 0;

    /* Only detect LAP if enabled (per-model flag AND global flag) */
    if (detect_special && ralph_get_detect_lap()) {
        MIPLAPSignature *lap_sig = (MIPLAPSignature *)calloc(1, sizeof(MIPLAPSignature));
        if (lap_sig && detect_lap_mip(model, lap_sig)) {
            solver->lap_sig = lap_sig;
            solver->use_lap_solver = 1;
            if (solver->verbose) {
                LP_LOG_STDOUT("LAP structure detected: %dx%d assignment\n",
                       lap_sig->base.n, lap_sig->base.n);
            }
        } else {
            free(lap_sig);
        }
    }

    /* Detect SCP structure for specialized cuts, heuristics, and Lagrangian */
    solver->use_scp_solver = 0;
    solver->scp_cuts_generated = 0;
    solver->lagrangian_bound = -RALPH_INFINITY;

    if (detect_special && is_scp_model(model)) {
        solver->use_scp_solver = 1;
        if (solver->verbose) {
            LP_LOG_STDOUT("SCP structure detected: %d elements, %d sets\n",
                   model->num_cons, model->num_vars);
        }
        /* Initialize pseudo-costs using SCP cost/coverage ratio */
        init_pseudo_costs_scp(solver);
    }

    return solver;
}

void mip_free(MIPSolver *solver) {
    if (!solver) return;

    lp_model_free(solver->working_model);
    simplex_free(solver->lp_solver);
    free(solver->integer_vars);
    free(solver->is_integer);
    free(solver->best_solution);
    free(solver->mip_start);
    free(solver->mip_start_mask);
    free(solver->pseudo_cost_down);
    free(solver->pseudo_cost_up);
    free(solver->pseudo_count_down);
    free(solver->pseudo_count_up);
    free(solver->branch_priorities);
    free(solver->branch_directions);
    node_queue_free_with_pool(solver->node_queue, solver->node_pool);
    bb_node_pool_free(solver->node_pool);
    cut_pool_free(solver->cut_pool);

    /* Free LAP signature if allocated */
    if (solver->lap_sig) {
        detect_lap_mip_free(solver->lap_sig);
        free(solver->lap_sig);
    }

    free(solver);
}

int mip_set_start(MIPSolver *solver, const double *x, int n) {
    return mip_set_start_ex(solver, x, NULL, n, solver ? solver->mip_start_repair_mode : 0);
}

int mip_set_start_ex(MIPSolver *solver, const double *x, const int *mask,
                     int n, int repair_mode) {
    if (!solver || !x || !solver->original_model) return -1;
    if (n != solver->original_model->num_vars || n <= 0) return -1;
    if (repair_mode < (int)RALPH_MIP_START_REPAIR_STRICT ||
        repair_mode > (int)RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) {
        return -1;
    }

    double *copy = (double*)malloc((size_t)n * sizeof(double));
    int *mask_copy = (int*)calloc((size_t)n, sizeof(int));
    if (!copy || !mask_copy) {
        free(copy);
        free(mask_copy);
        return -1;
    }
    memcpy(copy, x, (size_t)n * sizeof(double));

    int nnz = 0;
    if (mask) {
        for (int j = 0; j < n; j++) {
            mask_copy[j] = mask[j] ? 1 : 0;
            nnz += mask_copy[j];
        }
    } else {
        for (int j = 0; j < n; j++) mask_copy[j] = 1;
        nnz = n;
    }

    free(solver->mip_start);
    free(solver->mip_start_mask);
    solver->mip_start = copy;
    solver->mip_start_mask = mask_copy;
    solver->mip_start_n = n;
    solver->mip_start_nnz = nnz;
    solver->mip_start_repair_mode = repair_mode;
    solver->mip_start_attempted = 0;
    solver->mip_start_accepted = 0;
    return 0;
}

/* ============================================================================
 * Update Incumbent
 * ============================================================================ */

static void update_incumbent(MIPSolver *solver, const double *solution, double obj) {
    LPModel *model = solver->original_model;

    /* First verify constraint feasibility (Ax sense b) */
    if (model->A && model->num_cons > 0) {
        double *ax = (double*)calloc(model->num_cons, sizeof(double));
        if (ax) {
            sparse_matvec(model->A, solution, ax);

            for (int i = 0; i < model->num_cons; i++) {
                double lhs = ax[i];
                double rhs = model->b[i];
                char sense = model->sense[i];

                int violated = 0;
                if (sense == 'L' && lhs > rhs + RALPH_FEAS_TOL) {
                    violated = 1;
                } else if (sense == 'G' && lhs < rhs - RALPH_FEAS_TOL) {
                    violated = 1;
                } else if (sense == 'E' && fabs(lhs - rhs) > RALPH_FEAS_TOL) {
                    violated = 1;
                }

                if (violated) {
                    free(ax);
                    if (solver->verbose >= 2) {
                        LP_LOG_STDOUT("  [update_incumbent] Rejected infeasible solution (constraint %d violated)\n", i);
                    }
                    return;  /* Reject infeasible solution */
                }
            }
            free(ax);
        }
    }

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
        node_queue_set_incumbent_found(solver->node_queue);

        /* Update cutoff for pruning */
        solver->cutoff = obj;

        if (solver->verbose) {
            LP_LOG_STDOUT("*** New incumbent: %.6f\n", obj);
        }
    }
}

/* Validate and, if feasible, accept a user-provided MIP start as incumbent.
 * Returns 1 if accepted, 0 if rejected. */
static int mip_try_accept_start(MIPSolver *solver, const double *x, const int *mask) {
    if (!solver || !solver->original_model || !x) return 0;

    LPModel *model = solver->original_model;
    int n = model->num_vars;
    int mode = solver->mip_start_repair_mode;

    double *cand = (double*)malloc((size_t)n * sizeof(double));
    if (!cand) return 0;
    memcpy(cand, x, (size_t)n * sizeof(double));

    if (mode >= (int)RALPH_MIP_START_REPAIR_PROJECT_BOUNDS) {
        for (int j = 0; j < n; j++) {
            if (cand[j] < model->lb[j]) cand[j] = model->lb[j];
            if (cand[j] > model->ub[j]) cand[j] = model->ub[j];
        }
    }

    if (mode >= (int)RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) {
        for (int j = 0; j < n; j++) {
            if (!solver->is_integer || !solver->is_integer[j]) continue;
            /* Do not force-round imputed entries unless they were explicitly set. */
            if (mask && !mask[j]) continue;
            cand[j] = round(cand[j]);
            if (cand[j] < model->lb[j]) cand[j] = model->lb[j];
            if (cand[j] > model->ub[j]) cand[j] = model->ub[j];
        }
    }

    /* Bounds + finite values + integrality checks */
    for (int j = 0; j < n; j++) {
        double v = cand[j];
        if (!isfinite(v)) {
            free(cand);
            return 0;
        }
        if (v < model->lb[j] - RALPH_FEAS_TOL || v > model->ub[j] + RALPH_FEAS_TOL) {
            free(cand);
            return 0;
        }
        if (solver->is_integer && solver->is_integer[j] &&
            fabs(v - round(v)) > RALPH_INT_TOL) {
            free(cand);
            return 0;
        }
    }

    /* Constraint feasibility */
    if (model->A && model->num_cons > 0) {
        double *ax = (double*)calloc((size_t)model->num_cons, sizeof(double));
        if (!ax) {
            free(cand);
            return 0;
        }
        sparse_matvec(model->A, cand, ax);

        for (int i = 0; i < model->num_cons; i++) {
            double lhs = ax[i];
            double rhs = model->b[i];
            char sense = model->sense[i];
            int violated = 0;
            if (sense == 'L' && lhs > rhs + RALPH_FEAS_TOL) {
                violated = 1;
            } else if (sense == 'G' && lhs < rhs - RALPH_FEAS_TOL) {
                violated = 1;
            } else if (sense == 'E' && fabs(lhs - rhs) > RALPH_FEAS_TOL) {
                violated = 1;
            }
            if (violated) {
                free(ax);
                free(cand);
                return 0;
            }
        }

        free(ax);
    }

    /* Compute objective in model objective space and register incumbent. */
    double obj = 0.0;
    for (int j = 0; j < n; j++) obj += model->c[j] * cand[j];
    int had_incumbent = solver->has_incumbent;
    double prev_obj = solver->best_obj;
    update_incumbent(solver, cand, obj);
    free(cand);

    if (!had_incumbent && solver->has_incumbent) return 1;
    if (!had_incumbent) return 0;

    if (model->obj_sense == 1) {
        return (solver->best_obj < prev_obj - RALPH_OPT_TOL) ? 1 : 0;
    }
    return (solver->best_obj > prev_obj + RALPH_OPT_TOL) ? 1 : 0;
}

/* Local fallback selector used when probing mutates LP state and the chosen
 * branch variable is no longer fractional in the current solution view. */
static int mip_select_most_infeasible(const MIPSolver *solver, const double *solution) {
    if (!solver || !solution) return -1;

    int best_var = -1;
    double best_infeas = RALPH_INT_TOL;
    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = solution[j];
        double frac = val - floor(val);
        if (frac < 0.0) frac += 1.0;
        if (frac <= RALPH_INT_TOL || frac >= 1.0 - RALPH_INT_TOL) continue;

        double infeas = fmin(frac, 1.0 - frac);
        if (infeas > best_infeas) {
            best_infeas = infeas;
            best_var = j;
        }
    }
    return best_var;
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

    /* Disable P5/P6 during diving — v2 reinitializes DSE per call, but
     * disabling prevents any interaction with the outer solve state. */
    int saved_bflip = lp->use_dual_bound_flip;
    int saved_dse = lp->use_dual_steepest_edge;
    lp->use_dual_bound_flip = 0;
    lp->use_dual_steepest_edge = 0;

    /* Save original bounds */
    double *orig_lb = (double*)calloc(num_vars, sizeof(double));
    double *orig_ub = (double*)calloc(num_vars, sizeof(double));
    if (!orig_lb || !orig_ub) {
        free(orig_lb);
        free(orig_ub);
        lp->use_dual_bound_flip = saved_bflip;
        lp->use_dual_steepest_edge = saved_dse;
        return -1;
    }
    memcpy(orig_lb, model->lb, num_vars * sizeof(double));
    memcpy(orig_ub, model->ub, num_vars * sizeof(double));

    /* Re-read tableau each loop/recovery step because cold-start paths may replace it. */
    SimplexTableau *tab = lp->tableau;

    /* Work with copy of solution */
    double *sol = (double*)calloc(num_vars, sizeof(double));
    if (!sol) {
        free(orig_lb);
        free(orig_ub);
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
                /* Check constraint feasibility (Ax sense b) */
                int constraints_satisfied = 1;
                LPModel *orig_model = solver->original_model;
                if (orig_model->A && orig_model->num_cons > 0) {
                    double *ax = (double*)calloc(orig_model->num_cons, sizeof(double));
                    if (ax) {
                        sparse_matvec(orig_model->A, sol, ax);

                        for (int i = 0; i < orig_model->num_cons; i++) {
                            double lhs = ax[i];
                            double rhs = orig_model->b[i];
                            char sense = orig_model->sense[i];

                            if (sense == 'L' && lhs > rhs + RALPH_FEAS_TOL) {
                                constraints_satisfied = 0;
                                break;
                            } else if (sense == 'G' && lhs < rhs - RALPH_FEAS_TOL) {
                                constraints_satisfied = 0;
                                break;
                            } else if (sense == 'E' && fabs(lhs - rhs) > RALPH_FEAS_TOL) {
                                constraints_satisfied = 0;
                                break;
                            }
                        }
                        free(ax);
                    }
                }

                if (constraints_satisfied) {
                    /* Compute objective */
                    double obj = 0.0;
                    for (int j = 0; j < num_vars; j++) {
                        obj += solver->original_model->c[j] * sol[j];
                    }
                    update_incumbent(solver, sol, obj);
                    found_incumbent = 1;
                }
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

        /* Try warm re-optimization if tableau available, else cold-start. */
        tab = lp->tableau;
        if (tab &&
            mip_lp_apply_structural_bounds(tab, num_vars, model->lb, model->ub) == 0 &&
            mip_lp_recompute(tab) == 0) {
            int rc = -1;
            (void)mip_lp_dual_reopt(lp, 500, &rc);
            if (lp->status == RALPH_STATUS_OPTIMAL && lp->solution) {
                memcpy(sol, lp->solution, num_vars * sizeof(double));
                continue;
            }
            /* If dual simplex failed, LP is likely infeasible */
            break;
        }

        /* No usable tableau - cold start */
        if (mip_lp_cold_start_primal(lp, 2) != 0 || lp->status != RALPH_STATUS_OPTIMAL ||
            !lp->solution) {
            break;
        }
        memcpy(sol, lp->solution, num_vars * sizeof(double));
    }

    /* Restore original bounds (max_iterations restored AFTER cold-start below) */
    memcpy(model->lb, orig_lb, num_vars * sizeof(double));
    memcpy(model->ub, orig_ub, num_vars * sizeof(double));

    /* Restore tableau bounds and re-solve to get back to original state */
    tab = lp->tableau;
    if (tab &&
        mip_lp_apply_structural_bounds(tab, num_vars, orig_lb, orig_ub) == 0 &&
        mip_lp_recompute(tab) == 0) {
        int rc = -1;
        (void)mip_lp_dual_reopt(lp, 500, &rc);
        if (lp->status != RALPH_STATUS_OPTIMAL && lp->tableau) {
            (void)mip_lp_cold_start_primal(lp, 2);
        }
    } else if (mip_lp_cold_start_primal(lp, 2) != 0) {
        /* Keep best-effort recovery semantics. */
    }

    /* Restore original iteration limit AFTER all cold-start paths */
    lp->max_iterations = orig_max_iter;

    /* Restore P5/P6 flags */
    lp->use_dual_bound_flip = saved_bflip;
    lp->use_dual_steepest_edge = saved_dse;

    /* Force refactorization to clear any numerical drift from diving */
    tab = lp->tableau;
    if (tab) (void)mip_lp_refactor_and_recompute(tab);

    free(orig_lb);
    free(orig_ub);
    free(sol);

    return found_incumbent ? 0 : -1;
}

/* ============================================================================
 * Reduced-Cost Fixing
 *
 * After solving a node LP with bound z_LP and incumbent z*, the gap is
 * z* - z_LP (in minimization space). For non-basic integer variable x_j:
 *   - At lower bound with rc[j] > gap: fix x_j = lb
 *   - At upper bound with |rc[j]| > gap: fix x_j = ub
 * ============================================================================ */

static int rc_fix_node(MIPSolver *solver, BBNode *node) {
    if (!solver->has_incumbent || !solver->lp_solver || !solver->lp_solver->tableau) {
        return 0;
    }

    SimplexTableau *tab = solver->lp_solver->tableau;
    LPModel *model = solver->working_model;
    int num_vars = model->num_vars;

    /* Compute gap in internal (minimization) space */
    int obj_sense = solver->original_model->obj_sense;
    double internal_incumbent = solver->best_obj * obj_sense;
    double internal_lp = tab->obj_value;
    double gap = internal_incumbent - internal_lp;

    if (gap < MIP_RC_FIX_MIN_GAP) {
        return 0;  /* Gap too small for reliable fixing */
    }

    int fixed = 0;

    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        if (j >= num_vars) continue;  /* Only structural vars */

        /* Skip already-fixed variables */
        if (fabs(node->ub[j] - node->lb[j]) < RALPH_INT_TOL) continue;

        double rc = tab->rc[j];

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc > gap) {
            /* Increasing x_j from lb worsens objective beyond incumbent → fix at lb */
            node->ub[j] = node->lb[j];
            model->ub[j] = model->lb[j];
            fixed++;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && -rc > gap) {
            /* Decreasing x_j from ub worsens objective beyond incumbent → fix at ub */
            node->lb[j] = node->ub[j];
            model->lb[j] = model->ub[j];
            fixed++;
        }
    }

    if (fixed > 0) {
        (void)mip_lp_apply_structural_bounds(tab, num_vars, model->lb, model->ub);
    }

    solver->rc_fixings += fixed;
    return fixed;
}

/* ============================================================================
 * RINS Heuristic (Relaxation Induced Neighborhood Search)
 *
 * Fix integer variables where LP relaxation and incumbent agree (both
 * integer-valued, same value). Dive on remaining fractional variables.
 * ============================================================================ */

static int rins_heuristic(MIPSolver *solver) {
    if (!solver || !solver->has_incumbent || !solver->lp_solver ||
        !solver->lp_solver->solution || !solver->lp_solver->tableau) {
        return -1;
    }

    LPModel *model = solver->working_model;
    SimplexSolver *lp = solver->lp_solver;
    int num_vars = model->num_vars;
    double *lp_sol = lp->solution;
    double *inc_sol = solver->best_solution;

    solver->rins_calls++;

    /* Count agreeing and free integer variables */
    int num_agree = 0;
    int num_free = 0;

    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        if (j >= num_vars) continue;

        /* Skip already-fixed variables */
        if (fabs(model->ub[j] - model->lb[j]) < RALPH_INT_TOL) continue;

        double lp_val = lp_sol[j];
        double inc_val = inc_sol[j];
        double lp_frac = fabs(lp_val - round(lp_val));
        double inc_frac = fabs(inc_val - round(inc_val));

        if (lp_frac < RALPH_INT_TOL && inc_frac < RALPH_INT_TOL &&
            fabs(round(lp_val) - round(inc_val)) < RALPH_INT_TOL) {
            num_agree++;
        } else {
            num_free++;
        }
    }

    /* Skip if neighborhood is too restrictive */
    int total_unfixed = num_agree + num_free;
    if (total_unfixed == 0) return -1;
    if ((double)num_free / total_unfixed < MIP_RINS_MIN_FREE_PCT) return -1;

    /* Disable P5/P6 during RINS diving */
    int saved_bflip = lp->use_dual_bound_flip;
    int saved_dse = lp->use_dual_steepest_edge;
    lp->use_dual_bound_flip = 0;
    lp->use_dual_steepest_edge = 0;

    /* Save original bounds */
    double *orig_lb = (double*)calloc(num_vars, sizeof(double));
    double *orig_ub = (double*)calloc(num_vars, sizeof(double));
    if (!orig_lb || !orig_ub) {
        free(orig_lb);
        free(orig_ub);
        lp->use_dual_bound_flip = saved_bflip;
        lp->use_dual_steepest_edge = saved_dse;
        return -1;
    }
    memcpy(orig_lb, model->lb, num_vars * sizeof(double));
    memcpy(orig_ub, model->ub, num_vars * sizeof(double));

    /* Re-read tableau as needed because recovery paths may replace it. */
    SimplexTableau *tab = lp->tableau;

    /* Fix agreeing variables */
    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        if (j >= num_vars) continue;
        if (fabs(model->ub[j] - model->lb[j]) < RALPH_INT_TOL) continue;

        double lp_val = lp_sol[j];
        double inc_val = inc_sol[j];
        double lp_frac = fabs(lp_val - round(lp_val));
        double inc_frac = fabs(inc_val - round(inc_val));

        if (lp_frac < RALPH_INT_TOL && inc_frac < RALPH_INT_TOL &&
            fabs(round(lp_val) - round(inc_val)) < RALPH_INT_TOL) {
            double fix_val = round(lp_val);
            model->lb[j] = fix_val;
            model->ub[j] = fix_val;
        }
    }

    /* Re-solve LP with fixed neighborhood.
     * Invalidate DSE — many vars fixed, basis very different. */
    int orig_max_iter = lp->max_iterations;
    lp->max_iterations = MIP_RINS_LP_ITER_LIMIT;

    int found_incumbent = 0;
    double *sol = NULL;

    if (!tab ||
        mip_lp_apply_structural_bounds(tab, num_vars, model->lb, model->ub) != 0 ||
        mip_lp_recompute(tab) != 0) {
        goto rins_cleanup;
    }
    (void)mip_lp_dual_reopt(lp, MIP_RINS_LP_ITER_LIMIT, NULL);

    sol = (double*)calloc(num_vars, sizeof(double));
    if (!sol) goto rins_cleanup;

    if (lp->status != RALPH_STATUS_OPTIMAL) goto rins_cleanup;
    memcpy(sol, lp->solution, num_vars * sizeof(double));

    /* Dive on remaining fractional variables */
    for (int dive = 0; dive < MIP_RINS_MAX_DIVE; dive++) {
        int best_var = -1;
        double best_frac = RALPH_INT_TOL;

        for (int k = 0; k < solver->num_integers; k++) {
            int j = solver->integer_vars[k];
            if (j >= num_vars) continue;
            double val = sol[j];
            double frac = val - floor(val);
            double infeas = fmin(frac, 1.0 - frac);

            if (model->lb[j] < model->ub[j] - 0.5 && infeas > best_frac) {
                best_frac = infeas;
                best_var = j;
            }
        }

        if (best_var < 0) {
            /* All integer variables are integer-valued */
            if (check_integer_feasibility(solver, sol)) {
                /* Verify constraint feasibility */
                LPModel *orig_model = solver->original_model;
                int constraints_ok = 1;
                if (orig_model->A && orig_model->num_cons > 0) {
                    double *ax = (double*)calloc(orig_model->num_cons, sizeof(double));
                    if (ax) {
                        sparse_matvec(orig_model->A, sol, ax);
                        for (int i = 0; i < orig_model->num_cons; i++) {
                            double lhs = ax[i];
                            double rhs = orig_model->b[i];
                            char sense = orig_model->sense[i];
                            if ((sense == 'L' && lhs > rhs + RALPH_FEAS_TOL) ||
                                (sense == 'G' && lhs < rhs - RALPH_FEAS_TOL) ||
                                (sense == 'E' && fabs(lhs - rhs) > RALPH_FEAS_TOL)) {
                                constraints_ok = 0;
                                break;
                            }
                        }
                        free(ax);
                    }
                }

                if (constraints_ok) {
                    double obj = 0.0;
                    for (int j = 0; j < num_vars; j++) {
                        obj += solver->original_model->c[j] * sol[j];
                    }
                    update_incumbent(solver, sol, obj);
                    found_incumbent = 1;
                    solver->rins_found++;
                    if (solver->verbose) {
                        LP_LOG_STDOUT("  [rins] Found incumbent: %.6f\n", obj);
                    }
                }
            }
            break;
        }

        /* Round to nearest integer and fix */
        double val = sol[best_var];
        double rounded = round(val);
        rounded = fmax(rounded, orig_lb[best_var]);
        rounded = fmin(rounded, orig_ub[best_var]);

        model->lb[best_var] = rounded;
        model->ub[best_var] = rounded;

        tab = lp->tableau;
        if (!tab ||
            mip_lp_apply_structural_bounds(tab, num_vars, model->lb, model->ub) != 0 ||
            mip_lp_recompute(tab) != 0) {
            break;
        }
        (void)mip_lp_dual_reopt(lp, MIP_RINS_LP_ITER_LIMIT, NULL);

        if (lp->status != RALPH_STATUS_OPTIMAL) break;
        memcpy(sol, lp->solution, num_vars * sizeof(double));
    }

rins_cleanup:
    free(sol);

    /* Restore original bounds (max_iterations restored AFTER cold-start below) */
    memcpy(model->lb, orig_lb, num_vars * sizeof(double));
    memcpy(model->ub, orig_ub, num_vars * sizeof(double));

    /* Restore tableau bounds and re-solve */
    tab = lp->tableau;
    if (tab &&
        mip_lp_apply_structural_bounds(tab, num_vars, orig_lb, orig_ub) == 0 &&
        mip_lp_recompute(tab) == 0) {
        (void)mip_lp_dual_reopt(lp, 500, NULL);
        if (lp->status != RALPH_STATUS_OPTIMAL && lp->tableau) {
            (void)mip_lp_cold_start_primal(lp, 2);
        }
    } else {
        (void)mip_lp_cold_start_primal(lp, 2);
    }

    /* Restore original iteration limit AFTER all cold-start paths */
    lp->max_iterations = orig_max_iter;

    /* Restore P5/P6 flags */
    lp->use_dual_bound_flip = saved_bflip;
    lp->use_dual_steepest_edge = saved_dse;

    /* Refactorize to clear numerical drift */
    tab = lp->tableau;
    if (tab) (void)mip_lp_refactor_and_recompute(tab);

    free(orig_lb);
    free(orig_ub);

    return found_incumbent ? 0 : -1;
}

/* ============================================================================
 * Basis Warm Starting Helpers
 * ============================================================================ */

static int node_has_saved_basis(const BBNode *node) {
    return node && node->basis && node->var_status &&
           node->basis_size > 0 && node->var_status_size > 0;
}

static void clear_node_basis(BBNode *node) {
    if (!node) return;
    free(node->basis);
    free(node->var_status);
    node->basis = NULL;
    node->var_status = NULL;
    node->basis_size = 0;
    node->var_status_size = 0;
}

static int node_basis_snapshot_sane(const BBNode *node) {
    if (!node_has_saved_basis(node)) return 0;
    if (node->basis_size > node->var_status_size) return 0;

    int n = node->var_status_size;
    int m = node->basis_size;
    unsigned char *seen = (unsigned char*)calloc((size_t)n, sizeof(unsigned char));
    if (!seen) return 0;

    for (int j = 0; j < n; j++) {
        int st = (int)node->var_status[j];
        if (st < (int)RALPH_BASIC || st > (int)RALPH_FIXED) {
            free(seen);
            return 0;
        }
    }

    for (int i = 0; i < m; i++) {
        int bj = node->basis[i];
        if (bj < 0 || bj >= n || seen[bj]) {
            free(seen);
            return 0;
        }
        seen[bj] = 1;
    }

    free(seen);
    return 1;
}

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
        node->basis = (int*)calloc(m, sizeof(int));
        node->basis_size = m;
    }
    if (!node->var_status || node->var_status_size < n) {
        free(node->var_status);
        node->var_status = (VarStatus*)calloc(n, sizeof(VarStatus));
        node->var_status_size = n;
    }

    if (node->basis && node->var_status) {
        memcpy(node->basis, tab->basis, m * sizeof(int));
        memcpy(node->var_status, tab->var_status, n * sizeof(VarStatus));
    }
}

static int stage_node_basis_for_cold_start(MIPSolver *solver, SimplexSolver *lp,
                                           BBNode *node) {
    if (!solver || !lp || !node_has_saved_basis(node)) return -1;
    if (mip_lp_stage_warm_basis(lp, node->basis_size, node->var_status_size,
                                node->basis, node->var_status) != 0) {
        clear_node_basis(node);
        solver->node_basis_warm_rejected++;
        return -1;
    }
    solver->node_basis_staged++;
    return 0;
}

static int restore_node_basis_live(MIPSolver *solver, SimplexSolver *lp,
                                   BBNode *node) {
    if (!solver || !lp || !lp->tableau || !node_has_saved_basis(node)) return -1;

    SimplexTableau *tab = lp->tableau;
    if (tab->num_artificial != 0) return -1;
    if (node->basis_size != tab->m || node->var_status_size != tab->n) return -1;

    solver->node_basis_warm_attempts++;

    if (mip_lp_restore_warm_basis(lp, node->basis_size, node->var_status_size,
                                  node->basis, node->var_status) != 0) {
        clear_node_basis(node);
        solver->node_basis_warm_rejected++;
        return -1;
    }

    solver->node_basis_warm_applied++;
    return 0;
}

/* ============================================================================
 * Solve LP Relaxation at a Node
 * ============================================================================ */

/* ============================================================================
 * LAP-based LP Relaxation Solving
 * ============================================================================ */

/*
 * Solve LP relaxation using LAP solver.
 *
 * Returns:
 *   0 on success (solution in solver->lp_solver->solution, obj in obj_value)
 *   -1 on infeasible or error
 */
static int solve_node_lp_as_lap(MIPSolver *solver, BBNode *node) {
    if (!solver->use_lap_solver || !solver->lap_sig) {
        return -1;  /* LAP solving not available */
    }

    LPModel *model = solver->original_model;
    int num_vars = model->num_vars;

    /* Allocate solution array if needed */
    if (!solver->lp_solver) {
        solver->lp_solver = simplex_create(solver->working_model);
        if (!solver->lp_solver) return -1;
        solver->lp_solver->method = 2;  /* Phase E: clean dual simplex */
        solver->lp_solver->scaling = 0;
        mip_apply_dual_flags(solver);
    }

    SimplexSolver *lp = solver->lp_solver;

    /* Ensure solution array exists */
    if (!lp->solution) {
        lp->solution = (double *)calloc(num_vars, sizeof(double));
        if (!lp->solution) return -1;
    }

    /* Solve using LAP with current node bounds */
    double obj_val;
    int result = solve_lap_at_node(solver->lap_sig, node->lb, node->ub,
                                    lp->solution, &obj_val);

    if (result != 0) {
        lp->status = RALPH_STATUS_INFEASIBLE;
        return -1;
    }

    lp->obj_value = obj_val;
    lp->status = RALPH_STATUS_OPTIMAL;
    lp->iterations = 0;  /* LAP doesn't use simplex iterations */

    solver->lap_nodes_solved++;
    return 0;
}

static int solve_node_lp(MIPSolver *solver, BBNode *node) {
    /* Try LAP solver first if available */
    if (solver->use_lap_solver) {
        int lap_result = solve_node_lp_as_lap(solver, node);
        if (lap_result == 0) {
            return 0;  /* Successfully solved with LAP */
        }
        /* LAP failed (infeasible) - this is a valid result for pruning */
        if (solver->lp_solver && solver->lp_solver->status == RALPH_STATUS_INFEASIBLE) {
            return -1;
        }
        /* Otherwise fall through to simplex */
    }

    LPModel *model = solver->working_model;

    /* Create LP solver if needed — method=2 (auto: dual first, primal fallback) */
    if (!solver->lp_solver) {
        solver->lp_solver = simplex_create(model);
        if (!solver->lp_solver) return -1;
        solver->lp_solver->method = 2;  /* Phase E: clean dual simplex */
        solver->lp_solver->scaling = 0;
        mip_apply_dual_flags(solver);
    }

    SimplexSolver *lp = solver->lp_solver;
    SimplexTableau *tab = lp->tableau;
    int has_saved_basis = node_has_saved_basis(node);
    int can_warm_reuse = 0;
    int stage_allowed = (solver->node_basis_stage_cooldown <= 0);
    if (has_saved_basis && !node_basis_snapshot_sane(node)) {
        clear_node_basis(node);
        solver->node_basis_warm_rejected++;
        has_saved_basis = 0;
    }

    /* Update model bounds from node */
    for (int j = 0; j < model->num_vars; j++) {
        model->lb[j] = node->lb[j];
        model->ub[j] = node->ub[j];
    }

    solver->simplex_nodes_solved++;

    /* Set objective limit for early pruning in dual_simplex_solve_v2.
     * obj_sense converts to internal minimization space (1=min, -1=max). */
    if (solver->has_incumbent) {
        lp->objective_limit = solver->best_obj * model->obj_sense;
    } else {
        lp->objective_limit = RALPH_INFINITY;
    }

    /* Warm reuse is only allowed when this node carries a saved LP basis.
     * This removes the old ad-hoc "reuse whatever tableau is lying around" path. */
    if (has_saved_basis && tab && tab->num_artificial == 0) {
        int direct_reuse = (node->id == solver->last_solved_node_id) ||
                           (node->parent_id == solver->last_solved_node_id);
        if (direct_reuse) {
            solver->node_basis_warm_applied++;
            can_warm_reuse = 1;
            tab = lp->tableau;
        } else if (restore_node_basis_live(solver, lp, node) == 0) {
            can_warm_reuse = 1;
            tab = lp->tableau;
        }
    }

    if (can_warm_reuse) {
        if (mip_lp_apply_structural_bounds(tab, model->num_vars, node->lb, node->ub) == 0 &&
            mip_lp_recompute(tab) == 0) {
            int rc = -1;
            (void)mip_lp_dual_reopt(lp, 500, &rc);
            if (solver->verbose >= 2) {
                LP_LOG_STDOUT("  [solve_node_lp] warm v2: rc=%d status=%d iters=%d obj=%.4f\n",
                              rc, lp->status, lp->iterations, lp->obj_value);
            }
            if (rc == 0 && lp->status == RALPH_STATUS_OPTIMAL) {
                goto node_lp_done;
            }
            /* v2 detected infeasible or hit objective limit — valid result */
            if (lp->status == RALPH_STATUS_INFEASIBLE ||
                lp->status == RALPH_STATUS_OBJ_LIMIT) {
                goto node_lp_done;
            }
        }
        if (solver->verbose >= 2) {
            LP_LOG_STDOUT("  [solve_node_lp] warm path failed, cold starting\n");
        }
    } else if (solver->verbose >= 2) {
        LP_LOG_STDOUT("  [solve_node_lp] no usable node basis warm start\n");
    }

    /* Cold start: stage node basis (if any), rebuild tableau, solve with primal. */
    if (has_saved_basis && stage_allowed) {
        (void)stage_node_basis_for_cold_start(solver, lp, node);
    }
    (void)mip_lp_cold_start_primal(lp, 2);
    if (lp->warm_basis_last_rejected) {
        /* Cooldown gate: avoid repeatedly feeding staged warm bases when they
         * are rejected on this topology/branch neighborhood. */
        clear_node_basis(node);
        solver->node_basis_warm_rejected++;
        if (solver->node_basis_stage_cooldown < 64)
            solver->node_basis_stage_cooldown = 64;
    } else if (lp->warm_basis_last_applied) {
        solver->node_basis_stage_cooldown = 0;
    }
    if (solver->verbose >= 2) {
        LP_LOG_STDOUT("  [solve_node_lp] cold: status=%d iters=%d obj=%.4f\n",
               lp->status, lp->iterations, lp->obj_value);
    }

node_lp_done:
    node->lp_status = lp->status;
    node->lp_bound = lp->obj_value;
    node->lp_iterations = lp->iterations;
    solver->last_solved_node_id = node->id;
    if (solver->node_basis_stage_cooldown > 0) solver->node_basis_stage_cooldown--;

    if (solver->verbose && lp->status != RALPH_STATUS_OPTIMAL) {
        LP_LOG_STDOUT("  solve_node_lp: status=%d, obj=%.4f\n", lp->status, lp->obj_value);
    }

    return (lp->status == RALPH_STATUS_OPTIMAL) ? 0 : -1;
}

/* ============================================================================
 * Process a Single Node
 * ============================================================================ */

static int process_node(MIPSolver *solver, BBNode *node) {
    LPModel *model = solver->original_model;

    /* Capture parent LP bound for pseudocost update */
    double parent_lp_bound = node->lp_bound;

    /* Solve LP relaxation */
    if (solve_node_lp(solver, node) != 0) {
        /* LP infeasible or error - prune node */
        if (solver->verbose) {
            LP_LOG_STDOUT("  [process_node] Pruned: LP infeasible/error\n");
        }
        return 0;
    }

    /* Snapshot immediately after successful LP solve so descendants inherit
     * a basis from a clean node state (before strong-branch probing logic). */
    save_basis_to_node(solver->lp_solver, node, model->num_vars);

    double lp_obj = solver->lp_solver->obj_value;
    double *lp_sol = solver->lp_solver->solution;

    /* Invoke user-provided cut callback if available */
    if (solver->has_cut_callback && solver->cut_callback.generate_cuts) {
        RalphCut user_cuts[32];  /* Max cuts from callback per node */
        memset(user_cuts, 0, sizeof(user_cuts));

        int num_cuts = solver->cut_callback.generate_cuts(
            solver->cut_callback.user_data,
            lp_sol,
            model->num_vars,
            user_cuts,
            32
        );

        if (num_cuts > 0) {
            for (int i = 0; i < num_cuts && i < 32; i++) {
                RalphCut *uc = &user_cuts[i];
                if (!uc->indices || !uc->coeffs || uc->num_vars <= 0) continue;

                /* Convert RalphCut to internal Cut and add to pool */
                Cut *cut = cut_create(uc->num_vars);
                if (cut) {
                    for (int j = 0; j < uc->num_vars; j++) {
                        cut->indices[j] = uc->indices[j];
                        cut->values[j] = uc->coeffs[j];
                    }
                    cut->nnz = uc->num_vars;
                    cut->sense = (char)uc->sense;
                    cut->rhs = uc->rhs;
                    cut->type = CUT_GOMORY;  /* Generic cut type */
                    cut->violation = 0.0;
                    cut->age = 0;

                    cut_pool_add(solver->cut_pool, cut);
                    solver->cuts_generated++;
                }
            }

            if (solver->verbose) {
                LP_LOG_STDOUT("  [cut_callback] Added %d user cuts at node %d\n", num_cuts, node->id);
            }
        }
    }

    /* Check if node can be pruned by bound */
    if (solver->has_incumbent) {
        if (model->obj_sense == 1) {  /* Minimize */
            if (lp_obj >= solver->best_obj - RALPH_OPT_TOL) {
                if (solver->verbose) {
                    LP_LOG_STDOUT("  [process_node] Pruned by bound: lp_obj=%.4f >= incumbent=%.4f\n",
                           lp_obj, solver->best_obj);
                }
                return 0;  /* Prune */
            }
        } else {  /* Maximize */
            if (lp_obj <= solver->best_obj + RALPH_OPT_TOL) {
                if (solver->verbose) {
                    LP_LOG_STDOUT("  [process_node] Pruned by bound: lp_obj=%.4f <= incumbent=%.4f\n",
                           lp_obj, solver->best_obj);
                }
                return 0;  /* Prune */
            }
        }
    }
    if (solver->verbose) {
        LP_LOG_STDOUT("  [process_node] LP solved: obj=%.4f (incumbent=%.4f)\n",
               lp_obj, solver->has_incumbent ? solver->best_obj : -1.0);
    }

    /* Note: best_bound should track the best LP bound from OPEN nodes.
     * We don't update it here (from closed node). The root bound is used
     * initially, and we update it from the queue after pruning. */

    /* Update pseudo-costs from actual branching data */
    if (node->depth > 0 && node->branch_var >= 0) {
        update_pseudo_costs(solver, node->branch_var, node->branch_val,
                           parent_lp_bound, lp_obj, node->branch_dir);
    }

    /* Check integer feasibility */
    if (check_integer_feasibility(solver, lp_sol)) {
        /* Found integer solution */
        if (solver->verbose) {
            LP_LOG_STDOUT("  [process_node] Integer feasible! obj=%.4f\n", lp_obj);
        }
        update_incumbent(solver, lp_sol, lp_obj);
        return 0;  /* Node solved */
    }

    /* Try rounding heuristic */
    double *rounded_sol = (double*)calloc(model->num_vars, sizeof(double));
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
        double *greedy_sol = (double*)calloc(model->num_vars, sizeof(double));
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
                LP_LOG_STDOUT("  [greedy] Round-up incumbent: %.4f\n", greedy_obj);
            }
            free(greedy_sol);
        }
    }

    /* Reduced-cost fixing: tighten bounds using dual information + incumbent */
    if (solver->has_incumbent && solver->lp_solver && solver->lp_solver->tableau) {
        int rc_fixed = rc_fix_node(solver, node);
        if (solver->verbose && rc_fixed > 0) {
            LP_LOG_STDOUT("  [rc_fix] Fixed %d variables by reduced cost\n", rc_fixed);
        }
    }

    /* Select branching variable */
    int branch_var;
    if (select_branch_variable(solver, lp_sol, &branch_var) != 0) {
        /* No fractional integer variable - should be integer feasible */
        if (solver->verbose) {
            LP_LOG_STDOUT("  [process_node] No fractional var found - declaring integer feasible\n");
        }
        return 0;
    }

    /* Strong branching inside select_branch_variable may have corrupted
     * the LP state. Re-read solution and verify it's valid. */
    lp_sol = solver->lp_solver ? solver->lp_solver->solution : NULL;
    if (!lp_sol) {
        /* LP solution lost — recover through adapter-managed LP lifecycle. */
        if (solver->lp_solver) {
            (void)mip_lp_recover_state(solver->lp_solver);
            lp_sol = solver->lp_solver->solution;
        }
        if (!lp_sol) return -1;  /* Unrecoverable — prune node */
    }

    /* Guard against stale branch choice after reliability/strong probing. */
    double branch_val = lp_sol[branch_var];
    double branch_frac = branch_val - floor(branch_val);
    if (branch_frac < 0.0) branch_frac += 1.0;
    if (branch_frac <= RALPH_INT_TOL || branch_frac >= 1.0 - RALPH_INT_TOL) {
        int fallback = mip_select_most_infeasible(solver, lp_sol);
        if (fallback < 0) {
            if (solver->verbose >= 2) {
                LP_LOG_STDOUT("  [process_node] No fractional var after probing; pruning node\n");
            }
            return 0;
        }
        branch_var = fallback;
        branch_val = lp_sol[branch_var];
    }

    if (solver->verbose) {
        LP_LOG_STDOUT("  [process_node] Branching on var %d (val=%.4f)\n",
                      branch_var, branch_val);
    }

    /* Create child nodes */
    BBNode *child_down, *child_up;
    compute_branch_children(solver, node, branch_var, &child_down, &child_up);
    if (!child_down && !child_up) {
        if (solver->verbose >= 2) {
            LP_LOG_STDOUT("  [process_node] Branch produced no tightening; pruning node\n");
        }
        return 0;
    }

    /* Add children to queue */
    if (child_down) {
        child_down->id = solver->node_count++;
        node_queue_push(solver->node_queue, child_down);
        if (solver->verbose) {
            LP_LOG_STDOUT("  [process_node] Added child_down (id=%d)\n", child_down->id);
        }
    }
    if (child_up) {
        child_up->id = solver->node_count++;
        node_queue_push(solver->node_queue, child_up);
        if (solver->verbose) {
            LP_LOG_STDOUT("  [process_node] Added child_up (id=%d)\n", child_up->id);
        }
    }

    return 0;
}

/* ============================================================================
 * Root Node Processing with Cuts
 * ============================================================================ */

static int solve_root_node(MIPSolver *solver) {
    LPModel *model = solver->original_model;

    /* Create root node using pool if available, falls back to regular alloc */
    BBNode *root = solver->node_pool ?
                   bb_node_pool_get(solver->node_pool) :
                   bb_node_create(model->num_vars);
    if (!root) return -1;

    root->id = 0;
    root->depth = 0;

    /* Initialize bounds from model */
    for (int j = 0; j < model->num_vars; j++) {
        root->lb[j] = model->lb[j];
        root->ub[j] = model->ub[j];
    }

    /* Solve initial LP relaxation - use LAP solver if detected */
    int root_lp_solved = 0;

    if (solver->use_lap_solver && solver->lap_sig) {
        /* Use LAP solver for root LP */
        if (solve_node_lp_as_lap(solver, root) == 0) {
            root_lp_solved = 1;
            if (solver->verbose) {
                LP_LOG_STDOUT("Root LP solved with LAP solver\n");
            }
        }
        /* If LAP fails, fall through to simplex */
    }

    if (!root_lp_solved) {
        /* Use standard simplex for root LP */
        solver->lp_solver = simplex_create(solver->working_model);
        if (!solver->lp_solver) {
            bb_node_pool_return(solver->node_pool, root);
            return -1;
        }

        solver->lp_solver->method = 2;  /* Phase E: clean dual simplex */
        /* Disable scaling for MIP - cuts are generated from tableau which would need unscaling */
        solver->lp_solver->scaling = 0;
        solver->lp_solver->verbose = solver->verbose;
        mip_apply_dual_flags(solver);

        simplex_solve(solver->lp_solver);
    }

    if (solver->lp_solver->status != RALPH_STATUS_OPTIMAL) {
        solver->status = solver->lp_solver->status;
        bb_node_pool_return(solver->node_pool, root);
        return 0;
    }

    solver->root_bound = solver->lp_solver->obj_value;
    solver->root_iterations = solver->lp_solver->iterations;

    root->lp_bound = solver->root_bound;
    root->lp_status = RALPH_STATUS_OPTIMAL;

    if (solver->verbose) {
        LP_LOG_STDOUT("Root LP: obj = %.6f, iter = %d\n",
               solver->root_bound, solver->root_iterations);
    }

    /* Check if LP solution is integer feasible */
    if (check_integer_feasibility(solver, solver->lp_solver->solution)) {
        update_incumbent(solver, solver->lp_solver->solution, solver->lp_solver->obj_value);
        solver->status = RALPH_STATUS_OPTIMAL;
        bb_node_pool_return(solver->node_pool, root);
        return 0;
    }

    /* Try diving heuristic to find an incumbent early.
     * This enables bound-based pruning in the B&B search. */
    if (solver->verbose) {
        LP_LOG_STDOUT("Running diving heuristic...\n");
    }
    if (diving_heuristic(solver) == 0) {
        if (solver->verbose) {
            LP_LOG_STDOUT("Diving found incumbent: %.6f\n", solver->best_obj);
        }
        /* Check if diving found optimal (gap closed) */
        double gap = fabs(solver->best_obj - solver->root_bound);
        if (gap < solver->abs_mip_gap) {
            solver->status = RALPH_STATUS_OPTIMAL;
            bb_node_pool_return(solver->node_pool, root);
            return 0;
        }
    }

    /* Try SCP-specific heuristics if SCP structure detected */
    if (solver->use_scp_solver) {
        double *scp_solution = (double *)calloc(model->num_vars, sizeof(double));
        if (scp_solution) {
            if (solver->verbose) {
                LP_LOG_STDOUT("Running SCP heuristics...\n");
            }
            /* Run LP-guided greedy + local search */
            if (heuristic_scp(solver, solver->lp_solver->solution, scp_solution) == 0) {
                /* Compute objective value */
                double scp_obj = 0.0;
                for (int j = 0; j < model->num_vars; j++) {
                    scp_obj += model->c[j] * scp_solution[j];
                }
                /* Check if this is better than current incumbent */
                int is_better = (model->obj_sense == 1) ?
                                (scp_obj < solver->best_obj) : (scp_obj > solver->best_obj);
                if (!solver->has_incumbent || is_better) {
                    update_incumbent(solver, scp_solution, scp_obj);
                    if (solver->verbose) {
                        LP_LOG_STDOUT("SCP heuristic found incumbent: %.6f\n", scp_obj);
                    }
                }
            }
            free(scp_solution);

            /* Check if heuristic found optimal (gap closed) */
            if (solver->has_incumbent) {
                double gap = fabs(solver->best_obj - solver->root_bound);
                if (gap < solver->abs_mip_gap) {
                    solver->status = RALPH_STATUS_OPTIMAL;
                    bb_node_pool_return(solver->node_pool, root);
                    return 0;
                }
            }
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

        /* Generate SCP-specific cuts (clique, odd-hole, lifted cover) */
        if (solver->use_scp_solver) {
            int scp_cuts = generate_scp_cuts(solver, solver->cut_pool);
            cuts_added += scp_cuts;
            solver->scp_cuts_generated += scp_cuts;
        }

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
            LP_LOG_STDOUT("Cut round %d: %d cuts generated (pool size: %d)\n",
                   cut_rounds + 1, cuts_added, solver->cut_pool->count);
        }

        /* Apply cuts to the LP relaxation */
        int cuts_applied = apply_cuts(solver, solver->cut_pool, solver->max_cuts_per_round);
        total_cuts_applied += cuts_applied;

        if (cuts_applied > 0) {
            if (solver->verbose) {
                LP_LOG_STDOUT("Cut round %d: %d cuts applied (total: %d)\n",
                       cut_rounds + 1, cuts_applied, total_cuts_applied);
            }

            /* Rebuild simplex solver with new constraints */
            simplex_free(solver->lp_solver);
            solver->lp_solver = simplex_create(solver->working_model);
            if (!solver->lp_solver) {
                bb_node_pool_return(solver->node_pool, root);
                return -1;
            }

            solver->lp_solver->method = 2;  /* Phase E: clean dual simplex */
            /* Disable scaling for MIP and propagate verbose flag */
            solver->lp_solver->scaling = 0;
            solver->lp_solver->verbose = solver->verbose;
            mip_apply_dual_flags(solver);

            if (solver->verbose) {
                LP_LOG_STDOUT("  Re-solving LP with %d constraints...\n",
                       solver->working_model->num_cons);
            }

            /* Re-solve LP with cuts */
            simplex_solve(solver->lp_solver);

            if (solver->verbose) {
                LP_LOG_STDOUT("  LP after cuts: status=%d, obj=%.6f\n",
                       solver->lp_solver->status, solver->lp_solver->obj_value);
            }

            if (solver->lp_solver->status != RALPH_STATUS_OPTIMAL) {
                /* LP became infeasible with cuts - discard all cuts and continue.
                 * Safety: rebuild working model from original, re-solve. */
                if (solver->verbose) {
                    LP_LOG_STDOUT("  WARNING: LP non-optimal after cuts (status=%d), discarding cuts\n",
                           solver->lp_solver->status);
                }
                simplex_free(solver->lp_solver);
                lp_model_free(solver->working_model);
                solver->working_model = lp_model_copy(solver->original_model);
                if (!solver->working_model) {
                    bb_node_pool_return(solver->node_pool, root);
                    return -1;
                }
                solver->lp_solver = simplex_create(solver->working_model);
                if (!solver->lp_solver) {
                    bb_node_pool_return(solver->node_pool, root);
                    return -1;
                }
                solver->lp_solver->method = 2;  /* Phase E: clean dual simplex */
                solver->lp_solver->scaling = 0;
                solver->lp_solver->verbose = solver->verbose;
                mip_apply_dual_flags(solver);
                simplex_solve(solver->lp_solver);
                root->lp_bound = solver->lp_solver->obj_value;
                solver->cuts_applied = 0;
                break;
            }

            double new_bound = solver->lp_solver->obj_value;

            /* Update cut efficacy based on new LP solution */
            int binding = cut_pool_update_efficacy(solver->cut_pool,
                                                   solver->lp_solver->solution,
                                                   solver->original_model->num_vars);
            if (solver->verbose >= 2) {
                LP_LOG_STDOUT("Cut round %d: %d binding cuts\n", cut_rounds + 1, binding);
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
                LP_LOG_STDOUT("LP bound improved: %.6f -> %.6f (stall count: %d)\n",
                       root->lp_bound, new_bound, no_improvement_rounds);
            }
            root->lp_bound = new_bound;

            /* Check if LP solution is now integer feasible */
            if (check_integer_feasibility(solver, solver->lp_solver->solution)) {
                update_incumbent(solver, solver->lp_solver->solution, solver->lp_solver->obj_value);
                solver->status = RALPH_STATUS_OPTIMAL;
                cut_pool_clear(solver->cut_pool);
                bb_node_pool_return(solver->node_pool, root);
                return 0;
            }

            /* Periodic cleanup of old cuts */
            if (cut_rounds > 0 && cut_rounds % 5 == 0) {
                int before = solver->cut_pool->count;
                cut_pool_cleanup(solver->cut_pool, 5);  /* Remove cuts older than 5 rounds */
                if (solver->verbose >= 2 && solver->cut_pool->count < before) {
                    LP_LOG_STDOUT("Cut cleanup: removed %d old cuts\n", before - solver->cut_pool->count);
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
    solver->last_solved_node_id = root->id;

    /* Initialize best bound */
    solver->best_bound = root->lp_bound;

    /* Compute Lagrangian bound for SCP (often tighter than LP) */
    if (solver->use_scp_solver && solver->has_incumbent) {
        double *lagr_solution = (double *)calloc(model->num_vars, sizeof(double));
        double lagr_lower = -RALPH_INFINITY;

        if (lagr_solution) {
            if (solver->verbose) {
                LP_LOG_STDOUT("Computing Lagrangian bound...\n");
            }
            if (lagrangian_solve_scp(solver, lagr_solution, &lagr_lower) == 0) {
                solver->lagrangian_bound = lagr_lower;

                /* Use Lagrangian bound if tighter than LP bound */
                int lagr_tighter = (model->obj_sense == 1) ?
                                   (lagr_lower > solver->best_bound) :
                                   (lagr_lower < solver->best_bound);
                if (lagr_tighter) {
                    if (solver->verbose) {
                        LP_LOG_STDOUT("Lagrangian bound (%.6f) tighter than LP (%.6f)\n",
                               lagr_lower, solver->best_bound);
                    }
                    solver->best_bound = lagr_lower;
                    root->lp_bound = lagr_lower;
                }

                /* Check if Lagrangian found a better solution */
                double lagr_obj = 0.0;
                for (int j = 0; j < model->num_vars; j++) {
                    lagr_obj += model->c[j] * lagr_solution[j];
                }
                int is_better = (model->obj_sense == 1) ?
                                (lagr_obj < solver->best_obj) : (lagr_obj > solver->best_obj);
                if (is_better) {
                    update_incumbent(solver, lagr_solution, lagr_obj);
                    if (solver->verbose) {
                        LP_LOG_STDOUT("Lagrangian found better incumbent: %.6f\n", lagr_obj);
                    }
                }

                /* Check if gap is closed */
                double gap = fabs(solver->best_obj - solver->best_bound);
                if (gap < solver->abs_mip_gap) {
                    solver->status = RALPH_STATUS_OPTIMAL;
                    free(lagr_solution);
                    bb_node_pool_return(solver->node_pool, root);
                    return 0;
                }
            }
            free(lagr_solution);
        }
    }

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
        LP_LOG_STDOUT("\n=== Ralph MIP Solver ===\n");
        LP_LOG_STDOUT("Variables: %d (%d integer)\n", model->num_vars, solver->num_integers);
        LP_LOG_STDOUT("Constraints: %d\n", model->num_cons);
    }

    /* Try user-provided MIP start once per solve. */
    if (solver->mip_start && solver->mip_start_n == model->num_vars) {
        int accepted;
        solver->mip_start_attempted++;
        accepted = mip_try_accept_start(solver, solver->mip_start, solver->mip_start_mask);
        if (accepted) {
            solver->mip_start_accepted++;
            if (solver->verbose) {
                LP_LOG_STDOUT("Accepted MIP start incumbent: %.6f\n", solver->best_obj);
            }
        } else if (solver->verbose) {
            LP_LOG_STDOUT("Rejected MIP start (infeasible or incompatible)\n");
        }
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
                LP_LOG_STDOUT("[mip_solve] Queue empty, exiting loop\n");
            }
            break;
        }

        if (solver->verbose) {
            LP_LOG_STDOUT("[mip_solve] Processing node %d (depth=%d)\n", node->id, node->depth);
        }

        /* Process node */
        process_node(solver, node);
        solver->nodes_explored++;

        /* RINS heuristic: search LP-incumbent agreement neighborhood */
        if (solver->has_incumbent && solver->lp_solver && solver->lp_solver->tableau) {
            int rins_interval = (solver->num_integers < 50) ?
                                MIP_RINS_INTERVAL_SMALL : MIP_RINS_INTERVAL;
            if (solver->nodes_explored % rins_interval == 0) {
                rins_heuristic(solver);
            }
        }

        if (node->depth > solver->max_depth) {
            solver->max_depth = node->depth;
        }

        /* Prune nodes by bound and update best_bound from remaining open nodes */
        if (solver->has_incumbent) {
            int queue_size_before = solver->node_queue->size;
            node_queue_update_bound_with_pool(solver->node_queue, solver->cutoff, solver->node_pool);
            if (solver->verbose && solver->node_queue->size < queue_size_before) {
                LP_LOG_STDOUT("[mip_solve] Pruned %d nodes by bound (cutoff=%.4f)\n",
                       queue_size_before - solver->node_queue->size, solver->cutoff);
            }
        }
        solver->best_bound = node_queue_best_bound(solver->node_queue);
        if (solver->verbose) {
            LP_LOG_STDOUT("[mip_solve] Queue size=%d, best_bound=%.4f\n",
                   solver->node_queue->size, solver->best_bound);
        }

        /* Print progress */
        if (solver->verbose && solver->nodes_explored % 100 == 0) {
            LP_LOG_STDOUT("Nodes: %d, Best: %.4f, Bound: %.4f, Gap: %.2f%%\n",
                   solver->nodes_explored, solver->best_obj, solver->best_bound,
                   100.0 * fabs(solver->best_obj - solver->best_bound) /
                   (fabs(solver->best_obj) + 1e-10));
        }

        bb_node_pool_return(solver->node_pool, node);
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

    LP_LOG_STDOUT("\n=== MIP Statistics ===\n");
    LP_LOG_STDOUT("Status: %d\n", solver->status);
    LP_LOG_STDOUT("Nodes explored: %d\n", solver->nodes_explored);
    LP_LOG_STDOUT("Max depth: %d\n", solver->max_depth);
    LP_LOG_STDOUT("Cuts generated: %d\n", solver->cuts_generated);
    LP_LOG_STDOUT("RC fixings: %d\n", solver->rc_fixings);
    LP_LOG_STDOUT("RINS calls: %d (found %d incumbents)\n", solver->rins_calls, solver->rins_found);
    LP_LOG_STDOUT("Solve time: %.3f seconds\n", solver->solve_time);

    /* Node pool statistics */
    if (solver->node_pool) {
        BBNodePool *pool = solver->node_pool;
        int in_use = pool->capacity - pool->free_count;
        LP_LOG_STDOUT("Node pool: %d/%d capacity, peak %d (%.1f%% utilized)\n",
               in_use, pool->capacity, pool->nodes_allocated,
               100.0 * pool->nodes_allocated / pool->capacity);
        if (pool->nodes_allocated >= pool->capacity) {
            LP_LOG_STDOUT("  WARNING: Pool exhausted - fell back to malloc\n");
        }
    }

    if (solver->use_lap_solver) {
        LP_LOG_STDOUT("LAP solver: enabled (%dx%d assignment)\n",
               solver->lap_sig ? solver->lap_sig->base.n : 0,
               solver->lap_sig ? solver->lap_sig->base.n : 0);
        LP_LOG_STDOUT("Nodes solved with LAP: %d\n", solver->lap_nodes_solved);
        LP_LOG_STDOUT("Nodes solved with simplex: %d\n", solver->simplex_nodes_solved);
    }

    if (solver->has_incumbent) {
        LP_LOG_STDOUT("Best objective: %.10f\n", solver->best_obj);
        LP_LOG_STDOUT("Best bound: %.10f\n", solver->best_bound);
        LP_LOG_STDOUT("Gap: %.4f%%\n",
               100.0 * fabs(solver->best_obj - solver->best_bound) /
               (fabs(solver->best_obj) + 1e-10));
    } else {
        LP_LOG_STDOUT("No feasible solution found\n");
    }
}
