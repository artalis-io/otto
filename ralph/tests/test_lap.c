/*
 * test_lap.c - Tests for Linear Assignment Problem solver
 *
 * Tests the JVC algorithm against:
 * - Known optimal solutions
 * - LP solver results (cross-validation)
 * - Various problem structures and edge cases
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "lap.h"

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
 * Test helper: print assignment
 * ============================================================================ */
static void print_assignment(int n, const int *row_sol) {
    printf("  Assignment: ");
    for (int i = 0; i < n; i++) {
        printf("%d->%d ", i, row_sol[i]);
    }
    printf("\n");
}

/* ============================================================================
 * Test 1: Trivial 1x1 problem
 * ============================================================================ */
static void test_1x1(void) {
    printf("\n=== Test: 1x1 Trivial Problem ===\n");

    double cost[] = {5.0};
    int row_sol[1];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(1, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT(row_sol[0] == 0, "Row 0 assigned to column 0");
    ASSERT_NEAR(total_cost, 5.0, TOLERANCE, "Total cost");
}

/* ============================================================================
 * Test 2: Simple 2x2 problem
 * ============================================================================ */
static void test_2x2(void) {
    printf("\n=== Test: 2x2 Problem ===\n");

    /* Cost matrix:
     *      j=0  j=1
     * i=0   1    2
     * i=1   3    1
     *
     * Optimal: 0->0, 1->1, cost = 1 + 1 = 2
     */
    double cost[] = {1, 2, 3, 1};
    int row_sol[2], col_sol[2];
    double u[2], v[2];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(2, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, col_sol, u, v, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT_NEAR(total_cost, 2.0, TOLERANCE, "Total cost");
    ASSERT(row_sol[0] == 0, "Row 0 -> Column 0");
    ASSERT(row_sol[1] == 1, "Row 1 -> Column 1");
    ASSERT(col_sol[0] == 0, "Column 0 <- Row 0");
    ASSERT(col_sol[1] == 1, "Column 1 <- Row 1");

    /* Verify assignment */
    double verify_cost;
    ASSERT(ralph_lap_verify(2, cost, row_sol, &verify_cost), "Assignment is valid");
    ASSERT_NEAR(verify_cost, total_cost, TOLERANCE, "Verified cost matches");
}

/* ============================================================================
 * Test 3: Classic 3x3 problem
 * ============================================================================ */
static void test_3x3(void) {
    printf("\n=== Test: 3x3 Classic Problem ===\n");

    /* Cost matrix:
     *      j=0  j=1  j=2
     * i=0   9    2    7
     * i=1   6    4    3
     * i=2   5    8    1
     *
     * Optimal: 0->1, 1->0, 2->2, cost = 2 + 6 + 1 = 9
     */
    double cost[] = {
        9, 2, 7,
        6, 4, 3,
        5, 8, 1
    };
    int row_sol[3];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(3, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT_NEAR(total_cost, 9.0, TOLERANCE, "Total cost is 9");
    print_assignment(3, row_sol);

    /* Verify with LP solver */
    int lp_sol[3];
    double lp_cost;
    status = ralph_lap_solve_lp(3, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");
    ASSERT_NEAR(lp_cost, total_cost, TOLERANCE, "LP cost matches JVC cost");
}

/* ============================================================================
 * Test 4: Maximization problem
 * ============================================================================ */
static void test_maximize(void) {
    printf("\n=== Test: Maximization Problem ===\n");

    /* Same matrix, but maximize */
    double cost[] = {
        9, 2, 7,
        6, 4, 3,
        5, 8, 1
    };
    int row_sol[3];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(3, cost, RALPH_LAP_MAXIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    print_assignment(3, row_sol);

    /* Verify with LP solver */
    int lp_sol[3];
    double lp_cost;
    status = ralph_lap_solve_lp(3, cost, RALPH_LAP_MAXIMIZE, lp_sol, &lp_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");
    ASSERT_NEAR(lp_cost, total_cost, TOLERANCE, "LP cost matches JVC cost");
}

/* ============================================================================
 * Test 5: Identical costs (multiple optimal solutions)
 * ============================================================================ */
static void test_identical_costs(void) {
    printf("\n=== Test: Identical Costs ===\n");

    /* All costs are the same */
    double cost[] = {
        5, 5, 5,
        5, 5, 5,
        5, 5, 5
    };
    int row_sol[3];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(3, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT_NEAR(total_cost, 15.0, TOLERANCE, "Total cost is 15");
    ASSERT(ralph_lap_verify(3, cost, row_sol, NULL), "Valid permutation");
}

/* ============================================================================
 * Test 6: Diagonal matrix (identity optimal)
 * ============================================================================ */
static void test_diagonal(void) {
    printf("\n=== Test: Diagonal Matrix ===\n");

    /* Diagonal costs are minimum */
    double cost[] = {
        1, 10, 10, 10,
        10, 2, 10, 10,
        10, 10, 3, 10,
        10, 10, 10, 4
    };
    int row_sol[4];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(4, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT_NEAR(total_cost, 10.0, TOLERANCE, "Total cost is 1+2+3+4=10");
    ASSERT(row_sol[0] == 0, "Row 0 -> Column 0");
    ASSERT(row_sol[1] == 1, "Row 1 -> Column 1");
    ASSERT(row_sol[2] == 2, "Row 2 -> Column 2");
    ASSERT(row_sol[3] == 3, "Row 3 -> Column 3");
}

/* ============================================================================
 * Test 7: Anti-diagonal matrix
 * ============================================================================ */
static void test_antidiagonal(void) {
    printf("\n=== Test: Anti-diagonal Matrix ===\n");

    /* Anti-diagonal costs are minimum */
    double cost[] = {
        10, 10, 10, 1,
        10, 10, 2, 10,
        10, 3, 10, 10,
        4, 10, 10, 10
    };
    int row_sol[4];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(4, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT_NEAR(total_cost, 10.0, TOLERANCE, "Total cost is 1+2+3+4=10");
    ASSERT(row_sol[0] == 3, "Row 0 -> Column 3");
    ASSERT(row_sol[1] == 2, "Row 1 -> Column 2");
    ASSERT(row_sol[2] == 1, "Row 2 -> Column 1");
    ASSERT(row_sol[3] == 0, "Row 3 -> Column 0");
}

/* ============================================================================
 * Test 8: Larger random problem (5x5)
 * ============================================================================ */
static void test_5x5_random(void) {
    printf("\n=== Test: 5x5 Random Problem ===\n");

    double cost[] = {
        7, 2, 1, 9, 4,
        9, 6, 9, 5, 5,
        3, 8, 3, 1, 8,
        7, 9, 4, 2, 2,
        8, 4, 7, 4, 8
    };
    int n = 5;
    int row_sol[5], lp_sol[5];
    double jvc_cost, lp_cost;

    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &jvc_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");
    print_assignment(n, row_sol);

    status = ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");

    ASSERT_NEAR(jvc_cost, lp_cost, TOLERANCE, "JVC and LP costs match");
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Valid permutation");
}

/* ============================================================================
 * Test 9: 10x10 problem with cross-validation
 * ============================================================================ */
static void test_10x10(void) {
    printf("\n=== Test: 10x10 Cross-validation ===\n");

    int n = 10;
    double *cost = malloc(n * n * sizeof(double));

    /* Deterministic pseudo-random costs */
    srand(42);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    int *jvc_sol = malloc(n * sizeof(int));
    int *lp_sol = malloc(n * sizeof(int));
    double jvc_cost, lp_cost;

    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            jvc_sol, NULL, NULL, NULL, &jvc_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");

    status = ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");

    ASSERT_NEAR(jvc_cost, lp_cost, TOLERANCE, "JVC and LP costs match");
    ASSERT(ralph_lap_verify(n, cost, jvc_sol, NULL), "Valid permutation");

    printf("  JVC cost: %.2f, LP cost: %.2f\n", jvc_cost, lp_cost);

    free(cost);
    free(jvc_sol);
    free(lp_sol);
}

/* ============================================================================
 * Test 10: Problem with forbidden assignments (infinite costs)
 * ============================================================================ */
static void test_forbidden(void) {
    printf("\n=== Test: Forbidden Assignments ===\n");

    double INF = RALPH_LAP_INFINITY;
    /* Some assignments are forbidden */
    double cost[] = {
        INF, 2, 7,
        6, INF, 3,
        5, 8, INF
    };
    int row_sol[3];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(3, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    print_assignment(3, row_sol);

    /* Verify no forbidden pairs used */
    for (int i = 0; i < 3; i++) {
        ASSERT(row_sol[i] != i, "Diagonal (forbidden) not used");
    }

    ASSERT(ralph_lap_verify(3, cost, row_sol, NULL), "Valid permutation");
}

/* ============================================================================
 * Test 11: Negative costs
 * ============================================================================ */
static void test_negative_costs(void) {
    printf("\n=== Test: Negative Costs ===\n");

    double cost[] = {
        -5, -2, -1,
        -3, -4, -6,
        -1, -3, -2
    };
    int row_sol[3];
    double jvc_cost, lp_cost;

    RalphLapStatus status = ralph_lap_solve(3, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &jvc_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");

    int lp_sol[3];
    status = ralph_lap_solve_lp(3, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");

    ASSERT_NEAR(jvc_cost, lp_cost, TOLERANCE, "JVC and LP costs match");
    printf("  JVC cost: %.2f, LP cost: %.2f\n", jvc_cost, lp_cost);
}

/* ============================================================================
 * Test 12: Mixed positive/negative costs
 * ============================================================================ */
static void test_mixed_costs(void) {
    printf("\n=== Test: Mixed Positive/Negative Costs ===\n");

    double cost[] = {
        5, -2, 1, 0,
        -3, 4, -6, 2,
        1, -3, 2, 8,
        0, 7, -1, 3
    };
    int n = 4;
    int row_sol[4];
    double jvc_cost, lp_cost;

    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &jvc_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");

    int lp_sol[4];
    status = ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");

    ASSERT_NEAR(jvc_cost, lp_cost, TOLERANCE, "JVC and LP costs match");
}

/* ============================================================================
 * Test 13: Large problem (20x20)
 * ============================================================================ */
static void test_20x20(void) {
    printf("\n=== Test: 20x20 Large Problem ===\n");

    int n = 20;
    double *cost = malloc(n * n * sizeof(double));

    srand(123);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 1000) + 1;
    }

    int *jvc_sol = malloc(n * sizeof(int));
    int *lp_sol = malloc(n * sizeof(int));
    double jvc_cost, lp_cost;

    clock_t start = clock();
    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            jvc_sol, NULL, NULL, NULL, &jvc_cost);
    clock_t jvc_time = clock() - start;
    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");

    start = clock();
    status = ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    clock_t lp_time = clock() - start;
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");

    ASSERT_NEAR(jvc_cost, lp_cost, TOLERANCE, "JVC and LP costs match");
    ASSERT(ralph_lap_verify(n, cost, jvc_sol, NULL), "Valid permutation");

    printf("  JVC time: %.4f ms, LP time: %.4f ms\n",
           (double)jvc_time / CLOCKS_PER_SEC * 1000,
           (double)lp_time / CLOCKS_PER_SEC * 1000);

    free(cost);
    free(jvc_sol);
    free(lp_sol);
}

/* ============================================================================
 * Test 14: Sparse problem via sparse solver
 * ============================================================================ */
static void test_sparse(void) {
    printf("\n=== Test: Sparse LAP ===\n");

    int n = 4;
    /* Only some assignments are allowed */
    /* Row 0: cols 0, 1
     * Row 1: cols 1, 2
     * Row 2: cols 2, 3
     * Row 3: cols 0, 3
     */
    int row_ptr[] = {0, 2, 4, 6, 8};
    int col_idx[] = {0, 1, 1, 2, 2, 3, 0, 3};
    double values[] = {5, 3, 2, 4, 1, 6, 7, 2};

    int row_sol[4];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve_sparse(n, 8, row_ptr, col_idx, values,
                                                   RALPH_LAP_MINIMIZE,
                                                   row_sol, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Sparse solver succeeded");
    print_assignment(n, row_sol);

    /* Verify assignment uses only allowed edges */
    int valid = 1;
    for (int i = 0; i < n && valid; i++) {
        int j = row_sol[i];
        int found = 0;
        for (int k = row_ptr[i]; k < row_ptr[i+1]; k++) {
            if (col_idx[k] == j) {
                found = 1;
                break;
            }
        }
        if (!found) valid = 0;
    }
    ASSERT(valid, "Assignment uses only allowed edges");
}

/* ============================================================================
 * Test 15: Dual variables satisfy complementary slackness
 * ============================================================================ */
static void test_duals(void) {
    printf("\n=== Test: Dual Variables ===\n");

    double cost[] = {
        9, 2, 7,
        6, 4, 3,
        5, 8, 1
    };
    int n = 3;
    int row_sol[3];
    double u[3], v[3];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, u, v, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Solver succeeded");

    /* Check complementary slackness: c[i][j] - u[i] - v[j] = 0 for assigned pairs */
    int cs_valid = 1;
    for (int i = 0; i < n; i++) {
        int j = row_sol[i];
        double slack = cost[i * n + j] - u[i] - v[j];
        if (fabs(slack) > TOLERANCE) {
            cs_valid = 0;
            printf("  Slack at (%d,%d): %.6f\n", i, j, slack);
        }
    }
    ASSERT(cs_valid, "Complementary slackness satisfied");

    /* Check reduced costs are non-negative */
    int rc_valid = 1;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double rc = cost[i * n + j] - u[i] - v[j];
            if (rc < -TOLERANCE) {
                rc_valid = 0;
                printf("  Negative reduced cost at (%d,%d): %.6f\n", i, j, rc);
            }
        }
    }
    ASSERT(rc_valid, "All reduced costs non-negative");
}

/* ============================================================================
 * Test 16: Rectangular-like problem (padded to square)
 * ============================================================================ */
static void test_almost_rectangular(void) {
    printf("\n=== Test: Almost Rectangular (workers > jobs scenario) ===\n");

    /* Simulate 4 workers, 3 real jobs + 1 dummy job with high cost */
    double cost[] = {
        1, 5, 3, 100,
        4, 2, 6, 100,
        7, 3, 1, 100,
        2, 8, 4, 100
    };
    int n = 4;
    int row_sol[4];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Solver succeeded");
    print_assignment(n, row_sol);

    /* Exactly one worker should be assigned to the dummy job (col 3) */
    int dummy_count = 0;
    for (int i = 0; i < n; i++) {
        if (row_sol[i] == 3) dummy_count++;
    }
    ASSERT(dummy_count == 1, "Exactly one worker assigned to dummy job");

    /* Verify with LP */
    int lp_sol[4];
    double lp_cost;
    ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    ASSERT_NEAR(total_cost, lp_cost, TOLERANCE, "Matches LP solution");
}

/* ============================================================================
 * Test 17: Stress test with 50x50
 * ============================================================================ */
static void test_50x50(void) {
    printf("\n=== Test: 50x50 Stress Test ===\n");

    int n = 50;
    double *cost = malloc(n * n * sizeof(double));

    srand(999);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 10000) / 100.0;
    }

    int *row_sol = malloc(n * sizeof(int));
    double total_cost;

    clock_t start = clock();
    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);
    clock_t elapsed = clock() - start;

    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Valid permutation");

    printf("  JVC time: %.2f ms, cost: %.2f\n",
           (double)elapsed / CLOCKS_PER_SEC * 1000, total_cost);

    free(cost);
    free(row_sol);
}

/* ============================================================================
 * Test 18: Verify LP and JVC match on multiple random instances
 * ============================================================================ */
static void test_random_instances(void) {
    printf("\n=== Test: Random Instance Cross-validation ===\n");

    int num_instances = 10;
    int n = 8;
    double *cost = malloc(n * n * sizeof(double));
    int *jvc_sol = malloc(n * sizeof(int));
    int *lp_sol = malloc(n * sizeof(int));
    int all_match = 1;

    for (int inst = 0; inst < num_instances; inst++) {
        srand(1000 + inst);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 100) + 1;
        }

        double jvc_cost, lp_cost;
        ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, jvc_sol, NULL, NULL, NULL, &jvc_cost);
        ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);

        if (fabs(jvc_cost - lp_cost) > TOLERANCE) {
            printf("  Instance %d: JVC=%.2f, LP=%.2f (MISMATCH)\n",
                   inst, jvc_cost, lp_cost);
            all_match = 0;
        }
    }

    ASSERT(all_match, "All random instances match");
    printf("  Tested %d instances of size %dx%d\n", num_instances, n, n);

    free(cost);
    free(jvc_sol);
    free(lp_sol);
}

/* ============================================================================
 * Test 19: Status string function
 * ============================================================================ */
static void test_status_string(void) {
    printf("\n=== Test: Status Strings ===\n");

    ASSERT(strcmp(ralph_lap_status_string(RALPH_LAP_SUCCESS), "Success") == 0,
           "SUCCESS string");
    ASSERT(strcmp(ralph_lap_status_string(RALPH_LAP_INFEASIBLE), "Infeasible") == 0,
           "INFEASIBLE string");
    ASSERT(strcmp(ralph_lap_status_string(RALPH_LAP_INVALID_INPUT), "Invalid input") == 0,
           "INVALID_INPUT string");
    ASSERT(strcmp(ralph_lap_status_string(RALPH_LAP_MEMORY_ERROR), "Memory error") == 0,
           "MEMORY_ERROR string");
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Ralph LAP Solver Tests (JVC Algorithm)                   ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    test_1x1();
    test_2x2();
    test_3x3();
    test_maximize();
    test_identical_costs();
    test_diagonal();
    test_antidiagonal();
    test_5x5_random();
    test_10x10();
    test_forbidden();
    test_negative_costs();
    test_mixed_costs();
    test_20x20();
    test_sparse();
    test_duals();
    test_almost_rectangular();
    test_50x50();
    test_random_instances();
    test_status_string();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Test Summary: %d/%d passed (%.1f%%)\n",
           tests_passed, tests_run, 100.0 * tests_passed / tests_run);

    if (tests_passed == tests_run) {
        printf("\n✓ All tests passed!\n");
        return 0;
    } else {
        printf("\n✗ Some tests failed!\n");
        return 1;
    }
}
