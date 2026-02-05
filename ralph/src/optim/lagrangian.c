/*
 * lagrangian.c - Generic Lagrangian relaxation implementation
 *
 * Part of Ralph optimization module.
 */

#include "optim_lagrangian.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Small tolerance for comparisons */
#define LAGR_TOL 1e-9

/*
 * Internal context for subgradient optimization callback.
 */
typedef struct {
    RalphLagrangianContext *lagr_ctx;
    const RalphLagrangianParams *params;
    int iteration;
} SubgradientCallbackData;

/*
 * Compute reduced costs: c_bar[j] = c[j] - sum_i(lambda[i] * A[i,j])
 */
static void compute_reduced_costs(RalphLagrangianContext *ctx) {
    /* Start with c */
    memcpy(ctx->reduced_cost, ctx->c, ctx->n * sizeof(double));

    /* Subtract A'λ */
    for (int j = 0; j < ctx->n; j++) {
        int start = ctx->A_colptr[j];
        int end = ctx->A_colptr[j + 1];
        for (int k = start; k < end; k++) {
            int i = ctx->A_rowidx[k];
            double a_ij = ctx->A_values ? ctx->A_values[k] : 1.0;
            ctx->reduced_cost[j] -= ctx->lambda[i] * a_ij;
        }
    }
}

/*
 * Compute subgradient: g[i] = b[i] - sum_j(A[i,j] * x[j])
 *
 * For covering (Ax >= b): g = b - Ax
 * For packing (Ax <= b): g = Ax - b (negated for max)
 * For partitioning (Ax = b): g = b - Ax
 */
static void compute_subgradient(RalphLagrangianContext *ctx) {
    /* Start with b */
    memcpy(ctx->subgradient, ctx->b, ctx->m * sizeof(double));

    /* Subtract Ax */
    for (int j = 0; j < ctx->n; j++) {
        if (ctx->x_lagrangian[j] < LAGR_TOL) continue;

        int start = ctx->A_colptr[j];
        int end = ctx->A_colptr[j + 1];
        for (int k = start; k < end; k++) {
            int i = ctx->A_rowidx[k];
            double a_ij = ctx->A_values ? ctx->A_values[k] : 1.0;
            ctx->subgradient[i] -= a_ij * ctx->x_lagrangian[j];
        }
    }

    /* For packing (maximization), negate the subgradient */
    if (ctx->type == RALPH_LAGRANGIAN_PACKING) {
        for (int i = 0; i < ctx->m; i++) {
            ctx->subgradient[i] = -ctx->subgradient[i];
        }
    }
}

/*
 * Project multipliers onto feasible set.
 * For covering/packing: λ >= 0
 * For partitioning: λ free (no projection)
 */
static void project_lambda(double *lambda, int m, void *user_data) {
    SubgradientCallbackData *data = (SubgradientCallbackData *)user_data;
    RalphLagrangianContext *ctx = data->lagr_ctx;

    if (ctx->type != RALPH_LAGRANGIAN_PARTITIONING) {
        for (int i = 0; i < m; i++) {
            if (lambda[i] < 0.0) {
                lambda[i] = 0.0;
            }
        }
    }
}

/*
 * Subgradient objective callback for Lagrangian dual.
 *
 * Computes L(λ) and subgradient g.
 */
static double lagrangian_objective(
    const double *lambda,
    double *g,
    int m,
    void *user_data
) {
    SubgradientCallbackData *data = (SubgradientCallbackData *)user_data;
    RalphLagrangianContext *ctx = data->lagr_ctx;

    /* Copy lambda to context */
    memcpy(ctx->lambda, lambda, m * sizeof(double));

    /* Compute reduced costs */
    compute_reduced_costs(ctx);

    /* Solve subproblem */
    double subproblem_obj = ctx->subproblem(
        ctx->n,
        ctx->reduced_cost,
        ctx->x_lagrangian,
        ctx->user_data
    );

    /* Compute L(λ) = λ'b + subproblem_obj */
    double lambda_b = 0.0;
    for (int i = 0; i < m; i++) {
        lambda_b += lambda[i] * ctx->b[i];
    }
    double L = lambda_b + subproblem_obj;

    /* Compute subgradient */
    compute_subgradient(ctx);
    memcpy(g, ctx->subgradient, m * sizeof(double));

    /* Track iteration for repair frequency */
    data->iteration++;

    /* Try repair if callback set and frequency reached */
    if (ctx->repair && data->params->repair_frequency > 0) {
        if (data->iteration % data->params->repair_frequency == 0) {
            double *x_feasible = (double *)malloc(ctx->n * sizeof(double));
            if (x_feasible) {
                double primal_cost = ctx->repair(
                    ctx->n,
                    ctx->x_lagrangian,
                    x_feasible,
                    ctx->user_data
                );

                /* Update best primal if improved */
                if (primal_cost < ctx->best_primal_bound) {
                    ctx->best_primal_bound = primal_cost;
                    memcpy(ctx->best_solution, x_feasible, ctx->n * sizeof(double));
                    ctx->primal_improvements++;
                }
                free(x_feasible);
            }
        }
    }

    return L;
}

