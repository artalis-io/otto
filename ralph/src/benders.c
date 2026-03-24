/*
 * Ralph - Generic Benders Decomposition Implementation
 *
 * Implements Benders decomposition for MIP problems with complicating variables.
 * Currently implements classic Benders (cuts at integer solutions).
 * Modern branch-and-Benders-cut (cuts at LP nodes) is planned but not yet implemented.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <limits.h>

#include "benders.h"
#include "ralph_lp.h"

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BENDERS_THETA_LOWER_BOUND -1e15
#define BENDERS_THETA_UPPER_BOUND 1e15
#define BENDERS_CUT_TOLERANCE 1e-6
#define BENDERS_INITIAL_CUT_CAPACITY 64

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static double get_time_sec(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

static void benders_init_error_result(RalphBendersResult *result) {
    if (!result) return;
    memset(result, 0, sizeof(*result));
    result->status = RALPH_STATUS_ERROR;
}

/* Sense-aware certificate scalar in original subproblem row space.
 * For >= rows, multiply RHS by -1 to map to <= orientation. */
static double benders_farkas_rhs_dot(const LPModel *sub, const double *ray) {
    double dot = 0.0;
    for (int i = 0; i < sub->num_cons; i++) {
        double rhs = sub->b[i];
        if (sub->sense[i] == 'G') rhs = -rhs;
        dot += ray[i] * rhs;
    }
    return dot;
}

/* Explicitly drop the current subproblem tableau to force a cold start on
 * the next simplex_solve() call. Used when warm_start_subproblems=0. */
static void benders_force_cold_subproblem(SimplexSolver *solver) {
    if (!solver || !solver->tableau) return;
    tableau_free(solver->tableau);
    solver->tableau = NULL;
    solver->farkas_valid = 0;
}

/* ============================================================================
 * Context Creation/Destruction
 * ============================================================================ */

BendersContext* benders_create(LPModel *model, const RalphBendersConfig *config) {
    if (!model || !config) return NULL;
    if (config->num_master_vars <= 0 || !config->master_var_indices) return NULL;

    BendersContext *ctx = (BendersContext*)calloc(1, sizeof(BendersContext));
    if (!ctx) return NULL;

    ctx->original_model = model;
    ctx->config = *config;

    /* Set defaults */
    if (ctx->config.gap_tolerance <= 0) ctx->config.gap_tolerance = 1e-6;
    if (ctx->config.max_iterations <= 0) ctx->config.max_iterations = 1000;
    if (ctx->config.num_scenarios <= 0) ctx->config.num_scenarios = 1;
    ctx->config.warm_start_master = ctx->config.warm_start_master ? 1 : 0;
    ctx->config.warm_start_subproblems = ctx->config.warm_start_subproblems ? 1 : 0;

    int n = model->num_vars;
    int m = model->num_cons;

    /* Allocate classification arrays */
    ctx->is_master_var = (int*)calloc(n, sizeof(int));
    ctx->master_to_orig = (int*)calloc(config->num_master_vars + 1, sizeof(int)); /* +1 for theta */
    ctx->sub_to_orig = (int*)calloc(n, sizeof(int));
    ctx->orig_to_master = (int*)malloc(n * sizeof(int));
    ctx->orig_to_sub = (int*)malloc(n * sizeof(int));
    ctx->constraint_class = (int*)calloc(m, sizeof(int));

    if (!ctx->is_master_var || !ctx->master_to_orig || !ctx->sub_to_orig ||
        !ctx->orig_to_master || !ctx->orig_to_sub || !ctx->constraint_class) {
        benders_free(ctx);
        return NULL;
    }

    /* Initialize mappings to -1 */
    for (int j = 0; j < n; j++) {
        ctx->orig_to_master[j] = -1;
        ctx->orig_to_sub[j] = -1;
    }

    /* Allocate cut pool */
    ctx->cuts_capacity = BENDERS_INITIAL_CUT_CAPACITY;
    ctx->cuts = (BendersCut*)calloc(ctx->cuts_capacity, sizeof(BendersCut));
    if (!ctx->cuts) {
        benders_free(ctx);
        return NULL;
    }

    /* Allocate subproblem arrays (one per scenario) */
    ctx->sub_models = (LPModel**)calloc(ctx->config.num_scenarios, sizeof(LPModel*));
    ctx->sub_solvers = (SimplexSolver**)calloc(ctx->config.num_scenarios, sizeof(SimplexSolver*));
    ctx->sub_bases = (RalphBasis**)calloc(ctx->config.num_scenarios, sizeof(RalphBasis*));
    if (!ctx->sub_models || !ctx->sub_solvers || !ctx->sub_bases) {
        benders_free(ctx);
        return NULL;
    }

    /* Initialize bounds */
    ctx->best_ub = (model->obj_sense == 1) ? RALPH_INFINITY : -RALPH_INFINITY;
    ctx->best_lb = (model->obj_sense == 1) ? -RALPH_INFINITY : RALPH_INFINITY;

    return ctx;
}

void benders_free(BendersContext *ctx) {
    if (!ctx) return;

    free(ctx->is_master_var);
    free(ctx->master_to_orig);
    free(ctx->sub_to_orig);
    free(ctx->orig_to_master);
    free(ctx->orig_to_sub);
    free(ctx->constraint_class);

    /* Free linking constraints */
    if (ctx->linking) {
        for (int i = 0; i < ctx->num_linking; i++) {
            free(ctx->linking[i].master_var_indices);
            free(ctx->linking[i].master_coeffs);
            free(ctx->linking[i].sub_var_indices);
            free(ctx->linking[i].sub_coeffs);
        }
        free(ctx->linking);
    }

    /* Free cuts */
    if (ctx->cuts) {
        for (int i = 0; i < ctx->num_cuts; i++) {
            free(ctx->cuts[i].master_var_indices);
            free(ctx->cuts[i].coeffs);
        }
        free(ctx->cuts);
    }

    /* Free master problem */
    if (ctx->master_model) lp_model_free(ctx->master_model);
    if (ctx->master_solver) mip_free(ctx->master_solver);

    /* Free subproblems */
    if (ctx->sub_models) {
        for (int s = 0; s < ctx->config.num_scenarios; s++) {
            if (ctx->sub_models[s]) lp_model_free(ctx->sub_models[s]);
        }
        free(ctx->sub_models);
    }
    if (ctx->sub_solvers) {
        for (int s = 0; s < ctx->config.num_scenarios; s++) {
            if (ctx->sub_solvers[s]) simplex_free(ctx->sub_solvers[s]);
        }
        free(ctx->sub_solvers);
    }
    if (ctx->sub_bases) {
        for (int s = 0; s < ctx->config.num_scenarios; s++) {
            if (ctx->sub_bases[s]) {
                ralph_lp_free_basis((RalphLPBasis *)ctx->sub_bases[s]);
            }
        }
        free(ctx->sub_bases);
    }

    free(ctx->master_solution);
    free(ctx->master_start);
    free(ctx->full_solution);

    free(ctx);
}

