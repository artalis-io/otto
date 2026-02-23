/*
 * Ralph - Benders Decomposition Tests
 *
 * Tests the generic Benders decomposition solver as an orthogonal module.
 * These tests are independent of domain-specific applications (FuelWise, HoSE).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"

/* Test tracking */
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        printf("  at %s:%d\n", __FILE__, __LINE__); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_NEAR(a, b, tol, msg) ASSERT(fabs((a) - (b)) < (tol), msg)

/* ============================================================================
 * Test 1: Simple Two-Stage Problem
 * ============================================================================
 *
 * Classic two-stage stochastic programming example:
 *
 * Master (first stage):
 *   min  c1*x1 + c2*x2 + theta
 *   s.t. x1 + x2 >= 5       (capacity constraint)
 *        x1, x2 >= 0, integer
 *
 * Subproblem (second stage, given x1, x2):
 *   min  d1*y1 + d2*y2
 *   s.t. y1 <= x1           (linking: can't use more than bought)
 *        y2 <= x2           (linking)
 *        y1 + y2 >= 4       (demand)
 *        y1, y2 >= 0
 *
 * Total: min c1*x1 + c2*x2 + d1*y1 + d2*y2
 *
 * With c1=1, c2=2, d1=3, d2=4:
 * - Need x1+x2 >= 5 and y1+y2 >= 4
 * - y1 <= x1, y2 <= x2
 * - Optimal: x1=4, x2=1, y1=4, y2=0 -> cost = 4 + 2 + 12 + 0 = 18
 *   Or: x1=5, x2=0, y1=4, y2=0 -> cost = 5 + 0 + 12 + 0 = 17
 */
void test_simple_two_stage(void) {
    printf("\n=== Test: Simple Two-Stage Problem ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Variables: x1, x2 (master), y1, y2 (sub), theta (recourse) */
    /* x1: master, integer - use large but not infinite bounds */
    int x1 = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_INTEGER);
    /* x2: master, integer */
    int x2 = ralph_test_add_var(model, 0, 1e9, 2.0, RALPH_INTEGER);
    /* y1: subproblem, continuous */
    int y1 = ralph_test_add_var(model, 0, 1e9, 3.0, RALPH_CONTINUOUS);
    /* y2: subproblem, continuous */
    int y2 = ralph_test_add_var(model, 0, 1e9, 4.0, RALPH_CONTINUOUS);
    /* theta: recourse cost - large bounds for general problems */
    int theta = ralph_test_add_var(model, -1e9, 1e9, 1.0, RALPH_CONTINUOUS);

    ASSERT(x1 == 0 && x2 == 1 && y1 == 2 && y2 == 3 && theta == 4,
           "Variables added correctly");

    /* Master constraint: x1 + x2 >= 5 */
    {
        int idx[] = {x1, x2};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 5.0);
    }

    /* Linking constraint 1: y1 - x1 <= 0  (y1 <= x1) */
    {
        int idx[] = {y1, x1};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Linking constraint 2: y2 - x2 <= 0  (y2 <= x2) */
    {
        int idx[] = {y2, x2};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Subproblem constraint: y1 + y2 >= 4 */
    {
        int idx[] = {y1, y2};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 4.0);
    }

    /* Configure Benders */
    int master_vars[] = {x1, x2};
    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = 2;
    config.theta_var = theta;
    config.verbose = 1;
    config.max_iterations = 50;

    double solution[5];
    RalphBendersResult result;

    int ret = ralph_test_solve_benders(model, &config, solution, &result);

    printf("Benders returned: %d\n", ret);
    printf("Status: %d\n", result.status);
    printf("Objective: %.4f\n", result.objective);
    printf("Iterations: %d\n", result.iterations);
    printf("Optimality cuts: %d, Feasibility cuts: %d\n",
           result.optimality_cuts, result.feasibility_cuts);

    if (ret == 0) {
        printf("Solution: x1=%.2f, x2=%.2f, y1=%.2f, y2=%.2f, theta=%.2f\n",
               solution[0], solution[1], solution[2], solution[3], solution[4]);

        /* Check feasibility */
        double x1_val = solution[0];
        double x2_val = solution[1];
        double y1_val = solution[2];
        double y2_val = solution[3];

        ASSERT(x1_val + x2_val >= 5.0 - 1e-6, "Master constraint satisfied");
        ASSERT(y1_val <= x1_val + 1e-6, "Linking constraint 1 satisfied");
        ASSERT(y2_val <= x2_val + 1e-6, "Linking constraint 2 satisfied");
        ASSERT(y1_val + y2_val >= 4.0 - 1e-6, "Subproblem constraint satisfied");

        /* Check objective - optimal should be around 17 */
        double computed_obj = 1.0*x1_val + 2.0*x2_val + 3.0*y1_val + 4.0*y2_val;
        printf("Computed objective: %.4f\n", computed_obj);
        ASSERT(computed_obj <= 20.0, "Objective is reasonable");
    }

    ASSERT(result.iterations >= 1, "At least one iteration");
    ASSERT(result.iterations <= 50, "Terminated within iteration limit");

    ralph_test_free(model);
}

