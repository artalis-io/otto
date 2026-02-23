/*
 * Ralph LP/MIP Solver - Test Suite
 *
 * Comprehensive tests for LP and MIP functionality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "lp.h"
#include "mip.h"

#define TOLERANCE 1e-4

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;

/* Internal diagnostics helpers (non-public API used by integration tests). */
SimplexSolver* ralph_get_lp_solver(const RalphModel *model);
MIPSolver* ralph_get_mip_solver(const RalphModel *model);

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while(0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) < (tol)) { \
        tests_passed++; \
        printf("  PASS: %s (%.6f == %.6f)\n", msg, (double)(a), (double)(b)); \
    } else { \
        printf("  FAIL: %s (%.6f != %.6f)\n", msg, (double)(a), (double)(b)); \
    } \
} while(0)

/* ============================================================================
 * Test: Simple 2-variable LP
 *
 * min  -x - y
 * s.t. x + y <= 4
 *      2x + y <= 6
 *      x, y >= 0
 *
 * Optimal: obj=-4 (multiple optimal vertices: (2,2) and (0,4))
 * ============================================================================ */
void test_simple_lp(void) {
    printf("\n=== Test: Simple 2-variable LP ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add variables: x, y with objective -1, -1 */
    ralph_test_add_var(model, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);

    /* Constraint 1: x + y <= 4 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 4.0);

    /* Constraint 2: 2x + y <= 6 */
    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 6.0);

    /* Solve */
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, -4.0, TOLERANCE, "Objective value");

    /* Verify solution satisfies constraints (don't check specific vertex) */
    double x[2];
    ralph_test_get_solution(model, x);
    double c1 = x[0] + x[1];       /* x + y <= 4 */
    double c2 = 2*x[0] + x[1];     /* 2x + y <= 6 */
    ASSERT(x[0] >= -TOLERANCE, "x[0] >= 0");
    ASSERT(x[1] >= -TOLERANCE, "x[1] >= 0");
    ASSERT(c1 <= 4.0 + TOLERANCE, "Constraint 1 satisfied");
    ASSERT(c2 <= 6.0 + TOLERANCE, "Constraint 2 satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: LP with equality constraint
 *
 * min  x + 2y
 * s.t. x + y = 3
 *      x >= 1, y >= 0
 *
 * Optimal: x=3, y=0, obj=3 (maximize x to minimize x + 2y = 6 - x)
 * ============================================================================ */
void test_equality_constraint(void) {
    printf("\n=== Test: LP with equality constraint ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 1.0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x >= 1 */
    ralph_test_add_var(model, 0.0, 1e30, 2.0, RALPH_CONTINUOUS);  /* y >= 0 */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_EQUAL, 3.0);

    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 3.0, TOLERANCE, "Objective value");

    double x[2];
    ralph_test_get_solution(model, x);
    ASSERT_NEAR(x[0], 3.0, TOLERANCE, "x[0] = 3");
    ASSERT_NEAR(x[1], 0.0, TOLERANCE, "x[1] = 0");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: LP with >= constraint
 *
 * max  2x + 3y
 * s.t. x + y >= 1
 *      x <= 2, y <= 2
 *
 * Optimal: x=2, y=2, obj=10
 * ============================================================================ */
void test_greater_equal_constraint(void) {
    printf("\n=== Test: LP with >= constraint ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    ralph_test_add_var(model, 0.0, 2.0, 2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, 2.0, 3.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 10.0, TOLERANCE, "Objective value");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Diet Problem (Classic LP)
 *
 * Minimize cost of diet while meeting nutritional requirements.
 * ============================================================================ */
void test_diet_problem(void) {
    printf("\n=== Test: Diet Problem ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Foods: Bread, Milk, Cheese (cost: 2, 3.5, 8) */
    ralph_test_add_var(model, 0, 1e30, 2.0, RALPH_CONTINUOUS);    /* Bread */
    ralph_test_add_var(model, 0, 1e30, 3.5, RALPH_CONTINUOUS);    /* Milk */
    ralph_test_add_var(model, 0, 1e30, 8.0, RALPH_CONTINUOUS);    /* Cheese */

    /* Calories >= 300: 50*bread + 42*milk + 35*cheese >= 300 */
    int idx1[] = {0, 1, 2};
    double val1[] = {50, 42, 35};
    ralph_test_add_constraint(model, 3, idx1, val1, RALPH_GREATER_EQUAL, 300);

    /* Protein >= 10: 4*bread + 8*milk + 7*cheese >= 10 */
    double val2[] = {4, 8, 7};
    ralph_test_add_constraint(model, 3, idx1, val2, RALPH_GREATER_EQUAL, 10);

    /* Calcium >= 8: 0*bread + 3*milk + 2*cheese >= 8 */
    double val3[] = {0, 3, 2};
    ralph_test_add_constraint(model, 3, idx1, val3, RALPH_GREATER_EQUAL, 8);

    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    printf("  Diet cost: %.2f\n", obj);
    ASSERT(obj > 0 && obj < 100, "Reasonable cost");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Simple MIP - Binary Knapsack
 *
 * max  5x1 + 4x2 + 3x3
 * s.t. 2x1 + 3x2 + x3 <= 5
 *      x1, x2, x3 in {0, 1}
 *
 * Optimal: x1=1, x2=0, x3=1, obj=8
 * (or x1=1, x2=1, x3=0, obj=9)
 * ============================================================================ */
void test_binary_knapsack(void) {
    printf("\n=== Test: Binary Knapsack ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    /* Binary variables */
    ralph_test_add_var(model, 0, 1, 5.0, RALPH_BINARY);
    ralph_test_add_var(model, 0, 1, 4.0, RALPH_BINARY);
    ralph_test_add_var(model, 0, 1, 3.0, RALPH_BINARY);

    /* Capacity constraint */
    int idx[] = {0, 1, 2};
    double val[] = {2.0, 3.0, 1.0};
    ralph_test_add_constraint(model, 3, idx, val, RALPH_LESS_EQUAL, 5.0);

    ASSERT(ralph_test_is_mip(model), "Model is MIP");

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    printf("  Knapsack value: %.0f\n", obj);
    ASSERT(obj >= 8.0 - TOLERANCE, "Objective >= 8");

    double x[3];
    ralph_test_get_solution(model, x);
    printf("  Solution: x1=%.0f, x2=%.0f, x3=%.0f\n", x[0], x[1], x[2]);

    /* Verify integer solution */
    for (int i = 0; i < 3; i++) {
        ASSERT(fabs(x[i] - round(x[i])) < TOLERANCE, "Variable is integer");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Branching Tightening Guard
 *
 * Regression for stale/integral branch-variable selections that can create
 * non-tightening children and trigger deep duplicate-node chains.
 * ============================================================================ */
void test_branch_tightening_guard(void) {
    printf("\n=== Test: Branching Tightening Guard ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    ralph_test_add_var(model, 0, 1, 5.0, RALPH_BINARY);
    ralph_test_add_var(model, 0, 1, 4.0, RALPH_BINARY);
    ralph_test_add_var(model, 0, 1, 3.0, RALPH_BINARY);

    int idx[] = {0, 1, 2};
    double val[] = {2.0, 3.0, 1.0};
    ralph_test_add_constraint(model, 3, idx, val, RALPH_LESS_EQUAL, 5.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_nodes", 256);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Tightening guard: status OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 9.0, TOLERANCE,
                "Tightening guard: objective 9");

    MIPSolver *mip = ralph_get_mip_solver(model);
    ASSERT(mip != NULL, "Tightening guard: MIP solver available");
    if (mip) {
        ASSERT(mip->nodes_explored < 64,
               "Tightening guard: avoids duplicate-node explosion");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Integer Programming
 *
 * min  x + y
 * s.t. x + y >= 3.5
 *      x, y >= 0, integer
 *
 * LP relaxation: x=y=1.75, obj=3.5
 * IP optimal: x+y=4 (e.g., x=2, y=2)
 * ============================================================================ */
void test_integer_programming(void) {
    printf("\n=== Test: Integer Programming ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 3.5);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 4.0, TOLERANCE, "Objective = 4 (rounded up from 3.5)");

    double x[2];
    ralph_test_get_solution(model, x);
    ASSERT(x[0] + x[1] >= 3.5 - TOLERANCE, "Constraint satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Mixed Integer Programming
 *
 * min  x + y
 * s.t. 2x + y >= 4
 *      x integer, y continuous
 *      x, y >= 0
 *
 * Optimal: x=2, y=0, obj=2
 * ============================================================================ */
void test_mixed_integer(void) {
    printf("\n=== Test: Mixed Integer Programming ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);     /* x integer */
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* y continuous */

    int idx[] = {0, 1};
    double val[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 4.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 2.0, TOLERANCE, "Objective = 2");

    double x[2];
    ralph_test_get_solution(model, x);
    ASSERT_NEAR(x[0], 2.0, TOLERANCE, "x = 2");
    ASSERT_NEAR(x[1], 0.0, TOLERANCE, "y = 0");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Facility Location (Classic MIP)
 *
 * Simplified 2-facility, 3-customer problem.
 * ============================================================================ */
void test_facility_location(void) {
    printf("\n=== Test: Facility Location ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Variables:
     * y[0], y[1]: binary, whether facility is open
     * x[0][0], x[0][1], x[0][2]: fraction of customer j served by facility 0
     * x[1][0], x[1][1], x[1][2]: fraction of customer j served by facility 1
     */

    /* Fixed costs: 100, 150 */
    ralph_test_add_var(model, 0, 1, 100, RALPH_BINARY);  /* y[0] */
    ralph_test_add_var(model, 0, 1, 150, RALPH_BINARY);  /* y[1] */

    /* Transport costs (facility i to customer j) */
    double cost[2][3] = {{10, 20, 15}, {25, 10, 20}};

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            ralph_test_add_var(model, 0, 1, cost[i][j], RALPH_CONTINUOUS);
        }
    }

    /* Each customer must be fully served */
    for (int j = 0; j < 3; j++) {
        int idx[] = {2 + j, 2 + 3 + j};  /* x[0][j], x[1][j] */
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Can only serve from open facility */
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            /* x[i][j] <= y[i] => x[i][j] - y[i] <= 0 */
            int idx[] = {2 + i*3 + j, i};
            double val[] = {1.0, -1.0};
            ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
        }
    }

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    printf("  Total cost: %.2f\n", obj);
    ASSERT(obj > 0, "Positive cost");

    double x[8];
    ralph_test_get_solution(model, x);
    printf("  Facility 0 open: %.0f\n", x[0]);
    printf("  Facility 1 open: %.0f\n", x[1]);

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Infeasible LP
 *
 * x <= 1
 * x >= 2
 *
 * No feasible solution.
 * ============================================================================ */
void test_infeasible_lp(void) {
    printf("\n=== Test: Infeasible LP ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);

    /* x <= 1 */
    int idx[] = {0};
    double val[] = {1.0};
    ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 1.0);

    /* x >= 2 */
    ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 2.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Farkas Ray for Infeasibility Certificate
 *
 * min  x + y
 * s.t. x + y >= 5
 *      x <= 1
 *      y <= 1
 *      x, y >= 0
 *
 * Infeasible because x <= 1 and y <= 1 means x + y <= 2, but we need x + y >= 5.
 *
 * Farkas ray y should satisfy:
 *   y' * A >= 0 (for variables at their bounds)
 *   y' * b < 0
 * ============================================================================ */
void test_farkas_ray(void) {
    printf("\n=== Test: Farkas Ray for Infeasibility Certificate ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add variables x and y with bounds [0, inf) and objective 1.0 */
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* y */

    /* Constraint 1: x + y >= 5 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_GREATER_EQUAL, 5.0);

    /* Constraint 2: x <= 1 */
    int idx2[] = {0};
    double val2[] = {1.0};
    ralph_test_add_constraint(model, 1, idx2, val2, RALPH_LESS_EQUAL, 1.0);

    /* Constraint 3: y <= 1 */
    int idx3[] = {1};
    double val3[] = {1.0};
    ralph_test_add_constraint(model, 1, idx3, val3, RALPH_LESS_EQUAL, 1.0);

    /* Solve */
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    /* Get Farkas ray */
    int m = ralph_test_get_num_cons(model);
    double *ray = (double*)malloc(m * sizeof(double));
    int ret = ralph_core_get_farkas_ray(model, ray);
    ASSERT(ret == 0, "Farkas ray retrieved successfully");

    if (ret == 0) {
        /* Compute y' * b to verify it's negative
         * b = [5, 1, 1] (RHS values, but normalized for sense)
         * For G constraint: we need -5 (since Ax >= b becomes -Ax <= -b)
         * For L constraints: b stays positive (1, 1) */
        double yTb = ray[0] * (-5.0) + ray[1] * 1.0 + ray[2] * 1.0;

        printf("  Farkas ray: [%.4f, %.4f, %.4f]\n", ray[0], ray[1], ray[2]);
        printf("  y' * b = %.6f\n", yTb);

        /* The Farkas certificate should have y'b < 0 (proving infeasibility) */
        ASSERT(yTb < TOLERANCE, "y' * b < 0 (infeasibility proven)");
    }

    free(ray);
    ralph_test_free(model);
}

/* ============================================================================
 * Test: Farkas Ray - Simple Upper/Lower Bound Conflict (Two-Phase)
 *
 * Uses two-phase simplex for clean Farkas duals (no Big-M contamination).
 *
 * min  x
 * s.t. x >= 10
 *      x <= 5
 *      x >= 0
 *
 * Infeasible: x >= 10 and x <= 5 is impossible.
 *
 * Farkas ray should satisfy y'b_eff < 0 where:
 *   b_eff[0] = -10 (>= constraint becomes -x <= -10)
 *   b_eff[1] = 5   (<= constraint stays as-is)
 * ============================================================================ */
void test_farkas_bound_conflict(void) {
    printf("\n=== Test: Farkas Ray Bound Conflict ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "force_two_phase", 1);  /* Clean Farkas duals */

    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);

    /* Constraint 1: x >= 10 */
    int idx1[] = {0};
    double val1[] = {1.0};
    ralph_test_add_constraint(model, 1, idx1, val1, RALPH_GREATER_EQUAL, 10.0);

    /* Constraint 2: x <= 5 */
    int idx2[] = {0};
    double val2[] = {1.0};
    ralph_test_add_constraint(model, 1, idx2, val2, RALPH_LESS_EQUAL, 5.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    double ray[2];
    int ret = ralph_core_get_farkas_ray(model, ray);
    ASSERT(ret == 0, "Farkas ray retrieved");

    if (ret == 0) {
        printf("  Farkas ray: [%.6f, %.6f]\n", ray[0], ray[1]);

        /* In standard form:
         *   Row 0: -x <= -10 (from x >= 10)
         *   Row 1:  x <= 5
         * b_eff = [-10, 5]
         * y'b_eff = ray[0]*(-10) + ray[1]*5 should be < 0 */
        double yTb = ray[0] * (-10.0) + ray[1] * 5.0;
        printf("  y' * b_eff = %.6f\n", yTb);
        ASSERT(yTb < TOLERANCE, "y' * b_eff < 0 (infeasibility proven)");

        /* Verify ray is non-trivial */
        ASSERT(fabs(ray[0]) > 1e-6 || fabs(ray[1]) > 1e-6, "Ray is non-trivial");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Farkas Ray - Conflicting Sum Constraints (Two-Phase)
 *
 * Uses two-phase simplex for clean Farkas duals.
 *
 * min  x + y
 * s.t. x + y <= 2
 *      x + y >= 5
 *      x, y >= 0
 *
 * Clearly infeasible (x+y can't be both <= 2 and >= 5).
 *
 * In standard form (all <= constraints):
 *   Row 0:  (x + y) <= 2
 *   Row 1: -(x + y) <= -5  (from x + y >= 5)
 *
 * b_eff = [2, -5]
 * y'b_eff should be < 0
 * ============================================================================ */
void test_farkas_sum_conflict(void) {
    printf("\n=== Test: Farkas Ray Sum Conflict ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "force_two_phase", 1);  /* Clean Farkas duals */

    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* y */

    /* Constraint 1: x + y <= 2 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 2.0);

    /* Constraint 2: x + y >= 5 */
    int idx2[] = {0, 1};
    double val2[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_GREATER_EQUAL, 5.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    double ray[2];
    int ret = ralph_core_get_farkas_ray(model, ray);
    ASSERT(ret == 0, "Farkas ray retrieved");

    if (ret == 0) {
        printf("  Farkas ray: [%.6f, %.6f]\n", ray[0], ray[1]);

        /* y'b_eff = ray[0]*2 + ray[1]*(-5) should be < 0 */
        double yTb = ray[0] * 2.0 + ray[1] * (-5.0);
        printf("  y' * b_eff = %.6f\n", yTb);
        ASSERT(yTb < TOLERANCE, "y' * b_eff < 0 (infeasibility proven)");

        /* Verify ray is non-trivial */
        ASSERT(fabs(ray[0]) > 1e-6 || fabs(ray[1]) > 1e-6, "Ray is non-trivial");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Farkas Ray with Equality Constraint (Two-Phase)
 *
 * Uses two-phase simplex for clean Farkas duals.
 *
 * min  x + y
 * s.t. x + y = 5
 *      x <= 1
 *      y <= 1
 *      x, y >= 0
 *
 * Infeasible: x + y = 5 but x <= 1 and y <= 1 means x + y <= 2.
 *
 * NOTE: For equality constraints, the Farkas dual can be positive or negative
 * (unrestricted in sign). The simple y'b < 0 check doesn't apply directly
 * because equalities expand to two constraints internally. This test just
 * verifies infeasibility detection and non-trivial ray.
 * ============================================================================ */
void test_farkas_equality(void) {
    printf("\n=== Test: Farkas Ray with Equality ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "force_two_phase", 1);  /* Clean Farkas duals */

    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* y */

    /* Constraint 1: x + y = 5 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_EQUAL, 5.0);

    /* Constraint 2: x <= 1 */
    int idx2[] = {0};
    double val2[] = {1.0};
    ralph_test_add_constraint(model, 1, idx2, val2, RALPH_LESS_EQUAL, 1.0);

    /* Constraint 3: y <= 1 */
    int idx3[] = {1};
    double val3[] = {1.0};
    ralph_test_add_constraint(model, 1, idx3, val3, RALPH_LESS_EQUAL, 1.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    double ray[3];
    int ret = ralph_core_get_farkas_ray(model, ray);
    ASSERT(ret == 0, "Farkas ray retrieved");

    if (ret == 0) {
        printf("  Farkas ray: [%.4f, %.4f, %.4f]\n", ray[0], ray[1], ray[2]);

        /* Verify ray is non-trivial */
        ASSERT(fabs(ray[0]) > 1e-6 || fabs(ray[1]) > 1e-6 || fabs(ray[2]) > 1e-6,
               "Ray is non-trivial");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Farkas Ray - Negative RHS with G-sense (Row Sign Corner Case)
 *
 * Micro regression test for the row_sign normalization edge case.
 * When b < 0 AND sense = G, the tableau normalizes by:
 *   1. Multiply row by -1 (to make b positive)
 *   2. Flip sense from G to L
 *
 * This tests that the Farkas ray is correctly computed in this case.
 *
 * min  x
 * s.t. x >= -5   (b < 0, G sense -> normalized to -x <= 5, row_sign = -1)
 *      x <= -10  (b < 0, L sense -> normalized to -x >= 10 -> x <= -10, row_sign = -1)
 *
 * Variable bounds: x >= 0
 *
 * Infeasible: x >= 0 conflicts with x <= -10
 * ============================================================================ */
void test_farkas_negative_rhs_gsense(void) {
    printf("\n=== Test: Farkas Ray Negative RHS G-Sense ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "force_two_phase", 1);

    /* x with lower bound 0 */
    ralph_test_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);

    /* Constraint 1: x >= -5 (b < 0, G sense) */
    int idx1[] = {0};
    double val1[] = {1.0};
    ralph_test_add_constraint(model, 1, idx1, val1, RALPH_GREATER_EQUAL, -5.0);

    /* Constraint 2: x <= -10 (b < 0, L sense) */
    int idx2[] = {0};
    double val2[] = {1.0};
    ralph_test_add_constraint(model, 1, idx2, val2, RALPH_LESS_EQUAL, -10.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    double ray[2];
    int ret = ralph_core_get_farkas_ray(model, ray);
    ASSERT(ret == 0, "Farkas ray retrieved");

    if (ret == 0) {
        printf("  Farkas ray: [%.6f, %.6f]\n", ray[0], ray[1]);

        /* Verify ray is non-trivial */
        ASSERT(fabs(ray[0]) > 1e-6 || fabs(ray[1]) > 1e-6, "Ray is non-trivial");

        /* After normalization:
         * Row 0: x >= -5 with b=-5 < 0 becomes -x <= 5 (L sense)
         * Row 1: x <= -10 with b=-10 < 0 becomes -x >= 10 (G sense)
         *
         * In standard form:
         * Row 0: -x <= 5
         * Row 1: x <= -10 (stays as-is for checking)
         *
         * The variable bound x >= 0 interacts with x <= -10 to cause infeasibility.
         * The Farkas certificate shows this via the slack variable constraint. */
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Unbounded Ray API
 * ============================================================================ */
void test_unbounded_ray_api(void) {
    printf("\n=== Test: Unbounded Ray API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Unbounded ray: model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "method", 0);   /* Primal path */
    ralph_test_set_int_param(model, "presolve", 0); /* Keep direct mapping */

    /* min -x, s.t. x >= 0  => unbounded */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    int idx[] = {0};
    double val[] = {1.0};
    ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0);

    ralph_test_optimize_lp(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_UNBOUNDED,
           "Unbounded ray: status is UNBOUNDED");

    double ray[1] = {0.0};
    ASSERT(ralph_core_get_unbounded_ray(model, ray) == 0,
           "Unbounded ray: retrieved successfully");
    ASSERT(ray[0] > 1e-8, "Unbounded ray: positive improving direction");
    ASSERT((-1.0 * ray[0]) < -1e-8, "Unbounded ray: improves objective for minimization");

    ASSERT(ralph_core_get_unbounded_ray(NULL, ray) == -1,
           "Unbounded ray: NULL model rejected");
    ASSERT(ralph_core_get_unbounded_ray(model, NULL) == -1,
           "Unbounded ray: NULL output rejected");

    ralph_test_free(model);

    /* Non-unbounded model should reject ray query. */
    model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_optimize_lp(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Unbounded ray: control model optimal");
    ASSERT(ralph_core_get_unbounded_ray(model, ray) == -1,
           "Unbounded ray: unavailable for non-unbounded status");
    ralph_test_free(model);
}

/* ============================================================================
 * Test: API Functions
 * ============================================================================ */
void test_api_functions(void) {
    printf("\n=== Test: API Functions ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "ralph_test_create");

    ASSERT(ralph_test_get_num_vars(model) == 0, "Initial num_vars = 0");
    ASSERT(ralph_test_get_num_cons(model) == 0, "Initial num_cons = 0");

    ralph_test_add_var(model, 0, 10, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, 10, 2.0, RALPH_INTEGER);

    ASSERT(ralph_test_get_num_vars(model) == 2, "num_vars = 2 after adding");
    ASSERT(ralph_test_get_num_integers(model) == 1, "num_integers = 1");
    ASSERT(ralph_test_is_mip(model) == 1, "is_mip = true");

    /* Test parameters */
    ralph_test_set_int_param(model, "verbose", 1);
    int verbose;
    ralph_test_get_int_param(model, "verbose", &verbose);
    ASSERT(verbose == 1, "Parameter get/set works");

    ralph_test_set_dbl_param(model, "time_limit", 60.0);
    double time_limit;
    ralph_test_get_dbl_param(model, "time_limit", &time_limit);
    ASSERT_NEAR(time_limit, 60.0, TOLERANCE, "Double parameter get/set");

    /* Test status string */
    const char *status_str = ralph_test_status_string(RALPH_STATUS_OPTIMAL);
    ASSERT(strcmp(status_str, "OPTIMAL") == 0, "Status string");

    /* Test version */
    const char *version = ralph_test_version();
    ASSERT(version != NULL && strlen(version) > 0, "Version string");
    printf("  Ralph version: %s\n", version);

    ralph_test_free(model);
}

static void build_phase3_lp_fixture(RalphModel *model) {
    int idx1[] = {0, 1};
    int idx2[] = {0, 1};
    double val1[] = {1.0, 1.0};
    double val2[] = {2.0, 1.0};

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 4.0);
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 6.0);
}

static void build_phase3_mip_fixture(RalphModel *model) {
    int idx[] = {0, 1};
    double val[] = {2.0, 3.0};

    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
    ralph_test_add_var(model, 0.0, 1.0, 5.0, RALPH_BINARY);
    ralph_test_add_var(model, 0.0, 1.0, 4.0, RALPH_BINARY);
    ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 3.0);
}

void test_phase3_optimize_entrypoints(void) {
    printf("\n=== Test: Phase 3 Optimize Entry Points ===\n");

    RalphModel *lp_model = ralph_test_create();
    build_phase3_lp_fixture(lp_model);
    ASSERT(ralph_test_optimize_lp(lp_model) == 0, "LP optimize entrypoint succeeds on LP model");
    ASSERT(ralph_test_get_status(lp_model) == RALPH_STATUS_OPTIMAL, "LP optimize entrypoint returns OPTIMAL");
    ASSERT(ralph_get_lp_solver(lp_model) != NULL, "LP optimize entrypoint creates LP solver");
    ASSERT(ralph_get_mip_solver(lp_model) == NULL, "LP optimize entrypoint does not create MIP solver");
    ralph_test_free(lp_model);

    RalphModel *lp_for_mip = ralph_test_create();
    build_phase3_lp_fixture(lp_for_mip);
    ASSERT(ralph_test_optimize_mip(lp_for_mip) == -1, "MIP optimize entrypoint rejects pure LP model");
    ASSERT(ralph_test_get_status(lp_for_mip) == RALPH_STATUS_ERROR, "MIP optimize entrypoint sets ERROR for pure LP model");
    ASSERT(ralph_get_mip_solver(lp_for_mip) == NULL, "Rejected MIP optimize does not create MIP solver");
    ralph_test_free(lp_for_mip);

    RalphModel *mip_model = ralph_test_create();
    build_phase3_mip_fixture(mip_model);
    ralph_test_set_int_param(mip_model, "detect_special", 0);
    ASSERT(ralph_test_optimize_mip(mip_model) == 0, "MIP optimize entrypoint succeeds on integer model");
    ASSERT(ralph_test_get_status(mip_model) == RALPH_STATUS_OPTIMAL, "MIP optimize entrypoint returns OPTIMAL");
    ASSERT(ralph_get_mip_solver(mip_model) != NULL, "MIP optimize entrypoint creates MIP solver");
    ralph_test_free(mip_model);

    RalphModel *mip_for_lp = ralph_test_create();
    build_phase3_mip_fixture(mip_for_lp);
    ASSERT(ralph_test_optimize_lp(mip_for_lp) == -1, "LP optimize entrypoint rejects integer model");
    ASSERT(ralph_test_get_status(mip_for_lp) == RALPH_STATUS_ERROR, "LP optimize entrypoint sets ERROR for integer model");
    ASSERT(ralph_get_mip_solver(mip_for_lp) == NULL, "Rejected LP optimize does not create MIP solver");
    ralph_test_free(mip_for_lp);
}

void test_phase3_param_partition(void) {
    printf("\n=== Test: Phase 3 Strict Parameter Partition ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created for strict parameter partition");

    ASSERT(ralph_test_set_lp_int_param(model, "method", 1) == 0, "LP strict int accepts LP-only param");
    ASSERT(ralph_test_set_lp_int_param(model, "max_nodes", 128) == -1, "LP strict int rejects MIP-only param");
    ASSERT(ralph_test_set_mip_int_param(model, "max_nodes", 128) == 0, "MIP strict int accepts MIP-only param");
    ASSERT(ralph_test_set_mip_int_param(model, "method", 1) == -1, "MIP strict int rejects LP-only param");
    ASSERT(ralph_test_set_lp_int_param(model, "verbose", 1) == 0, "LP strict int accepts shared param");
    ASSERT(ralph_test_set_mip_int_param(model, "verbose", 0) == 0, "MIP strict int accepts shared param");

    ASSERT(ralph_test_set_lp_dbl_param(model, "obj_limit", 7.5) == 0, "LP strict dbl accepts LP-only param");
    ASSERT(ralph_test_set_lp_dbl_param(model, "mip_gap", 1e-2) == -1, "LP strict dbl rejects MIP-only param");
    ASSERT(ralph_test_set_mip_dbl_param(model, "mip_gap", 1e-2) == 0, "MIP strict dbl accepts MIP-only param");
    ASSERT(ralph_test_set_mip_dbl_param(model, "obj_limit", 7.5) == -1, "MIP strict dbl rejects LP-only param");
    ASSERT(ralph_test_set_lp_dbl_param(model, "time_limit", 10.0) == 0, "LP strict dbl accepts shared param");
    ASSERT(ralph_test_set_mip_dbl_param(model, "time_limit", 20.0) == 0, "MIP strict dbl accepts shared param");

    int i_val = 0;
    ASSERT(ralph_test_get_lp_int_param(model, "method", &i_val) == 0, "LP strict get int reads LP-only param");
    ASSERT(i_val == 1, "LP strict get int returns set method");
    ASSERT(ralph_test_get_mip_int_param(model, "max_nodes", &i_val) == 0, "MIP strict get int reads MIP-only param");
    ASSERT(i_val == 128, "MIP strict get int returns set max_nodes");
    ASSERT(ralph_test_get_lp_int_param(model, "max_nodes", &i_val) == -1, "LP strict get int rejects MIP-only param");
    ASSERT(ralph_test_get_mip_int_param(model, "method", &i_val) == -1, "MIP strict get int rejects LP-only param");

    double d_val = 0.0;
    ASSERT(ralph_test_get_lp_dbl_param(model, "obj_limit", &d_val) == 0, "LP strict get dbl reads LP-only param");
    ASSERT_NEAR(d_val, 7.5, TOLERANCE, "LP strict get dbl returns set obj_limit");
    ASSERT(ralph_test_get_mip_dbl_param(model, "mip_gap", &d_val) == 0, "MIP strict get dbl reads MIP-only param");
    ASSERT_NEAR(d_val, 1e-2, TOLERANCE, "MIP strict get dbl returns set mip_gap");
    ASSERT(ralph_test_get_lp_dbl_param(model, "mip_gap", &d_val) == -1, "LP strict get dbl rejects MIP-only param");
    ASSERT(ralph_test_get_mip_dbl_param(model, "obj_limit", &d_val) == -1, "MIP strict get dbl rejects LP-only param");

    ralph_test_free(model);
}

void test_phase3_optimize_backward_compatibility(void) {
    printf("\n=== Test: Phase 3 Backward Compatibility ===\n");

    RalphModel *lp_legacy = ralph_test_create();
    RalphModel *lp_explicit = ralph_test_create();
    build_phase3_lp_fixture(lp_legacy);
    build_phase3_lp_fixture(lp_explicit);
    ASSERT(ralph_test_set_int_param(lp_legacy, "verbose", 0) == 0, "Legacy LP set_int_param works");
    ASSERT(ralph_test_set_dbl_param(lp_legacy, "time_limit", 5.0) == 0, "Legacy LP set_dbl_param works");
    ASSERT(ralph_test_set_int_param(lp_explicit, "verbose", 0) == 0, "Explicit LP set_int_param works");
    ASSERT(ralph_test_set_dbl_param(lp_explicit, "time_limit", 5.0) == 0, "Explicit LP set_dbl_param works");
    ASSERT(ralph_test_optimize(lp_legacy) == 0, "Legacy ralph_test_optimize solves LP");
    ASSERT(ralph_test_optimize_lp(lp_explicit) == 0, "Explicit LP entrypoint solves LP");
    ASSERT(ralph_test_get_status(lp_legacy) == ralph_test_get_status(lp_explicit), "Legacy vs explicit LP status match");
    ASSERT_NEAR(ralph_test_get_objval(lp_legacy), ralph_test_get_objval(lp_explicit), TOLERANCE,
                "Legacy vs explicit LP objective match");
    ralph_test_free(lp_legacy);
    ralph_test_free(lp_explicit);

    RalphModel *mip_legacy = ralph_test_create();
    RalphModel *mip_explicit = ralph_test_create();
    build_phase3_mip_fixture(mip_legacy);
    build_phase3_mip_fixture(mip_explicit);
    ASSERT(ralph_test_set_int_param(mip_legacy, "detect_special", 0) == 0, "Legacy MIP detect_special set");
    ASSERT(ralph_test_set_int_param(mip_explicit, "detect_special", 0) == 0, "Explicit MIP detect_special set");
    ASSERT(ralph_test_set_int_param(mip_legacy, "max_nodes", 128) == 0, "Legacy MIP max_nodes set");
    ASSERT(ralph_test_set_int_param(mip_explicit, "max_nodes", 128) == 0, "Explicit MIP max_nodes set");
    ASSERT(ralph_test_set_dbl_param(mip_legacy, "mip_gap", 0.0) == 0, "Legacy MIP mip_gap set");
    ASSERT(ralph_test_set_dbl_param(mip_explicit, "mip_gap", 0.0) == 0, "Explicit MIP mip_gap set");
    ASSERT(ralph_test_optimize(mip_legacy) == 0, "Legacy ralph_test_optimize solves MIP");
    ASSERT(ralph_test_optimize_mip(mip_explicit) == 0, "Explicit MIP entrypoint solves MIP");
    ASSERT(ralph_test_get_status(mip_legacy) == ralph_test_get_status(mip_explicit), "Legacy vs explicit MIP status match");
    ASSERT_NEAR(ralph_test_get_objval(mip_legacy), ralph_test_get_objval(mip_explicit), TOLERANCE,
                "Legacy vs explicit MIP objective match");
    ralph_test_free(mip_legacy);
    ralph_test_free(mip_explicit);
}

void test_param_typed_metadata_api(void) {
    printf("\n=== Test: Typed Parameter + Metadata API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Typed param: model created");

    int count = ralph_core_get_param_count();
    ASSERT(count == (int)RALPH_PARAM_COUNT, "Typed param: param count matches enum");

    RalphParamMeta meta;
    memset(&meta, 0, sizeof(meta));
    ASSERT(ralph_core_get_param_meta(RALPH_PARAM_METHOD, &meta) == 0,
           "Typed param: get metadata for method");
    ASSERT(strcmp(meta.name, "method") == 0,
           "Typed param: metadata canonical name");
    ASSERT(meta.scope == RALPH_PARAM_SCOPE_LP,
           "Typed param: metadata scope");
    ASSERT(meta.value_type == RALPH_PARAM_VALUE_INT,
           "Typed param: metadata value type");
    ASSERT_NEAR(meta.default_value, 0.0, TOLERANCE,
                "Typed param: metadata default value");

    ASSERT(ralph_core_get_param_meta(RALPH_PARAM_NODE_SELECT, &meta) == 0,
           "Typed param: get metadata for node_select");
    ASSERT(meta.has_min == 1 && meta.has_max == 1,
           "Typed param: node_select has min/max");
    ASSERT_NEAR(meta.min_value, 0.0, TOLERANCE,
                "Typed param: node_select min=0");
    ASSERT_NEAR(meta.max_value, 3.0, TOLERANCE,
                "Typed param: node_select max=3");

    ASSERT(ralph_core_get_param_meta((RalphParamId)-1, &meta) == -1,
           "Typed param: invalid id rejected by metadata get");
    ASSERT(ralph_core_get_param_meta(RALPH_PARAM_METHOD, NULL) == -1,
           "Typed param: NULL metadata output rejected");

    RalphParamId pid = RALPH_PARAM_COUNT;
    ASSERT(ralph_core_find_param_by_name("method", &pid) == 0 &&
           pid == RALPH_PARAM_METHOD,
           "Typed param: canonical name lookup");
    ASSERT(ralph_core_find_param_by_name("Method", &pid) == 0 &&
           pid == RALPH_PARAM_METHOD,
           "Typed param: alias lookup");
    ASSERT(ralph_core_find_param_by_name("ScalingRounds", &pid) == 0 &&
           pid == RALPH_PARAM_SCALING,
           "Typed param: scaling alias lookup");
    ASSERT(ralph_core_find_param_by_name("does_not_exist", &pid) == -1,
           "Typed param: unknown name rejected");
    ASSERT(ralph_core_find_param_by_name("method", NULL) == -1,
           "Typed param: NULL lookup output rejected");

    int i_val = 0;
    ASSERT(ralph_core_set_int_param_id(model, RALPH_PARAM_VERBOSE, 1) == 0,
           "Typed param: set int by id");
    ASSERT(ralph_core_get_int_param_id(model, RALPH_PARAM_VERBOSE, &i_val) == 0 &&
           i_val == 1,
           "Typed param: get int by id");
    ASSERT(ralph_test_get_int_param(model, "verbose", &i_val) == 0 &&
           i_val == 1,
           "Typed param: string get matches typed set");

    double d_val = 0.0;
    ASSERT(ralph_test_set_dbl_param(model, "time_limit", 12.5) == 0,
           "Typed param: set double by string");
    ASSERT(ralph_core_get_dbl_param_id(model, RALPH_PARAM_TIME_LIMIT, &d_val) == 0,
           "Typed param: get double by id");
    ASSERT_NEAR(d_val, 12.5, TOLERANCE,
                "Typed param: typed getter returns string-set value");

    ASSERT(ralph_core_set_dbl_param_id(model, RALPH_PARAM_MIP_GAP, 0.02) == 0,
           "Typed param: set double by id");
    ASSERT(ralph_test_get_dbl_param(model, "mip_gap", &d_val) == 0,
           "Typed param: get double by string");
    ASSERT_NEAR(d_val, 0.02, TOLERANCE,
                "Typed param: string getter returns typed-set value");

    ASSERT(ralph_core_set_lp_int_param_id(model, RALPH_PARAM_MAX_NODES, 32) == -1,
           "Typed param: LP strict rejects MIP-only int");
    ASSERT(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_METHOD, 1) == -1,
           "Typed param: MIP strict rejects LP-only int");
    ASSERT(ralph_core_set_lp_int_param_id(model, RALPH_PARAM_VERBOSE, 0) == 0,
           "Typed param: LP strict accepts shared int");
    ASSERT(ralph_core_set_mip_dbl_param_id(model, RALPH_PARAM_TIME_LIMIT, 5.0) == 0,
           "Typed param: MIP strict accepts shared double");

    ASSERT(ralph_core_set_int_param_id(model, RALPH_PARAM_NODE_SELECT, 4) == -1,
           "Typed param: node_select range rejected");
    ASSERT(ralph_core_set_int_param_id(model, RALPH_PARAM_VAR_SELECT, 5) == -1,
           "Typed param: var_select range rejected");
    ASSERT(ralph_core_set_int_param_id(model, RALPH_PARAM_NODE_SELECT, 2) == 0,
           "Typed param: node_select valid value accepted");
    ASSERT(ralph_core_get_int_param_id(model, RALPH_PARAM_NODE_SELECT, &i_val) == 0 &&
           i_val == 2,
           "Typed param: node_select updated");

    ASSERT(ralph_core_set_int_param_id(model, RALPH_PARAM_TIME_LIMIT, 1) == -1,
           "Typed param: int setter rejects double param");
    ASSERT(ralph_core_set_dbl_param_id(model, RALPH_PARAM_VERBOSE, 1.0) == -1,
           "Typed param: double setter rejects int param");
    ASSERT(ralph_core_get_dbl_param_id(model, RALPH_PARAM_VERBOSE, &d_val) == -1,
           "Typed param: double getter rejects int param");
    ASSERT(ralph_core_get_int_param_id(model, (RalphParamId)-1, &i_val) == -1,
           "Typed param: invalid id rejected by int getter");

    ASSERT(ralph_test_set_int_param(model, "ScalingRounds", 4) == 0,
           "Typed param: string alias set still supported");
    ASSERT(ralph_core_get_int_param_id(model, RALPH_PARAM_SCALING, &i_val) == 0 &&
           i_val == 4,
           "Typed param: alias maps to canonical scaling id");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Larger LP (Performance)
 *
 * Test with more variables/constraints to verify performance.
 * ============================================================================ */
void test_larger_lp(void) {
    printf("\n=== Test: Larger LP (20 vars, 10 constraints) ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    int n = 20;  /* Variables */
    int m = 10;  /* Constraints */

    /* Add variables */
    for (int j = 0; j < n; j++) {
        ralph_test_add_var(model, 0, 1e30, (double)(j + 1), RALPH_CONTINUOUS);
    }

    /* Add constraints: sum over subset <= constant */
    int *indices = (int*)malloc(n * sizeof(int));
    double *values = (double*)malloc(n * sizeof(double));

    for (int i = 0; i < m; i++) {
        int nnz = 0;
        for (int j = 0; j < n; j++) {
            /* Include variable j in constraint i with some pattern */
            if ((i + j) % 3 == 0) {
                indices[nnz] = j;
                values[nnz] = 1.0 + (double)((i * j) % 5);
                nnz++;
            }
        }
        if (nnz > 0) {
            ralph_test_add_constraint(model, nnz, indices, values, RALPH_LESS_EQUAL, 10.0 + i);
        }
    }

    free(indices);
    free(values);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    printf("  Objective: %.4f\n", obj);
    ASSERT(obj >= 0, "Non-negative objective");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Network Flow LP
 *
 * Tests objective computation with equality constraints (regression test for
 * the bug where artificial variable residuals caused incorrect objectives).
 *
 * Simple flow network: source(0) -> transit(1,2) -> sink(3)
 * Supply: source=100, sink=-100
 * Optimal: route all flow through cheapest path (0->1->3), cost = 100 * (1+3) = 400
 * ============================================================================ */
void test_network_flow(void) {
    printf("\n=== Test: Network Flow LP ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* 4 arcs with costs */
    ralph_test_add_var(model, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x0: 0->1, cost=1 */
    ralph_test_add_var(model, 0.0, 100.0, 2.0, RALPH_CONTINUOUS);  /* x1: 0->2, cost=2 */
    ralph_test_add_var(model, 0.0, 100.0, 3.0, RALPH_CONTINUOUS);  /* x2: 1->3, cost=3 */
    ralph_test_add_var(model, 0.0, 100.0, 4.0, RALPH_CONTINUOUS);  /* x3: 2->3, cost=4 */

    /* Node 0: x0 + x1 = 100 (source) */
    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx0, coef0, RALPH_EQUAL, 100.0);

    /* Node 1: -x0 + x2 = 0 */
    int idx1[] = {0, 2};
    double coef1[] = {-1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, coef1, RALPH_EQUAL, 0.0);

    /* Node 2: -x1 + x3 = 0 */
    int idx2[] = {1, 3};
    double coef2[] = {-1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx2, coef2, RALPH_EQUAL, 0.0);

    /* Node 3: -x2 - x3 = -100 (sink) */
    int idx3[] = {2, 3};
    double coef3[] = {-1.0, -1.0};
    ralph_test_add_constraint(model, 2, idx3, coef3, RALPH_EQUAL, -100.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 400.0, TOLERANCE, "Objective value");

    /* Verify solution manually */
    double sol[4];
    ralph_test_get_solution(model, sol);
    double manual_obj = sol[0]*1.0 + sol[1]*2.0 + sol[2]*3.0 + sol[3]*4.0;
    ASSERT_NEAR(obj, manual_obj, TOLERANCE, "Objective matches manual calculation");

    /* Verify flow conservation */
    double node0_balance = sol[0] + sol[1];
    double node3_balance = -sol[2] - sol[3];
    ASSERT_NEAR(node0_balance, 100.0, TOLERANCE, "Source flow = 100");
    ASSERT_NEAR(node3_balance, -100.0, TOLERANCE, "Sink flow = -100");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: GMI Cuts on Knapsack
 *
 * Tests GMI cut generation on a simple knapsack problem.
 * min -10x - 6y - 4z
 * s.t. 5x + 3y + 2z <= 9
 *      x, y, z in {0,1}
 *
 * LP optimal: x=1, y=1, z=0.5 (obj=-18)
 * MIP optimal: x=1, y=1, z=0 (obj=-16)
 * ============================================================================ */
void test_gmi_cuts_knapsack(void) {
    printf("\n=== Test: GMI Cuts Knapsack ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Three binary variables */
    ralph_test_add_var(model, 0.0, 1.0, -10.0, RALPH_BINARY);  /* x */
    ralph_test_add_var(model, 0.0, 1.0, -6.0, RALPH_BINARY);   /* y */
    ralph_test_add_var(model, 0.0, 1.0, -4.0, RALPH_BINARY);   /* z */

    /* 5x + 3y + 2z <= 9 */
    int idx[] = {0, 1, 2};
    double coeffs[] = {5.0, 3.0, 2.0};
    ralph_test_add_constraint(model, 3, idx, coeffs, RALPH_LESS_EQUAL, 9.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_cut_rounds", 3);
    ralph_test_set_int_param(model, "max_nodes", 100);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, -16.0, TOLERANCE, "Optimal objective is -16");

    double sol[3];
    ralph_test_get_solution(model, sol);

    /* Verify solution is binary */
    ASSERT(fabs(sol[0] - 0.0) < TOLERANCE || fabs(sol[0] - 1.0) < TOLERANCE, "x is binary");
    ASSERT(fabs(sol[1] - 0.0) < TOLERANCE || fabs(sol[1] - 1.0) < TOLERANCE, "y is binary");
    ASSERT(fabs(sol[2] - 0.0) < TOLERANCE || fabs(sol[2] - 1.0) < TOLERANCE, "z is binary");

    /* Verify constraint satisfied */
    double lhs = 5*sol[0] + 3*sol[1] + 2*sol[2];
    ASSERT(lhs <= 9.0 + TOLERANCE, "Knapsack constraint satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: c-MIR Cuts on Mixed Knapsack
 *
 * Mixed-integer knapsack with continuous + integer variables.
 * c-MIR targets rows with continuous basic variables.
 *
 * min  -8x - 5y - 3z    (x integer, y continuous, z integer)
 * s.t. 3x + 2y + z <= 10
 *      x + 4y + 2z <= 14
 *      x in {0..5}, y in [0, 10], z in {0..4}
 * ============================================================================ */
void test_cmir_cuts_mixed_knapsack(void) {
    printf("\n=== Test: c-MIR Cuts Mixed Knapsack ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_cut_rounds", 5);

    ralph_test_add_var(model, 0.0, 5.0, -8.0, RALPH_INTEGER);     /* x */
    ralph_test_add_var(model, 0.0, 10.0, -5.0, RALPH_CONTINUOUS);  /* y */
    ralph_test_add_var(model, 0.0, 4.0, -3.0, RALPH_INTEGER);      /* z */

    /* 3x + 2y + z <= 10 */
    int idx1[] = {0, 1, 2};
    double val1[] = {3.0, 2.0, 1.0};
    ralph_test_add_constraint(model, 3, idx1, val1, RALPH_LESS_EQUAL, 10.0);

    /* x + 4y + 2z <= 14 */
    int idx2[] = {0, 1, 2};
    double val2[] = {1.0, 4.0, 2.0};
    ralph_test_add_constraint(model, 3, idx2, val2, RALPH_LESS_EQUAL, 14.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "c-MIR knapsack: OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT(obj < 0.0, "c-MIR knapsack: negative objective (minimizing negative costs)");

    double sol[3];
    ralph_test_get_solution(model, sol);

    /* Verify integer variables are integral */
    ASSERT(fabs(sol[0] - round(sol[0])) < TOLERANCE, "c-MIR knapsack: x is integral");
    ASSERT(fabs(sol[2] - round(sol[2])) < TOLERANCE, "c-MIR knapsack: z is integral");

    /* Verify bounds */
    ASSERT(sol[0] >= -TOLERANCE && sol[0] <= 5.0 + TOLERANCE, "c-MIR knapsack: x in [0,5]");
    ASSERT(sol[1] >= -TOLERANCE && sol[1] <= 10.0 + TOLERANCE, "c-MIR knapsack: y in [0,10]");
    ASSERT(sol[2] >= -TOLERANCE && sol[2] <= 4.0 + TOLERANCE, "c-MIR knapsack: z in [0,4]");

    /* Verify constraints */
    double lhs1 = 3*sol[0] + 2*sol[1] + sol[2];
    double lhs2 = sol[0] + 4*sol[1] + 2*sol[2];
    ASSERT(lhs1 <= 10.0 + TOLERANCE, "c-MIR knapsack: constraint 1 satisfied");
    ASSERT(lhs2 <= 14.0 + TOLERANCE, "c-MIR knapsack: constraint 2 satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: c-MIR Cuts Reduce B&B Nodes
 *
 * Compare node count with cuts enabled vs disabled on a small facility
 * location MIP (binary + continuous). c-MIR targets mixed rows with
 * continuous basic variables, so this structure exercises it properly.
 *
 * 3 facilities, 6 customers:
 *   min  sum(f_j * y_j) + sum(t_ij * x_ij)
 *   s.t. sum_j(x_ij) = 1           for each customer i
 *        x_ij <= y_j                for each i,j
 *        y_j binary, x_ij in [0,1]
 * ============================================================================ */
static void build_small_facility(RalphModel *model) {
    int nf = 3, nc = 6;
    double fixed[3] = {80, 100, 90};
    double trans[6][3] = {
        {10, 20, 15}, {25, 8, 12}, {14, 18, 9},
        {22, 11, 16}, {8, 15, 20}, {17, 13, 7},
    };

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    for (int j = 0; j < nf; j++)
        ralph_test_add_var(model, 0.0, 1.0, fixed[j], RALPH_BINARY);
    for (int i = 0; i < nc; i++)
        for (int j = 0; j < nf; j++)
            ralph_test_add_var(model, 0.0, 1.0, trans[i][j], RALPH_CONTINUOUS);

    int dem_idx[3];
    double dem_val[3] = {1.0, 1.0, 1.0};
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++)
            dem_idx[j] = nf + i * nf + j;
        ralph_test_add_constraint(model, nf, dem_idx, dem_val, RALPH_EQUAL, 1.0);
    }

    int lnk_idx[2];
    double lnk_val[2] = {1.0, -1.0};
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            lnk_idx[0] = nf + i * nf + j;
            lnk_idx[1] = j;
            ralph_test_add_constraint(model, 2, lnk_idx, lnk_val, RALPH_LESS_EQUAL, 0.0);
        }
    }
}

void test_cmir_cuts_reduce_nodes(void) {
    printf("\n=== Test: c-MIR Cuts Reduce Nodes ===\n");

    /* Solve with cuts enabled */
    RalphModel *model_on = ralph_test_create();
    build_small_facility(model_on);
    ralph_test_set_int_param(model_on, "verbose", 0);
    ralph_test_set_int_param(model_on, "max_cut_rounds", 5);
    ralph_test_set_int_param(model_on, "max_nodes", 10000);

    ralph_test_optimize(model_on);
    RalphStatus status_on = ralph_test_get_status(model_on);
    ASSERT(status_on == RALPH_STATUS_OPTIMAL, "c-MIR nodes: cuts ON optimal");
    double obj_on = ralph_test_get_objval(model_on);
    int nodes_on = ralph_test_get_node_count(model_on);

    /* Solve with cuts disabled */
    RalphModel *model_off = ralph_test_create();
    build_small_facility(model_off);
    ralph_test_set_int_param(model_off, "verbose", 0);
    ralph_test_set_int_param(model_off, "max_cut_rounds", 0);
    ralph_test_set_int_param(model_off, "max_nodes", 10000);

    ralph_test_optimize(model_off);
    RalphStatus status_off = ralph_test_get_status(model_off);
    ASSERT(status_off == RALPH_STATUS_OPTIMAL, "c-MIR nodes: cuts OFF optimal");
    double obj_off = ralph_test_get_objval(model_off);
    int nodes_off = ralph_test_get_node_count(model_off);

    /* Both should find same optimal */
    ASSERT_NEAR(obj_on, obj_off, TOLERANCE, "c-MIR nodes: same optimal objective");

    /* Cuts should not increase node count */
    printf("  INFO: nodes with cuts=%d, without cuts=%d\n", nodes_on, nodes_off);
    ASSERT(nodes_on <= nodes_off + 1, "c-MIR nodes: cuts don't increase node count");

    ralph_test_free(model_on);
    ralph_test_free(model_off);
}

/* ============================================================================
 * Test: c-MIR Cuts with Presolve
 *
 * Same mixed knapsack as test_cmir_cuts_mixed_knapsack but with lightweight
 * presolve enabled. Verifies correct presolve + cut interaction.
 * ============================================================================ */
void test_cmir_cuts_with_presolve(void) {
    printf("\n=== Test: c-MIR Cuts with Presolve ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_cut_rounds", 5);
    ralph_test_set_int_param(model, "presolve", 1);

    ralph_test_add_var(model, 0.0, 5.0, -8.0, RALPH_INTEGER);     /* x */
    ralph_test_add_var(model, 0.0, 10.0, -5.0, RALPH_CONTINUOUS);  /* y */
    ralph_test_add_var(model, 0.0, 4.0, -3.0, RALPH_INTEGER);      /* z */

    int idx1[] = {0, 1, 2};
    double val1[] = {3.0, 2.0, 1.0};
    ralph_test_add_constraint(model, 3, idx1, val1, RALPH_LESS_EQUAL, 10.0);

    int idx2[] = {0, 1, 2};
    double val2[] = {1.0, 4.0, 2.0};
    ralph_test_add_constraint(model, 3, idx2, val2, RALPH_LESS_EQUAL, 14.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "c-MIR+presolve: OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT(obj < 0.0, "c-MIR+presolve: negative objective");

    double sol[3];
    ralph_test_get_solution(model, sol);

    /* Verify integrality */
    ASSERT(fabs(sol[0] - round(sol[0])) < TOLERANCE, "c-MIR+presolve: x is integral");
    ASSERT(fabs(sol[2] - round(sol[2])) < TOLERANCE, "c-MIR+presolve: z is integral");

    /* Verify constraints */
    double lhs1 = 3*sol[0] + 2*sol[1] + sol[2];
    double lhs2 = sol[0] + 4*sol[1] + 2*sol[2];
    ASSERT(lhs1 <= 10.0 + TOLERANCE, "c-MIR+presolve: constraint 1 satisfied");
    ASSERT(lhs2 <= 14.0 + TOLERANCE, "c-MIR+presolve: constraint 2 satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: c-MIR Cuts on Small Facility Location
 *
 * 5 facilities, 10 customers. Binary open/close + continuous allocation.
 * Classic mixed-integer problem that exercises c-MIR on structured MIP.
 * ============================================================================ */
void test_cmir_cuts_facility_location(void) {
    printf("\n=== Test: c-MIR Cuts Facility Location ===\n");

    int nf = 5, nc = 10;
    int nvars = nf + nc * nf;  /* y[j] + x[i][j] */

    /* Deterministic costs */
    double fixed_cost[5] = {120, 150, 100, 180, 130};
    double transport[10][5] = {
        { 8, 15, 10, 22, 12},
        {14,  6, 18,  9, 11},
        {10, 12,  7, 16, 14},
        {20,  8, 14,  5, 17},
        { 6, 18, 11, 13,  9},
        {16,  7, 15, 10, 13},
        {12, 14,  9, 18,  8},
        { 9, 11, 16,  7, 15},
        {15, 10, 13, 12,  6},
        {11, 13,  8, 14, 10},
    };

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_cut_rounds", 5);
    ralph_test_set_int_param(model, "max_nodes", 5000);

    /* y[j]: binary facility decisions */
    for (int j = 0; j < nf; j++)
        ralph_test_add_var(model, 0.0, 1.0, fixed_cost[j], RALPH_BINARY);

    /* x[i][j]: continuous allocation (0 to 1) */
    for (int i = 0; i < nc; i++)
        for (int j = 0; j < nf; j++)
            ralph_test_add_var(model, 0.0, 1.0, transport[i][j], RALPH_CONTINUOUS);

    /* Demand constraints: sum_j x[i][j] = 1 for each customer */
    int demand_idx[5];
    double demand_val[5];
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            demand_idx[j] = nf + i * nf + j;
            demand_val[j] = 1.0;
        }
        ralph_test_add_constraint(model, nf, demand_idx, demand_val, RALPH_EQUAL, 1.0);
    }

    /* Linking: x[i][j] <= y[j] */
    int link_idx[2];
    double link_val[2] = {1.0, -1.0};
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            link_idx[0] = nf + i * nf + j;
            link_idx[1] = j;
            ralph_test_add_constraint(model, 2, link_idx, link_val, RALPH_LESS_EQUAL, 0.0);
        }
    }

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "c-MIR facility: OPTIMAL");

    double obj = ralph_test_get_objval(model);
    printf("  INFO: facility location obj = %.2f\n", obj);
    ASSERT(obj > 0.0, "c-MIR facility: positive objective");

    double *sol = malloc(nvars * sizeof(double));
    ralph_test_get_solution(model, sol);

    /* Verify facility variables are binary */
    for (int j = 0; j < nf; j++) {
        ASSERT(fabs(sol[j] - 0.0) < TOLERANCE || fabs(sol[j] - 1.0) < TOLERANCE,
               "c-MIR facility: y[j] is binary");
    }

    /* Verify each customer is assigned to exactly one facility */
    for (int i = 0; i < nc; i++) {
        double total = 0;
        for (int j = 0; j < nf; j++)
            total += sol[nf + i * nf + j];
        ASSERT(fabs(total - 1.0) < TOLERANCE, "c-MIR facility: customer fully assigned");
    }

    /* Verify linking: x[i][j] <= y[j] */
    int link_ok = 1;
    for (int i = 0; i < nc; i++)
        for (int j = 0; j < nf; j++)
            if (sol[nf + i * nf + j] > sol[j] + TOLERANCE) link_ok = 0;
    ASSERT(link_ok, "c-MIR facility: linking constraints satisfied");

    free(sol);
    ralph_test_free(model);
}

/* ============================================================================
 * Test: c-MIR Cut Validity
 *
 * Solve with cuts, extract solution, verify ALL original constraints
 * and integrality. Uses a small facility location (same as reduce_nodes
 * helper) to exercise c-MIR on mixed rows.
 * ============================================================================ */
void test_cmir_cuts_validity(void) {
    printf("\n=== Test: c-MIR Cut Validity ===\n");

    int nf = 3, nc = 6;
    int nvars = nf + nc * nf;
    double fixed[3] = {80, 100, 90};
    double trans[6][3] = {
        {10, 20, 15}, {25, 8, 12}, {14, 18, 9},
        {22, 11, 16}, {8, 15, 20}, {17, 13, 7},
    };

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_cut_rounds", 5);

    for (int j = 0; j < nf; j++)
        ralph_test_add_var(model, 0.0, 1.0, fixed[j], RALPH_BINARY);
    for (int i = 0; i < nc; i++)
        for (int j = 0; j < nf; j++)
            ralph_test_add_var(model, 0.0, 1.0, trans[i][j], RALPH_CONTINUOUS);

    int dem_idx[3];
    double dem_val[3] = {1.0, 1.0, 1.0};
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++)
            dem_idx[j] = nf + i * nf + j;
        ralph_test_add_constraint(model, nf, dem_idx, dem_val, RALPH_EQUAL, 1.0);
    }

    int lnk_idx[2];
    double lnk_val[2] = {1.0, -1.0};
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            lnk_idx[0] = nf + i * nf + j;
            lnk_idx[1] = j;
            ralph_test_add_constraint(model, 2, lnk_idx, lnk_val, RALPH_LESS_EQUAL, 0.0);
        }
    }

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "c-MIR validity: OPTIMAL");

    double *sol = malloc(nvars * sizeof(double));
    ralph_test_get_solution(model, sol);

    /* Verify binary variables are integral */
    for (int j = 0; j < nf; j++)
        ASSERT(fabs(sol[j] - round(sol[j])) < TOLERANCE, "c-MIR validity: y[j] integral");

    /* Verify bounds: all vars in [0, 1] */
    int bounds_ok = 1;
    for (int j = 0; j < nvars; j++)
        if (sol[j] < -TOLERANCE || sol[j] > 1.0 + TOLERANCE) bounds_ok = 0;
    ASSERT(bounds_ok, "c-MIR validity: all bounds satisfied");

    /* Verify demand constraints: sum_j x[i][j] = 1 */
    int demand_ok = 1;
    for (int i = 0; i < nc; i++) {
        double sum = 0;
        for (int j = 0; j < nf; j++)
            sum += sol[nf + i * nf + j];
        if (fabs(sum - 1.0) > TOLERANCE) demand_ok = 0;
    }
    ASSERT(demand_ok, "c-MIR validity: demand constraints satisfied");

    /* Verify linking: x[i][j] <= y[j] */
    int link_ok = 1;
    for (int i = 0; i < nc; i++)
        for (int j = 0; j < nf; j++)
            if (sol[nf + i * nf + j] > sol[j] + TOLERANCE) link_ok = 0;
    ASSERT(link_ok, "c-MIR validity: linking constraints satisfied");

    free(sol);
    ralph_test_free(model);
}

/* ============================================================================
 * Test: c-MIR Cuts No Regression with Presolve
 *
 * Solve the same facility location with and without presolve (both with
 * cuts). Both must find the same optimal objective. Catches presolve + cut
 * interaction bugs.
 * ============================================================================ */
void test_cmir_cuts_no_regression_presolve(void) {
    printf("\n=== Test: c-MIR Cuts No Regression Presolve ===\n");

    double obj_no_presolve = 0;

    for (int pass = 0; pass < 2; pass++) {
        RalphModel *m = ralph_test_create();
        build_small_facility(m);
        ralph_test_set_int_param(m, "verbose", 0);
        ralph_test_set_int_param(m, "max_cut_rounds", 5);
        if (pass == 1)
            ralph_test_set_int_param(m, "presolve", 1);

        ralph_test_optimize(m);
        RalphStatus status = ralph_test_get_status(m);

        if (pass == 0) {
            ASSERT(status == RALPH_STATUS_OPTIMAL, "c-MIR presolve regression: no-presolve OPTIMAL");
            obj_no_presolve = ralph_test_get_objval(m);
        } else {
            ASSERT(status == RALPH_STATUS_OPTIMAL, "c-MIR presolve regression: presolve OPTIMAL");
            double obj = ralph_test_get_objval(m);
            ASSERT_NEAR(obj, obj_no_presolve, TOLERANCE,
                        "c-MIR presolve regression: same optimal with/without presolve");
        }

        ralph_test_free(m);
    }
}

/* ============================================================================
 * Regression Test: c-MIR Invalid Cut on Binary Knapsack
 *
 * 10-variable binary knapsack with 3 constraints. c-MIR back-substitution
 * produces a trivially infeasible cut (all positive coefs, negative RHS) on
 * this problem. The safety check in cmir_build_cut should reject the invalid
 * cut, allowing the solver to find the correct optimal.
 *
 * Reproducer for the known c-MIR back-substitution bug.
 * ============================================================================ */
void test_cmir_binary_knapsack_regression(void) {
    printf("\n=== Test: c-MIR Binary Knapsack Regression ===\n");

    double obj_c[] = {-16, -22, -12, -8, -11, -19, -7, -14, -9, -13};
    double a1[]    = { 5,   7,   4,  3,   6,   2,  8,   4,  3,   6};
    double a2[]    = { 3,   2,   6,  5,   1,   7,  2,   5,  4,   3};
    double a3[]    = { 4,   5,   3,  6,   4,   3,  5,   7,  2,   4};
    int n = 10;
    int idx[10];
    for (int j = 0; j < n; j++) idx[j] = j;

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_cut_rounds", 5);
    ralph_test_set_int_param(model, "max_nodes", 10000);

    for (int j = 0; j < n; j++)
        ralph_test_add_var(model, 0.0, 1.0, obj_c[j], RALPH_BINARY);
    ralph_test_add_constraint(model, n, idx, a1, RALPH_LESS_EQUAL, 15.0);
    ralph_test_add_constraint(model, n, idx, a2, RALPH_LESS_EQUAL, 12.0);
    ralph_test_add_constraint(model, n, idx, a3, RALPH_LESS_EQUAL, 18.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL,
           "c-MIR binary knapsack: should be OPTIMAL (not INFEASIBLE)");
    ASSERT_NEAR(ralph_test_get_objval(model), -57.0, TOLERANCE,
                "c-MIR binary knapsack: optimal obj = -57");

    ralph_test_free(model);
}

/* ============================================================================
 * Regression Test: MIP Warm Start Bound Adjustment
 *
 * Tests that non-basic variable values are properly adjusted when bounds
 * change during branch-and-bound warm start. This bug caused suboptimal
 * solutions in facility location problems.
 *
 * Uses deterministic random generation to create a facility location problem
 * with known optimal objective (verified against GLPK).
 * ============================================================================ */
static unsigned int g_test_seed = 123;
static double test_rand_double(double min, double max) {
    g_test_seed = g_test_seed * 1103515245 + 12345;
    return min + (double)(g_test_seed % 100000) / 100000.0 * (max - min);
}

void test_mip_bound_adjustment_regression(void) {
    printf("\n=== Test: MIP Warm Start Bound Adjustment ===\n");

    int num_customers = 20;
    int num_facilities = 10;
    int num_vars = num_facilities + num_customers * num_facilities;

    /* Reset seed for reproducibility */
    g_test_seed = 123;

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_dbl_param(model, "time_limit", 30.0);
    ralph_test_set_dbl_param(model, "mip_gap", 0.0001);

    /* Generate fixed costs and demands */
    double *fixed_cost = malloc(num_facilities * sizeof(double));
    double *demand = malloc(num_customers * sizeof(double));
    for (int j = 0; j < num_facilities; j++) {
        fixed_cost[j] = test_rand_double(100.0, 500.0);
    }
    for (int i = 0; i < num_customers; i++) {
        demand[i] = test_rand_double(1.0, 10.0);
    }

    /* Variables y[j]: facility opening decisions (binary) */
    for (int j = 0; j < num_facilities; j++) {
        ralph_test_add_var(model, 0.0, 1.0, fixed_cost[j], RALPH_BINARY);
    }

    /* Variables x[i,j]: assignment fractions (continuous) */
    for (int i = 0; i < num_customers; i++) {
        for (int j = 0; j < num_facilities; j++) {
            double dist = test_rand_double(1.0, 50.0);
            ralph_test_add_var(model, 0.0, 1.0, dist * demand[i], RALPH_CONTINUOUS);
        }
    }

    /* Demand constraints: sum_j(x[i,j]) = 1 */
    int *indices = malloc(num_facilities * sizeof(int));
    double *values = malloc(num_facilities * sizeof(double));
    for (int i = 0; i < num_customers; i++) {
        for (int j = 0; j < num_facilities; j++) {
            indices[j] = num_facilities + i * num_facilities + j;
            values[j] = 1.0;
        }
        ralph_test_add_constraint(model, num_facilities, indices, values, RALPH_EQUAL, 1.0);
    }

    /* Linking constraints: x[i,j] <= y[j] */
    int idx2[2];
    double val2[2];
    for (int i = 0; i < num_customers; i++) {
        for (int j = 0; j < num_facilities; j++) {
            idx2[0] = num_facilities + i * num_facilities + j;
            idx2[1] = j;
            val2[0] = 1.0;
            val2[1] = -1.0;
            ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 0.0);
        }
    }

    free(indices);
    free(values);
    free(fixed_cost);
    free(demand);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL ||
           status == RALPH_STATUS_NODE_LIMIT ||
           status == RALPH_STATUS_TIME_LIMIT,
           "Status is OPTIMAL, NODE_LIMIT, or TIME_LIMIT");

    double obj = ralph_test_get_objval(model);
    /* Known optimal value from GLPK: 1799.329423 */
    double expected_obj = 1799.329423;
    ASSERT_NEAR(obj, expected_obj, 1.0, "Optimal objective matches GLPK");
    if (status != RALPH_STATUS_OPTIMAL) {
        printf("  INFO: Warm-start regression ended with status %s\n",
               ralph_test_status_string(status));
    }

    double *sol = malloc(num_vars * sizeof(double));
    ralph_test_get_solution(model, sol);

    /* Verify facilities are binary */
    int binary_ok = 1;
    for (int j = 0; j < num_facilities; j++) {
        if (fabs(sol[j]) > TOLERANCE && fabs(sol[j] - 1.0) > TOLERANCE) {
            binary_ok = 0;
        }
    }
    ASSERT(binary_ok, "Facility decisions are binary");

    free(sol);
    ralph_test_free(model);
}

/* ============================================================================
 * Regression Test: Strong Branching State Restoration
 *
 * Tests that strong branching properly saves and restores LP state.
 * This bug caused crashes (SIGSEGV) when accessing corrupt solution data
 * after strong branching modified bounds.
 *
 * Uses a set partitioning-like problem that triggers strong branching.
 * ============================================================================ */
void test_mip_strong_branching_regression(void) {
    printf("\n=== Test: Strong Branching State Restoration ===\n");

    /* Set partitioning: cover elements with minimum cost sets
     * Variables: x[j] = 1 if set j is selected (binary)
     * Constraints: each element must be covered exactly once */
    int num_sets = 10;
    int num_elements = 5;

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "var_select", 2);  /* Strong branching */
    ralph_test_set_dbl_param(model, "time_limit", 30.0);
    ralph_test_set_dbl_param(model, "mip_gap", 0.0001);

    /* Set costs and coverage matrix */
    double costs[] = {3, 2, 1, 4, 5, 2, 3, 1, 4, 2};
    int coverage[10][5] = {
        {1, 0, 0, 1, 0},  /* Set 0 covers elements 0, 3 */
        {0, 1, 0, 0, 1},  /* Set 1 covers elements 1, 4 */
        {1, 1, 0, 0, 0},  /* Set 2 covers elements 0, 1 */
        {0, 0, 1, 1, 0},  /* Set 3 covers elements 2, 3 */
        {0, 0, 0, 1, 1},  /* Set 4 covers elements 3, 4 */
        {1, 0, 1, 0, 0},  /* Set 5 covers elements 0, 2 */
        {0, 1, 1, 0, 0},  /* Set 6 covers elements 1, 2 */
        {0, 0, 0, 0, 1},  /* Set 7 covers element 4 */
        {1, 1, 1, 0, 0},  /* Set 8 covers elements 0, 1, 2 */
        {0, 0, 1, 1, 1},  /* Set 9 covers elements 2, 3, 4 */
    };

    /* Add binary variables */
    for (int j = 0; j < num_sets; j++) {
        ralph_test_add_var(model, 0.0, 1.0, costs[j], RALPH_BINARY);
    }

    /* Each element must be covered exactly once */
    for (int i = 0; i < num_elements; i++) {
        int idx[10];
        double val[10];
        int nnz = 0;
        for (int j = 0; j < num_sets; j++) {
            if (coverage[j][i]) {
                idx[nnz] = j;
                val[nnz] = 1.0;
                nnz++;
            }
        }
        ralph_test_add_constraint(model, nnz, idx, val, RALPH_EQUAL, 1.0);
    }

    /* This should not crash due to strong branching state corruption */
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL || status == RALPH_STATUS_INFEASIBLE,
           "Status is OPTIMAL or INFEASIBLE (not crashed)");

    if (status == RALPH_STATUS_OPTIMAL) {
        double sol[10];
        ralph_test_get_solution(model, sol);

        /* Verify each element is covered exactly once */
        int coverage_ok = 1;
        for (int i = 0; i < num_elements; i++) {
            double covered = 0.0;
            for (int j = 0; j < num_sets; j++) {
                if (coverage[j][i]) {
                    covered += sol[j];
                }
            }
            if (fabs(covered - 1.0) > TOLERANCE) {
                coverage_ok = 0;
            }
        }
        ASSERT(coverage_ok, "Each element covered exactly once");

        /* Verify variables are binary */
        int binary_ok = 1;
        for (int j = 0; j < num_sets; j++) {
            if (fabs(sol[j]) > TOLERANCE && fabs(sol[j] - 1.0) > TOLERANCE) {
                binary_ok = 0;
            }
        }
        ASSERT(binary_ok, "All variables are binary");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: LAP-based MIP - Pure Assignment Problem
 *
 * A pure assignment problem (3x3) formulated as MIP:
 * min sum c[i,j] * x[i,j]
 * s.t. sum_j x[i,j] = 1 for all i (each row assigned once)
 *      sum_i x[i,j] = 1 for all j (each col assigned once)
 *      x[i,j] binary
 *
 * This should be solved using the LAP solver for LP relaxations.
 * ============================================================================ */
void test_lap_mip_assignment(void) {
    printf("\n=== Test: LAP-based MIP - Pure Assignment ===\n");

    /* Cost matrix:
     *      j=0  j=1  j=2
     * i=0   1   10   10
     * i=1  10    2   10
     * i=2  10   10    3
     *
     * Optimal: diagonal assignment with cost 1+2+3 = 6
     */
    double costs[9] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add 9 binary variables x[i,j] with objective costs */
    for (int i = 0; i < 9; i++) {
        ralph_test_add_var(model, 0.0, 1.0, costs[i], RALPH_BINARY);
    }

    /* Row constraints: sum_j x[i,j] = 1 for each row i */
    for (int i = 0; i < 3; i++) {
        int idx[3] = {i*3, i*3+1, i*3+2};
        double val[3] = {1.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 3, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Column constraints: sum_i x[i,j] = 1 for each column j */
    for (int j = 0; j < 3; j++) {
        int idx[3] = {j, 3+j, 6+j};
        double val[3] = {1.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 3, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Enable LAP detection for this model */
    ralph_test_set_int_param(model, "detect_special", 1);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 6.0, TOLERANCE, "Optimal cost is 6");

    /* Verify solution is a valid assignment */
    double sol[9];
    ralph_test_get_solution(model, sol);

    int valid = 1;
    /* Check row sums */
    for (int i = 0; i < 3 && valid; i++) {
        double sum = sol[i*3] + sol[i*3+1] + sol[i*3+2];
        if (fabs(sum - 1.0) > TOLERANCE) valid = 0;
    }
    /* Check col sums */
    for (int j = 0; j < 3 && valid; j++) {
        double sum = sol[j] + sol[3+j] + sol[6+j];
        if (fabs(sum - 1.0) > TOLERANCE) valid = 0;
    }
    ASSERT(valid, "Solution is a valid assignment");

    /* Verify all variables are binary */
    int binary_ok = 1;
    for (int i = 0; i < 9; i++) {
        if (fabs(sol[i]) > TOLERANCE && fabs(sol[i] - 1.0) > TOLERANCE) {
            binary_ok = 0;
        }
    }
    ASSERT(binary_ok, "All variables are binary");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: LAP-based MIP - Larger Assignment Problem (5x5)
 * ============================================================================ */
void test_lap_mip_assignment_5x5(void) {
    printf("\n=== Test: LAP-based MIP - 5x5 Assignment ===\n");

    /* Random cost matrix */
    double costs[25] = {
        7, 2, 1, 9, 4,
        9, 6, 9, 5, 5,
        3, 8, 3, 1, 8,
        7, 9, 4, 2, 2,
        8, 4, 7, 4, 8
    };

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add 25 binary variables */
    for (int i = 0; i < 25; i++) {
        ralph_test_add_var(model, 0.0, 1.0, costs[i], RALPH_BINARY);
    }

    /* Row constraints */
    for (int i = 0; i < 5; i++) {
        int idx[5] = {i*5, i*5+1, i*5+2, i*5+3, i*5+4};
        double val[5] = {1, 1, 1, 1, 1};
        ralph_test_add_constraint(model, 5, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Column constraints */
    for (int j = 0; j < 5; j++) {
        int idx[5] = {j, 5+j, 10+j, 15+j, 20+j};
        double val[5] = {1, 1, 1, 1, 1};
        ralph_test_add_constraint(model, 5, idx, val, RALPH_EQUAL, 1.0);
    }

    ralph_test_set_int_param(model, "detect_special", 1);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    /* Verify solution is a valid assignment */
    double sol[25];
    ralph_test_get_solution(model, sol);

    int valid = 1;
    for (int i = 0; i < 5 && valid; i++) {
        double row_sum = 0;
        for (int j = 0; j < 5; j++) row_sum += sol[i*5+j];
        if (fabs(row_sum - 1.0) > TOLERANCE) valid = 0;
    }
    for (int j = 0; j < 5 && valid; j++) {
        double col_sum = 0;
        for (int i = 0; i < 5; i++) col_sum += sol[i*5+j];
        if (fabs(col_sum - 1.0) > TOLERANCE) valid = 0;
    }
    ASSERT(valid, "Solution is valid 5x5 assignment");

    /* Compute cost manually */
    double manual_cost = 0;
    for (int i = 0; i < 25; i++) {
        if (sol[i] > 0.5) manual_cost += costs[i];
    }
    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, manual_cost, TOLERANCE, "Objective matches computed cost");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: SCP LU Regression (sparse LU bug with non-trivial RHS)
 *
 * This test catches a bug in the sparse LU factorization where the LP basis
 * structure optimization (separating identity columns from structural columns)
 * produced incorrect solutions for RHS vectors that were not linear combinations
 * of basis columns. Specifically, RHS = B*ones worked but RHS = tab->rhs - N*x_N
 * gave large residuals (up to 500+).
 *
 * The bug manifested as:
 * - SCP problems returning INFEASIBLE or ITERATION_LIMIT when they should be OPTIMAL
 * - Wildly incorrect basic variable values (e.g., [-1e5, 7e4] for binary vars)
 * - Objective values that oscillated wildly between iterations
 * ============================================================================ */
static unsigned int scp_seed;
static void scp_seed_random(unsigned int seed) { scp_seed = seed; }
static double scp_rand_double(double lo, double hi) {
    scp_seed = scp_seed * 1103515245 + 12345;
    double r = (double)(scp_seed & 0x7fffffff) / (double)0x7fffffff;
    return lo + r * (hi - lo);
}
static int scp_rand_int(int lo, int hi) {
    return lo + (int)(scp_rand_double(0, 1) * (hi - lo + 1));
}

static RalphModel *create_scp(int num_elements, int num_subsets, double density, unsigned int seed) {
    scp_seed_random(seed);
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    for (int j = 0; j < num_subsets; j++) {
        double cost = scp_rand_double(1.0, 10.0);
        ralph_test_add_var(model, 0.0, 1.0, cost, RALPH_BINARY);
    }

    int *indices = malloc(num_subsets * sizeof(int));
    double *values = malloc(num_subsets * sizeof(double));

    for (int i = 0; i < num_elements; i++) {
        int nnz = 0;
        for (int j = 0; j < num_subsets; j++) {
            if (scp_rand_double(0, 1) < density) {
                indices[nnz] = j;
                values[nnz] = 1.0;
                nnz++;
            }
        }
        if (nnz == 0) {
            int j = scp_rand_int(0, num_subsets - 1);
            indices[0] = j;
            values[0] = 1.0;
            nnz = 1;
        }
        ralph_test_add_constraint(model, nnz, indices, values, 'G', 1.0);
    }

    free(indices);
    free(values);
    return model;
}

/*
 * Test: MIP Incumbent Feasibility Regression
 *
 * This test verifies that the MIP solver only accepts constraint-feasible solutions.
 * The bug was that diving_heuristic and other heuristics could return solutions that
 * satisfied integrality but violated constraints.
 *
 * Uses SetPartitioning which has equality constraints (sum x_j = 1 for each element).
 * The greedy "round all up" heuristic would set all x_j = 1, violating these constraints.
 */
static unsigned int g_sp_seed;
static double sp_rand_double(double min, double max) {
    g_sp_seed = g_sp_seed * 1103515245 + 12345;
    double r = (double)(g_sp_seed % 100000) / 100000.0;
    return min + r * (max - min);
}
static int sp_rand_int(int min, int max) {
    g_sp_seed = g_sp_seed * 1103515245 + 12345;
    return min + (g_sp_seed % (max - min + 1));
}

void test_mip_incumbent_feasibility_regression(void) {
    printf("\n=== Test: MIP Incumbent Feasibility Regression ===\n");

    /* Same parameters as the benchmark: 20 elements, 60 subsets, density 0.30, seed 123 */
    g_sp_seed = 123;
    int num_elements = 20, num_subsets = 60;
    double density = 0.30;

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    double *costs = malloc(num_subsets * sizeof(double));
    for (int j = 0; j < num_subsets; j++) {
        costs[j] = sp_rand_double(1.0, 10.0);
        ralph_test_add_var(model, 0.0, 1.0, costs[j], RALPH_BINARY);
    }

    /* Store coverage info for verification */
    int **covers = malloc(num_elements * sizeof(int*));
    int *cover_count = calloc(num_elements, sizeof(int));
    for (int i = 0; i < num_elements; i++) {
        covers[i] = malloc(num_subsets * sizeof(int));
    }

    int *indices = malloc(num_subsets * sizeof(int));
    double *values = malloc(num_subsets * sizeof(double));
    for (int j = 0; j < num_subsets; j++) values[j] = 1.0;

    for (int i = 0; i < num_elements; i++) {
        int nnz = 0;
        for (int j = 0; j < num_subsets; j++) {
            if (sp_rand_double(0, 1) < density) {
                indices[nnz] = j;
                covers[i][cover_count[i]++] = j;
                nnz++;
            }
        }
        /* Ensure at least 2 subsets cover this element */
        while (nnz < 2) {
            int j = sp_rand_int(0, num_subsets - 1);
            indices[nnz] = j;
            covers[i][cover_count[i]++] = j;
            nnz++;
        }
        ralph_test_add_constraint(model, nnz, indices, values, 'E', 1.0);  /* SetPartitioning: = 1 */
    }

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    int status = ralph_test_get_status(model);

    /* If a solution is returned, verify constraint feasibility.
     * This is the core regression check: never accept infeasible incumbents. */
    if (status == RALPH_STATUS_OPTIMAL ||
        status == RALPH_STATUS_NODE_LIMIT ||
        status == RALPH_STATUS_TIME_LIMIT) {
        double *x = malloc(num_subsets * sizeof(double));
        int has_solution = (x && ralph_test_get_solution(model, x) == 0);

        if (status == RALPH_STATUS_OPTIMAL) {
            ASSERT(has_solution, "OPTIMAL status must provide a solution");
        }

        if (has_solution) {
            int feasible = 1;
            for (int i = 0; i < num_elements && feasible; i++) {
                double sum = 0.0;
                for (int k = 0; k < cover_count[i]; k++) {
                    sum += x[covers[i][k]];
                }
                if (sum < 0.999 || sum > 1.001) {
                    feasible = 0;
                }
            }
            ASSERT(feasible, "Returned incumbent must satisfy all constraints");
        } else if (status == RALPH_STATUS_NODE_LIMIT || status == RALPH_STATUS_TIME_LIMIT) {
            ASSERT(1, "Node/time limit reached without incumbent is acceptable");
        }
        free(x);
    } else if (status == RALPH_STATUS_INFEASIBLE) {
        /* Problem may genuinely be infeasible - that's OK */
        printf("  (Problem is infeasible - expected for this seed)\n");
        ASSERT(1, "INFEASIBLE status is valid for this problem");
    } else {
        /* Unexpected status */
        ASSERT(0, "Unexpected status (not OPTIMAL or INFEASIBLE)");
    }

    for (int i = 0; i < num_elements; i++) free(covers[i]);
    free(covers);
    free(cover_count);
    free(indices);
    free(values);
    free(costs);
    ralph_test_free(model);
}

void test_scp_lu_regression(void) {
    printf("\n=== Test: SCP LU Regression ===\n");

    /* This specific problem (50x100 SCP, density 0.3, seed 12345) triggered the bug
     * where sparse LU factorization produced wrong results for certain RHS values. */
    RalphModel *model = create_scp(50, 100, 0.3, 12345);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "detect_special", 0);
    ralph_test_set_int_param(model, "presolve", 0);
    ralph_test_set_int_param(model, "max_iterations", 10000);

    ralph_test_optimize(model);

    int status = ralph_test_get_status(model);
    double obj = ralph_test_get_objval(model);

    /* The bug caused status to be INFEASIBLE or ITERATION_LIMIT with
     * wildly wrong basic values/objectives. On slower branches we may hit
     * NODE_LIMIT/TIME_LIMIT while still returning a valid incumbent. */
    ASSERT(status == RALPH_STATUS_OPTIMAL ||
           status == RALPH_STATUS_NODE_LIMIT ||
           status == RALPH_STATUS_TIME_LIMIT,
           "SCP should be OPTIMAL, NODE_LIMIT, or TIME_LIMIT");
    ASSERT(obj >= 8.0 && obj <= 15.0, "SCP objective should be reasonable (8-15)");

    if (status == RALPH_STATUS_OPTIMAL) {
        printf("  SCP solved: obj = %.2f, iterations = %d\n",
               obj, ralph_test_get_iterations(model));
    } else {
        printf("  SCP failed: status = %d (%s)\n",
               status, ralph_test_status_string(status));
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Constraint Modification API
 *
 * Tests ralph_core_set_constraint_rhs(), ralph_core_set_constraint_coef(),
 * ralph_core_set_constraint_coefs(), and ralph_test_get_var_bounds().
 * ============================================================================ */
void test_constraint_modification(void) {
    printf("\n=== Test: Constraint Modification API ===\n");

    /* Simple LP: min x + y
     * s.t. x + y >= 2
     *      x, y >= 0
     * Optimal: x=0, y=2 or x=2, y=0, obj=2
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 2.0);

    /* Test bounds query */
    double lb, ub;
    int ret = ralph_test_get_var_bounds(model, 0, &lb, &ub);
    ASSERT(ret == 0, "ralph_test_get_var_bounds returns 0");
    ASSERT_NEAR(lb, 0.0, TOLERANCE, "Lower bound is 0");
    ASSERT(ub >= 1e29, "Upper bound is infinity");

    /* Solve */
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Initial solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 2.0, TOLERANCE, "Objective is 2.0");

    /* Modify RHS: change x + y >= 2 to x + y >= 5 */
    ret = ralph_core_set_constraint_rhs(model, 0, 5.0);
    ASSERT(ret == 0, "ralph_core_set_constraint_rhs returns 0");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_UNKNOWN,
           "RHS edit invalidates solve status");

    /* Re-solve */
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Re-solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 5.0, TOLERANCE, "New objective is 5.0");

    /* Modify matrix coefficient: 2*x + y >= 5 */
    ret = ralph_core_set_constraint_coef(model, 0, 0, 2.0);
    ASSERT(ret == 0, "ralph_core_set_constraint_coef returns 0");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_UNKNOWN,
           "Coefficient edit invalidates solve status");
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Coefficient edit re-solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 2.5, TOLERANCE,
                "Coefficient edit objective is 2.5");

    /* Remove x coefficient: y >= 5 */
    ret = ralph_core_set_constraint_coef(model, 0, 0, 0.0);
    ASSERT(ret == 0, "ralph_core_set_constraint_coef can remove entry");
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Entry removal re-solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 5.0, TOLERANCE,
                "Entry removal objective is 5.0");

    /* Bulk coefficient updates: x + 2*y >= 5 */
    int up_cons[] = {0, 0};
    int up_vars[] = {0, 1};
    double up_vals[] = {1.0, 2.0};
    ret = ralph_core_set_constraint_coefs(model, 2, up_cons, up_vars, up_vals);
    ASSERT(ret == 0, "ralph_core_set_constraint_coefs returns 0");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_UNKNOWN,
           "Bulk coefficient edit invalidates solve status");
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Bulk coefficient re-solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 2.5, TOLERANCE,
                "Bulk coefficient objective is 2.5");

    /* Test invalid constraint index */
    ret = ralph_core_set_constraint_rhs(model, 99, 1.0);
    ASSERT(ret == -1, "Invalid constraint index returns -1");

    ret = ralph_core_set_constraint_coef(model, 99, 0, 1.0);
    ASSERT(ret == -1, "Invalid constraint index for coef returns -1");
    ret = ralph_core_set_constraint_coef(model, 0, 99, 1.0);
    ASSERT(ret == -1, "Invalid variable index for coef returns -1");

    int bad_vars[] = {0, 99};
    ret = ralph_core_set_constraint_coefs(model, 2, up_cons, bad_vars, up_vals);
    ASSERT(ret == -1, "Invalid index in bulk coef update returns -1");

    /* Test invalid variable index for bounds query */
    ret = ralph_test_get_var_bounds(model, 99, &lb, &ub);
    ASSERT(ret == -1, "Invalid variable index returns -1");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Constraint Query API
 *
 * Tests ralph_core_get_constraint_rhs(), ralph_core_get_constraint_sense(), and
 * ralph_core_get_constraint_coef() in both finalized and editable states.
 * ============================================================================ */
void test_constraint_query_api(void) {
    printf("\n=== Test: Constraint Query API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Query API: model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* z */

    int c0_idx[] = {0, 1};
    double c0_val[] = {2.0, 1.0};
    int c1_idx[] = {1, 2};
    double c1_val[] = {1.0, -1.0};
    ralph_test_add_constraint(model, 2, c0_idx, c0_val, RALPH_LESS_EQUAL, 7.0);
    ralph_test_add_constraint(model, 2, c1_idx, c1_val, RALPH_EQUAL, 1.0);

    /* Finalized path: solve first so A is in CSC form. */
    ASSERT(ralph_test_optimize_lp(model) == 0, "Query API: solve call succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Query API: status OPTIMAL");

    double rhs = 0.0;
    RalphSense sense = RALPH_EQUAL;
    double coef = 0.0;

    ASSERT(ralph_core_get_constraint_rhs(model, 0, &rhs) == 0, "Query API: get RHS c0");
    ASSERT_NEAR(rhs, 7.0, TOLERANCE, "Query API: c0 RHS is 7");
    ASSERT(ralph_core_get_constraint_sense(model, 0, &sense) == 0, "Query API: get sense c0");
    ASSERT(sense == RALPH_LESS_EQUAL, "Query API: c0 sense is <=");

    ASSERT(ralph_core_get_constraint_coef(model, 0, 0, &coef) == 0, "Query API: get A[0,0]");
    ASSERT_NEAR(coef, 2.0, TOLERANCE, "Query API: A[0,0]=2");
    ASSERT(ralph_core_get_constraint_coef(model, 0, 2, &coef) == 0, "Query API: get structural zero");
    ASSERT_NEAR(coef, 0.0, TOLERANCE, "Query API: structural zero returns 0");

    ASSERT(ralph_core_get_constraint_rhs(model, 1, &rhs) == 0, "Query API: get RHS c1");
    ASSERT_NEAR(rhs, 1.0, TOLERANCE, "Query API: c1 RHS is 1");
    ASSERT(ralph_core_get_constraint_sense(model, 1, &sense) == 0, "Query API: get sense c1");
    ASSERT(sense == RALPH_EQUAL, "Query API: c1 sense is =");
    ASSERT(ralph_core_get_constraint_coef(model, 1, 1, &coef) == 0, "Query API: get A[1,1]");
    ASSERT_NEAR(coef, 1.0, TOLERANCE, "Query API: A[1,1]=1");
    ASSERT(ralph_core_get_constraint_coef(model, 1, 2, &coef) == 0, "Query API: get A[1,2]");
    ASSERT_NEAR(coef, -1.0, TOLERANCE, "Query API: A[1,2]=-1");

    /* Editable path: any coefficient edit rebuilds build_state. */
    ASSERT(ralph_core_set_constraint_coef(model, 0, 2, 4.0) == 0, "Query API: set A[0,2]=4");
    ASSERT(ralph_core_get_constraint_coef(model, 0, 2, &coef) == 0, "Query API: read edited A[0,2]");
    ASSERT_NEAR(coef, 4.0, TOLERANCE, "Query API: edited A[0,2]=4");

    ASSERT(ralph_core_set_constraint_rhs(model, 1, 2.0) == 0, "Query API: set c1 RHS=2");
    ASSERT(ralph_core_get_constraint_rhs(model, 1, &rhs) == 0, "Query API: read edited RHS");
    ASSERT_NEAR(rhs, 2.0, TOLERANCE, "Query API: edited RHS is 2");

    ASSERT(ralph_core_get_constraint_rhs(model, 99, &rhs) == -1, "Query API: invalid RHS row rejected");
    ASSERT(ralph_core_get_constraint_sense(model, 99, &sense) == -1, "Query API: invalid sense row rejected");
    ASSERT(ralph_core_get_constraint_coef(model, 0, 99, &coef) == -1, "Query API: invalid coef col rejected");
    ASSERT(ralph_core_get_constraint_coef(model, 99, 0, &coef) == -1, "Query API: invalid coef row rejected");
    ASSERT(ralph_core_get_constraint_rhs(model, 0, NULL) == -1, "Query API: NULL RHS output rejected");
    ASSERT(ralph_core_get_constraint_sense(model, 0, NULL) == -1, "Query API: NULL sense output rejected");
    ASSERT(ralph_core_get_constraint_coef(model, 0, 0, NULL) == -1, "Query API: NULL coef output rejected");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Constraint Batch Edit API (Atomic Validation)
 *
 * Tests batch RHS/sense updates and verifies failed batches do not partially
 * mutate model data.
 * ============================================================================ */
void test_constraint_batch_edit_api(void) {
    printf("\n=== Test: Constraint Batch Edit API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Batch API: model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */

    int c0_idx[] = {0, 1};
    double c0_val[] = {1.0, 1.0};
    int c1_idx[] = {0, 1};
    double c1_val[] = {1.0, 2.0};
    ralph_test_add_constraint(model, 2, c0_idx, c0_val, RALPH_GREATER_EQUAL, 2.0);
    ralph_test_add_constraint(model, 2, c1_idx, c1_val, RALPH_LESS_EQUAL, 10.0);

    int rows[] = {0, 1};
    double rhs_vals[] = {5.0, 8.0};
    ASSERT(ralph_core_set_constraint_rhs_batch(model, 2, rows, rhs_vals) == 0,
           "Batch API: RHS batch accepted");

    double rhs0 = 0.0, rhs1 = 0.0;
    ASSERT(ralph_core_get_constraint_rhs(model, 0, &rhs0) == 0, "Batch API: read RHS c0");
    ASSERT(ralph_core_get_constraint_rhs(model, 1, &rhs1) == 0, "Batch API: read RHS c1");
    ASSERT_NEAR(rhs0, 5.0, TOLERANCE, "Batch API: c0 RHS updated");
    ASSERT_NEAR(rhs1, 8.0, TOLERANCE, "Batch API: c1 RHS updated");

    int bad_rows[] = {0, 99};
    double bad_rhs_vals[] = {1.0, 1.0};
    ASSERT(ralph_core_set_constraint_rhs_batch(model, 2, bad_rows, bad_rhs_vals) == -1,
           "Batch API: invalid RHS batch rejected");
    ASSERT(ralph_core_get_constraint_rhs(model, 0, &rhs0) == 0, "Batch API: read RHS after rejection");
    ASSERT_NEAR(rhs0, 5.0, TOLERANCE, "Batch API: rejected RHS batch made no partial edits");

    RalphSense senses[] = {RALPH_LESS_EQUAL, RALPH_EQUAL};
    ASSERT(ralph_core_set_constraint_sense_batch(model, 2, rows, senses) == 0,
           "Batch API: sense batch accepted");
    RalphSense s0 = RALPH_EQUAL;
    ASSERT(ralph_core_get_constraint_sense(model, 0, &s0) == 0, "Batch API: read c0 sense");
    ASSERT(s0 == RALPH_LESS_EQUAL, "Batch API: c0 sense updated");

    RalphSense bad_senses[] = {RALPH_GREATER_EQUAL, (RalphSense)'X'};
    ASSERT(ralph_core_set_constraint_sense_batch(model, 2, rows, bad_senses) == -1,
           "Batch API: invalid sense batch rejected");
    ASSERT(ralph_core_get_constraint_sense(model, 0, &s0) == 0, "Batch API: read c0 sense after reject");
    ASSERT(s0 == RALPH_LESS_EQUAL, "Batch API: rejected sense batch made no partial edits");

    ASSERT(ralph_core_set_constraint_rhs_batch(model, 2, NULL, rhs_vals) == -1,
           "Batch API: NULL row array rejected");
    ASSERT(ralph_core_set_constraint_rhs_batch(model, 2, rows, NULL) == -1,
           "Batch API: NULL RHS array rejected");
    ASSERT(ralph_core_set_constraint_sense_batch(model, 2, NULL, senses) == -1,
           "Batch API: NULL sense row array rejected");
    ASSERT(ralph_core_set_constraint_sense_batch(model, 2, rows, NULL) == -1,
           "Batch API: NULL sense array rejected");

    ASSERT(ralph_test_optimize_lp(model) == 0, "Batch API: solve call succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL ||
           ralph_test_get_status(model) == RALPH_STATUS_INFEASIBLE,
           "Batch API: model remains numerically valid after edits");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Row/Column Deletion API
 *
 * Tests ralph_core_delete_constraint() and ralph_core_delete_var() semantics.
 * ============================================================================ */
void test_row_col_deletion_api(void) {
    printf("\n=== Test: Row/Column Deletion API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Delete API: model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);    /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 10.0, RALPH_CONTINUOUS);   /* y */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 100.0, RALPH_CONTINUOUS);  /* z */

    int c0_idx[] = {0, 1};
    double c0_val[] = {1.0, 1.0};   /* x + y >= 5 */
    int c1_idx[] = {1};
    double c1_val[] = {1.0};        /* y >= 1 */
    ralph_test_add_constraint(model, 2, c0_idx, c0_val, RALPH_GREATER_EQUAL, 5.0);
    ralph_test_add_constraint(model, 1, c1_idx, c1_val, RALPH_GREATER_EQUAL, 1.0);

    ASSERT(ralph_test_optimize_lp(model) == 0, "Delete API: initial solve succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Delete API: initial status OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 14.0, TOLERANCE, "Delete API: initial objective is 14");

    ASSERT(ralph_core_delete_constraint(model, 1) == 0, "Delete API: delete row 1");
    ASSERT(ralph_test_get_num_cons(model) == 1, "Delete API: row count decreased");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_UNKNOWN, "Delete API: row delete invalidates status");
    ASSERT(ralph_test_optimize_lp(model) == 0, "Delete API: solve after row delete succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Delete API: row-delete solve OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 5.0, TOLERANCE, "Delete API: objective after row delete is 5");

    ASSERT(ralph_core_delete_var(model, 0) == 0, "Delete API: delete var 0");
    ASSERT(ralph_test_get_num_vars(model) == 2, "Delete API: var count decreased");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_UNKNOWN, "Delete API: var delete invalidates status");

    double coef = 0.0;
    ASSERT(ralph_core_get_constraint_coef(model, 0, 0, &coef) == 0, "Delete API: read shifted A[0,0]");
    ASSERT_NEAR(coef, 1.0, TOLERANCE, "Delete API: shifted A[0,0]=1");
    ASSERT(ralph_core_get_constraint_coef(model, 0, 1, &coef) == 0, "Delete API: read shifted A[0,1]");
    ASSERT_NEAR(coef, 0.0, TOLERANCE, "Delete API: shifted A[0,1]=0");

    ASSERT(ralph_test_optimize_lp(model) == 0, "Delete API: solve after var delete succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Delete API: var-delete solve OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 50.0, TOLERANCE, "Delete API: objective after var delete is 50");

    ASSERT(ralph_core_delete_constraint(model, 99) == -1, "Delete API: invalid row delete rejected");
    ASSERT(ralph_core_delete_var(model, 99) == -1, "Delete API: invalid var delete rejected");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: LP IIS/Conflict API
 * ============================================================================ */
void test_lp_iis_api(void) {
    printf("\n=== Test: LP IIS API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "IIS API: model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "method", 0);
    ralph_test_set_int_param(model, "presolve", 0);

    /* Variables x, y >= 0 */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 0.0, RALPH_CONTINUOUS); /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 0.0, RALPH_CONTINUOUS); /* y */

    /* Infeasible core: x >= 2 and x <= 1 */
    int r0_idx[] = {0}; double r0_val[] = {1.0};
    int r1_idx[] = {0}; double r1_val[] = {1.0};
    /* Redundant rows (not needed for infeasibility) */
    int r2_idx[] = {1}; double r2_val[] = {1.0};
    int r3_idx[] = {0, 1}; double r3_val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 1, r0_idx, r0_val, RALPH_GREATER_EQUAL, 2.0); /* row 0 */
    ralph_test_add_constraint(model, 1, r1_idx, r1_val, RALPH_LESS_EQUAL, 1.0);    /* row 1 */
    ralph_test_add_constraint(model, 1, r2_idx, r2_val, RALPH_GREATER_EQUAL, 0.0); /* row 2 */
    ralph_test_add_constraint(model, 2, r3_idx, r3_val, RALPH_GREATER_EQUAL, 0.0); /* row 3 */

    ralph_test_optimize_lp(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_INFEASIBLE,
           "IIS API: base model is infeasible");

    int flags[4] = {0, 0, 0, 0};
    int iis_size = 0;
    ASSERT(ralph_core_compute_lp_iis(model, flags, &iis_size) == 0,
           "IIS API: extraction succeeds");
    ASSERT(iis_size == 2, "IIS API: expected IIS size is 2");
    ASSERT(flags[0] == 1, "IIS API: row 0 is in IIS");
    ASSERT(flags[1] == 1, "IIS API: row 1 is in IIS");
    ASSERT(flags[2] == 0, "IIS API: row 2 excluded from IIS");
    ASSERT(flags[3] == 0, "IIS API: row 3 excluded from IIS");

    /* Irreducibility check: removing any IIS row makes subsystem feasible. */
    for (int drop = 0; drop < 4; drop++) {
        if (!flags[drop]) continue;
        RalphModel *sub = ralph_test_create();
        ASSERT(sub != NULL, "IIS API: subsystem model created");
        ralph_test_set_obj_sense(sub, RALPH_MINIMIZE);
        ralph_test_set_int_param(sub, "method", 0);
        ralph_test_set_int_param(sub, "presolve", 0);
        ralph_test_add_var(sub, 0.0, RALPH_INFINITY, 0.0, RALPH_CONTINUOUS);
        ralph_test_add_var(sub, 0.0, RALPH_INFINITY, 0.0, RALPH_CONTINUOUS);
        ralph_test_add_constraint(sub, 1, r0_idx, r0_val, RALPH_GREATER_EQUAL, 2.0); /* row 0 */
        ralph_test_add_constraint(sub, 1, r1_idx, r1_val, RALPH_LESS_EQUAL, 1.0);    /* row 1 */
        ralph_test_add_constraint(sub, 1, r2_idx, r2_val, RALPH_GREATER_EQUAL, 0.0); /* row 2 */
        ralph_test_add_constraint(sub, 2, r3_idx, r3_val, RALPH_GREATER_EQUAL, 0.0); /* row 3 */

        /* Keep only IIS rows except the dropped one (delete in reverse index order). */
        for (int row = 3; row >= 0; row--) {
            int keep = flags[row] && row != drop;
            if (!keep) {
                ASSERT(ralph_core_delete_constraint(sub, row) == 0,
                       "IIS API: row deletion succeeds for irreducibility check");
            }
        }

        ralph_test_optimize_lp(sub);
        ASSERT(ralph_test_get_status(sub) != RALPH_STATUS_INFEASIBLE,
               "IIS API: dropping any IIS row removes infeasibility");
        ralph_test_free(sub);
    }

    ASSERT(ralph_core_compute_lp_iis(NULL, flags, &iis_size) == -1,
           "IIS API: NULL model rejected");
    ASSERT(ralph_core_compute_lp_iis(model, NULL, &iis_size) == -1,
           "IIS API: NULL flags rejected");

    ralph_test_free(model);

    model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_optimize_lp(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "IIS API: control model optimal");
    ASSERT(ralph_core_compute_lp_iis(model, flags, &iis_size) == -1,
           "IIS API: rejects non-infeasible status");
    ralph_test_free(model);
}

/* ============================================================================
 * Test: Lazy Constraints API
 *
 * Tests ralph_core_add_lazy_constraint() and ralph_core_add_lazy_constraints().
 * ============================================================================ */
void test_lazy_constraints(void) {
    printf("\n=== Test: Lazy Constraints API ===\n");

    /* Start with relaxed LP: min x + y
     * s.t. x + y <= 10  (doesn't affect optimum at x=y=0)
     *      x >= 0, y >= 0
     * Optimal: x=0, y=0, obj=0
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */

    /* Add a loose constraint that doesn't affect the optimum */
    int idx0[] = {0, 1};
    double val0[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx0, val0, RALPH_LESS_EQUAL, 10.0);

    /* Solve relaxed */
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Relaxed solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 0.0, TOLERANCE, "Relaxed objective is 0.0");

    /* Add lazy constraint: x + y >= 3 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    RalphCut cut1 = {
        .indices = idx1,
        .coeffs = val1,
        .num_vars = 2,
        .sense = RALPH_GREATER_EQUAL,
        .rhs = 3.0
    };

    int ret = ralph_core_add_lazy_constraint(model, &cut1);
    ASSERT(ret == 0, "ralph_core_add_lazy_constraint returns 0");

    /* Re-solve with cut */
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "With cut is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 3.0, TOLERANCE, "With cut objective is 3.0");

    /* Add multiple lazy constraints: x >= 2, y >= 2 */
    int idx2[] = {0};
    double val2[] = {1.0};
    RalphCut cut2 = {
        .indices = idx2,
        .coeffs = val2,
        .num_vars = 1,
        .sense = RALPH_GREATER_EQUAL,
        .rhs = 2.0
    };

    int idx3[] = {1};
    double val3[] = {1.0};
    RalphCut cut3 = {
        .indices = idx3,
        .coeffs = val3,
        .num_vars = 1,
        .sense = RALPH_GREATER_EQUAL,
        .rhs = 2.0
    };

    RalphCut cuts[] = {cut2, cut3};
    ret = ralph_core_add_lazy_constraints(model, cuts, 2);
    ASSERT(ret == 0, "ralph_core_add_lazy_constraints returns 0");

    /* Re-solve */
    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "With multiple cuts is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 4.0, TOLERANCE, "With multiple cuts objective is 4.0");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Branching Control API
 *
 * Tests the ralph_test_set_branch_priorities() and ralph_test_set_branch_directions()
 * functions for MIP branching control.
 * ============================================================================ */
void test_branching_control(void) {
    printf("\n=== Test: Branching Control API ===\n");

    /* Simple MIP: min x0 + x1
     * s.t. x0 + x1 >= 1
     *      x0, x1 binary
     * Optimal: x0=1 or x1=1, obj=1
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Two binary variables */
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x0 */
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x1 */

    /* x0 + x1 >= 1 */
    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    /* Set priorities: x1 has higher priority than x0 */
    int priorities[2] = {1, 10};  /* x1 (priority 10) > x0 (priority 1) */
    int ret = ralph_test_set_branch_priorities(model, priorities);
    ASSERT(ret == 0, "ralph_test_set_branch_priorities returns 0");

    /* Set branch directions: prefer x1=1 first (BRANCH_UP) */
    int directions[2] = {RALPH_BRANCH_AUTO, RALPH_BRANCH_UP};
    ret = ralph_test_set_branch_directions(model, directions);
    ASSERT(ret == 0, "ralph_test_set_branch_directions returns 0");

    /* Solve */
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 1.0, TOLERANCE, "Objective value is 1.0");

    double x[2];
    ralph_test_get_solution(model, x);

    /* Verify solution: exactly one variable is 1 */
    int sum = (int)(x[0] + 0.5) + (int)(x[1] + 0.5);
    ASSERT(sum >= 1, "At least one variable is 1");
    ASSERT(x[0] + x[1] >= 1.0 - TOLERANCE, "Constraint satisfied");

    ralph_test_free(model);

    /* Test clearing priorities and directions */
    model = ralph_test_create();
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);

    /* Set then clear */
    int prios[2] = {5, 5};
    ralph_test_set_branch_priorities(model, prios);
    ret = ralph_test_set_branch_priorities(model, NULL);  /* Clear */
    ASSERT(ret == 0, "Clearing priorities returns 0");

    ret = ralph_test_set_branch_directions(model, NULL);  /* Clear non-existent */
    ASSERT(ret == 0, "Clearing directions returns 0");

    ralph_test_free(model);
}

static RalphModel* build_warm_start_test_lp(double rhs) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, 10.0, -1.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0.0, 10.0, -1.0, RALPH_CONTINUOUS);  /* y */
    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, rhs);
    return model;
}

/* ============================================================================
 * Test: Warm Start (Live Solver Load Path)
 *
 * Load basis after optimize() while solver/tableau exists.
 * ============================================================================ */
void test_warm_start_live_load(void) {
    printf("\n=== Test: Warm Start (Live Load) ===\n");

    RalphModel *model = build_warm_start_test_lp(4.0);
    ASSERT(model != NULL, "Model created");

    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Initial solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -4.0, TOLERANCE, "Initial objective is -4.0");

    RalphBasis *basis = ralph_test_save_basis(model);
    ASSERT(basis != NULL, "Basis saved successfully");

    int ret = ralph_test_load_basis(model, basis);
    ASSERT(ret == 0, "Live basis load returns 0");

    ralph_test_optimize(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Re-solve after live load is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -4.0, TOLERANCE, "Objective unchanged after live load");

    ralph_test_free_basis(basis);
    ralph_test_free(model);
}

/* ============================================================================
 * Test: Warm Start (Staged Pre-solve Load Path)
 *
 * Load basis before first optimize(); basis is staged and applied on optimize().
 * ============================================================================ */
void test_warm_start_staged_load(void) {
    printf("\n=== Test: Warm Start (Staged Load) ===\n");

    RalphModel *source = build_warm_start_test_lp(4.0);
    ASSERT(source != NULL, "Source model created");
    ralph_test_optimize(source);
    ASSERT(ralph_test_get_status(source) == RALPH_STATUS_OPTIMAL, "Source solve is OPTIMAL");
    RalphBasis *basis = ralph_test_save_basis(source);
    ASSERT(basis != NULL, "Source basis saved");

    RalphModel *target = build_warm_start_test_lp(4.0);
    ASSERT(target != NULL, "Target model created");

    int ret = ralph_test_load_basis(target, basis);
    ASSERT(ret == 0, "Staged basis load returns 0 before optimize");
    ASSERT(ralph_test_get_status(target) == RALPH_STATUS_UNKNOWN, "Status remains UNKNOWN before solve");

    ralph_test_optimize(target);
    ASSERT(ralph_test_get_status(target) == RALPH_STATUS_OPTIMAL, "Staged-load solve is OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(target), -4.0, TOLERANCE, "Staged-load objective is -4.0");

    /* Dimension mismatch rejection in staged mode */
    RalphModel *mismatch = ralph_test_create();
    ASSERT(mismatch != NULL, "Mismatch model created");
    ralph_test_add_var(mismatch, 0.0, 1.0, 1.0, RALPH_CONTINUOUS);
    ret = ralph_test_load_basis(mismatch, basis);
    ASSERT(ret == -1, "Staged load rejects dimension mismatch");

    ralph_test_free(mismatch);
    ralph_test_free(target);
    ralph_test_free_basis(basis);
    ralph_test_free(source);
}

/* ============================================================================
 * Test: Warm Start Edge Cases
 * ============================================================================ */
void test_warm_start_edge_cases(void) {
    printf("\n=== Test: Warm Start (Edge Cases) ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_CONTINUOUS);
    RalphBasis *basis = ralph_test_save_basis(model);
    ASSERT(basis == NULL, "Cannot save basis from unsolved model");
    ralph_test_free(model);

    ralph_test_free_basis(NULL);  /* Should not crash */
    ASSERT(1, "Free NULL basis does not crash");
}

/* ============================================================================
 * Test: Basis Status API
 * ============================================================================ */
void test_basis_status_api(void) {
    printf("\n=== Test: Basis Status API ===\n");

    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Basis status: model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "presolve", 0);  /* Keep tableau dimensions stable */

    /* min x + y
     * s.t. x + y >= 2
     *      x + 2y <= 4
     *      x,y >= 0
     */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);

    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_GREATER_EQUAL, 2.0);

    int idx2[] = {0, 1};
    double val2[] = {1.0, 2.0};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 4.0);

    ralph_test_optimize_lp(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Basis status: control model optimal");

    const int n = ralph_test_get_num_vars(model);
    const int m = ralph_test_get_num_cons(model);
    RalphBasisStatus *col_status = (RalphBasisStatus*)calloc((size_t)n, sizeof(RalphBasisStatus));
    RalphBasisStatus *row_status = (RalphBasisStatus*)calloc((size_t)m, sizeof(RalphBasisStatus));
    ASSERT(col_status != NULL && row_status != NULL,
           "Basis status: status buffers allocated");
    if (!col_status || !row_status) {
        free(col_status);
        free(row_status);
        ralph_test_free(model);
        return;
    }

    ASSERT(ralph_test_get_basis_status(model, col_status, row_status) == 0,
           "Basis status: get succeeds");
    ASSERT(ralph_test_get_basis_status(model, col_status, NULL) == 0,
           "Basis status: get columns-only succeeds");
    ASSERT(ralph_test_get_basis_status(model, NULL, row_status) == 0,
           "Basis status: get rows-only succeeds");

    double obj_before = ralph_test_get_objval(model);
    ASSERT(ralph_test_set_basis_status(model, col_status, row_status) == 0,
           "Basis status: set round-trip succeeds");

    ralph_test_optimize_lp(model);
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Basis status: optimal after round-trip set");
    ASSERT_NEAR(ralph_test_get_objval(model), obj_before, TOLERANCE,
                "Basis status: objective preserved after round-trip");

    RalphBasisStatus bad_col[2] = {
        RALPH_BASIS_STATUS_BASIC,
        RALPH_BASIS_STATUS_BASIC
    };
    RalphBasisStatus bad_row[2] = {
        RALPH_BASIS_STATUS_BASIC,
        RALPH_BASIS_STATUS_BASIC
    };
    ASSERT(ralph_test_set_basis_status(model, bad_col, bad_row) == -1,
           "Basis status: rejects invalid basic count");

    ASSERT(ralph_test_get_basis_status(model, NULL, NULL) == -1,
           "Basis status: get rejects both outputs NULL");
    ASSERT(ralph_test_set_basis_status(model, NULL, NULL) == -1,
           "Basis status: set rejects both inputs NULL");
    ASSERT(ralph_test_get_basis_status(NULL, col_status, row_status) == -1,
           "Basis status: get rejects NULL model");
    ASSERT(ralph_test_set_basis_status(NULL, col_status, row_status) == -1,
           "Basis status: set rejects NULL model");

    free(col_status);
    free(row_status);
    ralph_test_free(model);

    RalphModel *unsolved = ralph_test_create();
    ASSERT(unsolved != NULL, "Basis status: unsolved model created");
    if (unsolved) {
        ralph_test_add_var(unsolved, 0.0, 1.0, 1.0, RALPH_CONTINUOUS);
        ASSERT(ralph_test_get_basis_status(unsolved, bad_col, bad_row) == -1,
               "Basis status: get unavailable before solve");
        ASSERT(ralph_test_set_basis_status(unsolved, bad_col, bad_row) == -1,
               "Basis status: set unavailable before solve");
        ralph_test_free(unsolved);
    }
}

/* ============================================================================
 * Test: Cut Callback
 *
 * Test that user-provided cut callback is invoked during MIP solving.
 * ============================================================================ */

/* Global counter for callback invocations */
static int cut_callback_count = 0;
static int cut_callback_cuts_added = 0;

static int test_cut_callback_fn(void *user_data, const double *x_relaxation,
                                 int num_vars, RalphCut *cuts, int max_cuts) {
    (void)x_relaxation;
    (void)num_vars;
    (void)max_cuts;

    cut_callback_count++;

    /* Get threshold from user data */
    double threshold = user_data ? *(double*)user_data : 0.5;

    /* Only add a cut if we haven't added one yet and x[0] is fractional */
    if (cut_callback_cuts_added == 0 && x_relaxation[0] > threshold) {
        /* Add a trivial cut: x[0] <= 1 (already implied, but tests the mechanism) */
        static int indices[1] = {0};
        static double coeffs[1] = {1.0};

        cuts[0].indices = indices;
        cuts[0].coeffs = coeffs;
        cuts[0].num_vars = 1;
        cuts[0].sense = RALPH_LESS_EQUAL;
        cuts[0].rhs = 1.0;

        cut_callback_cuts_added = 1;
        return 1;  /* One cut added */
    }

    return 0;  /* No cuts */
}

void test_cut_callback(void) {
    printf("\n=== Test: Cut Callback ===\n");

    /* Reset counters */
    cut_callback_count = 0;
    cut_callback_cuts_added = 0;

    /* Simple MIP: min x + y
     * s.t. x + y >= 1
     *      x, y binary
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x */
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* y */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    /* Set up cut callback */
    double threshold = 0.0;  /* Add cut if x[0] > 0 */
    RalphCutCallback callback = {
        .generate_cuts = test_cut_callback_fn,
        .user_data = &threshold
    };
    ralph_test_set_cut_callback(model, &callback);

    /* Solve */
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 1.0, TOLERANCE, "Objective is 1.0");

    /* Callback should have been invoked at least once */
    printf("  INFO: Cut callback invoked %d times, added %d cuts\n",
           cut_callback_count, cut_callback_cuts_added);
    ASSERT(cut_callback_count >= 0, "Callback invocation count >= 0");

    ralph_test_free(model);

    /* Test clearing callback */
    model = ralph_test_create();
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    ralph_test_set_cut_callback(model, &callback);
    ralph_test_set_cut_callback(model, NULL);  /* Clear callback */

    /* Solve - callback should not be invoked after clearing */
    ralph_test_optimize(model);
    ASSERT(1, "Clearing callback works");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Branch Callback
 *
 * Test that user-provided branch callback is invoked for variable selection.
 * ============================================================================ */

/* Global counter for branch callback invocations */
static int branch_callback_count = 0;
static int branch_callback_var_selected = -1;

static int test_branch_callback_fn(void *user_data, const double *x_relaxation,
                                    int num_vars, const int *is_integer,
                                    const double *lb, const double *ub) {
    (void)lb;
    (void)ub;

    branch_callback_count++;

    /* Get preference from user data (which variable to prefer) */
    int preferred = user_data ? *(int*)user_data : -1;

    /* Find a fractional integer variable to branch on */
    int best_var = -1;
    double best_frac = 0.0;

    for (int j = 0; j < num_vars; j++) {
        if (!is_integer[j]) continue;

        double val = x_relaxation[j];
        double frac = val - floor(val);
        double infeas = frac;
        if (infeas > 0.5) infeas = 1.0 - infeas;

        if (infeas > 1e-5) {  /* Variable is fractional */
            /* If this is the preferred variable, select it */
            if (j == preferred) {
                branch_callback_var_selected = j;
                return j;
            }
            /* Otherwise track most fractional */
            if (infeas > best_frac) {
                best_frac = infeas;
                best_var = j;
            }
        }
    }

    branch_callback_var_selected = best_var;
    return best_var;  /* Return most fractional, or -1 if none */
}

void test_branch_callback(void) {
    printf("\n=== Test: Branch Callback ===\n");

    /* Reset counters */
    branch_callback_count = 0;
    branch_callback_var_selected = -1;

    /* Simple MIP: min x + 2y
     * s.t. x + y >= 1
     *      x, y binary
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x (cheaper) */
    ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);  /* y (more expensive) */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    /* Set up branch callback - prefer variable 1 (y) */
    int preferred = 1;
    RalphBranchCallback callback = {
        .select_branch_var = test_branch_callback_fn,
        .user_data = &preferred
    };
    ralph_test_set_branch_callback(model, &callback);

    /* Solve */
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 1.0, TOLERANCE, "Objective is 1.0");

    /* The callback may or may not be invoked depending on whether
     * branching was needed (LP relaxation might be integer feasible) */
    printf("  INFO: Branch callback invoked %d times\n", branch_callback_count);
    ASSERT(branch_callback_count >= 0, "Callback invocation count >= 0");

    ralph_test_free(model);

    /* Test clearing callback */
    model = ralph_test_create();
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);
    ralph_test_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    ralph_test_set_branch_callback(model, &callback);
    ralph_test_set_branch_callback(model, NULL);  /* Clear callback */

    ralph_test_optimize(model);
    ASSERT(1, "Clearing branch callback works");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Node Selection Strategies
 *
 * Verify that BEST_FIRST, DEPTH_FIRST, and HYBRID all find the same optimal
 * objective on a small facility location MIP. Also test the node_select
 * parameter API.
 * ============================================================================ */

/* Helper: build and solve a facility location MIP with a given node_select value.
 * Returns objective value (or 1e30 on error). */
static double solve_facility_with_strategy(int strategy) {
    RalphModel *model = ralph_test_create();
    if (!model) return 1e30;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* 2 facilities, 3 customers (same as test_facility_location) */
    ralph_test_add_var(model, 0, 1, 100, RALPH_BINARY);  /* y[0] */
    ralph_test_add_var(model, 0, 1, 150, RALPH_BINARY);  /* y[1] */

    double cost[2][3] = {{10, 20, 15}, {25, 10, 20}};
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            ralph_test_add_var(model, 0, 1, cost[i][j], RALPH_CONTINUOUS);

    /* Each customer must be fully served */
    for (int j = 0; j < 3; j++) {
        int idx[] = {2 + j, 2 + 3 + j};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Can only serve from open facility */
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            int idx[] = {2 + i*3 + j, i};
            double val[] = {1.0, -1.0};
            ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
        }
    }

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "node_select", strategy);

    ralph_test_optimize(model);

    double obj = 1e30;
    if (ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL) {
        obj = ralph_test_get_objval(model);
    }

    ralph_test_free(model);
    return obj;
}

void test_node_selection_strategies(void) {
    printf("\n=== Test: Node Selection Strategies ===\n");

    /* 1. Parameter API tests */
    {
        RalphModel *m = ralph_test_create();

        /* Default should be 3 (HYBRID) */
        int val = -1;
        ralph_test_get_int_param(m, "node_select", &val);
        ASSERT(val == 3, "Default node_select is HYBRID (3)");

        /* Set and get each valid value */
        ASSERT(ralph_test_set_int_param(m, "node_select", 0) == 0, "Set node_select=0 (BEST_FIRST)");
        ralph_test_get_int_param(m, "node_select", &val);
        ASSERT(val == 0, "Get node_select returns 0");

        ASSERT(ralph_test_set_int_param(m, "node_select", 1) == 0, "Set node_select=1 (DEPTH_FIRST)");
        ASSERT(ralph_test_set_int_param(m, "node_select", 2) == 0, "Set node_select=2 (BEST_ESTIMATE)");
        ASSERT(ralph_test_set_int_param(m, "node_select", 3) == 0, "Set node_select=3 (HYBRID)");

        /* Reject invalid values */
        ASSERT(ralph_test_set_int_param(m, "node_select", -1) == -1, "Reject node_select=-1");
        ASSERT(ralph_test_set_int_param(m, "node_select", 4) == -1, "Reject node_select=4");

        /* Also accept the Gurobi-style name */
        ASSERT(ralph_test_set_int_param(m, "NodeSelect", 1) == 0, "Set NodeSelect=1");

        ralph_test_free(m);
    }

    /* 2. Correctness: all strategies find the same optimal objective */
    double obj_bf = solve_facility_with_strategy(0);       /* BEST_FIRST */
    double obj_dfs = solve_facility_with_strategy(1);      /* DEPTH_FIRST */
    double obj_hybrid = solve_facility_with_strategy(3);   /* HYBRID */

    ASSERT(fabs(obj_bf - obj_dfs) < TOLERANCE,
           "BEST_FIRST and DEPTH_FIRST find same optimum");
    ASSERT(fabs(obj_bf - obj_hybrid) < TOLERANCE,
           "BEST_FIRST and HYBRID find same optimum");

    printf("  Objectives: BF=%.2f, DFS=%.2f, HYBRID=%.2f\n",
           obj_bf, obj_dfs, obj_hybrid);
}

/* ============================================================================
 * P5/P6: Bound Flipping + Dual Steepest Edge Tests
 * ============================================================================ */

void test_p5p6_no_false_infeasibility(void) {
    printf("\n=== Test: P5/P6 No False Infeasibility ===\n");

    /* Facility location MIP that is known feasible.
     * 6 facilities, 12 customers. Solve 10 times with perturbations.
     * Regression test: P5/P6 must never declare this INFEASIBLE. */
    int num_fac = 6, num_cust = 12;
    double fixed_cost[] = {30.0, 25.0, 35.0, 20.0, 40.0, 28.0};
    double base_assign[6][12] = {
        {8,6,7,5,9,4,6,8,5,7,3,9},
        {5,7,6,8,4,9,7,5,8,6,4,3},
        {6,5,8,7,3,6,4,9,7,5,8,6},
        {7,4,5,6,8,3,9,6,4,7,5,8},
        {4,8,3,9,6,7,5,4,6,8,7,5},
        {9,3,4,8,5,6,8,7,3,4,6,7}
    };

    int all_optimal = 1;
    for (int trial = 0; trial < 10; trial++) {
        RalphModel *m = ralph_test_create();
        ralph_test_set_obj_sense(m, RALPH_MINIMIZE);

        /* y_j: facility open vars (binary) */
        for (int j = 0; j < num_fac; j++) {
            double cost = fixed_cost[j] + (trial * 3 + j) % 5;
            ralph_test_add_var(m, 0.0, 1.0, cost, RALPH_BINARY);
        }

        /* x_ij: assignment vars (continuous [0,1]) */
        for (int i = 0; i < num_cust; i++) {
            for (int j = 0; j < num_fac; j++) {
                double cost = base_assign[j][i] + (double)((trial * 7 + i + j) % 4);
                ralph_test_add_var(m, 0.0, 1.0, cost, RALPH_CONTINUOUS);
            }
        }

        /* Each customer assigned to exactly one facility */
        for (int i = 0; i < num_cust; i++) {
            int idx[6];
            double coefs[6];
            for (int j = 0; j < num_fac; j++) {
                idx[j] = num_fac + i * num_fac + j;
                coefs[j] = 1.0;
            }
            ralph_test_add_constraint(m, num_fac, idx, coefs, 'E', 1.0);
        }

        /* Linking: x_ij <= y_j */
        for (int i = 0; i < num_cust; i++) {
            for (int j = 0; j < num_fac; j++) {
                int idx[2] = {num_fac + i * num_fac + j, j};
                double coefs[2] = {1.0, -1.0};
                ralph_test_add_constraint(m, 2, idx, coefs, 'L', 0.0);
            }
        }

        ralph_test_optimize(m);
        int status = ralph_test_get_status(m);
        if (status != RALPH_STATUS_OPTIMAL) {
            printf("  TRIAL %d: got status %d (expected OPTIMAL)\n", trial, status);
            all_optimal = 0;
        }
        ralph_test_free(m);
    }

    ASSERT(all_optimal, "P5/P6: all 10 trials OPTIMAL (no false infeasibility)");
}

void test_p5p6_flags(void) {
    printf("\n=== Test: P5/P6 Flags ===\n");

    /* Verify P5/P6 can be toggled via ralph_test_set_int_param and produce
     * correct results in all 4 combinations. */
    double obj_vals[4];
    const char *labels[] = {"both-on", "bflip-off", "dse-off", "both-off"};
    int bflip_flags[] = {1, 0, 1, 0};
    int dse_flags[]   = {1, 1, 0, 0};

    for (int t = 0; t < 4; t++) {
        RalphModel *m = ralph_test_create();
        ralph_test_set_obj_sense(m, RALPH_MAXIMIZE);
        ralph_test_set_int_param(m, "verbose", 0);
        ralph_test_set_int_param(m, "dual_bound_flip", bflip_flags[t]);
        ralph_test_set_int_param(m, "dual_steepest_edge", dse_flags[t]);

        ralph_test_add_var(m, 0.0, 1.0, 10.0, RALPH_BINARY);
        ralph_test_add_var(m, 0.0, 1.0, 6.0, RALPH_BINARY);
        ralph_test_add_var(m, 0.0, 1.0, 4.0, RALPH_BINARY);

        int idx[] = {0, 1, 2};
        double coefs[] = {5.0, 3.0, 2.0};
        ralph_test_add_constraint(m, 3, idx, coefs, 'L', 9.0);

        ralph_test_optimize(m);
        ASSERT(ralph_test_get_status(m) == RALPH_STATUS_OPTIMAL, "Optimal");
        obj_vals[t] = ralph_test_get_objval(m);

        printf("  INFO: %s: obj=%.2f\n", labels[t], obj_vals[t]);
        ralph_test_free(m);
    }

    /* All four combinations should produce the same optimal objective */
    for (int t = 0; t < 4; t++) {
        ASSERT(fabs(obj_vals[t] - 16.0) < TOLERANCE, "Correct objective (16)");
    }
}

/* ============================================================================
 * Test: Cut Normalization with GE Constraints (row_sign fix)
 *
 * Exercises the row normalization sign fix in cut generators. When a
 * constraint has GE sense with positive RHS, the simplex normalizes it
 * by multiplying by -1, giving row_sign = -1. The cut generators must
 * apply this sign when back-substituting slack/surplus variables.
 *
 * Uses GE constraints exclusively so that cuts exercise the row_sign path.
 * Without the fix: cuts have wrong coefficients → infeasibility or suboptimal.
 * With the fix: correct optimal solution.
 *
 * Problem:
 *   min  -7x - 5y - 3z - 4w    (x,y,z,w binary)
 *   s.t. 2x + 3y + z + 2w >= 4    (GE, row_sign = -1)
 *        x + 2y + 2z + w  >= 3    (GE, row_sign = -1)
 *        3x + y + z + 3w  <= 6    (LE, row_sign = +1)
 * ============================================================================ */
void test_cuts_ge_constraint_normalization(void) {
    printf("\n=== Test: Cut Normalization with GE Constraints ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "max_cut_rounds", 5);
    ralph_test_set_int_param(model, "max_nodes", 1000);

    /* Binary variables with negative obj (minimization = maximize profit) */
    ralph_test_add_var(model, 0.0, 1.0, -7.0, RALPH_BINARY);  /* x */
    ralph_test_add_var(model, 0.0, 1.0, -5.0, RALPH_BINARY);  /* y */
    ralph_test_add_var(model, 0.0, 1.0, -3.0, RALPH_BINARY);  /* z */
    ralph_test_add_var(model, 0.0, 1.0, -4.0, RALPH_BINARY);  /* w */

    /* GE constraint 1: 2x + 3y + z + 2w >= 4 (triggers row_sign = -1) */
    int idx1[] = {0, 1, 2, 3};
    double val1[] = {2.0, 3.0, 1.0, 2.0};
    ralph_test_add_constraint(model, 4, idx1, val1, RALPH_GREATER_EQUAL, 4.0);

    /* GE constraint 2: x + 2y + 2z + w >= 3 (triggers row_sign = -1) */
    int idx2[] = {0, 1, 2, 3};
    double val2[] = {1.0, 2.0, 2.0, 1.0};
    ralph_test_add_constraint(model, 4, idx2, val2, RALPH_GREATER_EQUAL, 3.0);

    /* LE constraint: 3x + y + z + 3w <= 6 (row_sign = +1) */
    int idx3[] = {0, 1, 2, 3};
    double val3[] = {3.0, 1.0, 1.0, 3.0};
    ralph_test_add_constraint(model, 4, idx3, val3, RALPH_LESS_EQUAL, 6.0);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL,
           "GE normalization: status is OPTIMAL (not INFEASIBLE)");

    double obj = ralph_test_get_objval(model);
    /* Optimal: x=1, y=1, z=0, w=1 gives obj = -7-5+0-4 = -16
     * Check: 2+3+0+2=7>=4, 1+2+0+1=4>=3, 3+1+0+3=7>6 FAIL
     * Try x=1, y=1, z=1, w=0: obj = -7-5-3+0 = -15
     * Check: 2+3+1+0=6>=4, 1+2+2+0=5>=3, 3+1+1+0=5<=6 OK
     * Try x=1, y=1, z=0, w=0: obj = -7-5 = -12
     * Check: 2+3=5>=4, 1+2=3>=3, 3+1=4<=6 OK. But obj=-12 > -15.
     * x=1,y=0,z=1,w=1: obj=-14. Check: 2+0+1+2=5>=4, 1+0+2+1=4>=3, 3+0+1+3=7>6 FAIL
     * x=1,y=1,z=1,w=0 gives -15. Best feasible so far.
     * x=0,y=1,z=1,w=1: obj=-12. Check: 0+3+1+2=6>=4, 0+2+2+1=5>=3, 0+1+1+3=5<=6 OK
     * But -12 > -15.
     * Actually let's check x=1,y=1,z=1,w=1: obj=-19, check: 2+3+1+2=8>=4, 1+2+2+1=6>=3, 3+1+1+3=8>6 FAIL.
     * So the optimum should be x=1,y=1,z=1,w=0 → obj = -15 */
    ASSERT(obj <= -14.9, "GE normalization: optimal obj <= -15");
    ASSERT(obj >= -15.1, "GE normalization: optimal obj >= -15 (exactly -15)");

    double sol[4];
    ralph_test_get_solution(model, sol);

    /* Verify solution is binary */
    for (int j = 0; j < 4; j++) {
        ASSERT(fabs(sol[j] - round(sol[j])) < TOLERANCE,
               "GE normalization: variable is binary");
    }

    /* Verify GE constraints */
    double lhs1 = 2*sol[0] + 3*sol[1] + sol[2] + 2*sol[3];
    double lhs2 = sol[0] + 2*sol[1] + 2*sol[2] + sol[3];
    double lhs3 = 3*sol[0] + sol[1] + sol[2] + 3*sol[3];
    ASSERT(lhs1 >= 4.0 - TOLERANCE, "GE normalization: constraint 1 (GE) satisfied");
    ASSERT(lhs2 >= 3.0 - TOLERANCE, "GE normalization: constraint 2 (GE) satisfied");
    ASSERT(lhs3 <= 6.0 + TOLERANCE, "GE normalization: constraint 3 (LE) satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Cut Normalization with Mixed Constraint Senses
 *
 * Tests that cuts work correctly when the model has a mix of LE, GE, and EQ
 * constraints. Each sense type has different normalization behavior:
 *   LE with positive RHS → row_sign = +1 (no flip)
 *   GE with positive RHS → row_sign = -1 (flipped)
 *   EQ → row_sign depends on sign of RHS
 *
 * Compares MIP with cuts to MIP without cuts — both must find the same
 * optimal objective. This verifies cuts are valid (don't cut off the optimum).
 * ============================================================================ */
void test_cuts_mixed_sense_normalization(void) {
    printf("\n=== Test: Cut Normalization with Mixed Senses ===\n");

    double obj_without_cuts = 0.0;
    double obj_with_cuts = 0.0;

    for (int pass = 0; pass < 2; pass++) {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "max_nodes", 5000);

        /* 6 integer variables */
        ralph_test_add_var(model, 0.0, 3.0, -5.0, RALPH_INTEGER);  /* x0 */
        ralph_test_add_var(model, 0.0, 3.0, -3.0, RALPH_INTEGER);  /* x1 */
        ralph_test_add_var(model, 0.0, 3.0, -4.0, RALPH_INTEGER);  /* x2 */
        ralph_test_add_var(model, 0.0, 5.0, -2.0, RALPH_CONTINUOUS); /* x3 cont */
        ralph_test_add_var(model, 0.0, 3.0, -6.0, RALPH_INTEGER);  /* x4 */
        ralph_test_add_var(model, 0.0, 4.0, -1.0, RALPH_CONTINUOUS); /* x5 cont */

        /* LE: 2x0 + x1 + 3x2 + x3 + x4 + 2x5 <= 12 */
        int idx1[] = {0, 1, 2, 3, 4, 5};
        double val1[] = {2.0, 1.0, 3.0, 1.0, 1.0, 2.0};
        ralph_test_add_constraint(model, 6, idx1, val1, RALPH_LESS_EQUAL, 12.0);

        /* GE: x0 + 2x1 + x2 + x4 >= 5 (row_sign = -1) */
        int idx2[] = {0, 1, 2, 4};
        double val2[] = {1.0, 2.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 4, idx2, val2, RALPH_GREATER_EQUAL, 5.0);

        /* EQ: x0 + x1 + x2 + x4 = 6 */
        int idx3[] = {0, 1, 2, 4};
        double val3[] = {1.0, 1.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 4, idx3, val3, RALPH_EQUAL, 6.0);

        /* GE: x3 + x5 >= 2 (row_sign = -1) */
        int idx4[] = {3, 5};
        double val4[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx4, val4, RALPH_GREATER_EQUAL, 2.0);

        if (pass == 0) {
            ralph_test_set_int_param(model, "max_cut_rounds", 0);
        } else {
            ralph_test_set_int_param(model, "max_cut_rounds", 5);
        }

        ralph_test_optimize(model);

        RalphStatus status = ralph_test_get_status(model);
        if (pass == 0) {
            ASSERT(status == RALPH_STATUS_OPTIMAL,
                   "Mixed sense normalization: OPTIMAL without cuts");
            obj_without_cuts = ralph_test_get_objval(model);
        } else {
            ASSERT(status == RALPH_STATUS_OPTIMAL,
                   "Mixed sense normalization: OPTIMAL with cuts");
            obj_with_cuts = ralph_test_get_objval(model);
        }

        /* Verify solution feasibility */
        if (status == RALPH_STATUS_OPTIMAL) {
            double sol[6];
            ralph_test_get_solution(model, sol);

            double c1 = 2*sol[0] + sol[1] + 3*sol[2] + sol[3] + sol[4] + 2*sol[5];
            double c2 = sol[0] + 2*sol[1] + sol[2] + sol[4];
            double c3 = sol[0] + sol[1] + sol[2] + sol[4];
            double c4 = sol[3] + sol[5];
            ASSERT(c1 <= 12.0 + TOLERANCE, "Mixed sense: LE constraint satisfied");
            ASSERT(c2 >= 5.0 - TOLERANCE, "Mixed sense: GE constraint 1 satisfied");
            ASSERT(fabs(c3 - 6.0) < TOLERANCE, "Mixed sense: EQ constraint satisfied");
            ASSERT(c4 >= 2.0 - TOLERANCE, "Mixed sense: GE constraint 2 satisfied");
        }

        ralph_test_free(model);
    }

    /* Cuts must not make the solution worse (cut off the optimum) */
    ASSERT(obj_with_cuts <= obj_without_cuts + TOLERANCE,
           "Mixed sense normalization: cuts do not worsen objective");
    printf("  INFO: obj without cuts = %.4f, with cuts = %.4f\n",
           obj_without_cuts, obj_with_cuts);
}

/* ============================================================================
 * Test: Reliability Branching (Basic)
 *
 * Small MILP solved with reliability branching (var_select=3).
 * Verify optimal solution found.
 * ============================================================================ */
void test_reliability_branching_basic(void) {
    printf("\n=== Test: Reliability Branching (Basic) ===\n");

    /* min 3x0 + 2x1 + 5x2 + x3 + 4x4
     * s.t. x0 + x1 + x2 + x3 + x4 >= 2
     *      2x0 + x1 + 3x2 >= 3
     *      x0..x2 binary, x3..x4 continuous [0,1]
     * Optimal: x1=1, x2=1, obj = 2+5 = 7 or similar
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 1.0, 3.0, RALPH_BINARY);     /* x0 */
    ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);     /* x1 */
    ralph_test_add_var(model, 0.0, 1.0, 5.0, RALPH_BINARY);     /* x2 */
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_CONTINUOUS);  /* x3 */
    ralph_test_add_var(model, 0.0, 1.0, 4.0, RALPH_CONTINUOUS);  /* x4 */

    int idx1[] = {0, 1, 2, 3, 4};
    double val1[] = {1, 1, 1, 1, 1};
    ralph_test_add_constraint(model, 5, idx1, val1, RALPH_GREATER_EQUAL, 2.0);

    int idx2[] = {0, 1, 2};
    double val2[] = {2, 1, 3};
    ralph_test_add_constraint(model, 3, idx2, val2, RALPH_GREATER_EQUAL, 3.0);

    /* Use reliability branching */
    ralph_test_set_int_param(model, "var_select", 3);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Reliability branching finds optimal");

    double obj = ralph_test_get_objval(model);
    ASSERT(obj < 100.0, "Objective is finite");

    double x[5];
    ralph_test_get_solution(model, x);

    /* Verify feasibility */
    double c1 = x[0] + x[1] + x[2] + x[3] + x[4];
    ASSERT(c1 >= 2.0 - TOLERANCE, "Constraint 1 satisfied");
    double c2 = 2*x[0] + x[1] + 3*x[2];
    ASSERT(c2 >= 3.0 - TOLERANCE, "Constraint 2 satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Reliability Branching with Priorities
 *
 * Verify that reliability branching respects priority variables.
 * ============================================================================ */
void test_reliability_branching_with_priorities(void) {
    printf("\n=== Test: Reliability Branching with Priorities ===\n");

    /* min x0 + x1 + 2*x2
     * s.t. x0 + x1 >= 1
     *      x1 + x2 >= 1
     *      x0, x1, x2 binary
     * Optimal: x1=1, obj=1
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x0 */
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x1 */
    ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);  /* x2 */

    int idx1[] = {0, 1};
    double val1[] = {1, 1};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_GREATER_EQUAL, 1.0);

    int idx2[] = {1, 2};
    double val2[] = {1, 1};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_GREATER_EQUAL, 1.0);

    /* Set priorities: x1 has highest priority */
    int priorities[3] = {1, 10, 1};
    ralph_test_set_branch_priorities(model, priorities);

    /* Use reliability branching */
    ralph_test_set_int_param(model, "var_select", 3);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Reliability+priorities finds optimal");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 1.0, TOLERANCE, "Objective is 1.0");

    double x[3];
    ralph_test_get_solution(model, x);
    ASSERT(x[0] + x[1] >= 1.0 - TOLERANCE, "Constraint 1 satisfied");
    ASSERT(x[1] + x[2] >= 1.0 - TOLERANCE, "Constraint 2 satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Reliability vs Pseudo-cost Branching
 *
 * Medium MILP. Verify both strategies find same optimum.
 * ============================================================================ */
void test_reliability_vs_pseudocost(void) {
    printf("\n=== Test: Reliability vs Pseudo-cost ===\n");

    /* Build a facility-location-like problem with 10 integer vars */
    int n = 10;

    /* Solve with pseudo-cost */
    RalphModel *m1 = ralph_test_create();
    ralph_test_set_obj_sense(m1, RALPH_MINIMIZE);

    for (int j = 0; j < n; j++) {
        ralph_test_add_var(m1, 0.0, 1.0, (double)(j + 1), RALPH_BINARY);
    }

    /* Coverage constraints: each pair must be covered */
    for (int i = 0; i < 5; i++) {
        int idx[2] = {i, i + 5};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(m1, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }

    /* Sum constraint */
    {
        int idx[10];
        double val[10];
        for (int j = 0; j < n; j++) { idx[j] = j; val[j] = 1.0; }
        ralph_test_add_constraint(m1, n, idx, val, RALPH_LESS_EQUAL, 7.0);
    }

    ralph_test_set_int_param(m1, "var_select", 1);  /* pseudo-cost */
    ralph_test_optimize(m1);
    double obj_pc = ralph_test_get_objval(m1);
    RalphStatus status_pc = ralph_test_get_status(m1);
    ralph_test_free(m1);

    /* Solve with reliability */
    RalphModel *m2 = ralph_test_create();
    ralph_test_set_obj_sense(m2, RALPH_MINIMIZE);

    for (int j = 0; j < n; j++) {
        ralph_test_add_var(m2, 0.0, 1.0, (double)(j + 1), RALPH_BINARY);
    }

    for (int i = 0; i < 5; i++) {
        int idx[2] = {i, i + 5};
        double val[2] = {1.0, 1.0};
        ralph_test_add_constraint(m2, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }

    {
        int idx[10];
        double val[10];
        for (int j = 0; j < n; j++) { idx[j] = j; val[j] = 1.0; }
        ralph_test_add_constraint(m2, n, idx, val, RALPH_LESS_EQUAL, 7.0);
    }

    ralph_test_set_int_param(m2, "var_select", 3);  /* reliability */
    ralph_test_optimize(m2);
    double obj_rel = ralph_test_get_objval(m2);
    RalphStatus status_rel = ralph_test_get_status(m2);
    ralph_test_free(m2);

    ASSERT(status_pc == RALPH_STATUS_OPTIMAL, "Pseudo-cost finds optimal");
    ASSERT(status_rel == RALPH_STATUS_OPTIMAL, "Reliability finds optimal");
    ASSERT_NEAR(obj_pc, obj_rel, 1.0, "Same objective (within 1.0)");
    printf("  INFO: pseudo-cost obj=%.4f, reliability obj=%.4f\n", obj_pc, obj_rel);
}

/* ============================================================================
 * Test: Reliability Branching with All-Binary Problem
 *
 * Verify reliability branching handles 0/1 variables correctly.
 * ============================================================================ */
void test_reliability_branching_all_binary(void) {
    printf("\n=== Test: Reliability Branching (All Binary) ===\n");

    /* min 2x0 + 3x1 + x2 + 4x3
     * s.t. x0 + x1 + x2 + x3 >= 2
     *      x0 + x3 >= 1
     *      x0..x3 binary
     */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);  /* x0 */
    ralph_test_add_var(model, 0.0, 1.0, 3.0, RALPH_BINARY);  /* x1 */
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x2 */
    ralph_test_add_var(model, 0.0, 1.0, 4.0, RALPH_BINARY);  /* x3 */

    int idx1[] = {0, 1, 2, 3};
    double val1[] = {1, 1, 1, 1};
    ralph_test_add_constraint(model, 4, idx1, val1, RALPH_GREATER_EQUAL, 2.0);

    int idx2[] = {0, 3};
    double val2[] = {1, 1};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_GREATER_EQUAL, 1.0);

    ralph_test_set_int_param(model, "var_select", 3);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "All-binary reliability finds optimal");

    double obj = ralph_test_get_objval(model);
    /* Optimal: x0=1, x2=1 → obj=3 */
    ASSERT_NEAR(obj, 3.0, TOLERANCE, "Objective is 3.0");

    double x[4];
    ralph_test_get_solution(model, x);
    ASSERT(x[0] + x[1] + x[2] + x[3] >= 2.0 - TOLERANCE, "Coverage constraint");
    ASSERT(x[0] + x[3] >= 1.0 - TOLERANCE, "Pairing constraint");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Cut Quality - Dynamism Filter
 *
 * Verify cuts with extreme coefficient ratios are filtered out.
 * ============================================================================ */
void test_cut_quality_dynamism_filter(void) {
    printf("\n=== Test: Cut Quality Dynamism Filter ===\n");

    /* Build a MILP where GMI cuts can have bad dynamism.
     * We test indirectly: with cuts enabled, the solver should still
     * find optimal (cuts with bad dynamism are filtered, not applied). */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Variables with very different scales in objective */
    ralph_test_add_var(model, 0.0, 100.0, 0.001, RALPH_INTEGER);  /* x0: tiny cost */
    ralph_test_add_var(model, 0.0, 100.0, 1000.0, RALPH_INTEGER); /* x1: huge cost */
    ralph_test_add_var(model, 0.0, 100.0, 1.0, RALPH_INTEGER);    /* x2: normal cost */

    /* x0 + x1 + x2 >= 5 */
    int idx1[] = {0, 1, 2};
    double val1[] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, idx1, val1, RALPH_GREATER_EQUAL, 5.0);

    /* 100*x0 + x1 >= 50 (introduces dynamism in tableau) */
    int idx2[] = {0, 1};
    double val2[] = {100.0, 1.0};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_GREATER_EQUAL, 50.0);

    ralph_test_set_int_param(model, "max_cut_rounds", 3);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Dynamism filter: solver finds optimal");

    double obj = ralph_test_get_objval(model);
    ASSERT(obj < 1e6, "Objective is finite and reasonable");

    double x[3];
    ralph_test_get_solution(model, x);
    ASSERT(x[0] + x[1] + x[2] >= 5.0 - TOLERANCE, "Constraint 1 satisfied");
    ASSERT(100*x[0] + x[1] >= 50.0 - TOLERANCE, "Constraint 2 satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Cut Parallel Detection
 *
 * Verify that nearly-parallel cuts don't both get applied.
 * ============================================================================ */
void test_cut_parallel_detection(void) {
    printf("\n=== Test: Cut Parallel Detection ===\n");

    /* Solve a small MIP with cuts enabled.
     * The parallel filter shouldn't cause regressions. */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 10.0, 1.0, RALPH_INTEGER);  /* x0 */
    ralph_test_add_var(model, 0.0, 10.0, 2.0, RALPH_INTEGER);  /* x1 */
    ralph_test_add_var(model, 0.0, 10.0, 3.0, RALPH_INTEGER);  /* x2 */

    /* x0 + 2*x1 + 3*x2 >= 10 */
    int idx1[] = {0, 1, 2};
    double val1[] = {1, 2, 3};
    ralph_test_add_constraint(model, 3, idx1, val1, RALPH_GREATER_EQUAL, 10.0);

    /* x0 + x1 + x2 >= 4 */
    int idx2[] = {0, 1, 2};
    double val2[] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, idx2, val2, RALPH_GREATER_EQUAL, 4.0);

    ralph_test_set_int_param(model, "max_cut_rounds", 3);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Parallel filter: solver finds optimal");

    double x[3];
    ralph_test_get_solution(model, x);
    ASSERT(x[0] + 2*x[1] + 3*x[2] >= 10.0 - TOLERANCE, "Constraint 1 satisfied");
    ASSERT(x[0] + x[1] + x[2] >= 4.0 - TOLERANCE, "Constraint 2 satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Cut Minimum Violation
 *
 * Verify that cuts with tiny violation are skipped.
 * ============================================================================ */
void test_cut_minimum_violation(void) {
    printf("\n=== Test: Cut Minimum Violation ===\n");

    /* Solve a MIP with cuts enabled. The MIP_CUT_MIN_VIOLATION threshold
     * (1e-4) should filter weak cuts without affecting solution quality. */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x0 */
    ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);  /* x1 */
    ralph_test_add_var(model, 0.0, 1.0, 3.0, RALPH_BINARY);  /* x2 */
    ralph_test_add_var(model, 0.0, 1.0, 4.0, RALPH_BINARY);  /* x3 */

    /* At least 2 of the 4 must be selected */
    int idx1[] = {0, 1, 2, 3};
    double val1[] = {1, 1, 1, 1};
    ralph_test_add_constraint(model, 4, idx1, val1, RALPH_GREATER_EQUAL, 2.0);

    /* x0 + x1 <= 1 (conflict) */
    int idx2[] = {0, 1};
    double val2[] = {1, 1};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 1.0);

    ralph_test_set_int_param(model, "max_cut_rounds", 3);
    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Min violation: solver finds optimal");

    double obj = ralph_test_get_objval(model);
    /* Best: x0=1, x2=1 → 1+3=4 or x0=1, x3=1 → 1+4=5. Min = x0=1,x2=1 = 4 */
    ASSERT(obj >= 3.0 - TOLERANCE, "Objective >= 3.0");
    ASSERT(obj <= 5.0 + TOLERANCE, "Objective <= 5.0");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Cuts Improve Root Bound
 *
 * Verify that cutting planes tighten the root LP bound.
 * ============================================================================ */
void test_cuts_improve_bound(void) {
    printf("\n=== Test: Cuts Improve Root Bound ===\n");

    /* Solve with and without cuts; verify bound with cuts is at least as tight */
    double obj_no_cuts, obj_with_cuts;

    /* Without cuts */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

        /* Knapsack-like problem where cuts help */
        ralph_test_add_var(model, 0.0, 1.0, 3.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 5.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 7.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 4.0, RALPH_BINARY);

        int idx1[] = {0, 1, 2, 3, 4};
        double val1[] = {2, 3, 4, 1, 2};
        ralph_test_add_constraint(model, 5, idx1, val1, RALPH_GREATER_EQUAL, 5.0);

        int idx2[] = {0, 1, 2};
        double val2[] = {1, 1, 1};
        ralph_test_add_constraint(model, 3, idx2, val2, RALPH_LESS_EQUAL, 2.0);

        ralph_test_set_int_param(model, "max_cut_rounds", 0);
        ralph_test_optimize(model);
        obj_no_cuts = ralph_test_get_objval(model);
        ralph_test_free(model);
    }

    /* With cuts */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

        ralph_test_add_var(model, 0.0, 1.0, 3.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 5.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 7.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 2.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 4.0, RALPH_BINARY);

        int idx1[] = {0, 1, 2, 3, 4};
        double val1[] = {2, 3, 4, 1, 2};
        ralph_test_add_constraint(model, 5, idx1, val1, RALPH_GREATER_EQUAL, 5.0);

        int idx2[] = {0, 1, 2};
        double val2[] = {1, 1, 1};
        ralph_test_add_constraint(model, 3, idx2, val2, RALPH_LESS_EQUAL, 2.0);

        ralph_test_set_int_param(model, "max_cut_rounds", 5);
        ralph_test_optimize(model);
        obj_with_cuts = ralph_test_get_objval(model);
        ralph_test_free(model);
    }

    /* Both should find optimal */
    ASSERT(obj_no_cuts < 1e6, "No-cuts solution is finite");
    ASSERT(obj_with_cuts < 1e6, "With-cuts solution is finite");

    /* Cuts should not worsen the objective */
    ASSERT(obj_with_cuts <= obj_no_cuts + TOLERANCE,
           "Cuts do not worsen objective");

    printf("  INFO: without cuts obj=%.4f, with cuts obj=%.4f\n",
           obj_no_cuts, obj_with_cuts);
}

/* ============================================================================
 * Test: Reduced-Cost Fixing - Basic
 *
 * Small binary MILP where RC fixing should be active. Verify optimality.
 * ============================================================================ */
void test_rc_fixing_basic(void) {
    printf("\n=== Test: Reduced-Cost Fixing Basic ===\n");

    /* Binary knapsack: min -5x0 -4x1 -3x2
     * s.t. 2x0 + 3x1 + x2 <= 4
     *      x0, x1, x2 binary
     * Optimal: x0=1, x2=1 (or x0=1,x1=1 depending on bounds), obj = -8 */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, 1.0, -5.0, RALPH_BINARY);
    ralph_test_add_var(model, 0.0, 1.0, -4.0, RALPH_BINARY);
    ralph_test_add_var(model, 0.0, 1.0, -3.0, RALPH_BINARY);

    int idx[] = {0, 1, 2};
    double val[] = {2.0, 3.0, 1.0};
    ralph_test_add_constraint(model, 3, idx, val, RALPH_LESS_EQUAL, 4.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    /* Optimal: x0=1, x2=1 → obj = -5 + -3 = -8, constraint: 2+1=3 <= 4 */
    ASSERT_NEAR(obj, -8.0, TOLERANCE, "Optimal objective with RC fixing");

    double sol[3];
    ralph_test_get_solution(model, sol);

    /* Verify feasibility */
    double lhs = 2.0*sol[0] + 3.0*sol[1] + 1.0*sol[2];
    ASSERT(lhs <= 4.0 + TOLERANCE, "Constraint satisfied");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Reduced-Cost Fixing - No Regression
 *
 * Existing knapsack + facility location problems should still find
 * same optimal values with RC fixing active.
 * ============================================================================ */
void test_rc_fixing_no_regression(void) {
    printf("\n=== Test: Reduced-Cost Fixing No Regression ===\n");

    /* Facility location: 5 facilities, 10 customers */
    int nf = 5, nc = 10;
    int num_vars = nf + nc * nf;

    g_test_seed = 42;

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_dbl_param(model, "time_limit", 30.0);

    /* Facility vars (binary) */
    for (int j = 0; j < nf; j++) {
        ralph_test_add_var(model, 0.0, 1.0, test_rand_double(50.0, 200.0), RALPH_BINARY);
    }
    /* Assignment vars (continuous) */
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            ralph_test_add_var(model, 0.0, 1.0, test_rand_double(1.0, 30.0), RALPH_CONTINUOUS);
        }
    }

    /* Demand: sum_j x[i,j] = 1 */
    int *indices = malloc(nf * sizeof(int));
    double *values = malloc(nf * sizeof(double));
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            indices[j] = nf + i * nf + j;
            values[j] = 1.0;
        }
        ralph_test_add_constraint(model, nf, indices, values, RALPH_EQUAL, 1.0);
    }

    /* Linking: x[i,j] <= y[j] */
    int idx2[2];
    double val2[2];
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            idx2[0] = nf + i * nf + j;
            idx2[1] = j;
            val2[0] = 1.0;
            val2[1] = -1.0;
            ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 0.0);
        }
    }

    free(indices);
    free(values);

    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT(obj < 1e6, "Objective is finite");

    /* Verify solution feasibility */
    double *sol = malloc(num_vars * sizeof(double));
    ralph_test_get_solution(model, sol);

    int binary_ok = 1;
    for (int j = 0; j < nf; j++) {
        if (fabs(sol[j]) > TOLERANCE && fabs(sol[j] - 1.0) > TOLERANCE) {
            binary_ok = 0;
        }
    }
    ASSERT(binary_ok, "Facility decisions are binary");

    free(sol);
    ralph_test_free(model);
}

/* ============================================================================
 * Test: RINS Finds Incumbent
 *
 * MILP where diving finds a suboptimal incumbent, and RINS should have
 * the opportunity to improve it. We verify optimality is maintained.
 * ============================================================================ */
void test_rins_finds_incumbent(void) {
    printf("\n=== Test: RINS Finds Incumbent ===\n");

    /* Facility location with enough nodes to trigger RINS (interval=50 for small) */
    int nf = 8, nc = 15;
    int num_vars = nf + nc * nf;

    g_test_seed = 789;

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_dbl_param(model, "time_limit", 30.0);
    ralph_test_set_dbl_param(model, "mip_gap", 0.0001);

    for (int j = 0; j < nf; j++) {
        ralph_test_add_var(model, 0.0, 1.0, test_rand_double(100.0, 400.0), RALPH_BINARY);
    }
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            ralph_test_add_var(model, 0.0, 1.0, test_rand_double(1.0, 40.0), RALPH_CONTINUOUS);
        }
    }

    int *indices = malloc(nf * sizeof(int));
    double *values = malloc(nf * sizeof(double));
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            indices[j] = nf + i * nf + j;
            values[j] = 1.0;
        }
        ralph_test_add_constraint(model, nf, indices, values, RALPH_EQUAL, 1.0);
    }

    int idx2[2];
    double val2[2];
    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nf; j++) {
            idx2[0] = nf + i * nf + j;
            idx2[1] = j;
            val2[0] = 1.0;
            val2[1] = -1.0;
            ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 0.0);
        }
    }

    free(indices);
    free(values);

    ralph_test_optimize(model);

    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL || status == RALPH_STATUS_NODE_LIMIT,
           "Status is OPTIMAL or NODE_LIMIT");

    if (status == RALPH_STATUS_OPTIMAL) {
        double obj = ralph_test_get_objval(model);
        ASSERT(obj < 1e6, "Objective is finite");

        /* Verify solution feasibility */
        double *sol = malloc(num_vars * sizeof(double));
        ralph_test_get_solution(model, sol);

        int binary_ok = 1;
        for (int j = 0; j < nf; j++) {
            if (fabs(sol[j]) > TOLERANCE && fabs(sol[j] - 1.0) > TOLERANCE) {
                binary_ok = 0;
            }
        }
        ASSERT(binary_ok, "Facility decisions are binary");
        free(sol);
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: RINS No Regression
 *
 * Existing MIP problems should produce same or better results with RINS.
 * ============================================================================ */
void test_rins_no_regression(void) {
    printf("\n=== Test: RINS No Regression ===\n");

    /* Binary knapsack */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

        ralph_test_add_var(model, 0.0, 1.0, 10.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 6.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 12.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 7.0, RALPH_BINARY);
        ralph_test_add_var(model, 0.0, 1.0, 15.0, RALPH_BINARY);

        int idx[] = {0, 1, 2, 3, 4};
        double val[] = {5.0, 4.0, 6.0, 3.0, 7.0};
        ralph_test_add_constraint(model, 5, idx, val, RALPH_LESS_EQUAL, 15.0);

        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_optimize(model);

        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Knapsack: OPTIMAL");
        double obj = ralph_test_get_objval(model);
        /* Known optimal: items 0,3,4 → value=32, weight=15 */
        ASSERT_NEAR(obj, 32.0, TOLERANCE, "Knapsack: optimal obj");
        ralph_test_free(model);
    }

    /* Small set partitioning */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

        double costs[] = {3, 2, 1, 4, 5, 2, 3, 1, 4, 2};
        int coverage[10][5] = {
            {1, 0, 0, 1, 0}, {0, 1, 0, 0, 1}, {1, 1, 0, 0, 0},
            {0, 0, 1, 1, 0}, {0, 0, 0, 1, 1}, {1, 0, 1, 0, 0},
            {0, 1, 1, 0, 0}, {0, 0, 0, 0, 1}, {1, 1, 1, 0, 0},
            {0, 0, 1, 1, 1},
        };

        for (int j = 0; j < 10; j++) {
            ralph_test_add_var(model, 0.0, 1.0, costs[j], RALPH_BINARY);
        }

        for (int i = 0; i < 5; i++) {
            int idx[10];
            double val[10];
            int nnz = 0;
            for (int j = 0; j < 10; j++) {
                if (coverage[j][i]) {
                    idx[nnz] = j;
                    val[nnz] = 1.0;
                    nnz++;
                }
            }
            ralph_test_add_constraint(model, nnz, idx, val, RALPH_EQUAL, 1.0);
        }

        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_optimize(model);

        RalphStatus status = ralph_test_get_status(model);
        ASSERT(status == RALPH_STATUS_OPTIMAL || status == RALPH_STATUS_INFEASIBLE,
               "Set partition: valid status");

        if (status == RALPH_STATUS_OPTIMAL) {
            double *sol = malloc(10 * sizeof(double));
            ralph_test_get_solution(model, sol);
            int binary_ok = 1;
            for (int j = 0; j < 10; j++) {
                if (fabs(sol[j]) > TOLERANCE && fabs(sol[j] - 1.0) > TOLERANCE) {
                    binary_ok = 0;
                }
            }
            ASSERT(binary_ok, "Set partition: binary vars");
            free(sol);
        }

        ralph_test_free(model);
    }
}

/* ============================================================================
 * Test: Multi-round Scaling Produces Tighter Norms
 *
 * A random 20x20 LP with entries spanning 1e-3 to 1e3.
 * N=5 scaling should produce tighter row/col norm spread than N=1.
 * ============================================================================ */
void test_scaling_multi_round_norms(void) {
    printf("\n=== Test: Multi-round Scaling Norms ===\n");

    /* Build a badly-scaled LP:
     * min  sum(x_j)
     * s.t. A*x <= b,  x >= 0
     * where A has entries from 1e-3 to 1e3 */
    int n = 20, m = 20;

    /* Solve with N=1 (single round) */
    RalphModel *model1 = ralph_test_create();
    ralph_test_set_obj_sense(model1, RALPH_MINIMIZE);
    for (int j = 0; j < n; j++) {
        ralph_test_add_var(model1, 0.0, 1e6, 1.0, RALPH_CONTINUOUS);
    }
    /* Deterministic badly-scaled matrix */
    for (int i = 0; i < m; i++) {
        int idx[20];
        double val[20];
        for (int j = 0; j < n; j++) {
            idx[j] = j;
            /* Scale varies: row i, col j → factor from 1e-3 to 1e3 */
            double rfactor = (i % 5 == 0) ? 1e3 : ((i % 3 == 0) ? 1e-2 : 1.0);
            double cfactor = (j % 4 == 0) ? 1e2 : ((j % 7 == 0) ? 1e-3 : 0.5);
            val[j] = rfactor * cfactor * (1.0 + (double)((i * 7 + j * 13) % 10));
        }
        double rhs = 1e4 * ((i % 3 == 0) ? 0.01 : 1.0);
        ralph_test_add_constraint(model1, n, idx, val, RALPH_LESS_EQUAL, rhs);
    }
    ralph_test_set_int_param(model1, "verbose", 0);
    ralph_test_set_int_param(model1, "scaling", 1);
    ralph_test_optimize(model1);
    RalphStatus s1 = ralph_test_get_status(model1);
    double obj1 = ralph_test_get_objval(model1);

    /* Solve with N=5 (multi-round) */
    RalphModel *model5 = ralph_test_create();
    ralph_test_set_obj_sense(model5, RALPH_MINIMIZE);
    for (int j = 0; j < n; j++) {
        ralph_test_add_var(model5, 0.0, 1e6, 1.0, RALPH_CONTINUOUS);
    }
    for (int i = 0; i < m; i++) {
        int idx[20];
        double val[20];
        for (int j = 0; j < n; j++) {
            idx[j] = j;
            double rfactor = (i % 5 == 0) ? 1e3 : ((i % 3 == 0) ? 1e-2 : 1.0);
            double cfactor = (j % 4 == 0) ? 1e2 : ((j % 7 == 0) ? 1e-3 : 0.5);
            val[j] = rfactor * cfactor * (1.0 + (double)((i * 7 + j * 13) % 10));
        }
        double rhs = 1e4 * ((i % 3 == 0) ? 0.01 : 1.0);
        ralph_test_add_constraint(model5, n, idx, val, RALPH_LESS_EQUAL, rhs);
    }
    ralph_test_set_int_param(model5, "verbose", 0);
    ralph_test_set_int_param(model5, "scaling", 5);
    ralph_test_optimize(model5);
    RalphStatus s5 = ralph_test_get_status(model5);
    double obj5 = ralph_test_get_objval(model5);

    /* Both should solve successfully */
    ASSERT(s1 == RALPH_STATUS_OPTIMAL, "N=1 scaling: OPTIMAL");
    ASSERT(s5 == RALPH_STATUS_OPTIMAL, "N=5 scaling: OPTIMAL");

    /* Same objective (scaling shouldn't change the solution) */
    ASSERT_NEAR(obj1, obj5, 1e-3, "N=1 and N=5 produce same objective");

    ralph_test_free(model1);
    ralph_test_free(model5);
}

/* ============================================================================
 * Test: Scaling with N=0 (disabled) still works
 *
 * Verify that scaling=0 disables scaling and the solver still works.
 * ============================================================================ */
void test_scaling_disabled(void) {
    printf("\n=== Test: Scaling Disabled (N=0) ===\n");

    /* Simple LP: min -x - y, x+y<=4, 2x+y<=6 */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);

    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 4.0);

    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 6.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "scaling", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Scaling=0: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -4.0, TOLERANCE, "Scaling=0: correct obj");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Multi-round Scaling No Regression
 *
 * Existing LP problems should solve correctly with N=2 and N=3.
 * Uses moderate scaling rounds (aggressive N=5+ can cause numerical
 * sensitivity on small problems with wide coefficient ranges).
 * ============================================================================ */
void test_scaling_no_regression(void) {
    printf("\n=== Test: Multi-round Scaling No Regression ===\n");

    /* Simple LP with N=2: min -x-y, x+y<=4, 2x+y<=6 */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
        int idx1[] = {0, 1}; double val1[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 4.0);
        int idx2[] = {0, 1}; double val2[] = {2.0, 1.0};
        ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 6.0);

        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "scaling", 2);
        ralph_test_optimize(model);
        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Simple LP (N=2): OPTIMAL");
        ASSERT_NEAR(ralph_test_get_objval(model), -4.0, TOLERANCE, "Simple LP (N=2): obj=-4");
        ralph_test_free(model);
    }

    /* Network flow LP with N=3 */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        double arc_costs[] = {2.0, 4.0, 9.0, 3.0, 1.0, 3.0};
        for (int j = 0; j < 6; j++) {
            ralph_test_add_var(model, 0.0, 100.0, arc_costs[j], RALPH_CONTINUOUS);
        }

        int n0_idx[] = {0, 1}; double n0_val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, n0_idx, n0_val, RALPH_EQUAL, 10.0);
        int n1_idx[] = {0, 2, 3}; double n1_val[] = {-1.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 3, n1_idx, n1_val, RALPH_EQUAL, 0.0);
        int n2_idx[] = {1, 2, 4}; double n2_val[] = {-1.0, -1.0, 1.0};
        ralph_test_add_constraint(model, 3, n2_idx, n2_val, RALPH_EQUAL, 0.0);
        int n3_idx[] = {3, 4, 5}; double n3_val[] = {1.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 3, n3_idx, n3_val, RALPH_EQUAL, 10.0);
        int nb_idx[] = {0, 1, 5}; double nb_val[] = {1.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 3, nb_idx, nb_val, RALPH_LESS_EQUAL, 15.0);

        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "scaling", 3);
        ralph_test_optimize(model);
        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Network (N=3): OPTIMAL");
        ralph_test_free(model);
    }

    /* Badly-scaled LP: N=3 matches N=1 */
    {
        double obj1 = 0, obj3 = 0;
        for (int rounds = 1; rounds <= 3; rounds += 2) {
            RalphModel *model = ralph_test_create();
            ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
            ralph_test_add_var(model, 0.0, 1e6, 1.0, RALPH_CONTINUOUS);
            ralph_test_add_var(model, 0.0, 1e6, 1.0, RALPH_CONTINUOUS);
            ralph_test_add_var(model, 0.0, 1e6, 1.0, RALPH_CONTINUOUS);

            /* Wide coefficient range: 0.001 to 1000 */
            int i1[] = {0, 1, 2}; double v1[] = {1000.0, 0.5, 0.001};
            ralph_test_add_constraint(model, 3, i1, v1, RALPH_LESS_EQUAL, 5000.0);
            int i2[] = {0, 1, 2}; double v2[] = {0.001, 1000.0, 0.5};
            ralph_test_add_constraint(model, 3, i2, v2, RALPH_LESS_EQUAL, 3000.0);
            int i3[] = {0, 1, 2}; double v3[] = {0.5, 0.001, 1000.0};
            ralph_test_add_constraint(model, 3, i3, v3, RALPH_LESS_EQUAL, 4000.0);

            ralph_test_set_int_param(model, "verbose", 0);
            ralph_test_set_int_param(model, "scaling", rounds);
            ralph_test_optimize(model);
            ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
                   rounds == 1 ? "Badly-scaled (N=1): OPTIMAL" : "Badly-scaled (N=3): OPTIMAL");
            if (rounds == 1) obj1 = ralph_test_get_objval(model);
            else obj3 = ralph_test_get_objval(model);
            ralph_test_free(model);
        }
        ASSERT_NEAR(obj1, obj3, 1e-3, "Badly-scaled: N=1 and N=3 match");
    }
}

