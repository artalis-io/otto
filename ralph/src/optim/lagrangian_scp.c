/*
 * lagrangian_scp.c - SCP-specific Lagrangian relaxation using generic framework
 *
 * Part of Ralph optimization module.
 *
 * Provides:
 *   - Trivial subproblem solver for SCP
 *   - Greedy repair heuristic for SCP
 *   - Integration with MIPSolver for full SCP Lagrangian solve
 */

#include "optim_lagrangian.h"
#include "mip.h"
#include "lp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Forward declarations for MIP functions we need */
extern int is_scp_model(const LPModel *model);
extern int heuristic_greedy_set_cover(MIPSolver *solver, double *solution);
extern int heuristic_local_search_scp(MIPSolver *solver, double *solution);

/*
 * SCP user context for callbacks.
 */
typedef struct {
    int m;                      /* Number of elements (constraints) */
    int n;                      /* Number of sets (variables) */
    const double *costs;        /* Set costs [n] */
    const int *col_ptr;         /* CSC column pointers [n+1] */
    const int *row_idx;         /* CSC row indices */
    int *coverage;              /* Working array for repair [m] */
    int *uncovered;             /* Working array for repair [m] */
} SCPContext;

/*
 * SCP subproblem: trivial binary selection.
 *
 * For SCP Lagrangian: min { (c - A'λ)'x : x ∈ {0,1}^n }
 * Solution: x[j] = 1 if reduced_cost[j] < 0, else 0
 */
static double scp_subproblem(int n, const double *reduced_cost, double *x, void *user_data) {
    (void)user_data;
    double obj = 0.0;
    for (int j = 0; j < n; j++) {
        if (reduced_cost[j] < -1e-9) {
            x[j] = 1.0;
            obj += reduced_cost[j];
        } else {
            x[j] = 0.0;
        }
    }
    return obj;
}

/*
 * SCP repair: greedy set covering with local search.
 *
 * 1. Start with Lagrangian solution
 * 2. Greedily add sets to cover uncovered elements (cost/coverage ratio)
 * 3. Remove redundant sets
 */
static double scp_repair(int n, const double *x_lagr, double *x_feas, void *user_data) {
    SCPContext *scp = (SCPContext *)user_data;
    int m = scp->m;

    /* Start with Lagrangian solution */
    memcpy(x_feas, x_lagr, n * sizeof(double));

    /* Initialize coverage tracking */
    memset(scp->coverage, 0, m * sizeof(int));

    /* Count coverage from Lagrangian solution */
    for (int j = 0; j < n; j++) {
        if (x_feas[j] > 0.5) {
            for (int p = scp->col_ptr[j]; p < scp->col_ptr[j + 1]; p++) {
                int i = scp->row_idx[p];
                scp->coverage[i]++;
            }
        }
    }

    /* Find uncovered elements */
    int num_uncovered = 0;
    for (int i = 0; i < m; i++) {
        if (scp->coverage[i] == 0) {
            scp->uncovered[num_uncovered++] = i;
        }
    }

    /* Greedy repair: add sets to cover uncovered elements */
    while (num_uncovered > 0) {
        int best_set = -1;
        double best_ratio = HUGE_VAL;

        for (int j = 0; j < n; j++) {
            if (x_feas[j] > 0.5) continue;  /* Already selected */

            /* Count how many uncovered elements this set covers */
            int covers = 0;
            for (int p = scp->col_ptr[j]; p < scp->col_ptr[j + 1]; p++) {
                int elem = scp->row_idx[p];
                if (scp->coverage[elem] == 0) {
                    covers++;
                }
            }

            if (covers > 0) {
                double ratio = scp->costs[j] / covers;
                if (ratio < best_ratio) {
                    best_ratio = ratio;
                    best_set = j;
                }
            }
        }

        if (best_set < 0) {
            /* No set can cover remaining elements - infeasible */
            return HUGE_VAL;
        }

        /* Add best set */
        x_feas[best_set] = 1.0;
        for (int p = scp->col_ptr[best_set]; p < scp->col_ptr[best_set + 1]; p++) {
            int elem = scp->row_idx[p];
            scp->coverage[elem]++;
        }

        /* Recount uncovered */
        num_uncovered = 0;
        for (int i = 0; i < m; i++) {
            if (scp->coverage[i] == 0) {
                scp->uncovered[num_uncovered++] = i;
            }
        }
    }

    /* Local search: remove redundant sets */
    for (int j = 0; j < n; j++) {
        if (x_feas[j] < 0.5) continue;

        /* Check if removing this set still covers everything */
        int can_remove = 1;
        for (int p = scp->col_ptr[j]; p < scp->col_ptr[j + 1]; p++) {
            int elem = scp->row_idx[p];
            if (scp->coverage[elem] <= 1) {
                can_remove = 0;
                break;
            }
        }

        if (can_remove) {
            x_feas[j] = 0.0;
            for (int p = scp->col_ptr[j]; p < scp->col_ptr[j + 1]; p++) {
                int elem = scp->row_idx[p];
                scp->coverage[elem]--;
            }
        }
    }

    /* Compute objective */
    double obj = 0.0;
    for (int j = 0; j < n; j++) {
        if (x_feas[j] > 0.5) {
            obj += scp->costs[j];
        }
    }
    return obj;
}

