/*
 * test_optim.c - Tests for Ralph optimization submodule
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "optim_subgradient.h"
#include "optim_lagrangian.h"

#define TOLERANCE 1e-6
#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAILED: %s\n", msg); \
        return 0; \
    } \
} while(0)

static int tests_run = 0;
static int tests_passed = 0;

/*
 * Test helper: simple quadratic function
 *   f(x) = -||x - target||^2 (maximization is finding target)
 *   g(x) = -2(x - target)
 */
typedef struct {
    double *target;
    int n;
} QuadraticData;

static double quadratic_objective(const double *x, double *g, int n, void *user_data) {
    QuadraticData *data = (QuadraticData *)user_data;
    double f = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = x[i] - data->target[i];
        f -= diff * diff;
        g[i] = -2.0 * diff;  /* subgradient for maximization */
    }
    return f;
}

/*
 * Test helper: projection onto non-negative orthant
 */
static void project_nonnegative(double *x, int n, void *user_data) {
    (void)user_data;
    for (int i = 0; i < n; i++) {
        if (x[i] < 0.0) {
            x[i] = 0.0;
        }
    }
}

/*
 * Test helper: piecewise linear (absolute value)
 *   f(x) = -|x - target| (max at x=target)
 *   g(x) = -sign(x - target)
 */
static double abs_objective(const double *x, double *g, int n, void *user_data) {
    QuadraticData *data = (QuadraticData *)user_data;
    double f = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = x[i] - data->target[i];
        f -= fabs(diff);
        g[i] = (diff > 0) ? -1.0 : ((diff < 0) ? 1.0 : 0.0);
    }
    return f;
}

/* ========== Tests ========== */

static int test_result_init_free(void) {
    printf("test_result_init_free...\n");

    RalphSubgradientResult result;
    ralph_subgradient_result_init(&result);

    ASSERT(result.best_x == NULL, "best_x should be NULL after init");
    ASSERT(result.best_value < -1e30, "best_value should be very negative after init");
    ASSERT(result.status == RALPH_SUBGRAD_ERROR, "status should be ERROR after init");

    /* Free should handle NULL safely */
    ralph_subgradient_result_free(&result);
    ASSERT(result.best_x == NULL, "best_x should remain NULL after free");

    printf("  PASSED\n");
    return 1;
}