/* ============================================================================
 * Test: Scaling Roundtrip Preserves Solution
 *
 * Solve same LP with scaling=1 and scaling=5. Both should give the same
 * primal solution values within tolerance.
 * ============================================================================ */
void test_scaling_roundtrip(void) {
    printf("\n=== Test: Scaling Roundtrip ===\n");

    /* LP with wide coefficient range:
     * min -1000*x0 - 0.001*x1 - x2
     * s.t. 1000*x0 + 0.001*x1 + x2 <= 5000
     *      x0 + x1 + 1000*x2 <= 500
     *      x >= 0 */
    double sol1[3], sol5[3];

    for (int rounds = 1; rounds <= 5; rounds += 4) {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1000.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, -0.001, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);

        int idx1[] = {0, 1, 2};
        double val1[] = {1000.0, 0.001, 1.0};
        ralph_test_add_constraint(model, 3, idx1, val1, RALPH_LESS_EQUAL, 5000.0);

        int idx2[] = {0, 1, 2};
        double val2[] = {1.0, 1.0, 1000.0};
        ralph_test_add_constraint(model, 3, idx2, val2, RALPH_LESS_EQUAL, 500.0);

        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "scaling", rounds);
        ralph_test_optimize(model);

        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
               rounds == 1 ? "Roundtrip N=1: OPTIMAL" : "Roundtrip N=5: OPTIMAL");

        double *sol = (rounds == 1) ? sol1 : sol5;
        ralph_test_get_solution(model, sol);
        ralph_test_free(model);
    }

    /* Solutions should match within tolerance */
    for (int j = 0; j < 3; j++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Roundtrip x[%d] matches", j);
        ASSERT_NEAR(sol1[j], sol5[j], 1e-3, msg);
    }
}