/*
 * Create SCP context for callbacks.
 */
static SCPContext *scp_context_create(int m, int n, const double *costs,
                                       const int *col_ptr, const int *row_idx) {
    SCPContext *ctx = (SCPContext *)calloc(1, sizeof(SCPContext));
    if (!ctx) return NULL;

    ctx->m = m;
    ctx->n = n;
    ctx->costs = costs;
    ctx->col_ptr = col_ptr;
    ctx->row_idx = row_idx;

    ctx->coverage = (int *)calloc(m, sizeof(int));
    ctx->uncovered = (int *)calloc(m, sizeof(int));

    if (!ctx->coverage || !ctx->uncovered) {
        free(ctx->coverage);
        free(ctx->uncovered);
        free(ctx);
        return NULL;
    }

    return ctx;
}

/*
 * Free SCP context.
 */
static void scp_context_free(SCPContext *ctx) {
    if (!ctx) return;
    free(ctx->coverage);
    free(ctx->uncovered);
    free(ctx);
}

/*
 * Solve SCP using generic Lagrangian framework.
 *
 * This is the new implementation using optim_lagrangian.h.
 */
int ralph_lagrangian_solve_scp_ex(
    int m, int n,
    const double *costs,
    const double *rhs,
    const int *col_ptr,
    const int *row_idx,
    double initial_ub,
    double *solution,
    double *lower_bound
) {
    if (m <= 0 || n <= 0 || !costs || !rhs || !col_ptr || !row_idx || !solution) {
        return -1;
    }

    /* Create SCP context for callbacks */
    SCPContext *scp_ctx = scp_context_create(m, n, costs, col_ptr, row_idx);
    if (!scp_ctx) return -1;

    /* Create generic Lagrangian context */
    RalphLagrangianContext *lagr_ctx = ralph_lagrangian_create(
        m, n, RALPH_LAGRANGIAN_COVERING,
        costs, rhs, col_ptr, row_idx, NULL,
        scp_subproblem, scp_ctx
    );

    if (!lagr_ctx) {
        scp_context_free(scp_ctx);
        return -1;
    }

    /* Set repair heuristic */
    ralph_lagrangian_set_repair(lagr_ctx, scp_repair);

    /* Set primal bound if available */
    if (initial_ub < HUGE_VAL) {
        ralph_lagrangian_set_primal_bound(lagr_ctx, initial_ub);
    }

    /* Configure optimization parameters */
    RalphLagrangianParams params = RALPH_LAGRANGIAN_PARAMS_DEFAULT;
    params.subgrad_params.max_iterations = 500;
    params.subgrad_params.initial_step = 2.0;
    params.subgrad_params.step_strategy = RALPH_STEP_ADAPTIVE;
    params.subgrad_params.no_improve_limit = 30;
    params.repair_frequency = 50;

    /* Run optimization */
    double lb = ralph_lagrangian_optimize(lagr_ctx, &params);

    /* Get results */
    if (lower_bound) {
        *lower_bound = lb;
    }

    /* Get Lagrangian solution if it's better than initial */
    const double *best_sol = ralph_lagrangian_solution(lagr_ctx);
    double lagr_primal = ralph_lagrangian_primal_bound(lagr_ctx);

    if (best_sol && lagr_primal < initial_ub) {
        /* Lagrangian repair found better solution */
        memcpy(solution, best_sol, n * sizeof(double));
    }
    /* Otherwise keep the initial solution (already in solution buffer) */

    /* Cleanup */
    ralph_lagrangian_free(lagr_ctx);
    scp_context_free(scp_ctx);

    return 0;
}

/* ============================================================================
 * MIP-integrated wrappers (backwards compatibility with existing API)
 * ============================================================================ */

/*
 * Create old-style Lagrangian context from MIPSolver.
 *
 * This is a wrapper that extracts problem data and creates both the old
 * LagrangianContext (for API compatibility) and internal generic context.
 */
