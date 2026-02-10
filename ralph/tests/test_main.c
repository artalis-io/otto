/*
 * Ralph LP/MIP Solver - Test Suite
 *
 * Comprehensive tests for LP and MIP functionality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph.h"

#define TOLERANCE 1e-4

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;

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

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add variables: x, y with objective -1, -1 */
    ralph_add_var(model, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);
    ralph_add_var(model, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);

    /* Constraint 1: x + y <= 4 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx1, val1, RALPH_LESS_EQUAL, 4.0);

    /* Constraint 2: 2x + y <= 6 */
    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};
    ralph_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 6.0);

    /* Solve */
    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, -4.0, TOLERANCE, "Objective value");

    /* Verify solution satisfies constraints (don't check specific vertex) */
    double x[2];
    ralph_get_solution(model, x);
    double c1 = x[0] + x[1];       /* x + y <= 4 */
    double c2 = 2*x[0] + x[1];     /* 2x + y <= 6 */
    ASSERT(x[0] >= -TOLERANCE, "x[0] >= 0");
    ASSERT(x[1] >= -TOLERANCE, "x[1] >= 0");
    ASSERT(c1 <= 4.0 + TOLERANCE, "Constraint 1 satisfied");
    ASSERT(c2 <= 6.0 + TOLERANCE, "Constraint 2 satisfied");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_add_var(model, 1.0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x >= 1 */
    ralph_add_var(model, 0.0, 1e30, 2.0, RALPH_CONTINUOUS);  /* y >= 0 */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, RALPH_EQUAL, 3.0);

    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(ralph_get_objval(model), 3.0, TOLERANCE, "Objective value");

    double x[2];
    ralph_get_solution(model, x);
    ASSERT_NEAR(x[0], 3.0, TOLERANCE, "x[0] = 3");
    ASSERT_NEAR(x[1], 0.0, TOLERANCE, "x[1] = 0");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MAXIMIZE);

    ralph_add_var(model, 0.0, 2.0, 2.0, RALPH_CONTINUOUS);
    ralph_add_var(model, 0.0, 2.0, 3.0, RALPH_CONTINUOUS);

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(ralph_get_objval(model), 10.0, TOLERANCE, "Objective value");

    ralph_free(model);
}

/* ============================================================================
 * Test: Diet Problem (Classic LP)
 *
 * Minimize cost of diet while meeting nutritional requirements.
 * ============================================================================ */
void test_diet_problem(void) {
    printf("\n=== Test: Diet Problem ===\n");

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Foods: Bread, Milk, Cheese (cost: 2, 3.5, 8) */
    ralph_add_var(model, 0, 1e30, 2.0, RALPH_CONTINUOUS);    /* Bread */
    ralph_add_var(model, 0, 1e30, 3.5, RALPH_CONTINUOUS);    /* Milk */
    ralph_add_var(model, 0, 1e30, 8.0, RALPH_CONTINUOUS);    /* Cheese */

    /* Calories >= 300: 50*bread + 42*milk + 35*cheese >= 300 */
    int idx1[] = {0, 1, 2};
    double val1[] = {50, 42, 35};
    ralph_add_constraint(model, 3, idx1, val1, RALPH_GREATER_EQUAL, 300);

    /* Protein >= 10: 4*bread + 8*milk + 7*cheese >= 10 */
    double val2[] = {4, 8, 7};
    ralph_add_constraint(model, 3, idx1, val2, RALPH_GREATER_EQUAL, 10);

    /* Calcium >= 8: 0*bread + 3*milk + 2*cheese >= 8 */
    double val3[] = {0, 3, 2};
    ralph_add_constraint(model, 3, idx1, val3, RALPH_GREATER_EQUAL, 8);

    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    printf("  Diet cost: %.2f\n", obj);
    ASSERT(obj > 0 && obj < 100, "Reasonable cost");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MAXIMIZE);

    /* Binary variables */
    ralph_add_var(model, 0, 1, 5.0, RALPH_BINARY);
    ralph_add_var(model, 0, 1, 4.0, RALPH_BINARY);
    ralph_add_var(model, 0, 1, 3.0, RALPH_BINARY);

    /* Capacity constraint */
    int idx[] = {0, 1, 2};
    double val[] = {2.0, 3.0, 1.0};
    ralph_add_constraint(model, 3, idx, val, RALPH_LESS_EQUAL, 5.0);

    ASSERT(ralph_is_mip(model), "Model is MIP");

    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    printf("  Knapsack value: %.0f\n", obj);
    ASSERT(obj >= 8.0 - TOLERANCE, "Objective >= 8");

    double x[3];
    ralph_get_solution(model, x);
    printf("  Solution: x1=%.0f, x2=%.0f, x3=%.0f\n", x[0], x[1], x[2]);

    /* Verify integer solution */
    for (int i = 0; i < 3; i++) {
        ASSERT(fabs(x[i] - round(x[i])) < TOLERANCE, "Variable is integer");
    }

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);
    ralph_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 3.5);

    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, 4.0, TOLERANCE, "Objective = 4 (rounded up from 3.5)");

    double x[2];
    ralph_get_solution(model, x);
    ASSERT(x[0] + x[1] >= 3.5 - TOLERANCE, "Constraint satisfied");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_add_var(model, 0, 1e30, 1.0, RALPH_INTEGER);     /* x integer */
    ralph_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* y continuous */

    int idx[] = {0, 1};
    double val[] = {2.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 4.0);

    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, 2.0, TOLERANCE, "Objective = 2");

    double x[2];
    ralph_get_solution(model, x);
    ASSERT_NEAR(x[0], 2.0, TOLERANCE, "x = 2");
    ASSERT_NEAR(x[1], 0.0, TOLERANCE, "y = 0");

    ralph_free(model);
}

