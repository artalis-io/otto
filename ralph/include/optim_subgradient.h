/*
 * optim_subgradient.h - Generic subgradient optimization for non-smooth problems
 *
 * Part of Ralph optimization module.
 *
 * Solves: max f(x) subject to x in feasible set
 * where f has subgradients (may be non-smooth).
 *
 * User provides:
 *   - Objective and subgradient evaluation (callback)
 *   - Projection to feasible set (callback, optional)
 *   - Step size strategy
 */

#ifndef RALPH_OPTIM_SUBGRADIENT_H
#define RALPH_OPTIM_SUBGRADIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Step size strategies for subgradient method.
 *
 * CONSTANT:    step = alpha (fixed)
 * DIMINISHING: step = alpha / sqrt(k) where k is iteration
 * POLYAK:      step = alpha * (f* - f(x)) / ||g||^2 where f* is known upper bound
 * ADAPTIVE:    Halve alpha when no improvement for N iterations
 */
typedef enum {
    RALPH_STEP_CONSTANT,
    RALPH_STEP_DIMINISHING,
    RALPH_STEP_POLYAK,
    RALPH_STEP_ADAPTIVE
} RalphStepStrategy;

/*
 * Subgradient optimization parameters.
 */
typedef struct {
    int max_iterations;           /* Maximum iterations (default: 500) */
    double initial_step;          /* Initial step size alpha (default: 2.0) */
    double min_step;              /* Minimum step before stopping (default: 0.001) */
    RalphStepStrategy step_strategy; /* Step size strategy (default: ADAPTIVE) */

    /* For Polyak step: upper bound on optimal value (for maximization) */
    double f_star;                /* Upper bound estimate */
    int use_f_star;               /* 1 if f_star is available */

    /* Adaptive parameters */
    int no_improve_limit;         /* Iterations without improvement before halving (default: 30) */
    double step_decay;            /* Step reduction factor (default: 0.5) */

    /* Convergence */
    double tol;                   /* Stop if ||g|| < tol (default: 1e-8) */
} RalphSubgradientParams;

/* Default parameters initializer */
#define RALPH_SUBGRADIENT_PARAMS_DEFAULT { \
    .max_iterations = 500, \
    .initial_step = 2.0, \
    .min_step = 0.001, \
    .step_strategy = RALPH_STEP_ADAPTIVE, \
    .f_star = 0.0, \
    .use_f_star = 0, \
    .no_improve_limit = 30, \
    .step_decay = 0.5, \
    .tol = 1e-8 \
}

/*
 * Callback: evaluate f(x) and compute subgradient g.
 *
 * Parameters:
 *   x         - Current point [n]
 *   g         - Output: subgradient at x [n] (must be filled by callback)
 *   n         - Dimension
 *   user_data - User context
 *
 * Returns:
 *   Objective value f(x).
 */
typedef double (*RalphSubgradientFunc)(
    const double *x,
    double *g,
    int n,
    void *user_data
);

/*
 * Callback: project x onto feasible set.
 *
 * Parameters:
 *   x         - In/out: point to project [n]
 *   n         - Dimension
 *   user_data - User context
 *
 * The callback should modify x in-place to be the nearest feasible point.
 * Can be NULL for unconstrained problems.
 */
typedef void (*RalphProjectFunc)(
    double *x,
    int n,
    void *user_data
);

/*
 * Subgradient optimization result status.
 */
typedef enum {
    RALPH_SUBGRAD_CONVERGED = 0,    /* Converged: ||g|| < tol */
    RALPH_SUBGRAD_MAX_ITER = 1,     /* Hit max iterations */
    RALPH_SUBGRAD_SMALL_STEP = 2,   /* Step size fell below min_step */
    RALPH_SUBGRAD_ERROR = -1        /* Error (allocation failure, etc.) */
} RalphSubgradientStatus;

/*
 * Subgradient optimization result.
 */
typedef struct {
    double best_value;            /* Best objective value found */
    double *best_x;               /* Best point found [n] (caller must free) */
    int iterations;               /* Total iterations performed */
    int improvements;             /* Number of times best_value improved */
    RalphSubgradientStatus status; /* Termination status */
    double final_step;            /* Final step size */
    double final_grad_norm;       /* ||g|| at termination */
} RalphSubgradientResult;

/*
 * Run subgradient optimization.
 *
 * Maximizes f(x) subject to x in feasible set using subgradient ascent.
 * The best point found (not necessarily the last) is returned.
 *
 * Parameters:
 *   n         - Dimension of x
 *   x0        - Initial point [n] (copied internally)
 *   params    - Optimization parameters (use RALPH_SUBGRADIENT_PARAMS_DEFAULT)
 *   objective - Callback for f(x) and subgradient g
 *   project   - Callback for projection (NULL for unconstrained)
 *   user_data - Passed to callbacks
 *   result    - Output: optimization result
 *
 * Returns:
 *   0 on success, -1 on error.
 *
 * Notes:
 *   - Caller must free result->best_x when done
 *   - For minimization, negate objective and subgradient in callback
 */
int ralph_subgradient_optimize(
    int n,
    const double *x0,
    const RalphSubgradientParams *params,
    RalphSubgradientFunc objective,
    RalphProjectFunc project,
    void *user_data,
    RalphSubgradientResult *result
);

/*
 * Initialize result structure.
 * Call before ralph_subgradient_optimize to set result to known state.
 */
void ralph_subgradient_result_init(RalphSubgradientResult *result);

/*
 * Free resources in result structure.
 * Frees result->best_x and sets it to NULL.
 */
void ralph_subgradient_result_free(RalphSubgradientResult *result);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_OPTIM_SUBGRADIENT_H */