LagrangianContext *lagrangian_create(MIPSolver *solver) {
    if (!solver || !solver->original_model) return NULL;

    LPModel *model = solver->original_model;
    SparseMatrix *A = model->A;

    /* Check if this is an SCP problem */
    if (!is_scp_model(model)) {
        return NULL;
    }

    LagrangianContext *ctx = (LagrangianContext *)calloc(1, sizeof(LagrangianContext));
    if (!ctx) return NULL;

    ctx->num_elements = A->nrows;
    ctx->num_sets = A->ncols;

    /* Allocate arrays */
    ctx->lambda = (double *)calloc(ctx->num_elements, sizeof(double));
    ctx->subgradient = (double *)calloc(ctx->num_elements, sizeof(double));
    ctx->best_lambda = (double *)calloc(ctx->num_elements, sizeof(double));
    ctx->x_lagrangian = (double *)calloc(ctx->num_sets, sizeof(double));

    if (!ctx->lambda || !ctx->subgradient || !ctx->best_lambda || !ctx->x_lagrangian) {
        lagrangian_free(ctx);
        return NULL;
    }

    /* Store pointers to problem data (not owned) */
    ctx->costs = model->c;
    ctx->col_ptr = A->colptr;
    ctx->row_idx = A->rowidx;

    /* Default parameters */
    ctx->max_iterations = 500;
    ctx->step_factor = 2.0;
    ctx->min_step_factor = 0.01;
    ctx->no_improve_limit = 30;

    /* Initialize bounds */
    ctx->best_bound = -RALPH_INFINITY;
    ctx->ub = RALPH_INFINITY;

    return ctx;
}

/*
 * Free Lagrangian context.
 */
void lagrangian_free(LagrangianContext *ctx) {
    if (!ctx) return;

    free(ctx->lambda);
    free(ctx->subgradient);
    free(ctx->best_lambda);
    free(ctx->x_lagrangian);
    free(ctx);
}

/*
 * Compute Lagrangian bound for current multipliers.
 *
 * L(lambda) = sum(lambda) + sum { min(0, c_j - sum(lambda_i : i in S_j)) }
 */
double lagrangian_bound(LagrangianContext *ctx) {
    if (!ctx) return -RALPH_INFINITY;

    int m = ctx->num_elements;
    int n = ctx->num_sets;

    /* Fused initialization: compute bound and init subgradient in single pass
     * bound = sum(lambda[i])    (λ'b where b=1)
     * subgradient[i] = 1.0      (b - Ax where b=1, before x updates)
     */
    double bound = 0.0;
    const double * restrict lambda = ctx->lambda;
    double * restrict subgrad = ctx->subgradient;

    #pragma omp simd reduction(+:bound)
    for (int i = 0; i < m; i++) {
        bound += lambda[i];
        subgrad[i] = 1.0;
    }

    /* Process each set (variable) */
    for (int j = 0; j < n; j++) {
        /* Compute reduced cost: c_j - sum(lambda_i : set j covers element i) */
        double reduced_cost = ctx->costs[j];
        for (int p = ctx->col_ptr[j]; p < ctx->col_ptr[j + 1]; p++) {
            int i = ctx->row_idx[p];
            reduced_cost -= ctx->lambda[i];
        }

        /* Solve trivial subproblem: x_j = 1 if reduced_cost < 0 */
        if (reduced_cost < -RALPH_ZERO_TOL) {
            ctx->x_lagrangian[j] = 1.0;
            bound += reduced_cost;

            /* Update subgradient: g_i = 1 - sum(x_j : j covers i) */
            for (int p = ctx->col_ptr[j]; p < ctx->col_ptr[j + 1]; p++) {
                int i = ctx->row_idx[p];
                ctx->subgradient[i] -= 1.0;
            }
        } else {
            ctx->x_lagrangian[j] = 0.0;
        }
    }

    return bound;
}

/*
 * Perform one subgradient update step.
 */
double lagrangian_step(LagrangianContext *ctx) {
    if (!ctx) return -RALPH_INFINITY;

    /* Compute current bound and subgradient */
    double current_bound = lagrangian_bound(ctx);

    /* Update best bound if improved */
    if (current_bound > ctx->best_bound + RALPH_ZERO_TOL) {
        ctx->best_bound = current_bound;
        memcpy(ctx->best_lambda, ctx->lambda, ctx->num_elements * sizeof(double));
        ctx->bound_improvements++;
    }

    /* Compute subgradient norm squared */
    double norm_sq = 0.0;
    for (int i = 0; i < ctx->num_elements; i++) {
        norm_sq += ctx->subgradient[i] * ctx->subgradient[i];
    }

    /* If subgradient is zero, we're at optimum */
    if (norm_sq < RALPH_ZERO_TOL) {
        return current_bound;
    }

    /* Compute step size (Polyak-like) */
    double gap = ctx->ub - current_bound;
    if (gap < RALPH_ZERO_TOL) {
        gap = 1.0;
    }
    double step = ctx->step_factor * gap / norm_sq;

    /* Update multipliers with projection to non-negative */
    for (int i = 0; i < ctx->num_elements; i++) {
        ctx->lambda[i] += step * ctx->subgradient[i];
        if (ctx->lambda[i] < 0.0) {
            ctx->lambda[i] = 0.0;
        }
    }

    ctx->iterations++;
    return current_bound;
}