static int test_constant_step(void) {
    printf("test_constant_step...\n");

    /* Target at (3, 4) */
    double target[2] = {3.0, 4.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {0.0, 0.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.step_strategy = RALPH_STEP_CONSTANT;
    params.initial_step = 0.1;
    params.max_iterations = 100;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, quadratic_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");
    ASSERT(result.best_x != NULL, "best_x should be allocated");
    ASSERT(result.iterations > 0, "should have run iterations");

    /* Check convergence towards target */
    double dist = sqrt(pow(result.best_x[0] - 3.0, 2) + pow(result.best_x[1] - 4.0, 2));
    ASSERT(dist < 1.0, "should converge near target");

    ralph_subgradient_result_free(&result);
    printf("  PASSED (dist=%.4f, iters=%d)\n", dist, result.iterations);
    return 1;
}

static int test_diminishing_step(void) {
    printf("test_diminishing_step...\n");

    double target[2] = {3.0, 4.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {0.0, 0.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.step_strategy = RALPH_STEP_DIMINISHING;
    params.initial_step = 1.0;
    params.max_iterations = 200;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, quadratic_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");

    double dist = sqrt(pow(result.best_x[0] - 3.0, 2) + pow(result.best_x[1] - 4.0, 2));
    ASSERT(dist < 1.0, "should converge near target");

    ralph_subgradient_result_free(&result);
    printf("  PASSED (dist=%.4f, iters=%d)\n", dist, result.iterations);
    return 1;
}

static int test_polyak_step(void) {
    printf("test_polyak_step...\n");

    double target[2] = {3.0, 4.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {0.0, 0.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.step_strategy = RALPH_STEP_POLYAK;
    params.initial_step = 1.5;
    params.f_star = 0.0;  /* Optimal value is 0 (at target) */
    params.use_f_star = 1;
    params.max_iterations = 100;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, quadratic_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");

    double dist = sqrt(pow(result.best_x[0] - 3.0, 2) + pow(result.best_x[1] - 4.0, 2));
    ASSERT(dist < 0.5, "Polyak should converge well with known f*");

    ralph_subgradient_result_free(&result);
    printf("  PASSED (dist=%.4f, iters=%d)\n", dist, result.iterations);
    return 1;
}

static int test_adaptive_step(void) {
    printf("test_adaptive_step...\n");

    double target[2] = {3.0, 4.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {0.0, 0.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    /* ADAPTIVE is default */
    params.initial_step = 2.0;
    params.max_iterations = 300;
    params.no_improve_limit = 20;
    params.step_decay = 0.5;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, quadratic_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");
    ASSERT(result.improvements > 0, "should have improvements");

    double dist = sqrt(pow(result.best_x[0] - 3.0, 2) + pow(result.best_x[1] - 4.0, 2));
    ASSERT(dist < 0.1, "adaptive should converge well");

    ralph_subgradient_result_free(&result);
    printf("  PASSED (dist=%.4f, iters=%d, improvements=%d)\n",
           dist, result.iterations, result.improvements);
    return 1;
}

static int test_nonsmooth_convergence(void) {
    printf("test_nonsmooth_convergence...\n");

    /* Test on non-smooth |x| function */
    double target[2] = {2.0, 3.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {10.0, 10.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.initial_step = 1.0;
    params.max_iterations = 200;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, abs_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");

    /* For non-smooth, we may not converge exactly but should be close */
    double dist = sqrt(pow(result.best_x[0] - 2.0, 2) + pow(result.best_x[1] - 3.0, 2));
    ASSERT(dist < 2.0, "should be reasonably close for non-smooth");

    ralph_subgradient_result_free(&result);
    printf("  PASSED (dist=%.4f, iters=%d)\n", dist, result.iterations);
    return 1;
}

static int test_projection(void) {
    printf("test_projection...\n");

    /* Target at (-2, 5) but constrained to non-negative */
    double target[2] = {-2.0, 5.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {10.0, 10.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.max_iterations = 200;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, quadratic_objective,
                                          project_nonnegative, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");

    /* Projected optimal is (0, 5) */
    ASSERT(result.best_x[0] >= -TOLERANCE, "x[0] should be non-negative");
    ASSERT(fabs(result.best_x[0]) < 0.5, "x[0] should be near 0");
    ASSERT(fabs(result.best_x[1] - 5.0) < 0.5, "x[1] should be near 5");

    double x0_val = result.best_x[0];
    double x1_val = result.best_x[1];
    ralph_subgradient_result_free(&result);
    printf("  PASSED (x=[%.4f, %.4f])\n", x0_val, x1_val);
    return 1;
}

static int test_high_dimension(void) {
    printf("test_high_dimension...\n");

    int n = 100;
    double *target = (double *)malloc(n * sizeof(double));
    double *x0 = (double *)malloc(n * sizeof(double));

    for (int i = 0; i < n; i++) {
        target[i] = (double)(i % 10);
        x0[i] = 0.0;
    }

    QuadraticData data = {.target = target, .n = n};

    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.max_iterations = 500;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(n, x0, &params, quadratic_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");

    /* Check convergence */
    double dist_sq = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = result.best_x[i] - target[i];
        dist_sq += diff * diff;
    }
    double dist = sqrt(dist_sq);
    ASSERT(dist < 5.0, "should converge in high dimension");

    ralph_subgradient_result_free(&result);
    free(target);
    free(x0);
    printf("  PASSED (dist=%.4f, iters=%d)\n", dist, result.iterations);
    return 1;
}

static int test_convergence_status(void) {
    printf("test_convergence_status...\n");

    /* Already at optimum */
    double target[2] = {0.0, 0.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {0.0, 0.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.tol = 1e-6;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, quadratic_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");
    ASSERT(result.status == RALPH_SUBGRAD_CONVERGED, "should report converged");
    ASSERT(result.final_grad_norm < params.tol, "gradient should be small");

    ralph_subgradient_result_free(&result);
    printf("  PASSED (status=%d, grad_norm=%.2e)\n", result.status, result.final_grad_norm);
    return 1;
}

static int test_small_step_termination(void) {
    printf("test_small_step_termination...\n");

    /*
     * Test SMALL_STEP termination using DIMINISHING step strategy.
     * With step = alpha/sqrt(k), step falls below min_step after k > (alpha/min_step)^2.
     * For alpha=0.01, min_step=0.001: k > 100 triggers termination.
     */
    double target[2] = {100.0, 100.0};
    QuadraticData data = {.target = target, .n = 2};

    double x0[2] = {0.0, 0.0};
    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    params.step_strategy = RALPH_STEP_DIMINISHING;
    params.initial_step = 0.01;
    params.min_step = 0.001;
    params.max_iterations = 500;

    RalphSubgradientResult result;
    int ret = ralph_subgradient_optimize(2, x0, &params, quadratic_objective,
                                          NULL, &data, &result);

    ASSERT(ret == 0, "optimize should succeed");
    ASSERT(result.status == RALPH_SUBGRAD_SMALL_STEP, "should terminate on small step");
    ASSERT(result.iterations > 100, "should run past k=100");

    RalphSubgradientStatus saved_status = result.status;
    double saved_step = result.final_step;
    int saved_iters = result.iterations;
    ralph_subgradient_result_free(&result);
    printf("  PASSED (status=%d, final_step=%.2e, iters=%d)\n",
           saved_status, saved_step, saved_iters);
    return 1;
}

static int test_invalid_inputs(void) {
    printf("test_invalid_inputs...\n");

    RalphSubgradientParams params = RALPH_SUBGRADIENT_PARAMS_DEFAULT;
    RalphSubgradientResult result;
    double x0[2] = {0.0, 0.0};
    double target[2] = {1.0, 1.0};
    QuadraticData data = {.target = target, .n = 2};

    /* NULL x0 */
    int ret = ralph_subgradient_optimize(2, NULL, &params, quadratic_objective,
                                          NULL, &data, &result);
    ASSERT(ret == -1, "should fail with NULL x0");

    /* NULL params */
    ret = ralph_subgradient_optimize(2, x0, NULL, quadratic_objective,
                                      NULL, &data, &result);
    ASSERT(ret == -1, "should fail with NULL params");

    /* NULL objective */
    ret = ralph_subgradient_optimize(2, x0, &params, NULL,
                                      NULL, &data, &result);
    ASSERT(ret == -1, "should fail with NULL objective");

    /* Zero dimension */
    ret = ralph_subgradient_optimize(0, x0, &params, quadratic_objective,
                                      NULL, &data, &result);
    ASSERT(ret == -1, "should fail with n=0");

    printf("  PASSED\n");
    return 1;
}

/* ========== Lagrangian Relaxation Tests ========== */

/*
 * Simple SCP test data:
 *   3 elements, 3 sets
 *   S0 = {0, 1}, cost=3
 *   S1 = {1, 2}, cost=2
 *   S2 = {0, 2}, cost=4
 *
 *   Optimal: S0 + S1 = cost 5 (or S1 + S2 = cost 6)
 *
 *   Matrix A (CSC):
 *   [1 0 1]   S0 covers e0, S2 covers e0
 *   [1 1 0]   S0 covers e1, S1 covers e1
 *   [0 1 1]   S1 covers e2, S2 covers e2
 */
static double scp_costs[3] = {3.0, 2.0, 4.0};
static double scp_rhs[3] = {1.0, 1.0, 1.0};
static int scp_colptr[4] = {0, 2, 4, 6};
static int scp_rowidx[6] = {0, 1, 1, 2, 0, 2};

/*
 * SCP subproblem: x[j] = 1 if reduced_cost[j] < 0
 */
static double scp_subproblem(int n, const double *rc, double *x, void *user_data) {
    (void)user_data;
    double obj = 0.0;
    for (int j = 0; j < n; j++) {
        if (rc[j] < -1e-9) {
            x[j] = 1.0;
            obj += rc[j];
        } else {
            x[j] = 0.0;
        }
    }
    return obj;
}

/*
 * SCP repair: greedy set covering
 */
static double scp_repair(int n, const double *x_lagr, double *x_feas, void *user_data) {
    (void)user_data;

    /* Copy Lagrangian solution */
    memcpy(x_feas, x_lagr, n * sizeof(double));

    /* Check coverage */
    int covered[3] = {0, 0, 0};
    for (int j = 0; j < n; j++) {
        if (x_feas[j] > 0.5) {
            int start = scp_colptr[j];
            int end = scp_colptr[j + 1];
            for (int k = start; k < end; k++) {
                covered[scp_rowidx[k]] = 1;
            }
        }
    }

    /* Greedy cover uncovered elements */
    for (int i = 0; i < 3; i++) {
        if (covered[i]) continue;

        /* Find cheapest set covering element i */
        int best_set = -1;
        double best_cost = HUGE_VAL;
        for (int j = 0; j < n; j++) {
            if (x_feas[j] > 0.5) continue;  /* Already selected */
            int start = scp_colptr[j];
            int end = scp_colptr[j + 1];
            for (int k = start; k < end; k++) {
                if (scp_rowidx[k] == i) {
                    if (scp_costs[j] < best_cost) {
                        best_cost = scp_costs[j];
                        best_set = j;
                    }
                    break;
                }
            }
        }

        if (best_set >= 0) {
            x_feas[best_set] = 1.0;
            /* Update coverage */
            int start = scp_colptr[best_set];
            int end = scp_colptr[best_set + 1];
            for (int k = start; k < end; k++) {
                covered[scp_rowidx[k]] = 1;
            }
        }
    }

    /* Compute cost */
    double cost = 0.0;
    for (int j = 0; j < n; j++) {
        if (x_feas[j] > 0.5) {
            cost += scp_costs[j];
        }
    }
    return cost;
}

static int test_lagrangian_create(void) {
    printf("test_lagrangian_create...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );

    ASSERT(ctx != NULL, "context should be created");
    ASSERT(ctx->m == 3, "m should be 3");
    ASSERT(ctx->n == 3, "n should be 3");
    ASSERT(ctx->type == RALPH_LAGRANGIAN_COVERING, "type should be COVERING");

    ralph_lagrangian_free(ctx);
    printf("  PASSED\n");
    return 1;
}

static int test_lagrangian_create_null_inputs(void) {
    printf("test_lagrangian_create_null_inputs...\n");

    /* NULL cost */
    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        NULL, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx == NULL, "should fail with NULL cost");

    /* NULL subproblem */
    ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        NULL, NULL
    );
    ASSERT(ctx == NULL, "should fail with NULL subproblem");

    /* Zero dimensions */
    ctx = ralph_lagrangian_create(
        0, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx == NULL, "should fail with m=0");

    printf("  PASSED\n");
    return 1;
}

static int test_lagrangian_bound_zero_lambda(void) {
    printf("test_lagrangian_bound_zero_lambda...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx != NULL, "context created");

    /* With lambda=0, reduced costs = c, all positive, so x=0, L(0) = 0 */
    double bound = ralph_lagrangian_bound(ctx);
    ASSERT(fabs(bound) < TOLERANCE, "L(0) should be 0");

    ralph_lagrangian_free(ctx);
    printf("  PASSED (bound=%.4f)\n", bound);
    return 1;
}

static int test_lagrangian_bound_positive_lambda(void) {
    printf("test_lagrangian_bound_positive_lambda...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx != NULL, "context created");

    /* Set lambda = [1, 1, 1] */
    double lambda[3] = {1.0, 1.0, 1.0};
    ralph_lagrangian_set_lambda(ctx, lambda);

    double bound = ralph_lagrangian_bound(ctx);

    /*
     * Reduced costs:
     *   c_bar[0] = 3 - 1 - 1 = 1 > 0, x[0] = 0
     *   c_bar[1] = 2 - 1 - 1 = 0, x[1] = 0
     *   c_bar[2] = 4 - 1 - 1 = 2 > 0, x[2] = 0
     * L(λ) = λ'b + 0 = 1 + 1 + 1 = 3
     */
    ASSERT(fabs(bound - 3.0) < TOLERANCE, "L([1,1,1]) should be 3");

    ralph_lagrangian_free(ctx);
    printf("  PASSED (bound=%.4f)\n", bound);
    return 1;
}

static int test_lagrangian_bound_high_lambda(void) {
    printf("test_lagrangian_bound_high_lambda...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx != NULL, "context created");

    /* Set lambda = [2, 2, 2] */
    double lambda[3] = {2.0, 2.0, 2.0};
    ralph_lagrangian_set_lambda(ctx, lambda);

    double bound = ralph_lagrangian_bound(ctx);

    /*
     * Reduced costs:
     *   c_bar[0] = 3 - 2 - 2 = -1 < 0, x[0] = 1
     *   c_bar[1] = 2 - 2 - 2 = -2 < 0, x[1] = 1
     *   c_bar[2] = 4 - 2 - 2 = 0, x[2] = 0
     * L(λ) = λ'b + subproblem = 6 + (-1 + -2) = 3
     */
    ASSERT(fabs(bound - 3.0) < TOLERANCE, "L([2,2,2]) should be 3");

    ralph_lagrangian_free(ctx);
    printf("  PASSED (bound=%.4f)\n", bound);
    return 1;
}

static int test_lagrangian_optimize(void) {
    printf("test_lagrangian_optimize...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx != NULL, "context created");

    RalphLagrangianParams params = RALPH_LAGRANGIAN_PARAMS_DEFAULT;
    params.subgrad_params.max_iterations = 100;

    double bound = ralph_lagrangian_optimize(ctx, &params);

    /* Lagrangian bound should be <= optimal (5) */
    ASSERT(bound <= 5.0 + TOLERANCE, "bound should be <= optimal");
    ASSERT(bound >= 0.0, "bound should be non-negative");

    int iters = ralph_lagrangian_iterations(ctx);
    int improvements = ralph_lagrangian_bound_improvements(ctx);
    ASSERT(iters > 0, "should have iterations");

    ralph_lagrangian_free(ctx);
    printf("  PASSED (bound=%.4f, iters=%d, improvements=%d)\n", bound, iters, improvements);
    return 1;
}

static int test_lagrangian_with_repair(void) {
    printf("test_lagrangian_with_repair...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx != NULL, "context created");

    ralph_lagrangian_set_repair(ctx, scp_repair);

    RalphLagrangianParams params = RALPH_LAGRANGIAN_PARAMS_DEFAULT;
    params.subgrad_params.max_iterations = 100;
    params.repair_frequency = 10;

    double dual = ralph_lagrangian_optimize(ctx, &params);
    double primal = ralph_lagrangian_primal_bound(ctx);
    const double *solution = ralph_lagrangian_solution(ctx);

    ASSERT(dual <= primal + TOLERANCE, "dual <= primal");
    ASSERT(primal <= 6.0, "primal <= 6 (worst feasible)");
    ASSERT(solution != NULL, "solution should exist");

    /* Verify solution is feasible */
    int covered[3] = {0, 0, 0};
    for (int j = 0; j < 3; j++) {
        if (solution[j] > 0.5) {
            int start = scp_colptr[j];
            int end = scp_colptr[j + 1];
            for (int k = start; k < end; k++) {
                covered[scp_rowidx[k]] = 1;
            }
        }
    }
    ASSERT(covered[0] && covered[1] && covered[2], "all elements covered");

    ralph_lagrangian_free(ctx);
    printf("  PASSED (dual=%.4f, primal=%.4f)\n", dual, primal);
    return 1;
}

static int test_lagrangian_primal_bound_hint(void) {
    printf("test_lagrangian_primal_bound_hint...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx != NULL, "context created");

    /* Set primal bound hint for Polyak step */
    ralph_lagrangian_set_primal_bound(ctx, 5.0);

    RalphLagrangianParams params = RALPH_LAGRANGIAN_PARAMS_DEFAULT;
    params.subgrad_params.max_iterations = 100;
    params.subgrad_params.step_strategy = RALPH_STEP_POLYAK;

    double bound = ralph_lagrangian_optimize(ctx, &params);

    ASSERT(bound <= 5.0 + TOLERANCE, "bound should be <= 5");
    ASSERT(bound >= 0.0, "bound should be non-negative");

    ralph_lagrangian_free(ctx);
    printf("  PASSED (bound=%.4f)\n", bound);
    return 1;
}

static int test_lagrangian_accessors(void) {
    printf("test_lagrangian_accessors...\n");

    RalphLagrangianContext *ctx = ralph_lagrangian_create(
        3, 3, RALPH_LAGRANGIAN_COVERING,
        scp_costs, scp_rhs, scp_colptr, scp_rowidx, NULL,
        scp_subproblem, NULL
    );
    ASSERT(ctx != NULL, "context created");

    /* Before optimization */
    double dual = ralph_lagrangian_dual_bound(ctx);
    double primal = ralph_lagrangian_primal_bound(ctx);
    const double *sol = ralph_lagrangian_solution(ctx);

    ASSERT(dual < -1e30, "initial dual should be -inf");
    ASSERT(primal > 1e30, "initial primal should be +inf");
    ASSERT(sol == NULL, "initial solution should be NULL");

    /* After optimization with repair */
    ralph_lagrangian_set_repair(ctx, scp_repair);
    RalphLagrangianParams params = RALPH_LAGRANGIAN_PARAMS_DEFAULT;
    params.repair_frequency = 5;
    ralph_lagrangian_optimize(ctx, &params);

    dual = ralph_lagrangian_dual_bound(ctx);
    primal = ralph_lagrangian_primal_bound(ctx);
    sol = ralph_lagrangian_solution(ctx);
    int iters = ralph_lagrangian_iterations(ctx);
    int bound_impr = ralph_lagrangian_bound_improvements(ctx);
    int primal_impr = ralph_lagrangian_primal_improvements(ctx);

    ASSERT(dual > -1e30, "dual should be finite");
    ASSERT(primal < 1e30, "primal should be finite");
    ASSERT(sol != NULL, "solution should exist");
    ASSERT(iters > 0, "iterations > 0");
    ASSERT(bound_impr >= 0, "bound_impr >= 0");
    ASSERT(primal_impr >= 0, "primal_impr >= 0");

    ralph_lagrangian_free(ctx);
    printf("  PASSED (dual=%.2f, primal=%.2f, iters=%d)\n", dual, primal, iters);
    return 1;
}

/* ========== Main ========== */

static void run_test(int (*test_fn)(void), const char *name) {
    tests_run++;
    if (test_fn()) {
        tests_passed++;
    } else {
        printf("FAILED: %s\n", name);
    }
}

int main(void) {
    printf("\n=== Ralph Optimization Module Tests ===\n\n");

    printf("--- Subgradient Optimization Tests ---\n\n");

    run_test(test_result_init_free, "test_result_init_free");
    run_test(test_constant_step, "test_constant_step");
    run_test(test_diminishing_step, "test_diminishing_step");
    run_test(test_polyak_step, "test_polyak_step");
    run_test(test_adaptive_step, "test_adaptive_step");
    run_test(test_nonsmooth_convergence, "test_nonsmooth_convergence");
    run_test(test_projection, "test_projection");
    run_test(test_high_dimension, "test_high_dimension");
    run_test(test_convergence_status, "test_convergence_status");
    run_test(test_small_step_termination, "test_small_step_termination");
    run_test(test_invalid_inputs, "test_invalid_inputs");

    printf("\n--- Lagrangian Relaxation Tests ---\n\n");

    run_test(test_lagrangian_create, "test_lagrangian_create");
    run_test(test_lagrangian_create_null_inputs, "test_lagrangian_create_null_inputs");
    run_test(test_lagrangian_bound_zero_lambda, "test_lagrangian_bound_zero_lambda");
    run_test(test_lagrangian_bound_positive_lambda, "test_lagrangian_bound_positive_lambda");
    run_test(test_lagrangian_bound_high_lambda, "test_lagrangian_bound_high_lambda");
    run_test(test_lagrangian_optimize, "test_lagrangian_optimize");
    run_test(test_lagrangian_with_repair, "test_lagrangian_with_repair");
    run_test(test_lagrangian_primal_bound_hint, "test_lagrangian_primal_bound_hint");
    run_test(test_lagrangian_accessors, "test_lagrangian_accessors");

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