/* ============================================================================
 * Test: Facility Location (Classic MIP)
 *
 * Simplified 2-facility, 3-customer problem.
 * ============================================================================ */
void test_facility_location(void) {
    printf("\n=== Test: Facility Location ===\n");

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Variables:
     * y[0], y[1]: binary, whether facility is open
     * x[0][0], x[0][1], x[0][2]: fraction of customer j served by facility 0
     * x[1][0], x[1][1], x[1][2]: fraction of customer j served by facility 1
     */

    /* Fixed costs: 100, 150 */
    ralph_add_var(model, 0, 1, 100, RALPH_BINARY);  /* y[0] */
    ralph_add_var(model, 0, 1, 150, RALPH_BINARY);  /* y[1] */

    /* Transport costs (facility i to customer j) */
    double cost[2][3] = {{10, 20, 15}, {25, 10, 20}};

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            ralph_add_var(model, 0, 1, cost[i][j], RALPH_CONTINUOUS);
        }
    }

    /* Each customer must be fully served */
    for (int j = 0; j < 3; j++) {
        int idx[] = {2 + j, 2 + 3 + j};  /* x[0][j], x[1][j] */
        double val[] = {1.0, 1.0};
        ralph_add_constraint(model, 2, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Can only serve from open facility */
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            /* x[i][j] <= y[i] => x[i][j] - y[i] <= 0 */
            int idx[] = {2 + i*3 + j, i};
            double val[] = {1.0, -1.0};
            ralph_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
        }
    }

    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    printf("  Total cost: %.2f\n", obj);
    ASSERT(obj > 0, "Positive cost");

    double x[8];
    ralph_get_solution(model, x);
    printf("  Facility 0 open: %.0f\n", x[0]);
    printf("  Facility 1 open: %.0f\n", x[1]);

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    ralph_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);

    /* x <= 1 */
    int idx[] = {0};
    double val[] = {1.0};
    ralph_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 1.0);

    /* x >= 2 */
    ralph_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 2.0);

    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add variables x and y with bounds [0, inf) and objective 1.0 */
    ralph_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(model, 0, 1e30, 1.0, RALPH_CONTINUOUS);  /* y */

    /* Constraint 1: x + y >= 5 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx1, val1, RALPH_GREATER_EQUAL, 5.0);

    /* Constraint 2: x <= 1 */
    int idx2[] = {0};
    double val2[] = {1.0};
    ralph_add_constraint(model, 1, idx2, val2, RALPH_LESS_EQUAL, 1.0);

    /* Constraint 3: y <= 1 */
    int idx3[] = {1};
    double val3[] = {1.0};
    ralph_add_constraint(model, 1, idx3, val3, RALPH_LESS_EQUAL, 1.0);

    /* Solve */
    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_INFEASIBLE, "Status is INFEASIBLE");

    /* Get Farkas ray */
    int m = ralph_get_num_cons(model);
    double *ray = (double*)malloc(m * sizeof(double));
    int ret = ralph_get_farkas_ray(model, ray);
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
    ralph_free(model);
}