/* ============================================================================
 * Test 2: Pure LP Benders (Continuous Master)
 * ============================================================================
 *
 * Benders with continuous master variables (no MIP).
 * Should converge in few iterations since master LP is easy.
 */
void test_continuous_master(void) {
    printf("\n=== Test: Continuous Master (LP) ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Variables: x (master), y1, y2 (sub) - use large bounds */
    int x = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_CONTINUOUS);
    int y1 = ralph_test_add_var(model, 0, 1e9, 2.0, RALPH_CONTINUOUS);
    int y2 = ralph_test_add_var(model, 0, 1e9, 3.0, RALPH_CONTINUOUS);

    /* Master: x >= 2 */
    {
        int idx[] = {x};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 2.0);
    }

    /* Linking: y1 + y2 <= x */
    {
        int idx[] = {y1, y2, x};
        double val[] = {1.0, 1.0, -1.0};
        ralph_test_add_constraint(model, 3, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Sub: y1 + y2 >= 1 */
    {
        int idx[] = {y1, y2};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }

    int master_vars[] = {x};
    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = 1;
    config.theta_var = -1; /* Auto-create theta */
    config.verbose = 1;

    RalphBendersResult result;
    double solution[3];

    int ret = ralph_test_solve_benders(model, &config, solution, &result);

    printf("Result: ret=%d, obj=%.4f, iters=%d\n", ret, result.objective, result.iterations);

    if (ret == 0) {
        /* Optimal: x=2, y1=1, y2=0 (or y1=0, y2=1) -> obj = 2 + 2 = 4 */
        ASSERT(result.objective <= 5.0, "Objective is optimal or near-optimal");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test 3: Infeasible Subproblem
 * ============================================================================
 *
 * Test that Benders correctly generates feasibility cuts when the
 * subproblem is infeasible for a given master solution.
 */
void test_infeasible_subproblem(void) {
    printf("\n=== Test: Infeasible Subproblem -> Feasibility Cut ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Master var x controls subproblem capacity - use large bounds */
    int x = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_INTEGER);
    /* Sub vars y1, y2 */
    int y1 = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_CONTINUOUS);
    int y2 = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_CONTINUOUS);

    /* Linking: y1 <= 2*x */
    {
        int idx[] = {y1, x};
        double val[] = {1.0, -2.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Linking: y2 <= x */
    {
        int idx[] = {y2, x};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Sub: y1 + y2 >= 10 (requires x >= 4 to be feasible) */
    {
        int idx[] = {y1, y2};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 10.0);
    }

    /* Pure-master constraint required by current Benders implementation. */
    {
        int idx[] = {x};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0);
    }

    int master_vars[] = {x};
    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = 1;
    config.theta_var = -1;
    config.verbose = 1;

    RalphBendersResult result;

    int ret = ralph_test_solve_benders(model, &config, NULL, &result);

    printf("Result: ret=%d, iters=%d, feas_cuts=%d\n",
           ret, result.iterations, result.feasibility_cuts);

    ASSERT(ret == 0, "Infeasible-subproblem test returns success");
    ASSERT(result.status == RALPH_STATUS_OPTIMAL, "Infeasible-subproblem test converges to optimal");
    ASSERT(result.feasibility_cuts > 0, "Feasibility cuts are generated");
    ASSERT(result.iterations >= 1, "At least one iteration executed");

    ralph_test_free(model);
}

/* ============================================================================
 * Test 3b: Infeasible Subproblem + strict_farkas
 * ============================================================================
 */
void test_infeasible_subproblem_strict_farkas(void) {
    printf("\n=== Test: Infeasible Subproblem + strict_farkas ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    int x = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_INTEGER);
    int y1 = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_CONTINUOUS);
    int y2 = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_CONTINUOUS);

    {
        int idx[] = {y1, x};
        double val[] = {1.0, -2.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }
    {
        int idx[] = {y2, x};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }
    {
        int idx[] = {y1, y2};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 10.0);
    }
    {
        int idx[] = {x};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0);
    }

    int master_vars[] = {x};
    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = 1;
    config.theta_var = -1;
    config.strict_farkas = 1;
    config.verbose = 1;

    RalphBendersResult result;
    int ret = ralph_test_solve_benders(model, &config, NULL, &result);

    ASSERT(ret == 0, "strict_farkas infeasible-subproblem test returns success");
    ASSERT(result.status == RALPH_STATUS_OPTIMAL, "strict_farkas converges to optimal");
    ASSERT(result.feasibility_cuts > 0, "strict_farkas generates feasibility cuts");
    ASSERT(result.iterations >= 1, "strict_farkas executes at least one iteration");

    ralph_test_free(model);
}

/* ============================================================================
 * Test 4: Multiple Scenarios (Stochastic Benders)
 * ============================================================================
 */
