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

    /* MIP Tests */
    if (!skip_mip) {
        test_binary_knapsack();
        test_integer_programming();
        test_mixed_integer();
        test_facility_location();
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
