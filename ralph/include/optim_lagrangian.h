/*
 * optim_lagrangian.h - Generic Lagrangian relaxation framework
 *
 * Part of Ralph optimization module.
 *
 * For a problem:
 *   min c'x  s.t. Ax >= b, x in X  (covering)
 *   max c'x  s.t. Ax <= b, x in X  (packing)
 *   min c'x  s.t. Ax = b, x in X   (partitioning)
 *
 * The Lagrangian relaxation with multipliers λ:
 *   L(λ) = min { c'x + λ'(b - Ax) : x in X }
 *        = λ'b + min { (c - A'λ)'x : x in X }
 *
 * The Lagrangian dual: max L(λ) s.t. λ >= 0 (for covering/packing)
 *
 * User provides:
 *   - Subproblem solver: given reduced costs, find optimal x in X
 *   - Optional repair heuristic: convert infeasible Lagrangian solution to feasible
 */

#ifndef RALPH_OPTIM_LAGRANGIAN_H
#define RALPH_OPTIM_LAGRANGIAN_H

#include "optim_subgradient.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lagrangian problem type.
 * Determines constraint sense and multiplier sign constraints.
 */
typedef enum {
    RALPH_LAGRANGIAN_COVERING,      /* Ax >= b, minimize, λ >= 0 */
    RALPH_LAGRANGIAN_PACKING,       /* Ax <= b, maximize, λ >= 0 */
    RALPH_LAGRANGIAN_PARTITIONING   /* Ax = b, minimize, λ free */
} RalphLagrangianType;

/*
 * Callback: solve Lagrangian subproblem with reduced costs.
 *
 * For covering/partitioning (minimization):
 *   min { (c - A'λ)'x : x in X }
 *
 * For packing (maximization):
 *   max { (c - A'λ)'x : x in X }
 *
 * Parameters:
 *   n            - Number of variables
 *   reduced_cost - Reduced costs c - A'λ [n]
 *   x            - Output: optimal x in X [n]
 *   user_data    - User context
 *
 * Returns:
 *   Optimal subproblem objective value.
 */
typedef double (*RalphLagrangianSubproblem)(
    int n,
    const double *reduced_cost,
    double *x,
    void *user_data
);

/*
 * Callback: repair infeasible Lagrangian solution to feasible.
 *
 * The Lagrangian solution x_lagr may violate the relaxed constraints.
 * This callback should produce a feasible solution x_feasible.
 *
 * Parameters:
 *   n            - Number of variables
 *   x_lagr       - Lagrangian solution (possibly infeasible) [n]
 *   x_feasible   - Output: feasible solution [n]
 *   user_data    - User context
 *
 * Returns:
 *   Cost of feasible solution (primal bound), or HUGE_VAL if repair failed.
 */
typedef double (*RalphLagrangianRepair)(
    int n,
    const double *x_lagr,
    double *x_feasible,
    void *user_data
);

/*
 * Lagrangian relaxation parameters.
 */
typedef struct {
    /* Subgradient optimization parameters */
    RalphSubgradientParams subgrad_params;

    /* Repair frequency: call repair every N iterations (0 = never) */
    int repair_frequency;

    /* Initial multipliers (NULL = zero) */
    const double *initial_lambda;
} RalphLagrangianParams;

/* Default parameters initializer */
#define RALPH_LAGRANGIAN_PARAMS_DEFAULT { \
    .subgrad_params = RALPH_SUBGRADIENT_PARAMS_DEFAULT, \
    .repair_frequency = 10, \
    .initial_lambda = NULL \
}

/*
 * Lagrangian relaxation context.
 * Holds problem data and optimization state.
 */