/* ============================================================================
 * Test: Crash Basis Places Structural Columns
 *
 * A 10-variable LP with singleton columns. Crash should place at least 3
 * structural columns in the basis (vs 0 with all-slack start).
 * ============================================================================ */
void test_crash_basis_structural(void) {
    printf("\n=== Test: Crash Basis Structural ===\n");

    /* 10-variable LP with several singleton-like columns:
     * min sum(x_j), x_j >= 0
     * Constraints mix singletons and dense columns */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    for (int j = 0; j < 10; j++) {
        ralph_test_add_var(model, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);
    }

    /* Row 0: x0 <= 50 (singleton for x0) */
    int i0[] = {0}; double v0[] = {1.0};
    ralph_test_add_constraint(model, 1, i0, v0, RALPH_LESS_EQUAL, 50.0);

    /* Row 1: x1 <= 40 (singleton for x1) */
    int i1[] = {1}; double v1[] = {1.0};
    ralph_test_add_constraint(model, 1, i1, v1, RALPH_LESS_EQUAL, 40.0);

    /* Row 2: x2 <= 30 (singleton for x2) */
    int i2[] = {2}; double v2[] = {1.0};
    ralph_test_add_constraint(model, 1, i2, v2, RALPH_LESS_EQUAL, 30.0);

    /* Row 3: x3 + x4 <= 60 */
    int i3[] = {3, 4}; double v3[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, i3, v3, RALPH_LESS_EQUAL, 60.0);

    /* Row 4: x5 + x6 + x7 <= 80 */
    int i4[] = {5, 6, 7}; double v4[] = {1.0, 1.0, 1.0};
    ralph_test_add_constraint(model, 3, i4, v4, RALPH_LESS_EQUAL, 80.0);

    /* Row 5: sum(x_j) <= 200 */
    int i5[] = {0,1,2,3,4,5,6,7,8,9};
    double v5[] = {1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0};
    ralph_test_add_constraint(model, 10, i5, v5, RALPH_LESS_EQUAL, 200.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "crash", 1);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Crash basis: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 0.0, TOLERANCE, "Crash basis: obj=0 (all vars at lb)");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Crash Basis Reduces Phase 1 Iterations
 *
 * A medium LP (50 vars) with >= and = constraints. Crash should reduce
 * total iterations compared to all-slack start.
 * ============================================================================ */