/*
 * Create Lagrangian relaxation context.
 */
RalphLagrangianContext *ralph_lagrangian_create(
    int m, int n,
    RalphLagrangianType type,
    const double *c,
    const double *b,
    const int *A_colptr,
    const int *A_rowidx,
    const double *A_values,
    RalphLagrangianSubproblem subproblem,
    void *user_data
) {
    if (m <= 0 || n <= 0 || !c || !b || !A_colptr || !A_rowidx || !subproblem) {
        return NULL;
    }

    RalphLagrangianContext *ctx = (RalphLagrangianContext *)calloc(1, sizeof(RalphLagrangianContext));
    if (!ctx) return NULL;

    ctx->m = m;
    ctx->n = n;
    ctx->type = type;
    ctx->c = c;
    ctx->b = b;
    ctx->A_colptr = A_colptr;
    ctx->A_rowidx = A_rowidx;
    ctx->A_values = A_values;
    ctx->subproblem = subproblem;
    ctx->user_data = user_data;

    /* Allocate working arrays */
    ctx->lambda = (double *)calloc(m, sizeof(double));
    ctx->best_lambda = (double *)calloc(m, sizeof(double));
    ctx->subgradient = (double *)calloc(m, sizeof(double));
    ctx->reduced_cost = (double *)calloc(n, sizeof(double));
    ctx->x_lagrangian = (double *)calloc(n, sizeof(double));
    ctx->best_solution = (double *)calloc(n, sizeof(double));

    if (!ctx->lambda || !ctx->best_lambda || !ctx->subgradient ||
        !ctx->reduced_cost || !ctx->x_lagrangian || !ctx->best_solution) {
        ralph_lagrangian_free(ctx);
        return NULL;
    }

    /* Initialize bounds */
    if (type == RALPH_LAGRANGIAN_PACKING) {
        /* Maximization: dual bound is upper, primal is lower */
        ctx->best_dual_bound = HUGE_VAL;
        ctx->best_primal_bound = -HUGE_VAL;
    } else {
        /* Minimization: dual bound is lower, primal is upper */
        ctx->best_dual_bound = -HUGE_VAL;
        ctx->best_primal_bound = HUGE_VAL;
    }

    return ctx;
}

/*
 * Free Lagrangian relaxation context.
 */
void ralph_lagrangian_free(RalphLagrangianContext *ctx) {
    if (!ctx) return;

    free(ctx->lambda);
    free(ctx->best_lambda);
    free(ctx->subgradient);
    free(ctx->reduced_cost);
    free(ctx->x_lagrangian);
    free(ctx->best_solution);
    free(ctx);
}

/*
 * Set repair heuristic callback.
 */
void ralph_lagrangian_set_repair(
    RalphLagrangianContext *ctx,
    RalphLagrangianRepair repair
) {
    if (ctx) {
        ctx->repair = repair;
    }
}

/*
 * Set initial multipliers.
 */
void ralph_lagrangian_set_lambda(
    RalphLagrangianContext *ctx,
    const double *lambda
) {
    if (ctx && lambda) {
        memcpy(ctx->lambda, lambda, ctx->m * sizeof(double));
    }
}

/*
 * Set primal bound.
 */
void ralph_lagrangian_set_primal_bound(
    RalphLagrangianContext *ctx,
    double bound
) {
    if (ctx) {
        ctx->best_primal_bound = bound;
    }
}

/*
 * Compute Lagrangian bound for current multipliers.
 */
double ralph_lagrangian_bound(RalphLagrangianContext *ctx) {
    if (!ctx) return -HUGE_VAL;

    /* Compute reduced costs */
    compute_reduced_costs(ctx);

    /* Solve subproblem */
    double subproblem_obj = ctx->subproblem(
        ctx->n,
        ctx->reduced_cost,
        ctx->x_lagrangian,
        ctx->user_data
    );

    /* Compute L(λ) = λ'b + subproblem_obj */
    double lambda_b = 0.0;
    for (int i = 0; i < ctx->m; i++) {
        lambda_b += ctx->lambda[i] * ctx->b[i];
    }

    /* Compute subgradient */
    compute_subgradient(ctx);

    return lambda_b + subproblem_obj;
}