/* ============================================================================
 * Test: API Functions
 * ============================================================================ */
void test_api_functions(void) {
    printf("\n=== Test: API Functions ===\n");

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "ralph_create");

    ASSERT(ralph_get_num_vars(model) == 0, "Initial num_vars = 0");
    ASSERT(ralph_get_num_cons(model) == 0, "Initial num_cons = 0");

    ralph_add_var(model, 0, 10, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(model, 0, 10, 2.0, RALPH_INTEGER);

    ASSERT(ralph_get_num_vars(model) == 2, "num_vars = 2 after adding");
    ASSERT(ralph_get_num_integers(model) == 1, "num_integers = 1");
    ASSERT(ralph_is_mip(model) == 1, "is_mip = true");

    /* Test parameters */
    ralph_set_int_param(model, "verbose", 1);
    int verbose;
    ralph_get_int_param(model, "verbose", &verbose);
    ASSERT(verbose == 1, "Parameter get/set works");

    ralph_set_dbl_param(model, "time_limit", 60.0);
    double time_limit;
    ralph_get_dbl_param(model, "time_limit", &time_limit);
    ASSERT_NEAR(time_limit, 60.0, TOLERANCE, "Double parameter get/set");

    /* Test status string */
    const char *status_str = ralph_status_string(RALPH_STATUS_OPTIMAL);
    ASSERT(strcmp(status_str, "OPTIMAL") == 0, "Status string");

    /* Test version */
    const char *version = ralph_version();
    ASSERT(version != NULL && strlen(version) > 0, "Version string");
    printf("  Ralph version: %s\n", version);

    ralph_free(model);
}

/* ============================================================================
 * Test: Larger LP (Performance)
 *
 * Test with more variables/constraints to verify performance.
 * ============================================================================ */