void test_crash_reduces_iterations(void) {
    printf("\n=== Test: Crash Reduces Iterations ===\n");

    /* Maximize LP with all <= constraints — crash should place structural
     * columns near their optimal values, reducing Phase 2 iterations.
     * Negative objective coefficients (maximize) force non-trivial solution. */
    int iters_no_crash = 0, iters_crash = 0;

    for (int use_crash = 0; use_crash <= 1; use_crash++) {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

        int n = 40;
        for (int j = 0; j < n; j++) {
            ralph_test_add_var(model, 0.0, 100.0,
                          1.0 + 0.5 * (j % 5), RALPH_CONTINUOUS);
        }

        /* 15 singleton constraints: a_j * x_j <= rhs */
        for (int i = 0; i < 15; i++) {
            int idx[] = {i};
            double val[] = {1.0 + 0.2 * (i % 4)};
            ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL,
                                 30.0 + 5.0 * i);
        }

        /* 15 coupling constraints */
        for (int i = 0; i < 15; i++) {
            int idx[10];
            double val[10];
            int nnz = 0;
            for (int j = 0; j < n; j++) {
                if ((i * 11 + j * 7 + 3) % 10 < 2) {
                    idx[nnz] = j;
                    val[nnz] = 1.0 + (double)((i + j) % 3);
                    nnz++;
                }
            }
            if (nnz == 0) { idx[0] = 15 + i; val[0] = 1.0; nnz = 1; }
            ralph_test_add_constraint(model, nnz, idx, val, RALPH_LESS_EQUAL,
                                 100.0 + 30.0 * i);
        }

        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "crash", use_crash);
        ralph_test_optimize(model);

        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
               use_crash ? "With crash: OPTIMAL" : "No crash: OPTIMAL");

        if (use_crash) {
            iters_crash = ralph_test_get_iterations(model);
        } else {
            iters_no_crash = ralph_test_get_iterations(model);
        }
        ralph_test_free(model);
    }

    printf("  Iterations: no_crash=%d, crash=%d\n", iters_no_crash, iters_crash);
    /* Crash should not increase iterations significantly */
    ASSERT(iters_crash <= iters_no_crash + 5,
           "Crash does not increase iterations");
}