/*
 * Run subgradient optimization to find best Lagrangian bound.
 */
double lagrangian_optimize(LagrangianContext *ctx) {
    if (!ctx) return -RALPH_INFINITY;

    int no_improve_count = 0;
    double prev_best = ctx->best_bound;

    for (int iter = 0; iter < ctx->max_iterations; iter++) {
        double bound = lagrangian_step(ctx);

        /* Check for improvement */
        if (bound > prev_best + RALPH_ZERO_TOL) {
            no_improve_count = 0;
            prev_best = bound;
        } else {
            no_improve_count++;
        }

        /* Reduce step factor if no improvement */
        if (no_improve_count >= ctx->no_improve_limit) {
            ctx->step_factor *= 0.5;
            no_improve_count = 0;

            /* Restore best multipliers */
            memcpy(ctx->lambda, ctx->best_lambda, ctx->num_elements * sizeof(double));

            /* Stop if step factor too small */
            if (ctx->step_factor < ctx->min_step_factor) {
                break;
            }
        }

        /* Check if gap is closed */
        if (ctx->ub - ctx->best_bound < RALPH_OPT_TOL * (fabs(ctx->ub) + 1.0)) {
            break;
        }
    }

    /* Restore best multipliers and recompute */
    memcpy(ctx->lambda, ctx->best_lambda, ctx->num_elements * sizeof(double));
    lagrangian_bound(ctx);

    return ctx->best_bound;
}

/*
 * Convert Lagrangian solution to feasible SCP solution.
 */
double lagrangian_repair(LagrangianContext *ctx, MIPSolver *solver, double *solution) {
    if (!ctx || !solver || !solution) return RALPH_INFINITY;

    LPModel *model = solver->original_model;
    SparseMatrix *A = model->A;
    int m = ctx->num_elements;
    int n = ctx->num_sets;

    /* Create temporary SCP context for repair */
    SCPContext *scp_ctx = scp_context_create(m, n, model->c, A->colptr, A->rowidx);
    if (!scp_ctx) return RALPH_INFINITY;

    /* Call generic repair */
    double obj = scp_repair(n, ctx->x_lagrangian, solution, scp_ctx);

    scp_context_free(scp_ctx);
    return obj;
}

/*
 * Full Lagrangian-based solve for SCP.
 */
int lagrangian_solve_scp(MIPSolver *solver, double *solution, double *lower_bound) {
    if (!solver || !solution) return -1;

    LPModel *model = solver->original_model;
    if (!model) return -1;

    /* Check if this is an SCP problem */
    if (!is_scp_model(model)) {
        return -1;
    }

    SparseMatrix *A = model->A;
    int n = model->num_vars;

    /* Run greedy heuristic to get initial upper bound */
    double *greedy_sol = (double *)calloc(n, sizeof(double));
    if (!greedy_sol) return -1;

    double ub = HUGE_VAL;
    if (heuristic_greedy_set_cover(solver, greedy_sol) == 0) {
        /* Apply local search */
        heuristic_local_search_scp(solver, greedy_sol);

        /* Compute objective */
        ub = 0.0;
        for (int j = 0; j < n; j++) {
            if (greedy_sol[j] > 0.5) {
                ub += model->c[j];
            }
        }
        memcpy(solution, greedy_sol, n * sizeof(double));
    }
    free(greedy_sol);

    /* Use generic framework */
    double *rhs = (double *)malloc(A->nrows * sizeof(double));
    if (!rhs) return -1;
    for (int i = 0; i < A->nrows; i++) {
        rhs[i] = 1.0;  /* SCP has b = 1 */
    }

    double lb = 0.0;
    int result = ralph_lagrangian_solve_scp_ex(
        A->nrows, A->ncols,
        model->c, rhs, A->colptr, A->rowidx,
        ub,
        solution, &lb
    );

    free(rhs);

    if (result == 0 && lower_bound) {
        *lower_bound = lb;
    }

    return result;
}