/* ============================================================================
 * Model Partitioning
 * ============================================================================ */

int benders_partition(BendersContext *ctx) {
    LPModel *model = ctx->original_model;
    const RalphBendersConfig *config = &ctx->config;
    int n = model->num_vars;
    int m = model->num_cons;

    /* Mark master variables */
    for (int i = 0; i < config->num_master_vars; i++) {
        int j = config->master_var_indices[i];
        if (j < 0 || j >= n) {
            if (ctx->config.verbose) {
                fprintf(stderr, "Benders: invalid master variable index %d\n", j);
            }
            return -1;
        }
        ctx->is_master_var[j] = 1;
    }

    /* Check theta variable */
    ctx->theta_auto_created = 0;
    if (config->theta_var >= 0) {
        if (config->theta_var >= n) {
            if (ctx->config.verbose) {
                fprintf(stderr, "Benders: invalid theta variable index %d\n", config->theta_var);
            }
            return -1;
        }
        /* Theta must be a master variable */
        if (!ctx->is_master_var[config->theta_var]) {
            ctx->is_master_var[config->theta_var] = 1;
        }
    } else {
        ctx->theta_auto_created = 1;
    }

    /* Build variable mappings */
    int master_idx = 0;
    int sub_idx = 0;

    for (int j = 0; j < n; j++) {
        if (ctx->is_master_var[j]) {
            ctx->orig_to_master[j] = master_idx;
            ctx->master_to_orig[master_idx] = j;
            master_idx++;
        } else {
            ctx->orig_to_sub[j] = sub_idx;
            ctx->sub_to_orig[sub_idx] = j;
            sub_idx++;
        }
    }

    ctx->num_master_vars = master_idx;
    ctx->num_sub_vars = sub_idx;

    if (ctx->num_sub_vars == 0) {
        if (ctx->config.verbose) {
            fprintf(stderr, "Benders: no subproblem variables (all vars are master)\n");
        }
        return -1;
    }

    /* Classify constraints */
    /* Access constraint matrix via CSC format */
    SparseMatrix *A = model->A;

    for (int i = 0; i < m; i++) {
        int has_master = 0;
        int has_sub = 0;

        /* Scan all columns to find entries in row i */
        for (int j = 0; j < n; j++) {
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                if (A->rowidx[p] == i) {
                    if (ctx->is_master_var[j]) {
                        has_master = 1;
                    } else {
                        has_sub = 1;
                    }
                    break;
                }
            }
            if (has_master && has_sub) break;
        }

        if (has_master && has_sub) {
            ctx->constraint_class[i] = 2; /* Linking */
            ctx->num_linking_cons++;
        } else if (has_master) {
            ctx->constraint_class[i] = 0; /* Master only */
            ctx->num_master_cons++;
        } else {
            ctx->constraint_class[i] = 1; /* Subproblem only */
            ctx->num_sub_cons++;
        }
    }

    if (ctx->num_master_cons == 0) {
        if (ctx->config.verbose) {
            fprintf(stderr,
                    "Benders: decomposition requires at least one pure-master constraint\n");
        }
        return -1;
    }

    /* Build linking constraint details */
    if (ctx->num_linking_cons > 0) {
        ctx->linking = (LinkingConstraint*)calloc(ctx->num_linking_cons, sizeof(LinkingConstraint));
        if (!ctx->linking) return -1;

        int link_idx = 0;
        for (int i = 0; i < m; i++) {
            if (ctx->constraint_class[i] != 2) continue;

            LinkingConstraint *lc = &ctx->linking[link_idx];
            lc->constraint_idx = i;
            lc->original_rhs = model->b[i];
            lc->sense = model->sense[i];

            /* Count terms */
            int master_count = 0, sub_count = 0;
            for (int j = 0; j < n; j++) {
                for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                    if (A->rowidx[p] == i) {
                        if (ctx->is_master_var[j]) master_count++;
                        else sub_count++;
                        break;
                    }
                }
            }

            lc->master_var_indices = (int*)malloc(master_count * sizeof(int));
            lc->master_coeffs = (double*)malloc(master_count * sizeof(double));
            lc->sub_var_indices = (int*)malloc(sub_count * sizeof(int));
            lc->sub_coeffs = (double*)malloc(sub_count * sizeof(double));

            if (!lc->master_var_indices || !lc->master_coeffs ||
                !lc->sub_var_indices || !lc->sub_coeffs) {
                /* Free partially allocated arrays for this linking constraint */
                free(lc->master_var_indices); lc->master_var_indices = NULL;
                free(lc->master_coeffs); lc->master_coeffs = NULL;
                free(lc->sub_var_indices); lc->sub_var_indices = NULL;
                free(lc->sub_coeffs); lc->sub_coeffs = NULL;
                /* benders_free will clean up ctx->linking and previous entries */
                return -1;
            }

            /* Fill terms */
            int mi = 0, si = 0;
            for (int j = 0; j < n; j++) {
                for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                    if (A->rowidx[p] == i) {
                        if (ctx->is_master_var[j]) {
                            lc->master_var_indices[mi] = ctx->orig_to_master[j];
                            lc->master_coeffs[mi] = A->values[p];
                            mi++;
                        } else {
                            lc->sub_var_indices[si] = ctx->orig_to_sub[j];
                            lc->sub_coeffs[si] = A->values[p];
                            si++;
                        }
                        break;
                    }
                }
            }
            lc->num_master_terms = master_count;
            lc->num_sub_terms = sub_count;

            link_idx++;
        }
        ctx->num_linking = link_idx;
    }

    if (ctx->config.verbose) {
        printf("Benders partition:\n");
        printf("  Master vars: %d, Subproblem vars: %d\n",
               ctx->num_master_vars, ctx->num_sub_vars);
        printf("  Master cons: %d, Sub cons: %d, Linking: %d\n",
               ctx->num_master_cons, ctx->num_sub_cons, ctx->num_linking_cons);
    }

    return 0;
}

/* ============================================================================
 * Master Problem Construction
 * ============================================================================ */