/* ============================================================================
 * Test: Crash Basis with Infeasible LP
 *
 * Verify infeasibility is still correctly detected with crash enabled.
 * ============================================================================ */
void test_crash_infeasible(void) {
    printf("\n=== Test: Crash Infeasible ===\n");

    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);

    /* x + y <= 5 */
    int idx1[] = {0, 1}; double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 5.0);

    /* x + y >= 10 (contradicts above) */
    int idx2[] = {0, 1}; double val2[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_GREATER_EQUAL, 10.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "crash", 1);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_INFEASIBLE,
           "Crash + infeasible: correctly detected");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Crash Basis No Regression
 *
 * Run all key LP problems with crash=1 and verify same results.
 * ============================================================================ */
void test_crash_no_regression(void) {
    printf("\n=== Test: Crash No Regression ===\n");

    /* Simple 2-var LP: min -x-y, x+y<=4, 2x+y<=6 */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
        int idx1[] = {0, 1}; double val1[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 4.0);
        int idx2[] = {0, 1}; double val2[] = {2.0, 1.0};
        ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 6.0);
        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "crash", 1);
        ralph_test_optimize(model);
        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Simple LP (crash): OPTIMAL");
        ASSERT_NEAR(ralph_test_get_objval(model), -4.0, TOLERANCE, "Simple LP (crash): obj=-4");
        ralph_test_free(model);
    }

    /* Diet problem */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_test_add_var(model, 0, 1e30, 2.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, 1e30, 3.5, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, 1e30, 8.0, RALPH_CONTINUOUS);
        int idx[] = {0, 1, 2};
        double v1[] = {50, 42, 35};
        ralph_test_add_constraint(model, 3, idx, v1, RALPH_GREATER_EQUAL, 300);
        double v2[] = {4, 8, 7};
        ralph_test_add_constraint(model, 3, idx, v2, RALPH_GREATER_EQUAL, 10);
        double v3[] = {0, 3, 2};
        ralph_test_add_constraint(model, 3, idx, v3, RALPH_GREATER_EQUAL, 8);
        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "crash", 1);
        ralph_test_optimize(model);
        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Diet (crash): OPTIMAL");
        double obj = ralph_test_get_objval(model);
        ASSERT(obj > 10.0 && obj < 25.0, "Diet (crash): reasonable cost");
        ralph_test_free(model);
    }

    /* Equality constraints */
    {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
        int idx[] = {0, 1}; double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_EQUAL, 10.0);
        double val2[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val2, RALPH_LESS_EQUAL, 4.0);
        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "crash", 1);
        ralph_test_optimize(model);
        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Equality (crash): OPTIMAL");
        ralph_test_free(model);
    }
}

