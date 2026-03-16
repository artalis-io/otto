/*
 * Ralph LP Solver - Edge Case Tests
 *
 * Tests that previously caused issues:
 * - >= constraints with Big-M method
 * - Problems that caused cycling
 * - Numerical stability edge cases
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"

static unsigned int seed;
static double randf(double lo, double hi) {
    seed = seed * 1103515245 + 12345;
    return lo + (seed % 10000) / 10000.0 * (hi - lo);
}

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        return 1; \
    } \
} while(0)

/*
 * Test 1: 50x25 problem with >= constraints
 * This problem previously caused cycling due to LU transpose solve bug.
 */
int test_50x25_geq_constraints(void) {
    printf("Test: 50x25 with >= constraints... ");
    fflush(stdout);

    seed = 42;
    int n = 50, m = 25;
    double density = 0.3;

    RalphModel *model = ralph_test_create();
    ralph_test_set_int_param(model, "verbose", 0);

    /* Store problem data for verification */
    double costs[50];
    double rhs[25];
    double coefs[25][50];
    memset(coefs, 0, sizeof(coefs));

    /* Add vars */
    for (int j = 0; j < n; j++) {
        costs[j] = randf(1, 10);
        ralph_test_add_var(model, 0.0, 100.0, costs[j], 'C');
    }

    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));

    /* Add >= constraints */
    for (int i = 0; i < m; i++) {
        int nnz = 0;
        for (int j = 0; j < n; j++) {
            if (randf(0, 1) < density) {
                idx[nnz] = j;
                val[nnz] = randf(1, 5);
                coefs[i][idx[nnz]] = val[nnz];
                nnz++;
            }
        }
        if (nnz == 0) { idx[0] = 0; val[0] = 1.0; coefs[i][0] = 1.0; nnz = 1; }
        rhs[i] = randf(50, 200);
        ralph_test_add_constraint(model, nnz, idx, val, 'G', rhs[i]);
    }

    ralph_test_optimize(model);

    int status = ralph_test_get_status(model);
    double obj = ralph_test_get_objval(model);

    /* Get solution and verify feasibility */
    double x[50];
    ralph_test_get_solution(model, x);

    /* Check constraints */
    int feasible = 1;
    double max_viol = 0;
    for (int i = 0; i < m; i++) {
        double lhs = 0;
        for (int j = 0; j < n; j++) {
            lhs += coefs[i][j] * x[j];
        }
        double viol = rhs[i] - lhs;
        if (viol > 1e-5) {
            feasible = 0;
        }
        if (viol > max_viol) max_viol = viol;
    }

    /* Check bounds */
    for (int j = 0; j < n; j++) {
        if (x[j] < -1e-5 || x[j] > 100 + 1e-5) {
            feasible = 0;
        }
    }

    free(idx);
    free(val);
    ralph_test_free(model);

    ASSERT(status == RALPH_STATUS_OPTIMAL, "Expected OPTIMAL status");
    ASSERT(feasible, "Solution must be feasible");
    ASSERT(obj > 400 && obj < 600, "Objective should be reasonable");

    printf("PASS (obj=%.2f, max_viol=%.2e)\n", obj, max_viol);
    return 0;
}

/*
 * Test 2: Problem that requires many iterations
 * Tests cycling prevention with Bland's rule.
 */
int test_degenerate_problem(void) {
    printf("Test: Degenerate problem (potential cycling)... ");
    fflush(stdout);

    seed = 123;
    int n = 30, m = 15;

    RalphModel *model = ralph_test_create();
    ralph_test_set_int_param(model, "verbose", 0);

    /* Create a problem with many degenerate vertices */
    for (int j = 0; j < n; j++) {
        ralph_test_add_var(model, 0.0, 10.0, randf(1, 5), 'C');
    }

    int idx[30];
    double val[30];

    for (int i = 0; i < m; i++) {
        int nnz = 0;
        /* Dense constraints to create degeneracy */
        for (int j = 0; j < n; j++) {
            if (randf(0, 1) < 0.5) {
                idx[nnz] = j;
                val[nnz] = randf(0.1, 2.0);
                nnz++;
            }
        }
        if (nnz == 0) { idx[0] = 0; val[0] = 1.0; nnz = 1; }
        ralph_test_add_constraint(model, nnz, idx, val, 'L', randf(5, 20));
    }

    ralph_test_optimize(model);
    int status = ralph_test_get_status(model);
    double obj = ralph_test_get_objval(model);

    ralph_test_free(model);

    ASSERT(status == RALPH_STATUS_OPTIMAL, "Expected OPTIMAL status");

    printf("PASS (obj=%.2f)\n", obj);
    return 0;
}