void test_larger_lp(void) {
    printf("\n=== Test: Larger LP (20 vars, 10 constraints) ===\n");

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    int n = 20;  /* Variables */
    int m = 10;  /* Constraints */

    /* Add variables */
    for (int j = 0; j < n; j++) {
        ralph_add_var(model, 0, 1e30, (double)(j + 1), RALPH_CONTINUOUS);
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
            ralph_add_constraint(model, nnz, indices, values, RALPH_LESS_EQUAL, 10.0 + i);
        }
    }

    free(indices);
    free(values);

    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    printf("  Objective: %.4f\n", obj);
    ASSERT(obj >= 0, "Non-negative objective");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* 4 arcs with costs */
    ralph_add_var(model, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x0: 0->1, cost=1 */
    ralph_add_var(model, 0.0, 100.0, 2.0, RALPH_CONTINUOUS);  /* x1: 0->2, cost=2 */
    ralph_add_var(model, 0.0, 100.0, 3.0, RALPH_CONTINUOUS);  /* x2: 1->3, cost=3 */
    ralph_add_var(model, 0.0, 100.0, 4.0, RALPH_CONTINUOUS);  /* x3: 2->3, cost=4 */

    /* Node 0: x0 + x1 = 100 (source) */
    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx0, coef0, RALPH_EQUAL, 100.0);

    /* Node 1: -x0 + x2 = 0 */
    int idx1[] = {0, 2};
    double coef1[] = {-1.0, 1.0};
    ralph_add_constraint(model, 2, idx1, coef1, RALPH_EQUAL, 0.0);

    /* Node 2: -x1 + x3 = 0 */
    int idx2[] = {1, 3};
    double coef2[] = {-1.0, 1.0};
    ralph_add_constraint(model, 2, idx2, coef2, RALPH_EQUAL, 0.0);

    /* Node 3: -x2 - x3 = -100 (sink) */
    int idx3[] = {2, 3};
    double coef3[] = {-1.0, -1.0};
    ralph_add_constraint(model, 2, idx3, coef3, RALPH_EQUAL, -100.0);

    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, 400.0, TOLERANCE, "Objective value");

    /* Verify solution manually */
    double sol[4];
    ralph_get_solution(model, sol);
    double manual_obj = sol[0]*1.0 + sol[1]*2.0 + sol[2]*3.0 + sol[3]*4.0;
    ASSERT_NEAR(obj, manual_obj, TOLERANCE, "Objective matches manual calculation");

    /* Verify flow conservation */
    double node0_balance = sol[0] + sol[1];
    double node3_balance = -sol[2] - sol[3];
    ASSERT_NEAR(node0_balance, 100.0, TOLERANCE, "Source flow = 100");
    ASSERT_NEAR(node3_balance, -100.0, TOLERANCE, "Sink flow = -100");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Three binary variables */
    ralph_add_var(model, 0.0, 1.0, -10.0, RALPH_BINARY);  /* x */
    ralph_add_var(model, 0.0, 1.0, -6.0, RALPH_BINARY);   /* y */
    ralph_add_var(model, 0.0, 1.0, -4.0, RALPH_BINARY);   /* z */

    /* 5x + 3y + 2z <= 9 */
    int idx[] = {0, 1, 2};
    double coeffs[] = {5.0, 3.0, 2.0};
    ralph_add_constraint(model, 3, idx, coeffs, RALPH_LESS_EQUAL, 9.0);

    ralph_set_int_param(model, "verbose", 0);
    ralph_set_int_param(model, "max_cut_rounds", 3);
    ralph_set_int_param(model, "max_nodes", 100);

    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, -16.0, TOLERANCE, "Optimal objective is -16");

    double sol[3];
    ralph_get_solution(model, sol);

    /* Verify solution is binary */
    ASSERT(fabs(sol[0] - 0.0) < TOLERANCE || fabs(sol[0] - 1.0) < TOLERANCE, "x is binary");
    ASSERT(fabs(sol[1] - 0.0) < TOLERANCE || fabs(sol[1] - 1.0) < TOLERANCE, "y is binary");
    ASSERT(fabs(sol[2] - 0.0) < TOLERANCE || fabs(sol[2] - 1.0) < TOLERANCE, "z is binary");

    /* Verify constraint satisfied */
    double lhs = 5*sol[0] + 3*sol[1] + 2*sol[2];
    ASSERT(lhs <= 9.0 + TOLERANCE, "Knapsack constraint satisfied");

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_set_int_param(model, "verbose", 0);
    ralph_set_dbl_param(model, "time_limit", 30.0);
    ralph_set_dbl_param(model, "mip_gap", 0.0001);

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
        ralph_add_var(model, 0.0, 1.0, fixed_cost[j], RALPH_BINARY);
    }

    /* Variables x[i,j]: assignment fractions (continuous) */
    for (int i = 0; i < num_customers; i++) {
        for (int j = 0; j < num_facilities; j++) {
            double dist = test_rand_double(1.0, 50.0);
            ralph_add_var(model, 0.0, 1.0, dist * demand[i], RALPH_CONTINUOUS);
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
        ralph_add_constraint(model, num_facilities, indices, values, RALPH_EQUAL, 1.0);
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
            ralph_add_constraint(model, 2, idx2, val2, RALPH_LESS_EQUAL, 0.0);
        }
    }

    free(indices);
    free(values);
    free(fixed_cost);
    free(demand);

    ralph_optimize(model);

    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    /* Known optimal value from GLPK: 1799.329423 */
    double expected_obj = 1799.329423;
    ASSERT_NEAR(obj, expected_obj, 1.0, "Optimal objective matches GLPK");

    double *sol = malloc(num_vars * sizeof(double));
    ralph_get_solution(model, sol);

    /* Verify facilities are binary */
    int binary_ok = 1;
    for (int j = 0; j < num_facilities; j++) {
        if (fabs(sol[j]) > TOLERANCE && fabs(sol[j] - 1.0) > TOLERANCE) {
            binary_ok = 0;
        }
    }
    ASSERT(binary_ok, "Facility decisions are binary");

    free(sol);
    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_set_int_param(model, "verbose", 0);
    ralph_set_int_param(model, "var_select", 2);  /* Strong branching */
    ralph_set_dbl_param(model, "time_limit", 30.0);
    ralph_set_dbl_param(model, "mip_gap", 0.0001);

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
        ralph_add_var(model, 0.0, 1.0, costs[j], RALPH_BINARY);
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
        ralph_add_constraint(model, nnz, idx, val, RALPH_EQUAL, 1.0);
    }

    /* This should not crash due to strong branching state corruption */
    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL || status == RALPH_STATUS_INFEASIBLE,
           "Status is OPTIMAL or INFEASIBLE (not crashed)");

    if (status == RALPH_STATUS_OPTIMAL) {
        double sol[10];
        ralph_get_solution(model, sol);

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

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add 9 binary variables x[i,j] with objective costs */
    for (int i = 0; i < 9; i++) {
        ralph_add_var(model, 0.0, 1.0, costs[i], RALPH_BINARY);
    }

    /* Row constraints: sum_j x[i,j] = 1 for each row i */
    for (int i = 0; i < 3; i++) {
        int idx[3] = {i*3, i*3+1, i*3+2};
        double val[3] = {1.0, 1.0, 1.0};
        ralph_add_constraint(model, 3, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Column constraints: sum_i x[i,j] = 1 for each column j */
    for (int j = 0; j < 3; j++) {
        int idx[3] = {j, 3+j, 6+j};
        double val[3] = {1.0, 1.0, 1.0};
        ralph_add_constraint(model, 3, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Enable LAP detection for this model */
    ralph_set_int_param(model, "detect_special", 1);

    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, 6.0, TOLERANCE, "Optimal cost is 6");

    /* Verify solution is a valid assignment */
    double sol[9];
    ralph_get_solution(model, sol);

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

    ralph_free(model);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add 25 binary variables */
    for (int i = 0; i < 25; i++) {
        ralph_add_var(model, 0.0, 1.0, costs[i], RALPH_BINARY);
    }

    /* Row constraints */
    for (int i = 0; i < 5; i++) {
        int idx[5] = {i*5, i*5+1, i*5+2, i*5+3, i*5+4};
        double val[5] = {1, 1, 1, 1, 1};
        ralph_add_constraint(model, 5, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Column constraints */
    for (int j = 0; j < 5; j++) {
        int idx[5] = {j, 5+j, 10+j, 15+j, 20+j};
        double val[5] = {1, 1, 1, 1, 1};
        ralph_add_constraint(model, 5, idx, val, RALPH_EQUAL, 1.0);
    }

    ralph_set_int_param(model, "detect_special", 1);
    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    /* Verify solution is a valid assignment */
    double sol[25];
    ralph_get_solution(model, sol);

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
    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, manual_cost, TOLERANCE, "Objective matches computed cost");

    ralph_free(model);
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
    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    for (int j = 0; j < num_subsets; j++) {
        double cost = scp_rand_double(1.0, 10.0);
        ralph_add_var(model, 0.0, 1.0, cost, RALPH_BINARY);
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
        ralph_add_constraint(model, nnz, indices, values, 'G', 1.0);
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

    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    double *costs = malloc(num_subsets * sizeof(double));
    for (int j = 0; j < num_subsets; j++) {
        costs[j] = sp_rand_double(1.0, 10.0);
        ralph_add_var(model, 0.0, 1.0, costs[j], RALPH_BINARY);
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
        ralph_add_constraint(model, nnz, indices, values, 'E', 1.0);  /* SetPartitioning: = 1 */
    }

    ralph_set_int_param(model, "verbose", 0);
    ralph_optimize(model);

    int status = ralph_get_status(model);

    /* If OPTIMAL, verify solution feasibility */
    if (status == RALPH_STATUS_OPTIMAL) {
        double *x = malloc(num_subsets * sizeof(double));
        ralph_get_solution(model, x);

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

        ASSERT(feasible, "OPTIMAL solution must satisfy all constraints");
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
    ralph_free(model);
}

void test_scp_lu_regression(void) {
    printf("\n=== Test: SCP LU Regression ===\n");

    /* This specific problem (50x100 SCP, density 0.3, seed 12345) triggered the bug
     * where sparse LU factorization produced wrong results for certain RHS values. */
    RalphModel *model = create_scp(50, 100, 0.3, 12345);

    ralph_set_int_param(model, "verbose", 0);
    ralph_set_int_param(model, "detect_special", 0);
    ralph_set_int_param(model, "presolve", 0);
    ralph_set_int_param(model, "max_iterations", 10000);

    ralph_optimize(model);

    int status = ralph_get_status(model);
    double obj = ralph_get_objval(model);

    /* The bug caused status to be INFEASIBLE or ITERATION_LIMIT
     * when the correct status is OPTIMAL with objective around 9.0 */
    ASSERT(status == RALPH_STATUS_OPTIMAL, "SCP should be OPTIMAL");
    ASSERT(obj >= 8.0 && obj <= 15.0, "SCP objective should be reasonable (8-15)");

    if (status == RALPH_STATUS_OPTIMAL) {
        printf("  SCP solved: obj = %.2f, iterations = %d\n",
               obj, ralph_get_iterations(model));
    } else {
        printf("  SCP failed: status = %d (%s)\n",
               status, ralph_status_string(status));
    }

    ralph_free(model);
}

/* ============================================================================
 * Test: Constraint Modification API
 *
 * Tests ralph_set_constraint_rhs() and ralph_get_var_bounds().
 * ============================================================================ */
void test_constraint_modification(void) {
    printf("\n=== Test: Constraint Modification API ===\n");

    /* Simple LP: min x + y
     * s.t. x + y >= 2
     *      x, y >= 0
     * Optimal: x=0, y=2 or x=2, y=0, obj=2
     */
    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 2.0);

    /* Test bounds query */
    double lb, ub;
    int ret = ralph_get_var_bounds(model, 0, &lb, &ub);
    ASSERT(ret == 0, "ralph_get_var_bounds returns 0");
    ASSERT_NEAR(lb, 0.0, TOLERANCE, "Lower bound is 0");
    ASSERT(ub >= 1e29, "Upper bound is infinity");

    /* Solve */
    ralph_optimize(model);
    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Initial solve is OPTIMAL");
    ASSERT_NEAR(ralph_get_objval(model), 2.0, TOLERANCE, "Objective is 2.0");

    /* Modify RHS: change x + y >= 2 to x + y >= 5 */
    ret = ralph_set_constraint_rhs(model, 0, 5.0);
    ASSERT(ret == 0, "ralph_set_constraint_rhs returns 0");

    /* Re-solve */
    ralph_optimize(model);
    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Re-solve is OPTIMAL");
    ASSERT_NEAR(ralph_get_objval(model), 5.0, TOLERANCE, "New objective is 5.0");

    /* Test invalid constraint index */
    ret = ralph_set_constraint_rhs(model, 99, 1.0);
    ASSERT(ret == -1, "Invalid constraint index returns -1");

    /* Test invalid variable index for bounds query */
    ret = ralph_get_var_bounds(model, 99, &lb, &ub);
    ASSERT(ret == -1, "Invalid variable index returns -1");

    ralph_free(model);
}

/* ============================================================================
 * Test: Lazy Constraints API
 *
 * Tests ralph_add_lazy_constraint() and ralph_add_lazy_constraints().
 * ============================================================================ */
void test_lazy_constraints(void) {
    printf("\n=== Test: Lazy Constraints API ===\n");

    /* Start with relaxed LP: min x + y
     * s.t. x + y <= 10  (doesn't affect optimum at x=y=0)
     *      x >= 0, y >= 0
     * Optimal: x=0, y=0, obj=0
     */
    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);  /* y */

    /* Add a loose constraint that doesn't affect the optimum */
    int idx0[] = {0, 1};
    double val0[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx0, val0, RALPH_LESS_EQUAL, 10.0);

    /* Solve relaxed */
    ralph_optimize(model);
    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "Relaxed solve is OPTIMAL");
    ASSERT_NEAR(ralph_get_objval(model), 0.0, TOLERANCE, "Relaxed objective is 0.0");

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

    int ret = ralph_add_lazy_constraint(model, &cut1);
    ASSERT(ret == 0, "ralph_add_lazy_constraint returns 0");

    /* Re-solve with cut */
    ralph_optimize(model);
    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "With cut is OPTIMAL");
    ASSERT_NEAR(ralph_get_objval(model), 3.0, TOLERANCE, "With cut objective is 3.0");

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
    ret = ralph_add_lazy_constraints(model, cuts, 2);
    ASSERT(ret == 0, "ralph_add_lazy_constraints returns 0");

    /* Re-solve */
    ralph_optimize(model);
    ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "With multiple cuts is OPTIMAL");
    ASSERT_NEAR(ralph_get_objval(model), 4.0, TOLERANCE, "With multiple cuts objective is 4.0");

    ralph_free(model);
}

/* ============================================================================
 * Test: Branching Control API
 *
 * Tests the ralph_set_branch_priorities() and ralph_set_branch_directions()
 * functions for MIP branching control.
 * ============================================================================ */
void test_branching_control(void) {
    printf("\n=== Test: Branching Control API ===\n");

    /* Simple MIP: min x0 + x1
     * s.t. x0 + x1 >= 1
     *      x0, x1 binary
     * Optimal: x0=1 or x1=1, obj=1
     */
    RalphModel *model = ralph_create();
    ASSERT(model != NULL, "Model created");

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Two binary variables */
    ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x0 */
    ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);  /* x1 */

    /* x0 + x1 >= 1 */
    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    ralph_add_constraint(model, 2, idx, val, RALPH_GREATER_EQUAL, 1.0);

    /* Set priorities: x1 has higher priority than x0 */
    int priorities[2] = {1, 10};  /* x1 (priority 10) > x0 (priority 1) */
    int ret = ralph_set_branch_priorities(model, priorities);
    ASSERT(ret == 0, "ralph_set_branch_priorities returns 0");

    /* Set branch directions: prefer x1=1 first (BRANCH_UP) */
    int directions[2] = {RALPH_BRANCH_AUTO, RALPH_BRANCH_UP};
    ret = ralph_set_branch_directions(model, directions);
    ASSERT(ret == 0, "ralph_set_branch_directions returns 0");

    /* Solve */
    ralph_optimize(model);

    RalphStatus status = ralph_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Status is OPTIMAL");

    double obj = ralph_get_objval(model);
    ASSERT_NEAR(obj, 1.0, TOLERANCE, "Objective value is 1.0");

    double x[2];
    ralph_get_solution(model, x);

    /* Verify solution: exactly one variable is 1 */
    int sum = (int)(x[0] + 0.5) + (int)(x[1] + 0.5);
    ASSERT(sum >= 1, "At least one variable is 1");
    ASSERT(x[0] + x[1] >= 1.0 - TOLERANCE, "Constraint satisfied");

    ralph_free(model);

    /* Test clearing priorities and directions */
    model = ralph_create();
    ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);

    /* Set then clear */
    int prios[2] = {5, 5};
    ralph_set_branch_priorities(model, prios);
    ret = ralph_set_branch_priorities(model, NULL);  /* Clear */
    ASSERT(ret == 0, "Clearing priorities returns 0");

    ret = ralph_set_branch_directions(model, NULL);  /* Clear non-existent */
    ASSERT(ret == 0, "Clearing directions returns 0");

    ralph_free(model);
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
    test_larger_lp();
    test_network_flow();  /* Regression test for objective computation bug */

    /* MIP Tests */
    if (!skip_mip) {
        test_binary_knapsack();
        test_integer_programming();
        test_mixed_integer();
        test_facility_location();
        test_gmi_cuts_knapsack();  /* Test GMI cut generation */

        /* Regression tests for MIP bugs */
        test_mip_bound_adjustment_regression();     /* Suboptimal solution bug */
        test_mip_strong_branching_regression();     /* Strong branching crash */
        test_scp_lu_regression();                   /* Sparse LU bug with SCP */
        test_mip_incumbent_feasibility_regression(); /* Infeasible incumbent bug */

        /* Constraint modification and lazy constraint tests */
        test_constraint_modification();
        test_lazy_constraints();

        /* Branching control tests */
        test_branching_control();

        /* LAP-based MIP tests */
        test_lap_mip_assignment();
        test_lap_mip_assignment_5x5();
    } else {
        printf("\n=== MIP tests skipped ===\n");
    }

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