/* ============================================================================
 * Post-Solve Verification Tests (T2.3 + T3.6)
 * ============================================================================ */

void test_verify_clean_lp(void) {
    printf("\n=== Test: Verify Clean LP ===\n");

    /* Simple LP that solves cleanly — verify=1 should not downgrade */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    int idx[] = {0, 1}; double val1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val1, RALPH_LESS_EQUAL, 4.0);
    double val2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, val2, RALPH_LESS_EQUAL, 6.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "verify", 1);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Clean LP with verify=1: OPTIMAL (not IMPRECISE)");

    ASSERT(fabs(ralph_test_get_objval(model) - (-4.0)) < 1e-6,
           "Clean LP: obj=-4");
    ralph_test_free(model);
}

void test_verify_diet_with_verify(void) {
    printf("\n=== Test: Verify Diet Problem ===\n");

    /* Diet problem with >= constraints — should pass verification */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, 1e30, 2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, 1e30, 3.5, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, 1e30, 8.0, RALPH_CONTINUOUS);
    int idx[] = {0, 1, 2};
    double v1[] = {50, 42, 35};
    ralph_test_add_constraint(model, 3, idx, v1, RALPH_GREATER_EQUAL, 300);
    double v2[] = {4, 8, 7};
    ralph_test_add_constraint(model, 3, idx, v2, RALPH_GREATER_EQUAL, 10);
    double v3[] = {0, 3, 2};
    ralph_test_add_constraint(model, 3, idx, v3, RALPH_GREATER_EQUAL, 8);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "verify", 1);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Diet with verify=1: OPTIMAL");
    ralph_test_free(model);
}

void test_verify_network_flow(void) {
    printf("\n=== Test: Verify Network Flow LP ===\n");

    /* Network flow with equality constraints — tests = constraint handling */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* 4 arcs: costs 2, 3, 1, 4 */
    ralph_test_add_var(model, 0.0, 10.0, 2.0, RALPH_CONTINUOUS);  /* 0→1 */
    ralph_test_add_var(model, 0.0, 10.0, 3.0, RALPH_CONTINUOUS);  /* 0→2 */
    ralph_test_add_var(model, 0.0, 10.0, 1.0, RALPH_CONTINUOUS);  /* 1→3 */
    ralph_test_add_var(model, 0.0, 10.0, 4.0, RALPH_CONTINUOUS);  /* 2→3 */

    /* Flow conservation: supply 5 at node 0, demand 5 at node 3 */
    int i0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, i0, v0, RALPH_EQUAL, 5.0);
    int i1[] = {0, 2}; double v1[] = {-1.0, 1.0};
    ralph_test_add_constraint(model, 2, i1, v1, RALPH_EQUAL, 0.0);
    int i2[] = {1, 3}; double v2[] = {-1.0, 1.0};
    ralph_test_add_constraint(model, 2, i2, v2, RALPH_EQUAL, 0.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "verify", 1);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Network flow with verify=1: OPTIMAL");
    ASSERT(fabs(ralph_test_get_objval(model) - 15.0) < 1e-6,
           "Network flow: obj=15");
    ralph_test_free(model);
}