void test_stochastic_benders(void) {
    printf("\n=== Test: Stochastic Benders (2 Scenarios) ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* First-stage: x (investment decision) - use large bounds */
    int x = ralph_test_add_var(model, 0, 1e9, 2.0, RALPH_INTEGER);

    /* Second-stage: y (recourse for both scenarios) */
    int y = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_CONTINUOUS);

    /* Linking: y <= x (can't use more than invested) */
    {
        int idx[] = {y, x};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Sub: y >= 3 (demand) */
    {
        int idx[] = {y};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 3.0);
    }

    /* Pure-master constraint required by current Benders implementation. */
    {
        int idx[] = {x};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0);
    }

    int master_vars[] = {x};
    double probs[] = {0.6, 0.4}; /* Two scenarios with probabilities */

    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = 1;
    config.theta_var = -1;
    config.num_scenarios = 2;
    config.scenario_probs = probs;
    config.verbose = 1;

    RalphBendersResult result;

    int ret = ralph_test_solve_benders(model, &config, NULL, &result);

    printf("Result: ret=%d, obj=%.4f, iters=%d\n", ret, result.objective, result.iterations);

    ASSERT(ret == 0, "Stochastic Benders returns success");
    ASSERT(result.status == RALPH_STATUS_OPTIMAL, "Stochastic Benders status is OPTIMAL");
    ASSERT(result.iterations >= 1, "Stochastic Benders runs at least one iteration");
    ASSERT(result.objective < 1e8, "Stochastic Benders objective is finite");

    ralph_test_free(model);
}

/* ============================================================================
 * Test 5: Zero Master Constraints Should Error
 * ============================================================================
 */
void test_zero_master_constraints_error(void) {
    printf("\n=== Test: Zero Master Constraints -> Error ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* x is master, y is sub. No pure-master constraints are added. */
    int x = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_INTEGER);
    int y = ralph_test_add_var(model, 0, 1e9, 1.0, RALPH_CONTINUOUS);

    /* Linking and sub-only constraints */
    {
        int idx[] = {y, x};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }
    {
        int idx[] = {y};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }

    int master_vars[] = {x};
    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = 1;
    config.theta_var = -1;
    config.verbose = 1;

    RalphBendersResult result;
    double solution[2] = {-1234.0, -1234.0};

    int ret = ralph_test_solve_benders(model, &config, solution, &result);

    ASSERT(ret == -1, "Zero-master-constraint model returns error");
    ASSERT(result.status == RALPH_STATUS_ERROR, "Result status is ERROR");
    ASSERT(solution[0] == -1234.0 && solution[1] == -1234.0,
           "Solution buffer is untouched on failure");

    ralph_test_free(model);
}

/* ============================================================================
 * Test 5: Config Defaults
 * ============================================================================
 */
void test_config_defaults(void) {
    printf("\n=== Test: Config Defaults ===\n");

    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;

    ASSERT(config.master_var_indices == NULL, "Default master_var_indices is NULL");
    ASSERT(config.num_master_vars == 0, "Default num_master_vars is 0");
    ASSERT(config.theta_var == -1, "Default theta_var is -1 (auto)");
    ASSERT(config.num_scenarios == 1, "Default num_scenarios is 1");
    ASSERT(config.gap_tolerance > 0, "Default gap_tolerance is positive");
    ASSERT(config.max_iterations > 0, "Default max_iterations is positive");
    ASSERT(config.cuts_at_lp_nodes == 1, "Default cuts_at_lp_nodes is 1 (modern)");
    ASSERT(config.warm_start_subproblems == 1, "Default warm_start is 1");
    ASSERT(config.strict_farkas == 0, "Default strict_farkas is 0");
}

/* ============================================================================
 * Test 6: Error Handling
 * ============================================================================
 */
void test_error_handling(void) {
    printf("\n=== Test: Error Handling ===\n");

    /* NULL model */
    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    int master_vars[] = {0};
    config.master_var_indices = master_vars;
    config.num_master_vars = 1;

    int ret = ralph_test_solve_benders(NULL, &config, NULL, NULL);
    ASSERT(ret == -1, "NULL model returns error");

    /* NULL config */
    RalphModel *model = ralph_test_create();
    if (model) {
        ret = ralph_test_solve_benders(model, NULL, NULL, NULL);
        ASSERT(ret == -1, "NULL config returns error");
        ralph_test_free(model);
    }

    /* Empty master vars */
    model = ralph_test_create();
    if (model) {
        ralph_test_add_var(model, 0, 1, 1.0, RALPH_CONTINUOUS);
        config.num_master_vars = 0;
        config.master_var_indices = NULL;
        ret = ralph_test_solve_benders(model, &config, NULL, NULL);
        ASSERT(ret == -1, "Empty master vars returns error");
        ralph_test_free(model);
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("Ralph Benders Decomposition Tests\n");
    printf("==================================\n");

    test_config_defaults();
    test_error_handling();
    test_zero_master_constraints_error();
    test_continuous_master();
    test_simple_two_stage();
    test_infeasible_subproblem();
    test_infeasible_subproblem_strict_farkas();
    test_stochastic_benders();

    printf("\n==================================\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("==================================\n");

    return (tests_failed > 0) ? 1 : 0;
}