int benders_build_master(BendersContext *ctx) {
    LPModel *orig = ctx->original_model;
    int n_master = ctx->num_master_vars;

    /* Add theta variable if auto-creating */
    int total_master_vars = n_master + (ctx->theta_auto_created ? 1 : 0);

    /* Create master model using incremental API */
    ctx->master_model = lp_model_create();
    if (!ctx->master_model) return -1;

    LPModel *master = ctx->master_model;
    master->obj_sense = orig->obj_sense;

    /* Add master variables */
    for (int j = 0; j < n_master; j++) {
        int orig_j = ctx->master_to_orig[j];
        lp_model_add_var(master, orig->lb[orig_j], orig->ub[orig_j],
                        orig->c[orig_j], orig->var_type[orig_j]);
    }

    /* Add theta variable */
    if (ctx->theta_auto_created) {
        ctx->theta_in_master = n_master;
        lp_model_add_var(master, BENDERS_THETA_LOWER_BOUND, BENDERS_THETA_UPPER_BOUND,
                        1.0, 'C'); /* Minimize theta */
    } else {
        ctx->theta_in_master = ctx->orig_to_master[ctx->config.theta_var];
    }

    /* Add master-only constraints */
    for (int i = 0; i < orig->num_cons; i++) {
        if (ctx->constraint_class[i] != 0) continue; /* Skip non-master */

        /* Build constraint in master variable space */
        int *indices = (int*)malloc(total_master_vars * sizeof(int));
        double *values = (double*)malloc(total_master_vars * sizeof(double));
        int nnz = 0;

        for (int j = 0; j < orig->num_vars; j++) {
            if (!ctx->is_master_var[j]) continue;
            for (int p = orig->A->colptr[j]; p < orig->A->colptr[j + 1]; p++) {
                if (orig->A->rowidx[p] == i) {
                    indices[nnz] = ctx->orig_to_master[j];
                    values[nnz] = orig->A->values[p];
                    nnz++;
                    break;
                }
            }
        }

        if (nnz > 0) {
            lp_model_add_constraint(master, nnz, indices, values,
                                   orig->sense[i], orig->b[i]);
        }

        free(indices);
        free(values);
    }

    /* Finalize master model */
    if (lp_model_finalize(master) != 0) {
        return -1;
    }

    /* Allocate master solution storage */
    ctx->master_solution = (double*)calloc(total_master_vars, sizeof(double));
    ctx->full_solution = (double*)calloc(orig->num_vars, sizeof(double));

    if (!ctx->master_solution || !ctx->full_solution) return -1;

    if (ctx->config.verbose) {
        printf("Master problem: %d vars (%d original + %s theta), %d constraints\n",
               total_master_vars, n_master,
               ctx->theta_auto_created ? "auto" : "user",
               master->num_cons);
    }

    return 0;
}

/* ============================================================================
 * Subproblem Construction
 * ============================================================================ */

int benders_build_subproblem(BendersContext *ctx, int scenario) {
    if (scenario < 0 || scenario >= ctx->config.num_scenarios) return -1;

    LPModel *orig = ctx->original_model;
    int n_sub = ctx->num_sub_vars;

    /* Create subproblem model using incremental API */
    ctx->sub_models[scenario] = lp_model_create();
    if (!ctx->sub_models[scenario]) return -1;

    LPModel *sub = ctx->sub_models[scenario];
    sub->obj_sense = orig->obj_sense;

    /* Add subproblem variables (all continuous in subproblem) */
    for (int j = 0; j < n_sub; j++) {
        int orig_j = ctx->sub_to_orig[j];
        lp_model_add_var(sub, orig->lb[orig_j], orig->ub[orig_j],
                        orig->c[orig_j], 'C'); /* Relax to continuous */
    }

    /* Map original subproblem constraint indices */
    int *orig_to_sub_con = (int*)malloc(orig->num_cons * sizeof(int));
    if (!orig_to_sub_con) return -1;

    /* Add subproblem-only constraints */
    for (int i = 0; i < orig->num_cons; i++) {
        orig_to_sub_con[i] = -1;
        if (ctx->constraint_class[i] != 1) continue; /* Skip non-sub */

        /* Build constraint in subproblem variable space */
        int *indices = (int*)malloc(n_sub * sizeof(int));
        double *values = (double*)malloc(n_sub * sizeof(double));
        int nnz = 0;

        for (int j = 0; j < orig->num_vars; j++) {
            if (ctx->is_master_var[j]) continue;
            for (int p = orig->A->colptr[j]; p < orig->A->colptr[j + 1]; p++) {
                if (orig->A->rowidx[p] == i) {
                    indices[nnz] = ctx->orig_to_sub[j];
                    values[nnz] = orig->A->values[p];
                    nnz++;
                    break;
                }
            }
        }

        if (nnz > 0) {
            lp_model_add_constraint(sub, nnz, indices, values,
                                   orig->sense[i], orig->b[i]);
        }

        free(indices);
        free(values);
    }

    /* Add linking constraints (RHS will be updated per iteration)
     * Store sub_row_idx for robust dual/Farkas extraction (no offset math) */
    for (int k = 0; k < ctx->num_linking; k++) {
        LinkingConstraint *lc = &ctx->linking[k];
        if (lc->num_sub_terms > 0) {
            /* Record row index before adding constraint */
            if (scenario == 0) {
                lc->sub_row_idx = sub->num_cons;
            }
            lp_model_add_constraint(sub, lc->num_sub_terms,
                                   lc->sub_var_indices, lc->sub_coeffs,
                                   lc->sense, lc->original_rhs);
        }
    }

    free(orig_to_sub_con);

    /* Finalize subproblem model */
    if (lp_model_finalize(sub) != 0) {
        return -1;
    }

    if (ctx->config.verbose >= 2) {
        printf("Subproblem %d: %d vars, %d constraints (%d sub-only + %d linking)\n",
               scenario, n_sub, sub->num_cons, ctx->num_sub_cons, ctx->num_linking_cons);
    }

    return 0;
}

/* ============================================================================
 * Subproblem RHS Update
 * ============================================================================ */

int benders_update_subproblem_rhs(BendersContext *ctx, int scenario,
                                   const double *x_master) {
    if (!ctx->sub_models[scenario]) return -1;

    LPModel *sub = ctx->sub_models[scenario];

    /* Update RHS for linking constraints: h - T*x_master
     * Use explicit sub_row_idx for robust indexing */
    for (int k = 0; k < ctx->num_linking; k++) {
        LinkingConstraint *lc = &ctx->linking[k];
        double rhs = lc->original_rhs;

        /* Subtract T*x contribution */
        for (int t = 0; t < lc->num_master_terms; t++) {
            int master_j = lc->master_var_indices[t];
            if (master_j < 0 || master_j >= ctx->num_master_vars) return -1;
            rhs -= lc->master_coeffs[t] * x_master[master_j];
        }

        if (lc->sub_row_idx < 0 || lc->sub_row_idx >= sub->num_cons) {
            return -1;
        }
        sub->b[lc->sub_row_idx] = rhs;
    }
    return 0;
}

/* ============================================================================
 * Subproblem Solving
 * ============================================================================ */