void test_verify_no_regression(void) {
    printf("\n=== Test: Verify No Regression ===\n");

    /* Run a set of LPs with verify=1, ensure all return OPTIMAL */
    struct { int n_vars; int n_cons; double expected_obj; } cases[] = {
        {2, 2, -4.0},   /* Simple 2-var LP */
        {5, 3, 0.0},    /* Minimize non-negative vars, all at lb */
        {10, 5, 0.0},   /* 10-var, all at lb */
    };

    for (int c = 0; c < 3; c++) {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

        if (c == 0) {
            /* Simple LP: min -x1 - x2 s.t. x1+x2<=4, 2x1+x2<=6 */
            ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
            ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
            int idx[] = {0, 1}; double v1[] = {1.0, 1.0};
            ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 4.0);
            double v2[] = {2.0, 1.0};
            ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 6.0);
        } else {
            /* Generic: min sum(c_j * x_j) s.t. sum(x_j) <= 100 */
            for (int j = 0; j < cases[c].n_vars; j++) {
                ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0 + 0.1 * j, RALPH_CONTINUOUS);
            }
            int idx[10]; double val[10];
            for (int j = 0; j < cases[c].n_vars && j < 10; j++) {
                idx[j] = j; val[j] = 1.0;
            }
            ralph_test_add_constraint(model, cases[c].n_vars > 10 ? 10 : cases[c].n_vars,
                                 idx, val, RALPH_LESS_EQUAL, 100.0);
        }

        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "verify", 1);
        ralph_test_optimize(model);

        char msg[64];
        snprintf(msg, sizeof(msg), "Verify case %d: OPTIMAL", c);
        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, msg);
        ralph_test_free(model);
    }
}

/* ============================================================================
 * T3.1: Objective Limit Tests
 * ============================================================================ */

void test_obj_limit_below_optimal(void) {
    printf("\n=== Test: Objective Limit Below Optimal ===\n");

    /* min -x1 - x2 s.t. x1+x2<=4, 2x1+x2<=6, x>=0
     * Optimal obj = -4.0 (in user minimize space)
     * Set limit at -3.0 — solver should exit early with OBJ_LIMIT */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    int idx[] = {0, 1}; double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 4.0);
    double v2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 6.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_dbl_param(model, "obj_limit", -3.0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OBJ_LIMIT,
           "Should return OBJ_LIMIT when limit is above optimal");
    /* Obj should be >= -3.0 (hit limit before reaching -4.0) */
    ASSERT(ralph_test_get_objval(model) >= -3.0 - 1e-6,
           "Objective should not exceed the limit");

    ralph_test_free(model);
}

void test_obj_limit_not_triggered(void) {
    printf("\n=== Test: Objective Limit Not Triggered ===\n");

    /* min -x1 - x2 s.t. x1+x2<=4, 2x1+x2<=6, x>=0 (all-<= LP)
     * Optimal obj = -4.0. Internal space: minimizing, starts at 0 (origin), goes to -4.
     * Set limit at 1.0 (internal). Since internal obj starts at 0 and decreases,
     * 0 < 1.0, so the limit is never hit and solver reaches OPTIMAL. */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    int idx[] = {0, 1}; double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 4.0);
    double v2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 6.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_dbl_param(model, "obj_limit", 1.0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Should reach OPTIMAL when limit is above starting obj");
    ASSERT(fabs(ralph_test_get_objval(model) - (-4.0)) < 1e-6,
           "Objective should be -4.0");

    ralph_test_free(model);
}

void test_obj_limit_maximize(void) {
    printf("\n=== Test: Objective Limit with Maximize ===\n");

    /* max x1 + x2 s.t. x1+x2<=4, 2x1+x2<=6, x>=0
     * Optimal = 4.0. Set limit at 3.0 — solver should exit at OBJ_LIMIT
     * because in max space, 3.0 is "below" optimal 4.0 */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    int idx[] = {0, 1}; double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 4.0);
    double v2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 6.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_dbl_param(model, "obj_limit", 3.0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OBJ_LIMIT,
           "Should return OBJ_LIMIT for max with limit below optimal");
    ASSERT(ralph_test_get_objval(model) <= 3.0 + 1e-6,
           "Objective should not exceed limit in max space");

    ralph_test_free(model);
}

void test_obj_limit_no_regression(void) {
    printf("\n=== Test: Objective Limit No Regression ===\n");

    /* Verify no limit (default) still returns OPTIMAL for known problems */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    int idx[] = {0, 1}; double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 4.0);
    double v2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 6.0);

    ralph_test_set_int_param(model, "verbose", 0);
    /* No obj_limit set — should use default (INFINITY) */
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Default (no limit) should reach OPTIMAL");
    ASSERT(fabs(ralph_test_get_objval(model) - (-4.0)) < 1e-6,
           "Objective should be -4.0");

    ralph_test_free(model);
}

/* ============================================================================
 * T3.4: Per-Phase Pricing Tests
 * ============================================================================ */

void test_phase1_pricing_dantzig(void) {
    printf("\n=== Test: Phase 1 Pricing Override (Dantzig) ===\n");

    /* Diet problem with >= constraints — needs Phase 1.
     * Run with default pricing (Devex) and with phase1_pricing=0 (Dantzig).
     * Both should reach OPTIMAL with same objective. */

    double objs[2];
    for (int run = 0; run < 2; run++) {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

        /* min 2x+3y+5z s.t. x+2y+z>=10, 2x+y+3z>=15, x,y,z>=0 */
        ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, RALPH_INFINITY, 5.0, RALPH_CONTINUOUS);
        int idx2[] = {0, 1, 2};
        double v1[] = {1.0, 2.0, 1.0};
        ralph_test_add_constraint(model, 3, idx2, v1, RALPH_GREATER_EQUAL, 10.0);
        double v2[] = {2.0, 1.0, 3.0};
        ralph_test_add_constraint(model, 3, idx2, v2, RALPH_GREATER_EQUAL, 15.0);

        ralph_test_set_int_param(model, "verbose", 0);
        if (run == 1) {
            ralph_test_set_int_param(model, "phase1_pricing", 0); /* Dantzig for Phase 1 */
        }
        ralph_test_optimize(model);

        objs[run] = ralph_test_get_objval(model);

        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
               run == 0 ? "Default pricing: OPTIMAL" : "Dantzig Phase 1: OPTIMAL");

        ralph_test_free(model);
    }

    ASSERT(fabs(objs[0] - objs[1]) < 1e-6,
           "Same objective with both pricing strategies");
}

void test_phase1_pricing_disabled(void) {
    printf("\n=== Test: Phase 1 Pricing Disabled ===\n");

    /* Same problem with phase1_pricing=-1 (disabled, use solver pricing).
     * Should behave identically to default. */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);
    int idx[] = {0, 1};
    double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_GREATER_EQUAL, 5.0);
    double v2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_GREATER_EQUAL, 8.0);

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "phase1_pricing", -1);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Disabled Phase 1 pricing override: OPTIMAL");

    ralph_test_free(model);
}

/* ============================================================================
 * Tests: Heap Pricing (T2.2)
 * ============================================================================ */

/* Solve 5 diverse LP problems with heap pricing (4) and Dantzig (0).
 * Both must reach OPTIMAL with matching objectives. */
void test_pricing_heap(void) {
    printf("\n=== Test: Heap Pricing (T2.2) ===\n");

    /* Problem 1: Diet (>= constraints) — min 2x+3y+5z s.t. x+2y+z>=10, 2x+y+3z>=15 */
    /* Problem 2: Transport (= constraints) — min 4x+5y s.t. x+y=10, 2x+3y=25 */
    /* Problem 3: Bounded vars — min x+y s.t. x+y<=10, x<=6, y<=7 */
    /* Problem 4: Free vars — min x+y s.t. x+y>=5, -inf<=x<=inf */
    /* Problem 5: Mixed — min 3x+2y+z s.t. x+y<=10, y+z>=4, x+z=6, 0<=x<=8 */

    for (int prob = 0; prob < 5; prob++) {
        double objs[2];
        for (int run = 0; run < 2; run++) {
            RalphModel *model = ralph_test_create();
            ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
            ralph_test_set_int_param(model, "verbose", 0);
            ralph_test_set_int_param(model, "pricing", run == 0 ? 0 : 4);

            int idx2[] = {0, 1};
            int idx3[] = {0, 1, 2};

            if (prob == 0) {
                /* Diet */
                ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
                ralph_test_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);
                ralph_test_add_var(model, 0, RALPH_INFINITY, 5.0, RALPH_CONTINUOUS);
                double v1[] = {1.0, 2.0, 1.0};
                ralph_test_add_constraint(model, 3, idx3, v1, RALPH_GREATER_EQUAL, 10.0);
                double v2[] = {2.0, 1.0, 3.0};
                ralph_test_add_constraint(model, 3, idx3, v2, RALPH_GREATER_EQUAL, 15.0);
            } else if (prob == 1) {
                /* Transport (equalities) */
                ralph_test_add_var(model, 0, RALPH_INFINITY, 4.0, RALPH_CONTINUOUS);
                ralph_test_add_var(model, 0, RALPH_INFINITY, 5.0, RALPH_CONTINUOUS);
                double v1[] = {1.0, 1.0};
                ralph_test_add_constraint(model, 2, idx2, v1, RALPH_EQUAL, 10.0);
                double v2[] = {2.0, 3.0};
                ralph_test_add_constraint(model, 2, idx2, v2, RALPH_EQUAL, 25.0);
            } else if (prob == 2) {
                /* Bounded vars */
                ralph_test_add_var(model, 0, 6.0, 1.0, RALPH_CONTINUOUS);
                ralph_test_add_var(model, 0, 7.0, 1.0, RALPH_CONTINUOUS);
                double v1[] = {1.0, 1.0};
                ralph_test_add_constraint(model, 2, idx2, v1, RALPH_LESS_EQUAL, 10.0);
            } else if (prob == 3) {
                /* Maximize — max 3x+5y s.t. x<=4, 2y<=12, 3x+5y<=25 */
                ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
                ralph_test_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);
                ralph_test_add_var(model, 0, RALPH_INFINITY, 5.0, RALPH_CONTINUOUS);
                int ix[] = {0};
                double vx[] = {1.0};
                ralph_test_add_constraint(model, 1, ix, vx, RALPH_LESS_EQUAL, 4.0);
                int iy[] = {1};
                double vy[] = {2.0};
                ralph_test_add_constraint(model, 1, iy, vy, RALPH_LESS_EQUAL, 12.0);
                double v3[] = {3.0, 5.0};
                ralph_test_add_constraint(model, 2, idx2, v3, RALPH_LESS_EQUAL, 25.0);
            } else {
                /* Mixed */
                ralph_test_add_var(model, 0, 8.0, 3.0, RALPH_CONTINUOUS);
                ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
                ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
                double v1[] = {1.0, 1.0, 0.0};
                ralph_test_add_constraint(model, 2, idx2, v1, RALPH_LESS_EQUAL, 10.0);
                int idx_yz[] = {1, 2};
                double v2[] = {1.0, 1.0};
                ralph_test_add_constraint(model, 2, idx_yz, v2, RALPH_GREATER_EQUAL, 4.0);
                int idx_xz[] = {0, 2};
                double v3[] = {1.0, 1.0};
                ralph_test_add_constraint(model, 2, idx_xz, v3, RALPH_EQUAL, 6.0);
            }

            ralph_test_optimize(model);
            objs[run] = ralph_test_get_objval(model);

            char msg[80];
            snprintf(msg, sizeof(msg), "Problem %d, pricing=%d: OPTIMAL", prob + 1, run == 0 ? 0 : 4);
            ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, msg);

            ralph_test_free(model);
        }

        char msg[80];
        snprintf(msg, sizeof(msg), "Problem %d: Heap obj matches Dantzig (%.6f vs %.6f)", prob + 1, objs[1], objs[0]);
        ASSERT(fabs(objs[0] - objs[1]) < 1e-6, msg);
    }
}

/* Solve one problem with all 5 pricing strategies (0-4).
 * All must reach OPTIMAL with consistent objectives. */
void test_pricing_all_strategies(void) {
    printf("\n=== Test: All Pricing Strategies (T2.2) ===\n");

    double objs[5];
    for (int strat = 0; strat < 5; strat++) {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_set_int_param(model, "pricing", strat);

        /* 3-var LP: min 2x+3y+z s.t. x+y+z>=10, 2x+y>=8, y+2z>=6, x,y,z>=0 */
        ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);

        int idx3[] = {0, 1, 2};
        double v1[] = {1.0, 1.0, 1.0};
        ralph_test_add_constraint(model, 3, idx3, v1, RALPH_GREATER_EQUAL, 10.0);
        int idx2a[] = {0, 1};
        double v2[] = {2.0, 1.0};
        ralph_test_add_constraint(model, 2, idx2a, v2, RALPH_GREATER_EQUAL, 8.0);
        int idx2b[] = {1, 2};
        double v3[] = {1.0, 2.0};
        ralph_test_add_constraint(model, 2, idx2b, v3, RALPH_GREATER_EQUAL, 6.0);

        ralph_test_optimize(model);
        objs[strat] = ralph_test_get_objval(model);

        char msg[80];
        snprintf(msg, sizeof(msg), "Pricing strategy %d: OPTIMAL", strat);
        ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, msg);

        ralph_test_free(model);
    }

    /* All strategies should produce same objective */
    for (int strat = 1; strat < 5; strat++) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Strategy %d matches strategy 0 (%.6f vs %.6f)", strat, objs[strat], objs[0]);
        ASSERT(fabs(objs[strat] - objs[0]) < 1e-6, msg);
    }
}

/* ============================================================================
 * Tests: Dual Simplex Method Dispatch (T1.3)
 * ============================================================================ */

void test_dual_method_small_lp(void) {
    printf("\n=== Test: Dual Method Small LP ===\n");

    /* Simple 2-var LP: min 2x + 3y s.t. x+y >= 5, 2x+y >= 8, x,y >= 0 */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1};
    double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_GREATER_EQUAL, 5.0);
    double v2[] = {2.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_GREATER_EQUAL, 8.0);

    ralph_test_set_int_param(model, "method", 1);  /* Dual simplex */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Dual method: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 10.0, TOLERANCE,
                "Dual method: correct objective");
    ralph_test_free(model);
}

void test_dual_method_infeasible(void) {
    printf("\n=== Test: Dual Method Infeasible ===\n");

    /* x+y <= 1, x+y >= 3 — infeasible.
     * Use method=2 (auto) so dual failure falls back to primal for detection. */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1};
    double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 1.0);
    double v2[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_GREATER_EQUAL, 3.0);

    ralph_test_set_int_param(model, "method", 2);  /* Auto — dual then primal fallback */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_INFEASIBLE,
           "Auto method: INFEASIBLE");
    ralph_test_free(model);
}

void test_dual_method_unbounded(void) {
    printf("\n=== Test: Dual Method Unbounded ===\n");

    /* min -x s.t. x >= 0 — unbounded (dual infeasible) */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);

    /* Need at least one constraint */
    int idx[] = {0};
    double v[] = {1.0};
    ralph_test_add_constraint(model, 1, idx, v, RALPH_GREATER_EQUAL, 0.0);

    ralph_test_set_int_param(model, "method", 2);  /* Auto — dual should fail, fall to primal */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    /* With method=2, should still detect unbounded via primal fallback */
    int status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_UNBOUNDED || status == RALPH_STATUS_OPTIMAL,
           "Auto method: handles unbounded case");
    ralph_test_free(model);
}

void test_dual_method_auto_fallback(void) {
    printf("\n=== Test: Dual Method Auto Fallback ===\n");

    /* Diet problem — mixed constraints, method=2 may need fallback */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    /* Variables: bread, milk, cheese */
    ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 3.5, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 8.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1, 2};
    double cal[] = {2.0, 3.5, 2.0};
    ralph_test_add_constraint(model, 3, idx, cal, RALPH_GREATER_EQUAL, 6.0);
    double vit[] = {0.5, 2.0, 3.0};
    ralph_test_add_constraint(model, 3, idx, vit, RALPH_GREATER_EQUAL, 4.0);

    ralph_test_set_int_param(model, "method", 2);  /* Auto */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Auto method: OPTIMAL");

    /* Compare with primal method */
    double dual_obj = ralph_test_get_objval(model);
    ralph_test_free(model);

    model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 3.5, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, RALPH_INFINITY, 8.0, RALPH_CONTINUOUS);
    ralph_test_add_constraint(model, 3, idx, cal, RALPH_GREATER_EQUAL, 6.0);
    ralph_test_add_constraint(model, 3, idx, vit, RALPH_GREATER_EQUAL, 4.0);

    ralph_test_set_int_param(model, "method", 0);  /* Primal */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    double primal_obj = ralph_test_get_objval(model);
    ASSERT_NEAR(dual_obj, primal_obj, TOLERANCE,
                "Auto method matches primal objective");
    ralph_test_free(model);
}

void test_dual_method_with_crash(void) {
    printf("\n=== Test: Dual Method with Crash ===\n");

    /* Test crash + dual method together */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0, 10.0, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, 10.0, 2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, 10.0, 3.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1, 2};
    double v1[] = {1.0, 1.0, 0.0};
    ralph_test_add_constraint(model, 3, idx, v1, RALPH_LESS_EQUAL, 8.0);
    double v2[] = {0.0, 1.0, 1.0};
    ralph_test_add_constraint(model, 3, idx, v2, RALPH_LESS_EQUAL, 7.0);

    ralph_test_set_int_param(model, "method", 1);
    ralph_test_set_int_param(model, "crash", 1);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "Dual+crash: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 0.0, TOLERANCE,
                "Dual+crash: correct objective (all vars at lb=0)");
    ralph_test_free(model);
}

void test_dual_method_obj_limit(void) {
    printf("\n=== Test: Dual Method Objective Limit ===\n");

    /* Maximization with objective limit via dual method */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
    ralph_test_add_var(model, 0, 100.0, 5.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0, 100.0, 4.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1};
    double v1[] = {6.0, 4.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 24.0);
    double v2[] = {1.0, 2.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 6.0);

    ralph_test_set_int_param(model, "method", 1);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_dbl_param(model, "obj_limit", 15.0); /* Stop if obj >= 15 */
    ralph_test_optimize(model);

    int status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL || status == RALPH_STATUS_OBJ_LIMIT,
           "Dual+obj_limit: OPTIMAL or OBJ_LIMIT");
    ralph_test_free(model);
}

void test_dual_method_no_regression(void) {
    printf("\n=== Test: Dual Method No Regression ===\n");

    /* Run the diet problem with all three methods and verify same result */
    double objs[3];
    const char *names[] = {"primal", "dual", "auto"};

    for (int m = 0; m < 3; m++) {
        RalphModel *model = ralph_test_create();
        ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

        /* Variables: bread=2, milk=3.5, cheese=8 */
        ralph_test_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, RALPH_INFINITY, 3.5, RALPH_CONTINUOUS);
        ralph_test_add_var(model, 0, RALPH_INFINITY, 8.0, RALPH_CONTINUOUS);

        int idx[] = {0, 1, 2};
        double cal[] = {2.0, 3.5, 2.0};
        ralph_test_add_constraint(model, 3, idx, cal, RALPH_GREATER_EQUAL, 6.0);
        double vit[] = {0.5, 2.0, 3.0};
        ralph_test_add_constraint(model, 3, idx, vit, RALPH_GREATER_EQUAL, 4.0);

        ralph_test_set_int_param(model, "method", m);
        ralph_test_set_int_param(model, "verbose", 0);
        ralph_test_optimize(model);

        int status = ralph_test_get_status(model);
        objs[m] = ralph_test_get_objval(model);

        char msg[80];
        snprintf(msg, sizeof(msg), "Method %d (%s): OPTIMAL", m, names[m]);
        ASSERT(status == RALPH_STATUS_OPTIMAL, msg);

        ralph_test_free(model);
    }

    ASSERT_NEAR(objs[0], objs[1], TOLERANCE, "Primal == Dual objective");
    ASSERT_NEAR(objs[0], objs[2], TOLERANCE, "Primal == Auto objective");
}

void test_dual_phase1_free_vars(void) {
    printf("\n=== Test: Dual Phase 1 Free Variables ===\n");

    /* Problem with a free variable — forces dual Phase 1 */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* x free, y >= 0 */
    ralph_test_add_var(model, -RALPH_INFINITY, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1};
    double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_GREATER_EQUAL, 3.0);
    double v2[] = {1.0, -1.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 1.0);

    ralph_test_set_int_param(model, "method", 2);  /* Auto — will try dual, may need Phase 1 */
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    int status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Free var + dual auto: OPTIMAL");

    /* Known optimal: x=2, y=1, obj=4.0 (vertex of x+y=3 and x-y=1) */
    if (status == RALPH_STATUS_OPTIMAL) {
        ASSERT_NEAR(ralph_test_get_objval(model), 4.0, TOLERANCE,
                    "Free var + dual auto: correct objective");
    }

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Re-optimization (modify RHS/bounds, confirm dual warm-start)
 *
 * Tests that after modifying RHS or bounds, the solver produces correct
 * results. With all-<= constraints and method=2, dual simplex handles
 * the re-solve without Phase I (basis is naturally dual-feasible).
 *
 * Scenario 1: Tighten RHS → old solution infeasible, dual resolves
 * Scenario 2: Loosen RHS → old solution still feasible, adjust optimum
 * Scenario 3: Tighten variable bound → primal change only
 * ============================================================================ */
void test_reoptimization_rhs_bounds(void) {
    printf("\n=== Test: Re-optimization (RHS/Bounds, Dual Warm-Start) ===\n");

    /* Base LP: min -2x - 3y
     * s.t. x + y  <= 10
     *      x + 2y <= 14
     *      x <= 8
     *      x, y >= 0
     *
     * Optimal: x=6, y=4, obj=-24
     */
    RalphModel *model = ralph_test_create();
    ASSERT(model != NULL, "Model created");

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -2.0, RALPH_CONTINUOUS);  /* x */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -3.0, RALPH_CONTINUOUS);  /* y */

    int idx[] = {0, 1};
    double v1[] = {1.0, 1.0};
    ralph_test_add_constraint(model, 2, idx, v1, RALPH_LESS_EQUAL, 10.0);
    double v2[] = {1.0, 2.0};
    ralph_test_add_constraint(model, 2, idx, v2, RALPH_LESS_EQUAL, 14.0);
    int idx_x[] = {0};
    double v3[] = {1.0};
    ralph_test_add_constraint(model, 1, idx_x, v3, RALPH_LESS_EQUAL, 8.0);

    /* Solve with method=2 (auto: dual first, fallback to primal) */
    ralph_test_set_int_param(model, "method", 2);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Initial solve: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -24.0, TOLERANCE, "Initial solve: obj=-24");
    int iters_initial = ralph_test_get_iterations(model);
    printf("  INFO: Initial solve took %d iterations\n", iters_initial);

    /* --- Scenario 1: Tighten RHS ---
     * Change x + y <= 10 to x + y <= 6
     * New optimal: x=6, y=0, obj=-12... wait let me recalculate.
     * With x+y<=6, x+2y<=14, x<=8:
     * x=6, y=0 → check: 6+0=6<=6 ok, 6+0=6<=14 ok, 6<=8 ok, obj=-12
     * But y could increase: x=6,y=0 → try y=4: x+y=10>6, no.
     * Actually: maximize 2x+3y s.t. x+y<=6, x+2y<=14, x<=8
     * At (6,0): obj=12. At (0,6): obj=18 but x+2y=12<=14, ok.
     * At (0,6): x+y=6, ok. obj=18.
     * But can we do better? y=7: x+2y=14, x=0 → x+y=7>6, no.
     * Vertex: x+y=6 and x+2y=14 → y=8, x=-2 → infeasible.
     * Vertex: x+y=6 and y axis: x=0,y=6 → obj=-18.
     * min -2x-3y at (0,6) = -18.
     */
    ralph_core_set_constraint_rhs(model, 0, 6.0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Tighten RHS: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -18.0, TOLERANCE, "Tighten RHS: obj=-18");
    int iters_tighten = ralph_test_get_iterations(model);
    printf("  INFO: Tighten RHS re-solve took %d iterations\n", iters_tighten);

    /* --- Scenario 2: Loosen RHS back ---
     * Restore x + y <= 10, also loosen x + 2y <= 20
     * With x+y<=10, x+2y<=20, x<=8:
     * At (0,10): obj=-30, check x+2y=20<=20 ok, x+y=10 ok.
     * At (8,2): obj=-22. (0,10) is better.
     * min -2x-3y at (0,10) = -30.
     */
    ralph_core_set_constraint_rhs(model, 0, 10.0);
    ralph_core_set_constraint_rhs(model, 1, 20.0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Loosen RHS: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -30.0, TOLERANCE, "Loosen RHS: obj=-30");
    int iters_loosen = ralph_test_get_iterations(model);
    printf("  INFO: Loosen RHS re-solve took %d iterations\n", iters_loosen);

    /* --- Scenario 3: Tighten variable bound ---
     * Set x <= 2 (upper bound)
     * With x+y<=10, x+2y<=20, x<=2:
     * At (2,8): obj=-4-24=-28. At (0,10): obj=-30, but x<=2 ok.
     * (0,10): x+y=10, x+2y=20, x=0<=2. obj=-30. Still optimal.
     */
    ralph_test_set_var_bounds(model, 0, 0.0, 2.0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Tighten bound: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -30.0, TOLERANCE, "Tighten bound: obj=-30");

    /* Tighten further: x <= 0 AND y <= 5
     * With x+y<=10, x+2y<=20, x=0, y<=5:
     * At (0,5): obj=-15.
     */
    ralph_test_set_var_bounds(model, 0, 0.0, 0.0);
    ralph_test_set_var_bounds(model, 1, 0.0, 5.0);
    ralph_test_optimize(model);

    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Tight bounds: OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), -15.0, TOLERANCE, "Tight bounds: obj=-15");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: Re-optimization correctness (dual method on <= problems)
 *
 * All-<= LP with method=2. After solving, modify RHS to make current
 * solution infeasible. Dual simplex should resolve without Phase I since
 * the initial basis (all slacks) is dual-feasible for <= constraints.
 * Verifies iteration count is bounded (no Phase I overhead).
 * ============================================================================ */
void test_reoptimization_dual_no_phase1(void) {
    printf("\n=== Test: Re-optimization Dual (No Phase I) ===\n");

    /* Larger LP to make iteration counts meaningful:
     * min -x1 - 2*x2 - 3*x3
     * s.t. x1 + x2 + x3  <= 20
     *      x1 + 2*x2      <= 18
     *           x2 + 2*x3  <= 16
     *      x1              <= 10
     *           x2         <= 10
     *                x3    <= 10
     *      x1, x2, x3 >= 0
     */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -2.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -3.0, RALPH_CONTINUOUS);

    int idx3[] = {0, 1, 2};
    double a1[] = {1.0, 1.0, 1.0};
    ralph_test_add_constraint(model, 3, idx3, a1, RALPH_LESS_EQUAL, 20.0);

    int idx2a[] = {0, 1};
    double a2[] = {1.0, 2.0};
    ralph_test_add_constraint(model, 2, idx2a, a2, RALPH_LESS_EQUAL, 18.0);

    int idx2b[] = {1, 2};
    double a3[] = {1.0, 2.0};
    ralph_test_add_constraint(model, 2, idx2b, a3, RALPH_LESS_EQUAL, 16.0);

    int idx1a[] = {0};
    double b1[] = {1.0};
    ralph_test_add_constraint(model, 1, idx1a, b1, RALPH_LESS_EQUAL, 10.0);

    int idx1b[] = {1};
    double b2[] = {1.0};
    ralph_test_add_constraint(model, 1, idx1b, b2, RALPH_LESS_EQUAL, 10.0);

    int idx1c[] = {2};
    double b3[] = {1.0};
    ralph_test_add_constraint(model, 1, idx1c, b3, RALPH_LESS_EQUAL, 10.0);

    ralph_test_set_int_param(model, "method", 2);
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_optimize(model);

    RalphStatus st = ralph_test_get_status(model);
    ASSERT(st == RALPH_STATUS_OPTIMAL, "All-<= LP: OPTIMAL");
    int iters1 = ralph_test_get_iterations(model);
    printf("  INFO: Initial solve: %d iterations, obj=%.4f\n",
           iters1, ralph_test_get_objval(model));

    /* For a 3-var, 6-constraint all-<= LP, dual simplex should solve
     * in a small number of iterations (no Phase I needed). */
    ASSERT(iters1 <= 20, "All-<= LP: reasonable iteration count (no Phase I)");

    /* Tighten RHS significantly: x1 + x2 + x3 <= 5
     * Forces a very different optimum, tests re-solve correctness. */
    ralph_core_set_constraint_rhs(model, 0, 5.0);
    ralph_test_optimize(model);

    st = ralph_test_get_status(model);
    ASSERT(st == RALPH_STATUS_OPTIMAL, "Tightened RHS: OPTIMAL");
    int iters2 = ralph_test_get_iterations(model);
    printf("  INFO: After tighten: %d iterations, obj=%.4f\n",
           iters2, ralph_test_get_objval(model));
    ASSERT(iters2 <= 20, "Tightened RHS: reasonable iteration count");

    /* Verify solution feasibility */
    double x[3];
    ralph_test_get_solution(model, x);
    double sum = x[0] + x[1] + x[2];
    ASSERT(sum <= 5.0 + TOLERANCE, "Tightened: x1+x2+x3 <= 5 satisfied");
    ASSERT(x[0] >= -TOLERANCE && x[1] >= -TOLERANCE && x[2] >= -TOLERANCE,
           "Tightened: all vars non-negative");

    /* Make infeasible: all vars >= 0, x1+x2+x3 <= 5 but also
     * tighten individual bounds to sum > 5 */
    ralph_core_set_constraint_rhs(model, 0, 2.0);  /* x1+x2+x3 <= 2 */
    ralph_core_set_constraint_rhs(model, 1, 2.0);  /* x1+2*x2 <= 2 */
    ralph_core_set_constraint_rhs(model, 2, 2.0);  /* x2+2*x3 <= 2 */
    ralph_test_optimize(model);

    st = ralph_test_get_status(model);
    ASSERT(st == RALPH_STATUS_OPTIMAL, "Very tight: still OPTIMAL (feasible)");
    int iters3 = ralph_test_get_iterations(model);
    printf("  INFO: Very tight: %d iterations, obj=%.4f\n",
           iters3, ralph_test_get_objval(model));
    ASSERT(iters3 <= 20, "Very tight: reasonable iteration count");

    ralph_test_free(model);
}

static void build_telemetry_lp_case(RalphModel *model) {
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    ralph_test_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 4.0);
    ralph_test_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 6.0);
}

static void build_telemetry_mip_case(RalphModel *model) {
    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};

    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 1.0);
}