/*
 * Test 3: Mixed constraint types
 */
int test_mixed_constraints(void) {
    printf("Test: Mixed constraint types (<=, =, >=)... ");
    fflush(stdout);

    RalphModel *model = ralph_test_create();
    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    /* Variables */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 3.0, 'C');  /* x0 */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 2.0, 'C');  /* x1 */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, 'C');  /* x2 */

    int idx[3];
    double val[3];

    /* x0 + x1 <= 10 */
    idx[0] = 0; idx[1] = 1;
    val[0] = 1.0; val[1] = 1.0;
    ralph_test_add_constraint(model, 2, idx, val, 'L', 10.0);

    /* x1 + x2 = 5 */
    idx[0] = 1; idx[1] = 2;
    val[0] = 1.0; val[1] = 1.0;
    ralph_test_add_constraint(model, 2, idx, val, 'E', 5.0);

    /* x0 + x2 >= 3 */
    idx[0] = 0; idx[1] = 2;
    val[0] = 1.0; val[1] = 1.0;
    ralph_test_add_constraint(model, 2, idx, val, 'G', 3.0);

    ralph_test_optimize(model);
    int status = ralph_test_get_status(model);
    double obj = ralph_test_get_objval(model);

    ralph_test_free(model);

    ASSERT(status == RALPH_STATUS_OPTIMAL, "Expected OPTIMAL status");
    /* Optimal: x0=10, x1=0, x2=5 gives obj=35 */
    ASSERT(fabs(obj - 35.0) < 1e-5, "Expected obj=35");

    printf("PASS (obj=%.2f)\n", obj);
    return 0;
}

/*
 * Test 4: 20x10 problem verified against GLPK
 */
int test_20x10_glpk_verified(void) {
    printf("Test: 20x10 (GLPK verified)... ");
    fflush(stdout);

    seed = 42;
    int n = 20, m = 10;
    double density = 0.3;

    RalphModel *model = ralph_test_create();
    ralph_test_set_int_param(model, "verbose", 0);

    for (int j = 0; j < n; j++) {
        ralph_test_add_var(model, 0.0, 100.0, randf(1, 10), 'C');
    }

    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));

    for (int i = 0; i < m; i++) {
        int nnz = 0;
        for (int j = 0; j < n; j++) {
            if (randf(0, 1) < density) {
                idx[nnz] = j;
                val[nnz] = randf(1, 5);
                nnz++;
            }
        }
        if (nnz == 0) { idx[0] = 0; val[0] = 1.0; nnz = 1; }
        ralph_test_add_constraint(model, nnz, idx, val, 'G', randf(50, 200));
    }

    ralph_test_optimize(model);
    int status = ralph_test_get_status(model);
    double obj = ralph_test_get_objval(model);

    free(idx);
    free(val);
    ralph_test_free(model);

    ASSERT(status == RALPH_STATUS_OPTIMAL, "Expected OPTIMAL status");
    /* GLPK gets 606.0002261 for this problem */
    ASSERT(fabs(obj - 606.0) < 1.0, "Expected obj near 606 (GLPK: 606.0002)");

    printf("PASS (obj=%.2f, GLPK=606.00)\n", obj);
    return 0;
}

int main(void) {
    printf("Ralph LP Solver - Edge Case Tests\n");
    printf("==================================\n\n");

    int failures = 0;

    failures += test_50x25_geq_constraints();
    failures += test_degenerate_problem();
    failures += test_mixed_constraints();
    failures += test_20x10_glpk_verified();

    printf("\n");
    if (failures == 0) {
        printf("All edge case tests passed!\n");
    } else {
        printf("%d test(s) failed.\n", failures);
    }

    return failures;
}