/*
 * Run full Lagrangian optimization.
 */
double ralph_lagrangian_optimize(
    RalphLagrangianContext *ctx,
    const RalphLagrangianParams *params
) {
    if (!ctx || !params) return -HUGE_VAL;

    /* Set up callback data */
    SubgradientCallbackData callback_data = {
        .lagr_ctx = ctx,
        .params = params,
        .iteration = 0
    };

    /* Set initial lambda if provided */
    if (params->initial_lambda) {
        memcpy(ctx->lambda, params->initial_lambda, ctx->m * sizeof(double));
    }

    /* Configure subgradient params */
    RalphSubgradientParams subgrad_params = params->subgrad_params;

    /* Set up Polyak step if primal bound available */
    if (ctx->type == RALPH_LAGRANGIAN_PACKING) {
        /* Maximization: f_star is lower bound */
        if (ctx->best_primal_bound > -HUGE_VAL + 1e10) {
            subgrad_params.f_star = ctx->best_primal_bound;
            subgrad_params.use_f_star = 1;
        }
    } else {
        /* Minimization: f_star is upper bound */
        if (ctx->best_primal_bound < HUGE_VAL - 1e10) {
            subgrad_params.f_star = ctx->best_primal_bound;
            subgrad_params.use_f_star = 1;
        }
    }

    /* Run subgradient optimization */
    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(
        ctx->m,
        ctx->lambda,
        &subgrad_params,
        lagrangian_objective,
        project_lambda,
        &callback_data,
        &result
    );

    if (ret != 0) {
        return ctx->best_dual_bound;
    }

    /* Update best dual bound and lambda */
    ctx->best_dual_bound = result.best_value;
    if (result.best_x) {
        memcpy(ctx->best_lambda, result.best_x, ctx->m * sizeof(double));
    }

    /* Update statistics */
    ctx->iterations = result.iterations;
    ctx->bound_improvements = result.improvements;

    /* Final repair attempt with best lambda */
    if (ctx->repair && result.best_x) {
        memcpy(ctx->lambda, result.best_x, ctx->m * sizeof(double));
        ralph_lagrangian_bound(ctx);  /* Recompute x_lagrangian */

        double *x_feasible = (double *)malloc(ctx->n * sizeof(double));
        if (x_feasible) {
            double primal_cost = ctx->repair(
                ctx->n,
                ctx->x_lagrangian,
                x_feasible,
                ctx->user_data
            );

            if (ctx->type == RALPH_LAGRANGIAN_PACKING) {
                /* Maximization: primal is lower bound */
                if (primal_cost > ctx->best_primal_bound) {
                    ctx->best_primal_bound = primal_cost;
                    memcpy(ctx->best_solution, x_feasible, ctx->n * sizeof(double));
                    ctx->primal_improvements++;
                }
            } else {
                /* Minimization: primal is upper bound */
                if (primal_cost < ctx->best_primal_bound) {
                    ctx->best_primal_bound = primal_cost;
                    memcpy(ctx->best_solution, x_feasible, ctx->n * sizeof(double));
                    ctx->primal_improvements++;
                }
            }
            free(x_feasible);
        }
    }

    ralph_subgradient_result_free(&result);

    return ctx->best_dual_bound;
}

/*
 * Accessor functions.
 */
double ralph_lagrangian_dual_bound(const RalphLagrangianContext *ctx) {
    return ctx ? ctx->best_dual_bound : -HUGE_VAL;
}

double ralph_lagrangian_primal_bound(const RalphLagrangianContext *ctx) {
    return ctx ? ctx->best_primal_bound : HUGE_VAL;
}

const double *ralph_lagrangian_solution(const RalphLagrangianContext *ctx) {
    if (!ctx) return NULL;
    /* Return solution only if at least one primal improvement occurred */
    return (ctx->primal_improvements > 0) ? ctx->best_solution : NULL;
}

int ralph_lagrangian_iterations(const RalphLagrangianContext *ctx) {
    return ctx ? ctx->iterations : 0;
}

int ralph_lagrangian_bound_improvements(const RalphLagrangianContext *ctx) {
    return ctx ? ctx->bound_improvements : 0;
}

int ralph_lagrangian_primal_improvements(const RalphLagrangianContext *ctx) {
    return ctx ? ctx->primal_improvements : 0;
}