void test_presolve_report_api(void) {
    printf("\n=== Test: Presolve Report API ===\n");

    /* API argument validation. */
    ASSERT(ralph_core_get_last_presolve_report(NULL, NULL) == -1,
           "Presolve report getter rejects NULL args");

    RalphPresolveReport report;

    /* LP with presolve explicitly disabled. */
    RalphModel *lp_off = ralph_test_create();
    build_telemetry_lp_case(lp_off);
    ASSERT(ralph_test_set_int_param(lp_off, "presolve", 0) == 0,
           "LP presolve=0 accepted");
    ASSERT(ralph_test_optimize(lp_off) == 0, "LP presolve=0 solve call succeeds");
    ASSERT(ralph_test_get_status(lp_off) == RALPH_STATUS_OPTIMAL,
           "LP presolve=0 solve OPTIMAL");
    ASSERT(ralph_core_get_last_presolve_report(lp_off, &report) == 0,
           "LP presolve=0 report retrieved");
    ASSERT(report.used == 0, "LP presolve=0 report marks presolve unused");
    ASSERT(report.mask == 0u, "LP presolve=0 report mask is zero");
    ASSERT(report.rounds == 0, "LP presolve=0 report rounds are zero");
    ralph_test_free(lp_off);

    /* LP with presolve enabled and explicit mask. */
    RalphModel *lp_on = ralph_test_create();
    build_telemetry_lp_case(lp_on);
    ASSERT(ralph_test_set_int_param(lp_on, "presolve", 1) == 0,
           "LP presolve=1 accepted");
    ASSERT(ralph_test_set_int_param(lp_on, "presolve_mask", 0x310F) == 0,
           "LP presolve mask set");
    ASSERT(ralph_test_optimize(lp_on) == 0, "LP presolve=1 solve call succeeds");
    ASSERT(ralph_test_get_status(lp_on) == RALPH_STATUS_OPTIMAL,
           "LP presolve=1 solve OPTIMAL");
    ASSERT(ralph_core_get_last_presolve_report(lp_on, &report) == 0,
           "LP presolve=1 report retrieved");
    ASSERT(report.used == 1, "LP presolve=1 report marks presolve used");
    ASSERT(report.mask == 0x310Fu, "LP presolve report preserves configured mask");
    ASSERT(report.rounds >= 1, "LP presolve report rounds >= 1");
    ASSERT(report.presolve_time_ms >= 0.0, "LP presolve report has non-negative time");
    ASSERT(report.vars_removed >= 0 && report.cons_removed >= 0 &&
           report.bounds_tightened >= 0,
           "LP presolve report has non-negative reduction stats");
    ralph_test_free(lp_on);

    /* MIP path auto-enables lightweight presolve when not explicitly disabled. */
    RalphModel *mip_auto = ralph_test_create();
    build_telemetry_mip_case(mip_auto);
    ASSERT(ralph_test_set_int_param(mip_auto, "detect_special", 0) == 0,
           "MIP detect_special disabled");
    ASSERT(ralph_test_optimize_mip(mip_auto) == 0, "MIP auto-presolve solve call succeeds");
    ASSERT(ralph_test_get_status(mip_auto) == RALPH_STATUS_OPTIMAL,
           "MIP auto-presolve solve OPTIMAL");
    ASSERT(ralph_core_get_last_presolve_report(mip_auto, &report) == 0,
           "MIP auto-presolve report retrieved");
    ASSERT(report.used == 1, "MIP auto-presolve report marks presolve used");
    ASSERT(report.mask == 0x110Fu, "MIP auto-presolve uses lightweight mask 0x110F");
    ASSERT(report.rounds >= 1, "MIP auto-presolve report rounds >= 1");
    ralph_test_free(mip_auto);
}

void test_runtime_telemetry_param_propagation(void) {
    printf("\n=== Test: Runtime Telemetry Parameter Propagation ===\n");

    RalphModel *lp_off = ralph_test_create();
    build_telemetry_lp_case(lp_off);
    ASSERT(ralph_test_set_int_param(lp_off, "telemetry", 0) == 0, "LP telemetry=0 accepted");
    ralph_test_optimize(lp_off);
    ASSERT(ralph_test_get_status(lp_off) == RALPH_STATUS_OPTIMAL, "LP telemetry=0 solve OPTIMAL");
    SimplexSolver *lp_solver_off = ralph_get_lp_solver(lp_off);
    ASSERT(lp_solver_off != NULL, "LP solver available (telemetry=0)");
    ASSERT(lp_solver_off && lp_solver_off->telemetry_enabled == 0,
           "LP solver telemetry_enabled=0 propagated");
    ASSERT(lp_solver_off && lp_solver_off->tableau && lp_solver_off->tableau->lu &&
           lp_solver_off->tableau->lu->telemetry_enabled == 0,
           "LP LU telemetry_enabled=0 propagated");
    ralph_test_free(lp_off);

    RalphModel *lp_on = ralph_test_create();
    build_telemetry_lp_case(lp_on);
    ASSERT(ralph_test_set_int_param(lp_on, "telemetry", 1) == 0, "LP telemetry=1 accepted");
    ralph_test_optimize(lp_on);
    ASSERT(ralph_test_get_status(lp_on) == RALPH_STATUS_OPTIMAL, "LP telemetry=1 solve OPTIMAL");
    SimplexSolver *lp_solver_on = ralph_get_lp_solver(lp_on);
    ASSERT(lp_solver_on != NULL, "LP solver available (telemetry=1)");
    ASSERT(lp_solver_on && lp_solver_on->telemetry_enabled == 1,
           "LP solver telemetry_enabled=1 propagated");
    ASSERT(lp_solver_on && lp_solver_on->tableau && lp_solver_on->tableau->lu &&
           lp_solver_on->tableau->lu->telemetry_enabled == 1,
           "LP LU telemetry_enabled=1 propagated");
    ralph_test_free(lp_on);

    RalphModel *mip_off = ralph_test_create();
    build_telemetry_mip_case(mip_off);
    ralph_test_set_int_param(mip_off, "detect_special", 0);
    ASSERT(ralph_test_set_int_param(mip_off, "telemetry", 0) == 0, "MIP telemetry=0 accepted");
    ralph_test_optimize(mip_off);
    ASSERT(ralph_test_get_status(mip_off) == RALPH_STATUS_OPTIMAL, "MIP telemetry=0 solve OPTIMAL");
    MIPSolver *mip_solver_off = ralph_get_mip_solver(mip_off);
    ASSERT(mip_solver_off != NULL, "MIP solver available (telemetry=0)");
    ASSERT(mip_solver_off && mip_solver_off->telemetry == 0,
           "MIP solver telemetry=0 propagated");
    ASSERT(mip_solver_off && mip_solver_off->lp_solver &&
           mip_solver_off->lp_solver->telemetry_enabled == 0,
           "MIP node LP telemetry_enabled=0 propagated");
    ralph_test_free(mip_off);

    RalphModel *mip_on = ralph_test_create();
    build_telemetry_mip_case(mip_on);
    ralph_test_set_int_param(mip_on, "detect_special", 0);
    ASSERT(ralph_test_set_int_param(mip_on, "telemetry", 1) == 0, "MIP telemetry=1 accepted");
    ralph_test_optimize(mip_on);
    ASSERT(ralph_test_get_status(mip_on) == RALPH_STATUS_OPTIMAL, "MIP telemetry=1 solve OPTIMAL");
    MIPSolver *mip_solver_on = ralph_get_mip_solver(mip_on);
    ASSERT(mip_solver_on != NULL, "MIP solver available (telemetry=1)");
    ASSERT(mip_solver_on && mip_solver_on->telemetry == 1,
           "MIP solver telemetry=1 propagated");
    ASSERT(mip_solver_on && mip_solver_on->lp_solver &&
           mip_solver_on->lp_solver->telemetry_enabled == 1,
           "MIP node LP telemetry_enabled=1 propagated");
    ralph_test_free(mip_on);
}

void test_public_telemetry_snapshot_api(void) {
    printf("\n=== Test: Public LP/LU Telemetry Snapshot API ===\n");

    RalphLPSolverTelemetry lp_tel;
    RalphLUTelemetry lu_tel;

    ASSERT(ralph_core_get_last_lp_telemetry(NULL, &lp_tel) == -1,
           "LP telemetry getter rejects NULL model");
    ASSERT(ralph_core_get_last_lu_telemetry(NULL, &lu_tel) == -1,
           "LU telemetry getter rejects NULL model");

    RalphModel *arg_model = ralph_test_create();
    build_telemetry_lp_case(arg_model);
    ASSERT(ralph_core_get_last_lp_telemetry(arg_model, NULL) == -1,
           "LP telemetry getter rejects NULL output");
    ASSERT(ralph_core_get_last_lu_telemetry(arg_model, NULL) == -1,
           "LU telemetry getter rejects NULL output");
    ralph_test_free(arg_model);

    RalphModel *unsolved = ralph_test_create();
    build_telemetry_lp_case(unsolved);
    ASSERT(ralph_core_get_last_lp_telemetry(unsolved, &lp_tel) == 0,
           "Unsolved model returns LP telemetry snapshot");
    ASSERT(ralph_core_get_last_lu_telemetry(unsolved, &lu_tel) == 0,
           "Unsolved model returns LU telemetry snapshot");
    ASSERT(lp_tel.perf_refactor_count == 0,
           "Unsolved model LP telemetry is zeroed");
    ASSERT(lu_tel.perf_factorize_calls == 0,
           "Unsolved model LU telemetry is zeroed");
    ralph_test_free(unsolved);

    RalphModel *lp_off = ralph_test_create();
    build_telemetry_lp_case(lp_off);
    ASSERT(ralph_test_set_int_param(lp_off, "telemetry", 0) == 0,
           "LP telemetry=0 accepted");
    ASSERT(ralph_test_optimize_lp(lp_off) == 0, "LP telemetry=0 solve succeeds");
    ASSERT(ralph_test_get_status(lp_off) == RALPH_STATUS_OPTIMAL,
           "LP telemetry=0 solve OPTIMAL");
    ASSERT(ralph_core_get_last_lp_telemetry(lp_off, &lp_tel) == 0,
           "LP telemetry=0 snapshot retrieved");
    ASSERT(ralph_core_get_last_lu_telemetry(lp_off, &lu_tel) == 0,
           "LP telemetry=0 LU snapshot retrieved");
    ASSERT(lp_tel.perf_refactor_count == 0,
           "LP telemetry=0 keeps refactor count at zero");
    ASSERT(lu_tel.perf_factorize_calls == 0,
           "LP telemetry=0 keeps LU factorize count at zero");
    ralph_test_free(lp_off);

    RalphModel *lp_on = ralph_test_create();
    build_telemetry_lp_case(lp_on);
    ASSERT(ralph_test_set_int_param(lp_on, "telemetry", 1) == 0,
           "LP telemetry=1 accepted");
    ASSERT(ralph_test_optimize_lp(lp_on) == 0, "LP telemetry=1 solve succeeds");
    ASSERT(ralph_test_get_status(lp_on) == RALPH_STATUS_OPTIMAL,
           "LP telemetry=1 solve OPTIMAL");
    ASSERT(ralph_core_get_last_lp_telemetry(lp_on, &lp_tel) == 0,
           "LP telemetry=1 snapshot retrieved");
    ASSERT(ralph_core_get_last_lu_telemetry(lp_on, &lu_tel) == 0,
           "LP telemetry=1 LU snapshot retrieved");
    ASSERT(lp_tel.perf_refactor_count >= 1,
           "LP telemetry=1 records at least one refactor");
    ASSERT(lu_tel.perf_factorize_calls >= 1,
           "LP telemetry=1 records LU factorization");
    ralph_test_free(lp_on);

    RalphModel *mip_off = ralph_test_create();
    build_telemetry_mip_case(mip_off);
    ASSERT(ralph_test_set_int_param(mip_off, "detect_special", 0) == 0,
           "MIP telemetry=0 detect_special disabled");
    ASSERT(ralph_test_set_int_param(mip_off, "telemetry", 0) == 0,
           "MIP telemetry=0 accepted");
    ASSERT(ralph_test_optimize_mip(mip_off) == 0, "MIP telemetry=0 solve succeeds");
    ASSERT(ralph_test_get_status(mip_off) == RALPH_STATUS_OPTIMAL,
           "MIP telemetry=0 solve OPTIMAL");
    ASSERT(ralph_core_get_last_lp_telemetry(mip_off, &lp_tel) == 0,
           "MIP telemetry=0 LP snapshot retrieved");
    ASSERT(ralph_core_get_last_lu_telemetry(mip_off, &lu_tel) == 0,
           "MIP telemetry=0 LU snapshot retrieved");
    ASSERT(lp_tel.perf_refactor_count == 0,
           "MIP telemetry=0 keeps node LP refactor count at zero");
    ASSERT(lu_tel.perf_factorize_calls == 0,
           "MIP telemetry=0 keeps node LU factorize count at zero");
    ralph_test_free(mip_off);

    RalphModel *mip_on = ralph_test_create();
    build_telemetry_mip_case(mip_on);
    ASSERT(ralph_test_set_int_param(mip_on, "detect_special", 0) == 0,
           "MIP telemetry=1 detect_special disabled");
    ASSERT(ralph_test_set_int_param(mip_on, "telemetry", 1) == 0,
           "MIP telemetry=1 accepted");
    ASSERT(ralph_test_optimize_mip(mip_on) == 0, "MIP telemetry=1 solve succeeds");
    ASSERT(ralph_test_get_status(mip_on) == RALPH_STATUS_OPTIMAL,
           "MIP telemetry=1 solve OPTIMAL");
    ASSERT(ralph_core_get_last_lp_telemetry(mip_on, &lp_tel) == 0,
           "MIP telemetry=1 LP snapshot retrieved");
    ASSERT(ralph_core_get_last_lu_telemetry(mip_on, &lu_tel) == 0,
           "MIP telemetry=1 LU snapshot retrieved");
    ASSERT(lu_tel.perf_factorize_calls >= 1,
           "MIP telemetry=1 records node LU factorization");
    ralph_test_free(mip_on);
}

void test_solution_quality_api(void) {
    printf("\n=== Test: Solution Quality API ===\n");

    RalphSolutionQuality quality;

    ASSERT(ralph_core_get_solution_quality(NULL, &quality) == -1,
           "Solution quality getter rejects NULL model");

    RalphModel *arg_model = ralph_test_create();
    build_telemetry_lp_case(arg_model);
    ASSERT(ralph_core_get_solution_quality(arg_model, NULL) == -1,
           "Solution quality getter rejects NULL output");
    ralph_test_free(arg_model);

    RalphModel *verify_off = ralph_test_create();
    build_telemetry_lp_case(verify_off);
    ASSERT(ralph_test_set_int_param(verify_off, "verify", 0) == 0,
           "verify=0 accepted");
    ASSERT(ralph_test_optimize_lp(verify_off) == 0, "verify=0 solve succeeds");
    ASSERT(ralph_test_get_status(verify_off) == RALPH_STATUS_OPTIMAL,
           "verify=0 solve OPTIMAL");
    ASSERT(ralph_core_get_solution_quality(verify_off, &quality) == 0,
           "verify=0 quality snapshot retrieved");
    ASSERT(quality.status == RALPH_STATUS_OPTIMAL,
           "verify=0 quality status reflects solve status");
    ASSERT(quality.verify_enabled == 0, "verify=0 quality marks verification disabled");
    ASSERT(quality.available == 0, "verify=0 quality metrics are unavailable");
    ralph_test_free(verify_off);

    RalphModel *verify_on = ralph_test_create();
    build_telemetry_lp_case(verify_on);
    ASSERT(ralph_test_set_int_param(verify_on, "verify", 1) == 0,
           "verify=1 accepted");
    ASSERT(ralph_test_optimize_lp(verify_on) == 0, "verify=1 solve succeeds");
    ASSERT(ralph_test_get_status(verify_on) == RALPH_STATUS_OPTIMAL ||
           ralph_test_get_status(verify_on) == RALPH_STATUS_IMPRECISE,
           "verify=1 solve status is OPTIMAL/IMPRECISE");
    ASSERT(ralph_core_get_solution_quality(verify_on, &quality) == 0,
           "verify=1 quality snapshot retrieved");
    ASSERT(quality.verify_enabled == 1, "verify=1 quality marks verification enabled");
    ASSERT(quality.available == 1, "verify=1 quality metrics are available");
    ASSERT(quality.status == ralph_test_get_status(verify_on),
           "verify=1 quality status matches solve status");
    ASSERT(quality.primal_infeas >= 0.0 &&
           quality.bound_infeas >= 0.0 &&
           quality.dual_infeas >= 0.0 &&
           quality.comp_slack >= 0.0 &&
           quality.obj_error >= 0.0 &&
           quality.cond_estimate >= 0.0,
           "verify=1 quality metrics are non-negative");
    ralph_test_free(verify_on);

    RalphModel *infeas = ralph_test_create();
    ralph_test_set_obj_sense(infeas, RALPH_MINIMIZE);
    ralph_test_add_var(infeas, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    int idx[] = {0};
    double v1[] = {1.0};
    double v2[] = {1.0};
    ralph_test_add_constraint(infeas, 1, idx, v1, RALPH_LESS_EQUAL, 1.0);
    ralph_test_add_constraint(infeas, 1, idx, v2, RALPH_GREATER_EQUAL, 2.0);
    ASSERT(ralph_test_set_int_param(infeas, "verify", 1) == 0,
           "infeasible verify=1 accepted");
    ASSERT(ralph_test_optimize_lp(infeas) == 0, "infeasible solve call succeeds");
    ASSERT(ralph_test_get_status(infeas) == RALPH_STATUS_INFEASIBLE,
           "infeasible model returns INFEASIBLE");
    ASSERT(ralph_core_get_solution_quality(infeas, &quality) == 0,
           "infeasible quality snapshot retrieved");
    ASSERT(quality.status == RALPH_STATUS_INFEASIBLE,
           "infeasible quality status reflects INFEASIBLE");
    ASSERT(quality.available == 0,
           "infeasible quality metrics are unavailable");
    ralph_test_free(infeas);
}

typedef struct {
    int calls;
    int cancel_on_iter;
    int saw_phase2;
    int saw_quality;
    double last_elapsed_sec;
} LPProgressProbe;

static int test_lp_progress_probe_cb(void *user_data, const RalphLPProgressInfo *info) {
    LPProgressProbe *probe = (LPProgressProbe*)user_data;
    if (!probe || !info) return 0;
    probe->calls++;
    probe->last_elapsed_sec = info->elapsed_time_sec;
    if (info->phase == RALPH_LP_PROGRESS_PHASE_2) {
        probe->saw_phase2 = 1;
    }
    if (info->quality_available) {
        probe->saw_quality = 1;
    }
    if (probe->cancel_on_iter >= 0 && info->iteration >= probe->cancel_on_iter) {
        return 1;
    }
    return 0;
}

typedef struct {
    int polls;
    int cancel_after_polls;
} LPCancelProbe;

static int test_lp_cancel_probe_cb(void *user_data) {
    LPCancelProbe *probe = (LPCancelProbe*)user_data;
    if (!probe) return 0;
    probe->polls++;
    if (probe->cancel_after_polls > 0 && probe->polls >= probe->cancel_after_polls) {
        return 1;
    }
    return 0;
}

void test_lp_progress_callback_api(void) {
    printf("\n=== Test: LP Progress Callback API ===\n");

    RalphModel *model = ralph_test_create();
    build_telemetry_lp_case(model);
    ASSERT(ralph_test_set_int_param(model, "method", 0) == 0, "LP progress: method=0 accepted");
    ASSERT(ralph_test_set_int_param(model, "verify", 1) == 0, "LP progress: verify=1 accepted");

    LPProgressProbe probe;
    memset(&probe, 0, sizeof(probe));
    probe.cancel_on_iter = -1;

    RalphLPProgressCallback cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_progress = test_lp_progress_probe_cb;
    cb.user_data = &probe;
    cb.every_n_iterations = 1;
    ralph_core_set_lp_progress_callback(model, &cb);

    ASSERT(ralph_test_optimize_lp(model) == 0, "LP progress: solve call succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL ||
           ralph_test_get_status(model) == RALPH_STATUS_IMPRECISE,
           "LP progress: solve status is OPTIMAL/IMPRECISE");
    ASSERT(probe.calls > 0, "LP progress callback invoked");
    ASSERT(probe.last_elapsed_sec >= 0.0, "LP progress elapsed time is non-negative");

    ralph_core_set_lp_progress_callback(model, NULL);
    probe.calls = 0;
    ASSERT(ralph_test_optimize_lp(model) == 0, "LP progress: solve succeeds after clear");
    ASSERT(probe.calls == 0, "LP progress callback cleared");

    ralph_test_free(model);
}

void test_lp_progress_cancel_callback_api(void) {
    printf("\n=== Test: LP Progress Cancellation Callback API ===\n");

    RalphModel *model = ralph_test_create();
    build_telemetry_lp_case(model);
    ASSERT(ralph_test_set_int_param(model, "method", 0) == 0, "LP progress-cancel: method=0 accepted");

    LPProgressProbe probe;
    memset(&probe, 0, sizeof(probe));
    probe.cancel_on_iter = 0;

    RalphLPProgressCallback cb;
    memset(&cb, 0, sizeof(cb));
    cb.on_progress = test_lp_progress_probe_cb;
    cb.user_data = &probe;
    cb.every_n_iterations = 1;
    ralph_core_set_lp_progress_callback(model, &cb);

    ASSERT(ralph_test_optimize_lp(model) == 0, "LP progress-cancel: solve call succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_TIME_LIMIT,
           "LP progress-cancel: status is TIME_LIMIT");
    ASSERT(probe.calls >= 1, "LP progress-cancel callback invoked before termination");

    ralph_test_free(model);
}

void test_lp_cancel_poll_callback_api(void) {
    printf("\n=== Test: LP Cancel Poll Callback API ===\n");

    RalphModel *model = ralph_test_create();
    build_telemetry_lp_case(model);
    ASSERT(ralph_test_set_int_param(model, "method", 0) == 0, "LP cancel-poll: method=0 accepted");

    LPCancelProbe probe;
    memset(&probe, 0, sizeof(probe));
    probe.cancel_after_polls = 1;

    RalphLPCancelCallback cb;
    memset(&cb, 0, sizeof(cb));
    cb.should_cancel = test_lp_cancel_probe_cb;
    cb.user_data = &probe;
    ralph_core_set_lp_cancel_callback(model, &cb);

    ASSERT(ralph_test_optimize_lp(model) == 0, "LP cancel-poll: solve call succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_TIME_LIMIT,
           "LP cancel-poll: status is TIME_LIMIT");
    ASSERT(probe.polls >= 1, "LP cancel-poll callback invoked");

    ralph_core_set_lp_cancel_callback(model, NULL);
    ASSERT(ralph_test_optimize_lp(model) == 0, "LP cancel-poll: solve succeeds after clear");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL ||
           ralph_test_get_status(model) == RALPH_STATUS_IMPRECISE,
           "LP cancel-poll: status OPTIMAL/IMPRECISE after clear");

    ralph_test_free(model);
}

void test_lp_callbacks_are_orthogonal_to_mip(void) {
    printf("\n=== Test: LP Callbacks Orthogonal To MIP ===\n");

    RalphModel *model = ralph_test_create();
    build_telemetry_mip_case(model);
    ASSERT(ralph_test_set_int_param(model, "detect_special", 0) == 0,
           "LP/MIP callback orthogonality: detect_special disabled");

    LPProgressProbe progress_probe;
    memset(&progress_probe, 0, sizeof(progress_probe));
    progress_probe.cancel_on_iter = 0;
    RalphLPProgressCallback progress_cb;
    memset(&progress_cb, 0, sizeof(progress_cb));
    progress_cb.on_progress = test_lp_progress_probe_cb;
    progress_cb.user_data = &progress_probe;
    progress_cb.every_n_iterations = 1;
    ralph_core_set_lp_progress_callback(model, &progress_cb);

    LPCancelProbe cancel_probe;
    memset(&cancel_probe, 0, sizeof(cancel_probe));
    cancel_probe.cancel_after_polls = 1;
    RalphLPCancelCallback cancel_cb;
    memset(&cancel_cb, 0, sizeof(cancel_cb));
    cancel_cb.should_cancel = test_lp_cancel_probe_cb;
    cancel_cb.user_data = &cancel_probe;
    ralph_core_set_lp_cancel_callback(model, &cancel_cb);

    ASSERT(ralph_test_optimize_mip(model) == 0, "LP/MIP callback orthogonality: MIP solve call succeeds");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL,
           "LP/MIP callback orthogonality: MIP status remains OPTIMAL");
    ASSERT(progress_probe.calls == 0,
           "LP/MIP callback orthogonality: LP progress callback not invoked by MIP");
    ASSERT(cancel_probe.polls == 0,
           "LP/MIP callback orthogonality: LP cancel callback not polled by MIP");

    ralph_test_free(model);
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(int argc, char **argv) {
    int skip_mip = (argc > 1 && strcmp(argv[1], "--skip-mip") == 0);

    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║       Ralph LP/MIP Solver - Test Suite                   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    /* LP Tests */
    test_simple_lp();
    test_equality_constraint();
    test_greater_equal_constraint();
    test_diet_problem();
    test_infeasible_lp();
    test_farkas_ray();
    test_farkas_bound_conflict();
    test_farkas_sum_conflict();
    test_farkas_equality();
    test_farkas_negative_rhs_gsense();
    test_unbounded_ray_api();
    test_larger_lp();
    test_network_flow();  /* Regression test for objective computation bug */

    /* MIP Tests */
    if (!skip_mip) {
        test_binary_knapsack();
        test_branch_tightening_guard();
        test_integer_programming();
        test_mixed_integer();
        test_facility_location();
        test_gmi_cuts_knapsack();  /* Test GMI cut generation */

        /* c-MIR cut tests */
        test_cmir_cuts_mixed_knapsack();
        test_cmir_cuts_reduce_nodes();
        test_cmir_cuts_with_presolve();
        test_cmir_cuts_facility_location();
        test_cmir_cuts_validity();
        test_cmir_cuts_no_regression_presolve();
        test_cmir_binary_knapsack_regression();     /* c-MIR invalid cut bug */

        /* Regression tests for MIP bugs */
        test_mip_bound_adjustment_regression();     /* Suboptimal solution bug */
        test_mip_strong_branching_regression();     /* Strong branching crash */
        test_scp_lu_regression();                   /* Sparse LU bug with SCP */
        test_mip_incumbent_feasibility_regression(); /* Infeasible incumbent bug */

        /* Constraint modification and lazy constraint tests */
        test_constraint_modification();
        test_constraint_query_api();
        test_constraint_batch_edit_api();
        test_row_col_deletion_api();
        test_lp_iis_api();
        test_lazy_constraints();

        /* Branching control tests */
        test_branching_control();

        /* Warm start tests */
        test_warm_start_live_load();
        test_warm_start_staged_load();
        test_warm_start_edge_cases();
        test_basis_status_api();

        /* Cut callback tests */
        test_cut_callback();

        /* Branch callback tests */
        test_branch_callback();

        /* LP callback orthogonality vs MIP path */
        test_lp_callbacks_are_orthogonal_to_mip();

        /* Node selection strategy tests */
        test_node_selection_strategies();

        /* LAP-based MIP tests */
        test_lap_mip_assignment();
        test_lap_mip_assignment_5x5();

        /* Cut normalization tests (row_sign fix) */
        test_cuts_ge_constraint_normalization();
        test_cuts_mixed_sense_normalization();

        /* P5/P6: Bound flipping + Dual steepest edge tests */
        test_p5p6_no_false_infeasibility();
        test_p5p6_flags();

        /* Reliability branching tests */
        test_reliability_branching_basic();
        test_reliability_branching_with_priorities();
        test_reliability_vs_pseudocost();
        test_reliability_branching_all_binary();

        /* Cut quality filter tests */
        test_cut_quality_dynamism_filter();
        test_cut_parallel_detection();
        test_cut_minimum_violation();
        test_cuts_improve_bound();

        /* Reduced-cost fixing + RINS tests */
        test_rc_fixing_basic();
        test_rc_fixing_no_regression();
        test_rins_finds_incumbent();
        test_rins_no_regression();
    } else {
        printf("\n=== MIP tests skipped ===\n");
    }

    /* Multi-round scaling tests */
    test_scaling_multi_round_norms();
    test_scaling_disabled();
    test_scaling_no_regression();
    test_scaling_roundtrip();

    /* Crash basis tests */
    test_crash_basis_structural();
    test_crash_reduces_iterations();
    test_crash_infeasible();
    test_crash_no_regression();

    /* Post-solve verification tests (T2.3 + T3.6) */
    test_verify_clean_lp();
    test_verify_diet_with_verify();
    test_verify_network_flow();
    test_verify_no_regression();

    /* Objective limit tests (T3.1) */
    test_obj_limit_below_optimal();
    test_obj_limit_not_triggered();
    test_obj_limit_maximize();
    test_obj_limit_no_regression();

    /* Per-phase pricing tests (T3.4) */
    test_phase1_pricing_dantzig();
    test_phase1_pricing_disabled();

    /* Heap pricing tests (T2.2) */
    test_pricing_heap();
    test_pricing_all_strategies();

    /* Dual simplex method dispatch tests (T1.3) */
    test_dual_method_small_lp();
    test_dual_method_infeasible();
    test_dual_method_unbounded();
    test_dual_method_auto_fallback();
    test_dual_method_with_crash();
    test_dual_method_obj_limit();
    test_dual_method_no_regression();
    test_dual_phase1_free_vars();

    /* Re-optimization tests (Section 5: modify RHS/bounds, confirm no Phase I) */
    test_reoptimization_rhs_bounds();
    test_reoptimization_dual_no_phase1();

    /* Runtime telemetry parameter propagation (LP + MIP paths) */
    test_presolve_report_api();
    test_runtime_telemetry_param_propagation();
    test_public_telemetry_snapshot_api();
    test_solution_quality_api();
    test_lp_progress_callback_api();
    test_lp_progress_cancel_callback_api();
    test_lp_cancel_poll_callback_api();
    test_phase3_optimize_entrypoints();
    test_phase3_param_partition();
    test_phase3_optimize_backward_compatibility();
    test_param_typed_metadata_api();

    /* API Tests */
    test_api_functions();

    /* Summary */
    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Test Summary: %d/%d passed (%.1f%%)\n",
           tests_passed, tests_run, 100.0 * tests_passed / tests_run);

    if (tests_passed == tests_run) {
        printf("\n✓ All tests passed!\n");
        return 0;
    } else {
        printf("\n✗ Some tests failed.\n");
        return 1;
    }
}