int benders_solve_subproblem(BendersContext *ctx, int scenario,
                              double *obj, double *duals, double *farkas,
                              int *is_feasible) {
    if (!ctx->sub_models[scenario]) {
        if (benders_build_subproblem(ctx, scenario) != 0) {
            return -1;
        }
    }

    LPModel *sub = ctx->sub_models[scenario];

    /* Create or reuse solver */
    if (!ctx->sub_solvers[scenario]) {
        ctx->sub_solvers[scenario] = simplex_create(sub);
        if (!ctx->sub_solvers[scenario]) return -1;
        /* Force two-phase simplex for Benders subproblems.
         * This prevents BigM dual contamination that invalidates cuts. */
        ctx->sub_solvers[scenario]->force_two_phase = 1;
        ctx->sub_solvers[scenario]->verbose = ctx->config.verbose;
    }

    SimplexSolver *solver = ctx->sub_solvers[scenario];

    if (!ctx->config.warm_start_subproblems) {
        /* Orthogonal cold-start mode: explicitly drop tableau state so this
         * solve does not reuse prior basis/tableau information. */
        benders_force_cold_subproblem(solver);
    }

    if (solver->tableau && ctx->config.warm_start_subproblems) {
        ctx->subproblem_warm_starts++;
    } else {
        ctx->subproblem_cold_starts++;
    }

    /* Solve */
    int status = simplex_solve(solver);
    ctx->subproblems_solved++;

    if (solver->status == RALPH_STATUS_OPTIMAL) {
        *is_feasible = 1;
        *obj = solver->obj_value;

        /* Extract duals for linking constraints using explicit row indices */
        if (duals && solver->dual_solution) {
            for (int k = 0; k < ctx->num_linking; k++) {
                LinkingConstraint *lc = &ctx->linking[k];
                if (lc->sub_row_idx < 0 || lc->sub_row_idx >= solver->model->num_cons) {
                    return -1;
                }
                duals[k] = solver->dual_solution[lc->sub_row_idx];
            }
        }

    } else if (solver->status == RALPH_STATUS_INFEASIBLE) {
        *is_feasible = 0;
        *obj = RALPH_INFINITY;

        /* Extract Farkas ray for linking constraints + compute sub-only RHS contribution */
        if (!farkas || !solver->farkas_valid || !solver->farkas_ray) {
            return -1;
        }

        memset(farkas, 0, (ctx->num_linking + 1) * sizeof(double));

        /* Strict mode: validate in original constraint space (sense-aware),
         * and try sign flip if needed. */
        double ray_sign = 1.0;
        if (ctx->config.strict_farkas) {
            double yb = benders_farkas_rhs_dot(sub, solver->farkas_ray);
            if (yb >= -1e-9) {
                double yb_flip = -yb;
                if (yb_flip < -1e-9) {
                    ray_sign = -1.0;
                } else {
                    return -1;
                }
            }
        }

        /* Extract linking constraint Farkas multipliers using explicit row indices */
        for (int k = 0; k < ctx->num_linking; k++) {
            LinkingConstraint *lc = &ctx->linking[k];
            if (lc->sub_row_idx < 0 || lc->sub_row_idx >= solver->model->num_cons) {
                return -1;
            }
            farkas[k] = ray_sign * solver->farkas_ray[lc->sub_row_idx];
        }

        /* Compute sub-only constraint RHS contribution: sum(y_sub[i] * b_sub[i])
         * This is needed for the feasibility cut RHS: y'b = y_sub'b_sub + y_link'b_link
         * We store this in farkas[num_linking] as a special "RHS constant" slot */
        double sub_rhs_contrib = 0.0;
        LPModel *sub = ctx->sub_models[scenario];
        for (int i = 0; i < ctx->num_sub_cons; i++) {
            sub_rhs_contrib += (ray_sign * solver->farkas_ray[i]) * sub->b[i];
        }

        /* Normalize the Farkas ray to avoid numerical issues with huge coefficients.
         * We scale so the largest coefficient is 1.0. */
        double max_abs = fabs(sub_rhs_contrib);
        for (int k = 0; k < ctx->num_linking; k++) {
            double absval = fabs(farkas[k]);
            if (absval > max_abs) max_abs = absval;
        }
        if (max_abs > 1e-9) {
            for (int k = 0; k < ctx->num_linking; k++) {
                farkas[k] /= max_abs;
            }
            sub_rhs_contrib /= max_abs;
        }

        /* Store in a special slot - caller should allocate num_linking+1 */
        farkas[ctx->num_linking] = sub_rhs_contrib;

    } else {
        /* Solver error */
        (void)status; /* Suppress unused warning */
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Cut Generation
 * ============================================================================ */

static int benders_ensure_cut_capacity(BendersContext *ctx) {
    if (ctx->num_cuts >= ctx->cuts_capacity) {
        int new_cap = (ctx->cuts_capacity <= INT_MAX / 2)
                      ? ctx->cuts_capacity * 2
                      : INT_MAX;
        BendersCut *new_cuts = (BendersCut*)realloc(ctx->cuts,
                                                     new_cap * sizeof(BendersCut));
        if (!new_cuts) return -1;
        memset(new_cuts + ctx->cuts_capacity, 0,
               (new_cap - ctx->cuts_capacity) * sizeof(BendersCut));
        ctx->cuts = new_cuts;
        ctx->cuts_capacity = new_cap;
    }
    return 0;
}

int benders_add_optimality_cut(BendersContext *ctx, int scenario,
                                const double *duals, double sub_obj) {
    (void)duals; /* Duals are used for coefficient calculation below */

    if (benders_ensure_cut_capacity(ctx) != 0) return -1;

    BendersCut *cut = &ctx->cuts[ctx->num_cuts];
    memset(cut, 0, sizeof(BendersCut));

    cut->is_feasibility = 0;
    cut->scenario = scenario;

    /* Build cut: θ >= sub_obj - Σ π_k * T_k * (x - x_current) for linking k
     *
     * The standard Benders optimality cut from dual information is:
     *   θ >= π'(h - Tx) = π'h - π'Tx
     *
     * Where π are duals for ALL subproblem constraints and h is their RHS.
     * The constant π'h includes contributions from both sub-only and linking.
     *
     * A simpler approach: use sub_obj directly and add the linearization term
     * for how θ changes with x:
     *   θ >= sub_obj + Σ_k π_link_k * Σ_j T_kj * (x_j - x_current_j)
     *
     * Rearranging:
     *   θ + Σ_j (-Σ_k π_link_k * T_kj) * x_j >= sub_obj - Σ_j (Σ_k π_link_k * T_kj) * x_current_j
     *
     * Or equivalently, using the fact that:
     *   constant = sub_obj + Σ_k π_link_k * Σ_j T_kj * x_current_j
     *            = sub_obj + sum of (pi_k * lc->master_coeffs * x_master)
     */

    /* Count non-zero coefficients for master variables */
    int max_terms = ctx->num_master_vars;
    cut->master_var_indices = NULL;
    cut->coeffs = NULL;

    cut->master_var_indices = (int*)malloc(max_terms * sizeof(int));
    cut->coeffs = (double*)malloc(max_terms * sizeof(double));

    if (!cut->master_var_indices || !cut->coeffs) {
        free(cut->master_var_indices); cut->master_var_indices = NULL;
        free(cut->coeffs); cut->coeffs = NULL;
        return -1;
    }

    /* Accumulate coefficients for each master variable */
    double *master_coeffs = (double*)calloc(ctx->num_master_vars, sizeof(double));
    if (!master_coeffs) {
        free(cut->master_var_indices); cut->master_var_indices = NULL;
        free(cut->coeffs); cut->coeffs = NULL;
        return -1;
    }

    /* Get full dual solution from subproblem solver */
    SimplexSolver *sub_solver = ctx->sub_solvers[scenario];
    if (!sub_solver || !sub_solver->dual_solution) {
        free(master_coeffs);
        free(cut->master_var_indices);
        free(cut->coeffs);
        return -1;
    }

    /* The cut is: θ >= sub_obj evaluated at current x
     * The slope with respect to x is: -π_link * T
     * (negative because increasing x relaxes the subproblem, reducing cost)
     *
     * Cut: θ - Σ (π_link_k * T_kj) * x_j >= sub_obj - Σ (π_link_k * T_kj) * x_current_j
     *
     * Let coeff_j = -Σ_k (π_link_k * T_kj) = -π_link' * T_j
     * And constant = sub_obj + Σ_j coeff_j * x_current_j
     *
     * Actually, the standard form is:
     * θ + Σ (π_link_k * T_kj) * x_j >= π'h (where π'h = sub_obj + Σ (π_link_k * T_kj) * x_current)
     */

    double constant = sub_obj;  /* Start with subproblem objective */

    for (int k = 0; k < ctx->num_linking; k++) {
        LinkingConstraint *lc = &ctx->linking[k];
        double pi_k = sub_solver->dual_solution[lc->sub_row_idx];

        /* Add contribution to x coefficients: π_k * T_kj for each master var j */
        for (int t = 0; t < lc->num_master_terms; t++) {
            int master_j = lc->master_var_indices[t];
            if (master_j < 0 || master_j >= ctx->num_master_vars) {
                free(master_coeffs);
                free(cut->master_var_indices); cut->master_var_indices = NULL;
                free(cut->coeffs); cut->coeffs = NULL;
                return -1;
            }
            double T_kj = lc->master_coeffs[t];
            master_coeffs[master_j] += pi_k * T_kj;
        }

        /* Add to constant: π_k * T_k * x_current */
        for (int t = 0; t < lc->num_master_terms; t++) {
            int master_j = lc->master_var_indices[t];
            /* Already bounds-checked above */
            double T_kj = lc->master_coeffs[t];
            constant += pi_k * T_kj * ctx->master_solution[master_j];
        }
    }

    /* Build sparse cut */
    int num_terms = 0;
    for (int j = 0; j < ctx->num_master_vars; j++) {
        if (fabs(master_coeffs[j]) > BENDERS_CUT_TOLERANCE) {
            cut->master_var_indices[num_terms] = j;
            cut->coeffs[num_terms] = master_coeffs[j];
            num_terms++;
        }
    }

    free(master_coeffs);

    cut->num_terms = num_terms;
    cut->rhs = constant; /* θ + Σ coeff[i]*x[i] >= constant */

    ctx->num_cuts++;
    ctx->optimality_cuts_added++;

    if (ctx->config.verbose >= 2) {
        printf("  Optimality cut: θ");
        for (int i = 0; i < num_terms; i++) {
            printf(" + (%.4f)*x[%d]", cut->coeffs[i], cut->master_var_indices[i]);
        }
        printf(" >= %.4f (scenario %d, sub_obj=%.4f)\n", constant, scenario, sub_obj);

        /* Debug: show linking duals */
        printf("    Linking duals: ");
        for (int k = 0; k < ctx->num_linking; k++) {
            LinkingConstraint *lc = &ctx->linking[k];
            printf("π[%d]=%.4f ", k, sub_solver->dual_solution[lc->sub_row_idx]);
        }
        printf("\n");
    }

    return 0;
}

int benders_add_feasibility_cut(BendersContext *ctx, int scenario,
                                 const double *farkas) {
    if (benders_ensure_cut_capacity(ctx) != 0) return -1;

    BendersCut *cut = &ctx->cuts[ctx->num_cuts];
    memset(cut, 0, sizeof(BendersCut));

    cut->is_feasibility = 1;
    cut->scenario = scenario;

    /* Feasibility cut: 0 >= y'(h - Tx) where y is Farkas ray
     * Rearranged: y'Tx >= y'h
     */

    int max_terms = ctx->num_master_vars;
    cut->master_var_indices = (int*)malloc(max_terms * sizeof(int));
    cut->coeffs = (double*)malloc(max_terms * sizeof(double));

    if (!cut->master_var_indices || !cut->coeffs) {
        free(cut->master_var_indices);
        free(cut->coeffs);
        return -1;
    }

    double *master_coeffs = (double*)calloc(ctx->num_master_vars, sizeof(double));
    if (!master_coeffs) {
        free(cut->master_var_indices);
        free(cut->coeffs);
        return -1;
    }

    /* The constant (RHS) is y'b for the full subproblem:
     * constant = sum(y_sub[i] * b_sub[i]) + sum(y_link[k] * b_link[k])
     * The sub-only contribution is stored in farkas[num_linking] */
    double constant = farkas[ctx->num_linking];  /* Sub-only RHS contribution */

    for (int k = 0; k < ctx->num_linking; k++) {
        double y_k = farkas[k];
        if (fabs(y_k) < BENDERS_CUT_TOLERANCE) continue;

        LinkingConstraint *lc = &ctx->linking[k];

        for (int t = 0; t < lc->num_master_terms; t++) {
            int master_j = lc->master_var_indices[t];
            if (master_j < 0 || master_j >= ctx->num_master_vars) {
                free(master_coeffs);
                free(cut->master_var_indices); cut->master_var_indices = NULL;
                free(cut->coeffs); cut->coeffs = NULL;
                return -1;
            }
            master_coeffs[master_j] += y_k * lc->master_coeffs[t];
        }

        /* Add linking RHS contribution */
        constant += y_k * lc->original_rhs;
    }

    if (ctx->config.strict_farkas) {
        /* The feasibility cut must cut the current infeasible master point.
         * If orientation is wrong, try sign-flipping once and re-check. */
        double lhs = 0.0;
        for (int j = 0; j < ctx->num_master_vars; j++) {
            lhs += master_coeffs[j] * ctx->master_solution[j];
        }

        if (lhs >= constant - BENDERS_CUT_TOLERANCE) {
            for (int j = 0; j < ctx->num_master_vars; j++) {
                master_coeffs[j] = -master_coeffs[j];
            }
            constant = -constant;

            lhs = 0.0;
            for (int j = 0; j < ctx->num_master_vars; j++) {
                lhs += master_coeffs[j] * ctx->master_solution[j];
            }

            if (lhs >= constant - BENDERS_CUT_TOLERANCE) {
                free(master_coeffs);
                free(cut->master_var_indices);
                free(cut->coeffs);
                cut->master_var_indices = NULL;
                cut->coeffs = NULL;
                return -1;
            }
        }

        /* Normalize for numerical stability and re-check violation. */
        double scale = fabs(constant);
        for (int j = 0; j < ctx->num_master_vars; j++) {
            double a = fabs(master_coeffs[j]);
            if (a > scale) scale = a;
        }
        if (scale > 1e-12) {
            for (int j = 0; j < ctx->num_master_vars; j++) {
                master_coeffs[j] /= scale;
            }
            constant /= scale;
        }

        lhs = 0.0;
        for (int j = 0; j < ctx->num_master_vars; j++) {
            lhs += master_coeffs[j] * ctx->master_solution[j];
        }
        if (lhs >= constant - BENDERS_CUT_TOLERANCE) {
            free(master_coeffs);
            free(cut->master_var_indices);
            free(cut->coeffs);
            cut->master_var_indices = NULL;
            cut->coeffs = NULL;
            return -1;
        }
    }

    int num_terms = 0;
    for (int j = 0; j < ctx->num_master_vars; j++) {
        if (fabs(master_coeffs[j]) > BENDERS_CUT_TOLERANCE) {
            cut->master_var_indices[num_terms] = j;
            cut->coeffs[num_terms] = master_coeffs[j];
            num_terms++;
        }
    }

    if (num_terms == 0) {
        free(master_coeffs);
        free(cut->master_var_indices);
        free(cut->coeffs);
        cut->master_var_indices = NULL;
        cut->coeffs = NULL;
        return -1;
    }

    free(master_coeffs);

    cut->num_terms = num_terms;
    cut->rhs = constant;

    ctx->num_cuts++;
    ctx->feasibility_cuts_added++;

    if (ctx->config.verbose >= 2) {
        printf("  Added feasibility cut: ");
        for (int i = 0; i < num_terms; i++) {
            printf("%.4f*z[%d] ", cut->coeffs[i], cut->master_var_indices[i]);
            if (i < num_terms - 1) printf("+ ");
        }
        printf(">= %.4f (scenario %d)\n", constant, scenario);

        /* Also print the Farkas ray values */
        printf("    Farkas ray for linking: ");
        for (int kk = 0; kk < ctx->num_linking; kk++) {
            printf("y[%d]=%.6f ", kk, farkas[kk]);
        }
        printf("\n");
    }

    return 0;
}

/* ============================================================================
 * Apply Cuts to Master
 * ============================================================================ */

int benders_apply_cuts_to_master(BendersContext *ctx) {
    LPModel *master = ctx->master_model;
    int theta_idx = ctx->theta_in_master;

    for (int c = 0; c < ctx->num_cuts; c++) {
        BendersCut *cut = &ctx->cuts[c];

        /* Build cut constraint */
        int nnz = cut->num_terms + (cut->is_feasibility ? 0 : 1); /* +1 for theta */
        int *indices = (int*)malloc(nnz * sizeof(int));
        double *values = (double*)malloc(nnz * sizeof(double));

        if (!indices || !values) {
            free(indices);
            free(values);
            return -1;
        }

        int idx = 0;
        for (int t = 0; t < cut->num_terms; t++) {
            indices[idx] = cut->master_var_indices[t];
            values[idx] = cut->coeffs[t];
            idx++;
        }

        if (!cut->is_feasibility) {
            /* Optimality cut: θ + Σ coeff*x >= rhs
             * We want: theta >= rhs - sum(coeff * x)
             * Or equivalently: theta + sum(coeff * x) >= rhs
             * Note: coeffs are already the coefficients for master vars
             */
            indices[idx] = theta_idx;
            values[idx] = 1.0; /* +θ coefficient */
            idx++;

            /* Add as >= constraint: θ + Σ coeff*x >= rhs */
            lp_model_add_constraint(master, cut->num_terms + 1, indices, values,
                                   'G', cut->rhs);
        } else {
            /* Feasibility cut: Σ coeff*x >= rhs */
            lp_model_add_constraint(master, cut->num_terms, indices, values,
                                   'G', cut->rhs);
        }

        free(indices);
        free(values);
    }

    return 0;
}

/* ============================================================================
 * Convergence Check
 * ============================================================================ */

int benders_check_convergence(BendersContext *ctx, double master_obj,
                               double sub_obj, double *gap) {
    /* For minimization: master_obj is lower bound, master_obj + sub_obj is upper bound */
    /* Actually: master_obj includes θ which estimates sub_obj */

    double ub = ctx->best_ub;
    double lb = ctx->best_lb;

    /* Update bounds */
    if (ctx->original_model->obj_sense == 1) { /* Minimize */
        if (master_obj > lb) lb = master_obj;
        double total = master_obj - ctx->master_solution[ctx->theta_in_master] + sub_obj;
        if (total < ub) ub = total;
    } else { /* Maximize */
        if (master_obj < lb) lb = master_obj;
        double total = master_obj - ctx->master_solution[ctx->theta_in_master] + sub_obj;
        if (total > ub) ub = total;
    }

    ctx->best_lb = lb;
    ctx->best_ub = ub;

    /* Compute gap */
    double abs_gap = fabs(ub - lb);
    double rel_gap = (fabs(ub) > 1e-10) ? abs_gap / fabs(ub) : abs_gap;

    if (gap) *gap = rel_gap;

    return (rel_gap <= ctx->config.gap_tolerance ||
            abs_gap <= BENDERS_CUT_TOLERANCE);
}

/* ============================================================================
 * Classic Benders Algorithm
 * ============================================================================ */

int benders_solve_classic(BendersContext *ctx) {
    double start_time = get_time_sec();
    RalphStatus final_status = RALPH_STATUS_UNKNOWN;

    /* Allocate dual/Farkas arrays
     * farkas has num_linking+1 slots: [0..num_linking-1] for link duals,
     * [num_linking] for sub-only RHS contribution */
    double *duals = (double*)malloc(ctx->num_linking * sizeof(double));
    double *farkas = (double*)malloc((ctx->num_linking + 1) * sizeof(double));

    if (!duals || !farkas) {
        free(duals);
        free(farkas);
        return -1;
    }

    /* Create master MIP solver.
     * Disable cut generation - Benders adds its own cuts (optimality/feasibility).
     * MIP's Gomory cuts can conflict with the iterative constraint addition. */
    ctx->master_solver = mip_create(ctx->master_model, 0, 1024);
    if (!ctx->master_solver) {
        free(duals);
        free(farkas);
        return -1;
    }
    ctx->master_solver->max_cut_rounds = 0;  /* Disable Gomory/MIR cuts */
    ctx->master_solver->verbose = ctx->config.verbose;

    /* Apply user's branching priorities/directions to master variables */
    if (ctx->config.branch_priorities) {
        int n_master = ctx->master_model->num_vars;
        int *mp = (int*)calloc(n_master, sizeof(int));
        if (mp) {
            for (int j = 0; j < ctx->config.num_master_vars; j++) {
                int orig_j = ctx->master_to_orig[j];
                int master_j = ctx->orig_to_master[orig_j];
                if (master_j >= 0 && master_j < n_master)
                    mp[master_j] = ctx->config.branch_priorities[orig_j];
            }
            ctx->master_solver->branch_priorities = mp;
        }
    }
    if (ctx->config.branch_directions) {
        int n_master = ctx->master_model->num_vars;
        int *md = (int*)calloc(n_master, sizeof(int));
        if (md) {
            for (int j = 0; j < ctx->config.num_master_vars; j++) {
                int orig_j = ctx->master_to_orig[j];
                int master_j = ctx->orig_to_master[orig_j];
                if (master_j >= 0 && master_j < n_master)
                    md[master_j] = ctx->config.branch_directions[orig_j];
            }
            ctx->master_solver->branch_directions = md;
        }
    }

    if (ctx->config.warm_start_master &&
        ctx->config.initial_master_solution &&
        !ctx->master_start) {
        int n_master = ctx->master_model->num_vars;
        ctx->master_start = (double*)calloc((size_t)n_master, sizeof(double));
        if (!ctx->master_start) {
            free(duals);
            free(farkas);
            return RALPH_STATUS_ERROR;
        }

        for (int j = 0; j < ctx->num_master_vars; j++) {
            int orig_j = ctx->master_to_orig[j];
            ctx->master_start[j] = ctx->config.initial_master_solution[orig_j];
        }

        if (ctx->theta_auto_created &&
            ctx->theta_in_master >= 0 &&
            ctx->theta_in_master < n_master) {
            ctx->master_start[ctx->theta_in_master] =
                ctx->master_model->lb[ctx->theta_in_master];
        }
    }

    if (ctx->config.warm_start_master && ctx->master_start) {
        (void)mip_set_start(ctx->master_solver,
                            ctx->master_start,
                            ctx->master_model->num_vars);
    }

    for (int iter = 0; iter < ctx->config.max_iterations; iter++) {
        ctx->iterations = iter + 1;

        if (ctx->config.verbose) {
            printf("\n=== Benders Iteration %d ===\n", iter + 1);
        }

        /* Solve master problem */
        int ret = mip_solve(ctx->master_solver);
        ctx->master_warm_starts_attempted += ctx->master_solver->mip_start_attempted;
        ctx->master_warm_starts_accepted += ctx->master_solver->mip_start_accepted;
        RalphStatus master_status = ctx->master_solver->status;

        if (ret != 0 || master_status != RALPH_STATUS_OPTIMAL) {
            if (ctx->config.verbose) {
                printf("Master problem not optimal (ret=%d, status %d)\n", ret, master_status);
            }

            if (master_status == RALPH_STATUS_INFEASIBLE) {
                final_status = RALPH_STATUS_INFEASIBLE;
            } else if (master_status == RALPH_STATUS_UNKNOWN && ret != 0) {
                final_status = RALPH_STATUS_ERROR;
            } else {
                final_status = master_status;
            }
            goto done;
        }

        /* Get master solution */
        double master_obj = ctx->master_solver->best_obj;
        memcpy(ctx->master_solution, ctx->master_solver->best_solution,
               ctx->master_model->num_vars * sizeof(double));

        if (ctx->config.warm_start_master) {
            int n_master = ctx->master_model->num_vars;
            if (!ctx->master_start) {
                ctx->master_start = (double*)malloc((size_t)n_master * sizeof(double));
            }
            if (ctx->master_start) {
                memcpy(ctx->master_start, ctx->master_solution,
                       (size_t)n_master * sizeof(double));
            }
        }

        double theta_val = ctx->master_solution[ctx->theta_in_master];

        if (ctx->config.verbose) {
            printf("Master obj: %.6f (theta = %.6f)\n", master_obj, theta_val);
            printf("  Master z values: ");
            for (int j = 0; j < ctx->config.num_master_vars; j++) {
                printf("z[%d]=%.2f ", j, ctx->master_solution[j]);
            }
            printf("\n");
        }

        /* Solve subproblems and generate cuts */
        double total_sub_obj = 0.0;
        int any_infeasible = 0;
        int cuts_added = 0;
        int subproblem_failed = 0;

        for (int s = 0; s < ctx->config.num_scenarios; s++) {
            /* Update subproblem RHS */
            if (benders_update_subproblem_rhs(ctx, s, ctx->master_solution) != 0) {
                subproblem_failed = 1;
                break;
            }

            /* Solve subproblem */
            double sub_obj;
            int is_feasible;
            int ret = benders_solve_subproblem(ctx, s, &sub_obj, duals, farkas,
                                                &is_feasible);
            if (ret != 0) {
                if (ctx->config.verbose) {
                    fprintf(stderr, "Subproblem %d solve failed\n", s);
                }
                subproblem_failed = 1;
                break;
            }

            double prob = (ctx->config.scenario_probs) ?
                          ctx->config.scenario_probs[s] :
                          (1.0 / ctx->config.num_scenarios);

            if (is_feasible) {
                total_sub_obj += prob * sub_obj;

                /* Check if we need optimality cut */
                if (sub_obj > theta_val + BENDERS_CUT_TOLERANCE) {
                    if (benders_add_optimality_cut(ctx, s, duals, sub_obj) != 0) {
                        subproblem_failed = 1;
                        break;
                    }
                    cuts_added++;
                }

                if (ctx->config.verbose) {
                    printf("Subproblem %d: feasible, obj = %.6f\n", s, sub_obj);
                }
            } else {
                any_infeasible = 1;
                if (benders_add_feasibility_cut(ctx, s, farkas) != 0) {
                    subproblem_failed = 1;
                    break;
                }
                cuts_added++;

                if (ctx->config.verbose) {
                    printf("Subproblem %d: infeasible, added feasibility cut\n", s);
                }
            }
        }

        if (subproblem_failed) {
            final_status = RALPH_STATUS_ERROR;
            goto done;
        }

        /* Check convergence */
        if (!any_infeasible && cuts_added == 0) {
            /* Compute bounds */
            double ub = master_obj - theta_val + total_sub_obj;
            double lb = master_obj;

            /* Check if UB < LB - this can happen when theta is over-estimated
             * due to cuts from different z values. In this case, we should add
             * an optimality cut to bring theta down. */
            if (ub < lb - 1e-6) {
                if (ctx->config.verbose) {
                    printf("UB (%.6f) < LB (%.6f) - inconsistent bounds\n", ub, lb);
                    printf("  theta = %.6f but sub_obj = %.6f\n", theta_val, total_sub_obj);
                }
                final_status = RALPH_STATUS_ERROR;
                goto done;
            }

            /* No cuts added - optimal */
            ctx->best_ub = ub;
            ctx->best_lb = lb;

            if (ctx->config.verbose) {
                printf("Converged! UB = %.6f, LB = %.6f\n",
                       ctx->best_ub, ctx->best_lb);
                printf("  master_obj = %.6f, theta_val = %.6f, total_sub_obj = %.6f\n",
                       master_obj, theta_val, total_sub_obj);
            }
            final_status = RALPH_STATUS_OPTIMAL;
            goto done;
        }

        double gap;
        if (!any_infeasible &&
            benders_check_convergence(ctx, master_obj, total_sub_obj, &gap)) {
            if (ctx->config.verbose) {
                printf("Converged within tolerance (gap = %.2e)\n", gap);
            }
            final_status = RALPH_STATUS_OPTIMAL;
            goto done;
        }

        /* Apply cuts to master and re-solve */
        if (benders_apply_cuts_to_master(ctx) != 0) {
            final_status = RALPH_STATUS_ERROR;
            goto done;
        }
        ctx->num_cuts = 0; /* Clear applied cuts */

        if (ctx->config.verbose >= 2) {
            printf("  Master model after adding cuts: %d vars, %d cons\n",
                   ctx->master_model->num_vars, ctx->master_model->num_cons);
            for (int i = 0; i < ctx->master_model->num_cons && i < 10; i++) {
                printf("    Con %d: sense=%c rhs=%.4f\n", i,
                       ctx->master_model->sense[i], ctx->master_model->b[i]);
            }
        }

        /* Re-finalize master model after adding cuts (rebuilds sparse matrix) */
        if (lp_model_finalize(ctx->master_model) != 0) {
            if (ctx->config.verbose) {
                fprintf(stderr, "Failed to finalize master model after adding cuts\n");
            }
            free(duals);
            free(farkas);
            return RALPH_STATUS_ERROR;
        }

        /* Recreate master solver with new constraints */
        mip_free(ctx->master_solver);
        ctx->master_solver = mip_create(ctx->master_model, 0, 1024);
        if (!ctx->master_solver) {
            free(duals);
            free(farkas);
            return RALPH_STATUS_ERROR;
        }
        ctx->master_solver->max_cut_rounds = 0;  /* Disable Gomory/MIR cuts */
        ctx->master_solver->verbose = ctx->config.verbose;

        /* Re-apply branching priorities/directions after master recreation */
        if (ctx->config.branch_priorities) {
            int n_master = ctx->master_model->num_vars;
            int *mp = (int*)calloc(n_master, sizeof(int));
            if (mp) {
                for (int j = 0; j < ctx->config.num_master_vars; j++) {
                    int orig_j = ctx->master_to_orig[j];
                    int master_j = ctx->orig_to_master[orig_j];
                    if (master_j >= 0 && master_j < n_master)
                        mp[master_j] = ctx->config.branch_priorities[orig_j];
                }
                ctx->master_solver->branch_priorities = mp;
            }
        }
        if (ctx->config.branch_directions) {
            int n_master = ctx->master_model->num_vars;
            int *md = (int*)calloc(n_master, sizeof(int));
            if (md) {
                for (int j = 0; j < ctx->config.num_master_vars; j++) {
                    int orig_j = ctx->master_to_orig[j];
                    int master_j = ctx->orig_to_master[orig_j];
                    if (master_j >= 0 && master_j < n_master)
                        md[master_j] = ctx->config.branch_directions[orig_j];
                }
                ctx->master_solver->branch_directions = md;
            }
        }

        if (ctx->config.warm_start_master && ctx->master_start) {
            (void)mip_set_start(ctx->master_solver,
                                ctx->master_start,
                                ctx->master_model->num_vars);
        }
    }

    if (final_status == RALPH_STATUS_UNKNOWN) {
        final_status = RALPH_STATUS_ITERATION_LIMIT;
    }

done:
    ctx->total_time = get_time_sec() - start_time;

    free(duals);
    free(farkas);

    return final_status;
}

/* ============================================================================
 * Solution Extraction
 * ============================================================================ */

int benders_extract_solution(BendersContext *ctx, double *x) {
    if (!x) return 0;

    /* Copy master variables */
    for (int j = 0; j < ctx->num_master_vars; j++) {
        if (j == ctx->theta_in_master && ctx->theta_auto_created) continue;
        int orig_j = ctx->master_to_orig[j];
        x[orig_j] = ctx->master_solution[j];
    }

    /* Copy subproblem variables from last solve */
    if (ctx->sub_solvers[0] &&
        ctx->sub_solvers[0]->status == RALPH_STATUS_OPTIMAL &&
        ctx->sub_solvers[0]->solution &&
        ctx->num_sub_vars == ctx->sub_solvers[0]->model->num_vars) {
        SimplexSolver *solver = ctx->sub_solvers[0];

        for (int j = 0; j < ctx->num_sub_vars; j++) {
            int orig_j = ctx->sub_to_orig[j];
            x[orig_j] = solver->solution[j];
        }
    }

    return 0;
}

/* ============================================================================
 * Internal Entry Point (called from ralph.c)
 * ============================================================================ */

int benders_solve(
    LPModel *lp_model,
    const RalphBendersConfig *config,
    double *x,
    RalphBendersResult *result)
{
    if (!lp_model || !config) {
        benders_init_error_result(result);
        return -1;
    }

    benders_init_error_result(result);

    /* Create Benders context */
    BendersContext *ctx = benders_create(lp_model, config);
    if (!ctx) return -1;

    /* Partition model */
    if (benders_partition(ctx) != 0) {
        benders_free(ctx);
        return -1;
    }

    /* Build master problem */
    if (benders_build_master(ctx) != 0) {
        benders_free(ctx);
        return -1;
    }

    /* Build initial subproblems */
    for (int s = 0; s < config->num_scenarios; s++) {
        if (benders_build_subproblem(ctx, s) != 0) {
            benders_free(ctx);
            return -1;
        }
    }

    /* Solve */
    int status;
    if (config->cuts_at_lp_nodes) {
        /* Modern branch-and-Benders-cut - for now use classic */
        status = benders_solve_classic(ctx);
    } else {
        status = benders_solve_classic(ctx);
    }

    /* Extract solution */
    if (status == RALPH_STATUS_OPTIMAL && x) {
        benders_extract_solution(ctx, x);
    }

    /* Fill result */
    if (result) {
        if (status == RALPH_STATUS_ERROR || status < 0) {
            result->status = RALPH_STATUS_ERROR;
        } else {
            result->status = (RalphStatus)status;
        }
        result->objective = ctx->best_ub;
        result->master_obj = ctx->best_lb;
        result->subproblem_obj = ctx->best_ub - ctx->best_lb;
        result->gap = (fabs(ctx->best_ub) > 1e-10) ?
                      fabs(ctx->best_ub - ctx->best_lb) / fabs(ctx->best_ub) : 0.0;
        result->iterations = ctx->iterations;
        result->optimality_cuts = ctx->optimality_cuts_added;
        result->feasibility_cuts = ctx->feasibility_cuts_added;
        result->nodes_explored = 0;
        result->subproblems_solved = ctx->subproblems_solved;
        result->master_warm_starts_attempted = ctx->master_warm_starts_attempted;
        result->master_warm_starts_accepted = ctx->master_warm_starts_accepted;
        result->subproblem_warm_starts = ctx->subproblem_warm_starts;
        result->subproblem_cold_starts = ctx->subproblem_cold_starts;
        result->solve_time = ctx->total_time;
    }

    benders_free(ctx);

    if (status == RALPH_STATUS_ERROR || status < 0) {
        return -1;
    }
    return 0;
}
