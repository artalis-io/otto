/*
 * subgradient.c - Generic subgradient optimization implementation
 *
 * Part of Ralph optimization module.
 */

#include "optim_subgradient.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Small tolerance for gradient norm check */
#define GRAD_TOL 1e-12

/*
 * Compute squared L2 norm of vector.
 */
static double vec_norm_sq(const double *v, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += v[i] * v[i];
    }
    return sum;
}

/*
 * Compute step size based on strategy.
 */
static double compute_step_size(
    const RalphSubgradientParams *params,
    double alpha,           /* Current alpha (may be reduced from initial) */
    int iteration,          /* 1-indexed iteration number */
    double f_x,             /* Current objective value */
    double grad_norm_sq     /* ||g||^2 */
) {
    switch (params->step_strategy) {
        case RALPH_STEP_CONSTANT:
            return alpha;

        case RALPH_STEP_DIMINISHING:
            /* step = alpha / sqrt(k) */
            return alpha / sqrt((double)iteration);

        case RALPH_STEP_POLYAK:
            /* step = alpha * (f* - f(x)) / ||g||^2 */
            if (params->use_f_star && grad_norm_sq > GRAD_TOL) {
                double gap = params->f_star - f_x;
                if (gap > 0) {
                    return alpha * gap / grad_norm_sq;
                }
            }
            /* Fallback to constant if Polyak not applicable */
            return alpha;

        case RALPH_STEP_ADAPTIVE:
            /* Alpha is managed externally; just return current */
            return alpha;

        default:
            return alpha;
    }
}

/*
 * Initialize result structure.
 */
void ralph_subgradient_result_init(RalphSubgradientResult *result) {
    if (!result) return;
    result->best_value = -HUGE_VAL;
    result->best_x = NULL;
    result->iterations = 0;
    result->improvements = 0;
    result->status = RALPH_SUBGRAD_ERROR;
    result->final_step = 0.0;
    result->final_grad_norm = 0.0;
}

/*
 * Free resources in result structure.
 */
void ralph_subgradient_result_free(RalphSubgradientResult *result) {
    if (!result) return;
    if (result->best_x) {
        free(result->best_x);
        result->best_x = NULL;
    }
}

/*
 * Run subgradient optimization.
 */
int ralph_subgradient_optimize(
    int n,
    const double *x0,
    const RalphSubgradientParams *params,
    RalphSubgradientFunc objective,
    RalphProjectFunc project,
    void *user_data,
    RalphSubgradientResult *result
) {
    /* Validate inputs */
    if (n <= 0 || !x0 || !params || !objective || !result) {
        return -1;
    }

    /* Initialize result */
    ralph_subgradient_result_init(result);

    /* Allocate working arrays */
    double *x = (double *)malloc(n * sizeof(double));
    double *g = (double *)malloc(n * sizeof(double));
    double *best_x = (double *)malloc(n * sizeof(double));
    if (!x || !g || !best_x) {
        free(x);
        free(g);
        free(best_x);
        return -1;
    }

    /* Initialize x from x0 */
    memcpy(x, x0, n * sizeof(double));

    /* Project initial point if needed */
    if (project) {
        project(x, n, user_data);
    }

    /* Initialize tracking variables */
    double alpha = params->initial_step;
    double best_value = -HUGE_VAL;
    int no_improve_count = 0;
    int total_improvements = 0;
    RalphSubgradientStatus status = RALPH_SUBGRAD_MAX_ITER;
    double final_step = alpha;
    double final_grad_norm = 0.0;

    /* Main iteration loop */
    for (int k = 1; k <= params->max_iterations; k++) {
        /* Evaluate objective and subgradient */
        double f_x = objective(x, g, n, user_data);

        /* Compute gradient norm squared */
        double grad_norm_sq = vec_norm_sq(g, n);
        double grad_norm = sqrt(grad_norm_sq);
        final_grad_norm = grad_norm;

        /* Check for convergence (small gradient) */
        if (grad_norm < params->tol) {
            status = RALPH_SUBGRAD_CONVERGED;
            /* Update best if this is an improvement */
            if (f_x > best_value) {
                best_value = f_x;
                memcpy(best_x, x, n * sizeof(double));
                total_improvements++;
            }
            result->iterations = k;
            break;
        }

        /* Update best solution if improved */
        if (f_x > best_value) {
            best_value = f_x;
            memcpy(best_x, x, n * sizeof(double));
            total_improvements++;
            no_improve_count = 0;
        } else {
            no_improve_count++;
        }

        /* Adaptive step reduction */
        if (params->step_strategy == RALPH_STEP_ADAPTIVE) {
            if (no_improve_count >= params->no_improve_limit) {
                alpha *= params->step_decay;
                no_improve_count = 0;

                /* Check minimum step */
                if (alpha < params->min_step) {
                    status = RALPH_SUBGRAD_SMALL_STEP;
                    result->iterations = k;
                    final_step = alpha;
                    break;
                }
            }
        }

        /* Compute step size */
        double step = compute_step_size(params, alpha, k, f_x, grad_norm_sq);
        final_step = step;

        /* Check minimum step for non-adaptive strategies */
        if (params->step_strategy != RALPH_STEP_ADAPTIVE && step < params->min_step) {
            status = RALPH_SUBGRAD_SMALL_STEP;
            result->iterations = k;
            break;
        }

        /* Subgradient ascent step: x = x + step * g */
        for (int i = 0; i < n; i++) {
            x[i] += step * g[i];
        }

        /* Project onto feasible set */
        if (project) {
            project(x, n, user_data);
        }

        result->iterations = k;
    }

    /* Store results */
    result->best_value = best_value;
    result->best_x = best_x;
    result->improvements = total_improvements;
    result->status = status;
    result->final_step = final_step;
    result->final_grad_norm = final_grad_norm;

    /* Free working arrays (but not best_x - caller owns it) */
    free(x);
    free(g);

    return 0;
}