typedef struct {
    /* Problem dimensions */
    int m;                          /* Number of constraints (dual dimension) */
    int n;                          /* Number of variables (primal dimension) */
    RalphLagrangianType type;       /* Problem type */

    /* Problem data (borrowed, not owned) */
    const double *c;                /* Objective coefficients [n] */
    const double *b;                /* RHS values [m] */
    const int *A_colptr;            /* CSC column pointers [n+1] */
    const int *A_rowidx;            /* CSC row indices */
    const double *A_values;         /* CSC values (NULL for 0-1 matrix) */

    /* Callbacks */
    RalphLagrangianSubproblem subproblem;
    RalphLagrangianRepair repair;   /* Optional */
    void *user_data;

    /* Optimization state (owned) */
    double *lambda;                 /* Current multipliers [m] */
    double *best_lambda;            /* Best multipliers found [m] */
    double *subgradient;            /* Current subgradient [m] */
    double *reduced_cost;           /* Reduced costs c - A'λ [n] */
    double *x_lagrangian;           /* Current subproblem solution [n] */

    /* Best solutions */
    double best_dual_bound;         /* Best Lagrangian bound (lower for min) */
    double best_primal_bound;       /* Best feasible solution cost (upper for min) */
    double *best_solution;          /* Best feasible solution [n] */

    /* Statistics */
    int iterations;
    int bound_improvements;
    int primal_improvements;
} RalphLagrangianContext;

/*
 * Create Lagrangian relaxation context.
 *
 * The matrix A is in CSC format. For 0-1 matrices, A_values can be NULL.
 *
 * Parameters:
 *   m, n       - Problem dimensions
 *   type       - Problem type (covering, packing, partitioning)
 *   c          - Objective coefficients [n] (borrowed)
 *   b          - RHS values [m] (borrowed)
 *   A_colptr   - CSC column pointers [n+1] (borrowed)
 *   A_rowidx   - CSC row indices (borrowed)
 *   A_values   - CSC values, NULL for 0-1 matrix (borrowed)
 *   subproblem - Callback to solve subproblem
 *   user_data  - Passed to callbacks
 *
 * Returns:
 *   New context, or NULL on error. Caller must free with ralph_lagrangian_free().
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
);

/*
 * Free Lagrangian relaxation context.
 */
void ralph_lagrangian_free(RalphLagrangianContext *ctx);

/*
 * Set repair heuristic callback.
 */
void ralph_lagrangian_set_repair(
    RalphLagrangianContext *ctx,
    RalphLagrangianRepair repair
);

/*
 * Set initial multipliers.
 * If not called, multipliers start at zero.
 */
void ralph_lagrangian_set_lambda(
    RalphLagrangianContext *ctx,
    const double *lambda
);

/*
 * Set primal bound (for Polyak step size).
 * For minimization: upper bound on optimal.
 * For maximization: lower bound on optimal.
 */
void ralph_lagrangian_set_primal_bound(
    RalphLagrangianContext *ctx,
    double bound
);

/*
 * Compute Lagrangian bound for current multipliers.
 *
 * Updates ctx->subgradient and ctx->x_lagrangian.
 *
 * Returns:
 *   L(λ) = λ'b + subproblem_obj
 */
double ralph_lagrangian_bound(RalphLagrangianContext *ctx);

/*
 * Run full Lagrangian optimization.
 *
 * Uses subgradient method to maximize L(λ) over λ >= 0 (or λ free).
 * If repair callback is set, periodically generates primal bounds.
 *
 * Parameters:
 *   ctx    - Lagrangian context
 *   params - Optimization parameters
 *
 * Returns:
 *   Best dual bound found.
 */
double ralph_lagrangian_optimize(
    RalphLagrangianContext *ctx,
    const RalphLagrangianParams *params
);

/*
 * Get best dual bound (Lagrangian bound).
 */
double ralph_lagrangian_dual_bound(const RalphLagrangianContext *ctx);

/*
 * Get best primal bound (feasible solution cost).
 * Returns HUGE_VAL if no feasible solution found.
 */
double ralph_lagrangian_primal_bound(const RalphLagrangianContext *ctx);

/*
 * Get best feasible solution.
 * Returns NULL if no feasible solution found.
 */
const double *ralph_lagrangian_solution(const RalphLagrangianContext *ctx);

/*
 * Get optimization statistics.
 */
int ralph_lagrangian_iterations(const RalphLagrangianContext *ctx);
int ralph_lagrangian_bound_improvements(const RalphLagrangianContext *ctx);
int ralph_lagrangian_primal_improvements(const RalphLagrangianContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_OPTIM_LAGRANGIAN_H */
