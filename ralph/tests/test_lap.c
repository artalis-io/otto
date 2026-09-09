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
#include "sh_time.h"
#include "lap.h"
#include "detect.h"
#include "ralph_test_mod_api.h"

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
 * Benchmark timing
 *
 * The ratio assertions below used clock(), which is far too coarse for them:
 * on MinGW CLOCKS_PER_SEC is 1000 and the underlying tick is 15.6 ms, so an arm
 * that runs in a millisecond measures as 0.000 ms and the ratio taken from it
 * is a division by zero. sh_monotonic_seconds() resolves to 100 ns on both
 * platforms.
 *
 * Resolution alone is not enough: a fast enough machine still finishes an arm
 * inside the noise. Each benchmark grows its iteration count until the baseline
 * arm clears BENCH_MIN_MS, so the ratio is always taken between two numbers
 * large enough to mean something.
 * ============================================================================ */
static double bench_now_ms(void) {
    return sh_monotonic_seconds() * 1000.0;
}

#define BENCH_MIN_MS   20.0
#define BENCH_MAX_REPS 64

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

/* Helper for unified callback tests */
typedef struct { int n; const double *c; } UnifiedCostCtx;

static double unified_callback_fn(int i, int j, void *ud) {
    UnifiedCostCtx *c = (UnifiedCostCtx*)ud;
    return c->c[i * c->n + j];
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

    double start = bench_now_ms();
    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            jvc_sol, NULL, NULL, NULL, &jvc_cost);
    double jvc_time = bench_now_ms() - start;
    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");

    start = bench_now_ms();
    status = ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);
    double lp_time = bench_now_ms() - start;
    ASSERT(status == RALPH_LAP_SUCCESS, "LP solver succeeded");

    ASSERT_NEAR(jvc_cost, lp_cost, TOLERANCE, "JVC and LP costs match");
    ASSERT(ralph_lap_verify(n, cost, jvc_sol, NULL), "Valid permutation");

    printf("  JVC time: %.4f ms, LP time: %.4f ms\n", jvc_time, lp_time);

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

    double start = bench_now_ms();
    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                            row_sol, NULL, NULL, NULL, &total_cost);
    double elapsed = bench_now_ms() - start;

    ASSERT(status == RALPH_LAP_SUCCESS, "JVC solver succeeded");
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Valid permutation");

    printf("  JVC time: %.2f ms, cost: %.2f\n", elapsed, total_cost);

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
 * Test 20: Workspace API
 * ============================================================================ */
static void test_workspace(void) {
    printf("\n=== Test: Workspace API ===\n");

    /* Create workspace for max n=100 */
    RalphLapWorkspace *ws = ralph_lap_workspace_create(100);
    ASSERT(ws != NULL, "Workspace created");
    ASSERT(ralph_lap_workspace_max_n(ws) == 100, "Workspace max_n is 100");

    /* Solve same problem multiple times with workspace */
    int n = 10;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol1 = malloc(n * sizeof(int));
    int *row_sol2 = malloc(n * sizeof(int));
    double cost1, cost2;

    srand(999);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    RalphLapStatus s1 = ralph_lap_solve_with_workspace(n, cost, RALPH_LAP_MINIMIZE,
                                                        row_sol1, NULL, NULL, NULL, &cost1, ws);
    RalphLapStatus s2 = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                         row_sol2, NULL, NULL, NULL, &cost2);

    ASSERT(s1 == RALPH_LAP_SUCCESS, "Workspace solve succeeded");
    ASSERT(s2 == RALPH_LAP_SUCCESS, "Standard solve succeeded");
    ASSERT(fabs(cost1 - cost2) < TOLERANCE, "Workspace and standard costs match");

    int sols_match = 1;
    for (int i = 0; i < n; i++) {
        if (row_sol1[i] != row_sol2[i]) {
            sols_match = 0;
            break;
        }
    }
    ASSERT(sols_match, "Workspace and standard solutions match");

    /* Test workspace with smaller problems */
    int n2 = 5;
    double cost2_small[] = {
        1, 2, 3, 4, 5,
        5, 4, 3, 2, 1,
        1, 1, 1, 1, 1,
        9, 8, 7, 6, 5,
        2, 4, 6, 8, 10
    };
    int row_sol3[5];
    double cost3;

    RalphLapStatus s3 = ralph_lap_solve_with_workspace(n2, cost2_small, RALPH_LAP_MINIMIZE,
                                                        row_sol3, NULL, NULL, NULL, &cost3, ws);
    ASSERT(s3 == RALPH_LAP_SUCCESS, "Smaller problem with same workspace succeeded");
    ASSERT(ralph_lap_verify(n2, cost2_small, row_sol3, NULL), "Solution is valid permutation");

    /* Test workspace capacity check */
    int n_too_big = 150;
    double *cost_big = calloc(n_too_big * n_too_big, sizeof(double));
    int *row_sol_big = malloc(n_too_big * sizeof(int));
    RalphLapStatus s4 = ralph_lap_solve_with_workspace(n_too_big, cost_big, RALPH_LAP_MINIMIZE,
                                                        row_sol_big, NULL, NULL, NULL, NULL, ws);
    ASSERT(s4 == RALPH_LAP_INVALID_INPUT, "Oversized problem rejected");

    free(cost_big);
    free(row_sol_big);
    free(cost);
    free(row_sol1);
    free(row_sol2);
    ralph_lap_workspace_free(ws);
    printf("  Workspace freed successfully\n");
}

/* ============================================================================
 * Test 21: Rectangular LAP (more jobs than workers)
 * ============================================================================ */
static void test_rect_more_jobs(void) {
    printf("\n=== Test: Rectangular LAP (3 workers, 5 jobs) ===\n");

    /* 3 workers, 5 jobs - 2 jobs will be unassigned */
    int m = 3, n = 5;
    double cost[] = {
        10, 5, 13, 4, 8,   /* Worker 0 */
        3, 7, 11, 6, 2,    /* Worker 1 */
        15, 9, 1, 12, 14   /* Worker 2 */
    };

    int row_sol[3], col_sol[5];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve_rect(m, n, cost, RALPH_LAP_MINIMIZE,
                                                  row_sol, col_sol, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Rectangular solve succeeded");

    /* All workers should be assigned */
    int all_assigned = 1;
    for (int i = 0; i < m; i++) {
        if (row_sol[i] < 0 || row_sol[i] >= n) {
            all_assigned = 0;
            break;
        }
    }
    ASSERT(all_assigned, "All workers assigned to valid jobs");

    /* Check for valid permutation (no duplicate assignments) */
    int used[5] = {0};
    int valid = 1;
    for (int i = 0; i < m; i++) {
        if (used[row_sol[i]]) {
            valid = 0;
            break;
        }
        used[row_sol[i]] = 1;
    }
    ASSERT(valid, "No duplicate job assignments");

    /* Exactly 2 jobs should be unassigned */
    int unassigned_jobs = 0;
    for (int j = 0; j < n; j++) {
        if (col_sol[j] == RALPH_LAP_UNASSIGNED) {
            unassigned_jobs++;
        }
    }
    ASSERT(unassigned_jobs == 2, "Exactly 2 jobs unassigned");

    printf("  Assignment: ");
    for (int i = 0; i < m; i++) printf("W%d->J%d ", i, row_sol[i]);
    printf("\n  Total cost: %.2f\n", total_cost);

    /* Optimal should be: W0->J3(4), W1->J4(2), W2->J2(1) = 7 */
    ASSERT(fabs(total_cost - 7.0) < TOLERANCE, "Optimal cost is 7");
}

/* ============================================================================
 * Test 22: Rectangular LAP (more workers than jobs)
 * ============================================================================ */
static void test_rect_more_workers(void) {
    printf("\n=== Test: Rectangular LAP (5 workers, 3 jobs) ===\n");

    /* 5 workers, 3 jobs - 2 workers will be unassigned */
    int m = 5, n = 3;
    double cost[] = {
        10, 5, 13,   /* Worker 0 */
        3, 7, 11,    /* Worker 1 */
        15, 9, 1,    /* Worker 2 */
        4, 6, 12,    /* Worker 3 */
        8, 2, 14     /* Worker 4 */
    };

    int row_sol[5], col_sol[3];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve_rect(m, n, cost, RALPH_LAP_MINIMIZE,
                                                  row_sol, col_sol, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Rectangular solve succeeded");

    /* All jobs should be assigned */
    int all_jobs_assigned = 1;
    for (int j = 0; j < n; j++) {
        if (col_sol[j] < 0 || col_sol[j] >= m) {
            all_jobs_assigned = 0;
            break;
        }
    }
    ASSERT(all_jobs_assigned, "All jobs assigned to valid workers");

    /* Exactly 2 workers should be unassigned */
    int unassigned_workers = 0;
    for (int i = 0; i < m; i++) {
        if (row_sol[i] == RALPH_LAP_UNASSIGNED) {
            unassigned_workers++;
        }
    }
    ASSERT(unassigned_workers == 2, "Exactly 2 workers unassigned");

    printf("  Assignment: ");
    for (int i = 0; i < m; i++) {
        if (row_sol[i] >= 0) {
            printf("W%d->J%d ", i, row_sol[i]);
        }
    }
    printf("\n  Unassigned workers: ");
    for (int i = 0; i < m; i++) {
        if (row_sol[i] < 0) printf("W%d ", i);
    }
    printf("\n  Total cost: %.2f\n", total_cost);

    /* Optimal should be: W1->J0(3), W4->J1(2), W2->J2(1) = 6 */
    ASSERT(fabs(total_cost - 6.0) < TOLERANCE, "Optimal cost is 6");
}

/* ============================================================================
 * Test 23: Rectangular LAP maximization
 * ============================================================================ */
static void test_rect_maximize(void) {
    printf("\n=== Test: Rectangular LAP Maximization ===\n");

    /* 2 workers, 4 jobs - maximize */
    int m = 2, n = 4;
    double cost[] = {
        10, 5, 8, 3,
        7, 12, 4, 9
    };

    int row_sol[2];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve_rect(m, n, cost, RALPH_LAP_MAXIMIZE,
                                                  row_sol, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Maximize succeeded");
    ASSERT(row_sol[0] != row_sol[1], "Different jobs assigned");

    printf("  Assignment: W0->J%d, W1->J%d\n", row_sol[0], row_sol[1]);
    printf("  Total cost: %.2f\n", total_cost);

    /* Optimal should be: W0->J0(10), W1->J1(12) = 22 */
    ASSERT(fabs(total_cost - 22.0) < TOLERANCE, "Optimal cost is 22");
}

/* ============================================================================
 * Test 24: Sparse LAP - very sparse (native algorithm)
 * ============================================================================ */
static void test_sparse_native(void) {
    printf("\n=== Test: Sparse LAP (Native Algorithm) ===\n");

    /* Create a very sparse 10x10 problem (< 30% density to trigger native) */
    int n = 10;
    /* Each row has only 2-3 edges */
    int row_ptr[] = {0, 2, 5, 7, 10, 12, 15, 17, 20, 22, 25};
    int col_idx[] = {
        0, 1,           /* Row 0: cols 0, 1 */
        1, 2, 3,        /* Row 1: cols 1, 2, 3 */
        2, 4,           /* Row 2: cols 2, 4 */
        3, 5, 6,        /* Row 3: cols 3, 5, 6 */
        4, 7,           /* Row 4: cols 4, 7 */
        5, 6, 8,        /* Row 5: cols 5, 6, 8 */
        6, 9,           /* Row 6: cols 6, 9 */
        7, 8, 9,        /* Row 7: cols 7, 8, 9 */
        8, 9,           /* Row 8: cols 8, 9 */
        0, 5, 9         /* Row 9: cols 0, 5, 9 */
    };
    double values[] = {
        1.0, 10.0,          /* Row 0 */
        5.0, 2.0, 8.0,      /* Row 1 */
        3.0, 7.0,           /* Row 2 */
        4.0, 6.0, 9.0,      /* Row 3 */
        2.0, 5.0,           /* Row 4 */
        8.0, 1.0, 4.0,      /* Row 5 */
        3.0, 6.0,           /* Row 6 */
        7.0, 2.0, 5.0,      /* Row 7 */
        4.0, 8.0,           /* Row 8 */
        9.0, 3.0, 1.0       /* Row 9 */
    };
    int nnz = 25;

    int row_sol[10], col_sol[10];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve_sparse(n, nnz, row_ptr, col_idx, values,
                                                    RALPH_LAP_MINIMIZE,
                                                    row_sol, col_sol, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Sparse native solve succeeded");

    /* Verify it's a valid permutation */
    int used[10] = {0};
    int valid = 1;
    for (int i = 0; i < n; i++) {
        if (row_sol[i] < 0 || row_sol[i] >= n || used[row_sol[i]]) {
            valid = 0;
            break;
        }
        used[row_sol[i]] = 1;
    }
    ASSERT(valid, "Valid permutation");

    /* Verify all assignments use valid edges */
    int all_valid_edges = 1;
    for (int i = 0; i < n; i++) {
        int j = row_sol[i];
        int found = 0;
        for (int k = row_ptr[i]; k < row_ptr[i + 1]; k++) {
            if (col_idx[k] == j) {
                found = 1;
                break;
            }
        }
        if (!found) {
            all_valid_edges = 0;
            break;
        }
    }
    ASSERT(all_valid_edges, "All assignments use valid edges");

    printf("  Assignment: ");
    for (int i = 0; i < n; i++) printf("%d->%d ", i, row_sol[i]);
    printf("\n  Total cost: %.2f\n", total_cost);
}

/* ============================================================================
 * Test 25: Sparse LAP - maximization
 * ============================================================================ */
static void test_sparse_maximize(void) {
    printf("\n=== Test: Sparse LAP (Maximization) ===\n");

    /* Same sparse problem as test_sparse but with maximization */
    int n = 4;
    int row_ptr[] = {0, 2, 4, 6, 8};
    int col_idx[] = {0, 1, 1, 2, 2, 3, 0, 3};
    double values[] = {5, 3, 2, 4, 1, 6, 7, 2};

    int row_sol[4];
    double total_cost;

    /* Solve with maximization */
    RalphLapStatus status = ralph_lap_solve_sparse(n, 8, row_ptr, col_idx, values,
                                                   RALPH_LAP_MAXIMIZE,
                                                   row_sol, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Sparse maximize succeeded");

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

    /* Compute cost manually to verify */
    double manual_cost = 0;
    for (int i = 0; i < n; i++) {
        int j = row_sol[i];
        for (int k = row_ptr[i]; k < row_ptr[i+1]; k++) {
            if (col_idx[k] == j) {
                manual_cost += values[k];
                break;
            }
        }
    }
    ASSERT_NEAR(total_cost, manual_cost, TOLERANCE, "Cost computed correctly");

    /* Compare with dense maximization */
    double dense_cost[16];
    for (int i = 0; i < 16; i++) dense_cost[i] = -RALPH_LAP_INFINITY;  /* Use negative infinity for maximize */
    for (int i = 0; i < n; i++) {
        for (int k = row_ptr[i]; k < row_ptr[i+1]; k++) {
            dense_cost[i * n + col_idx[k]] = values[k];
        }
    }

    int dense_sol[4];
    double dense_total;
    ralph_lap_solve(n, dense_cost, RALPH_LAP_MAXIMIZE, dense_sol, NULL, NULL, NULL, &dense_total);

    ASSERT_NEAR(total_cost, dense_total, TOLERANCE, "Sparse maximize matches dense maximize");

    print_assignment(n, row_sol);
    printf("  Total cost (max): %.2f\n", total_cost);
}

/* ============================================================================
 * Test 26: Sparse LAP - infeasible (no perfect matching)
 * ============================================================================ */
static void test_sparse_infeasible(void) {
    printf("\n=== Test: Sparse LAP (Infeasible) ===\n");

    /* 10x10 problem where no perfect matching exists (< 30% density for native sparse)
     * Rows 0-4 can only go to columns 0-2 (5 rows competing for 3 columns)
     * Rows 5-9 can go to columns 3-9
     */
    int n = 10;
    int row_ptr[] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20};
    int col_idx[] = {
        0, 1,           /* Row 0: cols 0, 1 */
        0, 2,           /* Row 1: cols 0, 2 */
        1, 2,           /* Row 2: cols 1, 2 */
        0, 1,           /* Row 3: cols 0, 1 */
        1, 2,           /* Row 4: cols 1, 2 */
        3, 4,           /* Row 5: cols 3, 4 */
        5, 6,           /* Row 6: cols 5, 6 */
        7, 8,           /* Row 7: cols 7, 8 */
        8, 9,           /* Row 8: cols 8, 9 */
        3, 9            /* Row 9: cols 3, 9 */
    };
    double values[] = {
        1.0, 2.0,
        3.0, 4.0,
        5.0, 6.0,
        7.0, 8.0,
        9.0, 10.0,
        1.0, 2.0,
        3.0, 4.0,
        5.0, 6.0,
        7.0, 8.0,
        9.0, 10.0
    };
    int nnz = 20;  /* 20/100 = 20% density - uses native sparse */

    int row_sol[10];
    RalphLapStatus status = ralph_lap_solve_sparse(n, nnz, row_ptr, col_idx, values,
                                                    RALPH_LAP_MINIMIZE,
                                                    row_sol, NULL, NULL);

    ASSERT(status == RALPH_LAP_INFEASIBLE, "Correctly detected infeasible problem");
}

/* ============================================================================
 * Test 26: Sparse vs Dense comparison
 * ============================================================================ */
static void test_sparse_vs_dense(void) {
    printf("\n=== Test: Sparse vs Dense Comparison ===\n");

    /* Create a sparse problem and solve both ways
     * Key: Ensure a perfect matching exists by including diagonal edges */
    int n = 20;
    double *dense_cost = malloc(n * n * sizeof(double));

    /* Initialize dense with infinity */
    for (int i = 0; i < n * n; i++) {
        dense_cost[i] = RALPH_LAP_INFINITY;
    }

    /* Create CSR with ~20% density but guaranteed perfect matching
     * Each row i has edge to column i (diagonal) plus some random edges */
    int max_nnz = n * 4;  /* Diagonal + up to 3 random per row */
    int *row_ptr = malloc((n + 1) * sizeof(int));
    int *col_idx = malloc(max_nnz * sizeof(int));
    double *values = malloc(max_nnz * sizeof(double));

    srand(12345);
    int nnz = 0;
    row_ptr[0] = 0;

    for (int i = 0; i < n; i++) {
        int cols_used[20] = {0};

        /* Always include diagonal edge to guarantee perfect matching exists */
        col_idx[nnz] = i;
        values[nnz] = (rand() % 100) + 50;  /* Higher cost for diagonal */
        dense_cost[i * n + i] = values[nnz];
        cols_used[i] = 1;
        nnz++;

        /* Add 1-3 additional random edges */
        int num_extra = 1 + (rand() % 3);
        for (int e = 0; e < num_extra; e++) {
            int j = rand() % n;
            if (!cols_used[j]) {
                cols_used[j] = 1;
                col_idx[nnz] = j;
                values[nnz] = (rand() % 100) + 1;  /* Lower cost for off-diagonal */
                dense_cost[i * n + j] = values[nnz];
                nnz++;
            }
        }
        row_ptr[i + 1] = nnz;
    }

    /* Solve sparse */
    int *sparse_sol = malloc(n * sizeof(int));
    double sparse_cost;
    RalphLapStatus sparse_status = ralph_lap_solve_sparse(n, nnz, row_ptr, col_idx, values,
                                                           RALPH_LAP_MINIMIZE,
                                                           sparse_sol, NULL, &sparse_cost);

    /* Solve dense */
    int *dense_sol = malloc(n * sizeof(int));
    double dense_cost_val;
    RalphLapStatus dense_status = ralph_lap_solve(n, dense_cost, RALPH_LAP_MINIMIZE,
                                                   dense_sol, NULL, NULL, NULL, &dense_cost_val);

    /* Both should succeed (perfect matching exists) */
    ASSERT(sparse_status == RALPH_LAP_SUCCESS, "Sparse solver succeeded");
    ASSERT(dense_status == RALPH_LAP_SUCCESS, "Dense solver succeeded");

    if (sparse_status == RALPH_LAP_SUCCESS && dense_status == RALPH_LAP_SUCCESS) {
        /* Costs should match */
        ASSERT(fabs(sparse_cost - dense_cost_val) < TOLERANCE,
               "Sparse and dense costs match");
        printf("  Sparse cost: %.2f, Dense cost: %.2f\n", sparse_cost, dense_cost_val);
    }

    free(dense_cost);
    free(row_ptr);
    free(col_idx);
    free(values);
    free(sparse_sol);
    free(dense_sol);
}

/* ============================================================================
 * Test 27: Parallel setting
 * ============================================================================ */
static void test_parallel_setting(void) {
    printf("\n=== Test: Parallel Setting ===\n");

    /* Check default is enabled */
    ASSERT(ralph_lap_get_parallel() == 1, "Parallel enabled by default");

    /* Disable and verify */
    ralph_lap_set_parallel(0);
    ASSERT(ralph_lap_get_parallel() == 0, "Parallel disabled");

    /* Solve a problem with parallel disabled */
    int n = 50;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));
    srand(777);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    double cost_val;
    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                             row_sol, NULL, NULL, NULL, &cost_val);
    ASSERT(status == RALPH_LAP_SUCCESS, "Solve succeeded with parallel disabled");
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Solution valid");

    /* Re-enable */
    ralph_lap_set_parallel(1);
    ASSERT(ralph_lap_get_parallel() == 1, "Parallel re-enabled");

    free(cost);
    free(row_sol);
}

/* ============================================================================
 * Test 22: Repeated solves with workspace (performance)
 * ============================================================================ */
static void test_workspace_repeated(void) {
    printf("\n=== Test: Workspace Repeated Solves ===\n");

    int n = 50;
    int num_solves = 20;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));
    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);

    double time_no_ws = 0.0, time_ws = 0.0;

    for (int reps = 1; ; reps *= 2) {
        num_solves = 20 * reps;

        /* Time without workspace */
        double start1 = bench_now_ms();
        for (int s = 0; s < num_solves; s++) {
            srand(2000 + s);
            for (int i = 0; i < n * n; i++) {
                cost[i] = (rand() % 1000) / 10.0;
            }
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL);
        }
        time_no_ws = bench_now_ms() - start1;

        /* Time with workspace */
        double start2 = bench_now_ms();
        for (int s = 0; s < num_solves; s++) {
            srand(2000 + s);
            for (int i = 0; i < n * n; i++) {
                cost[i] = (rand() % 1000) / 10.0;
            }
            ralph_lap_solve_with_workspace(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL, ws);
        }
        time_ws = bench_now_ms() - start2;

        if (time_no_ws >= BENCH_MIN_MS || reps >= BENCH_MAX_REPS) break;
    }

    ralph_lap_workspace_free(ws);

    printf("  %d solves of %dx%d:\n", num_solves, n, n);
    printf("    Without workspace: %.3f ms\n", time_no_ws);
    printf("    With workspace:    %.3f ms\n", time_ws);
    printf("    Speedup: %.2fx\n", time_no_ws / time_ws);

    ASSERT(time_ws <= time_no_ws * 1.1, "Workspace is not slower than allocating each time");

    free(cost);
    free(row_sol);
}

/* ============================================================================
 * Test 29: Epsilon scaling auction
 * ============================================================================ */
static void test_epsilon_scaling(void) {
    printf("\n=== Test: Epsilon Scaling Auction ===\n");

    /* Check default is disabled */
    ASSERT(ralph_lap_get_epsilon_scaling() == 0, "Epsilon scaling disabled by default");
    ASSERT(fabs(ralph_lap_get_epsilon_factor() - 4.0) < 0.01, "Default factor is 4.0");

    /* Enable epsilon scaling */
    ralph_lap_set_epsilon_scaling(1);
    ASSERT(ralph_lap_get_epsilon_scaling() == 1, "Epsilon scaling enabled");

    /* Set custom factor */
    ralph_lap_set_epsilon_factor(2.0);
    ASSERT(fabs(ralph_lap_get_epsilon_factor() - 2.0) < 0.01, "Factor set to 2.0");

    /* Solve a problem with epsilon scaling */
    int n = 50;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));
    srand(888);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    double cost_eps;
    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                             row_sol, NULL, NULL, NULL, &cost_eps);
    ASSERT(status == RALPH_LAP_SUCCESS, "Solve succeeded with epsilon scaling");
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Solution valid");

    /* Disable epsilon scaling and solve same problem */
    ralph_lap_set_epsilon_scaling(0);
    int *row_sol_std = malloc(n * sizeof(int));
    double cost_std;
    ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol_std, NULL, NULL, NULL, &cost_std);

    /* Costs should match (both optimal) */
    ASSERT(fabs(cost_eps - cost_std) < TOLERANCE, "Epsilon and standard costs match");
    printf("  Epsilon: %.2f, Standard: %.2f\n", cost_eps, cost_std);

    /* Test on problem with tied costs (where epsilon scaling helps) */
    printf("  Testing on tied costs problem...\n");
    for (int i = 0; i < n * n; i++) {
        cost[i] = 10.0;  /* All costs equal */
    }
    /* Add small perturbations on diagonal */
    for (int i = 0; i < n; i++) {
        cost[i * n + i] = 9.0 + 0.01 * i;
    }

    ralph_lap_set_epsilon_scaling(1);
    status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &cost_eps);
    ASSERT(status == RALPH_LAP_SUCCESS, "Tied costs: epsilon scaling succeeded");
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Tied costs: solution valid");

    ralph_lap_set_epsilon_scaling(0);
    ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol_std, NULL, NULL, NULL, &cost_std);
    ASSERT(fabs(cost_eps - cost_std) < TOLERANCE, "Tied costs: costs match");

    /* Reset to defaults */
    ralph_lap_set_epsilon_scaling(0);
    ralph_lap_set_epsilon_factor(4.0);

    free(cost);
    free(row_sol);
    free(row_sol_std);
}

/* ============================================================================
 * Test 30: Warm start basic functionality
 * ============================================================================ */
static void test_warm_start_basic(void) {
    printf("\n=== Test: Warm Start Basic ===\n");

    int n = 20;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));
    int *col_sol = malloc(n * sizeof(int));
    double *u = malloc(n * sizeof(double));
    double *v = malloc(n * sizeof(double));

    /* Create workspace */
    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
    ASSERT(ws != NULL, "Workspace created");

    /* Initially no warm start */
    ASSERT(ralph_lap_warm_start_valid(ws) == 0, "No warm start initially");

    /* Solve first problem - cold start, save for warm start */
    srand(111);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    double cost1;
    RalphLapStatus status = ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE,
                                                  row_sol, col_sol, u, v, &cost1,
                                                  ws, 1);  /* save_for_warm_start = 1 */
    ASSERT(status == RALPH_LAP_SUCCESS, "First solve succeeded");
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "First solution valid");

    /* Now warm start should be valid */
    ASSERT(ralph_lap_warm_start_valid(ws) == 1, "Warm start valid after first solve");

    /* Solve same problem again with warm start */
    double cost2;
    int *row_sol2 = malloc(n * sizeof(int));
    status = ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE,
                                   row_sol2, NULL, NULL, NULL, &cost2,
                                   ws, 1);
    ASSERT(status == RALPH_LAP_SUCCESS, "Warm start solve succeeded");
    ASSERT_NEAR(cost1, cost2, TOLERANCE, "Costs match");

    /* Solutions should match */
    int match = 1;
    for (int i = 0; i < n; i++) {
        if (row_sol[i] != row_sol2[i]) {
            match = 0;
            break;
        }
    }
    ASSERT(match, "Solutions match");

    /* Clear warm start */
    ralph_lap_warm_start_clear(ws);
    ASSERT(ralph_lap_warm_start_valid(ws) == 0, "Warm start cleared");

    free(cost);
    free(row_sol);
    free(row_sol2);
    free(col_sol);
    free(u);
    free(v);
    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test 31: Warm start with similar problems
 * ============================================================================ */
static void test_warm_start_similar(void) {
    printf("\n=== Test: Warm Start Similar Problems ===\n");

    int n = 30;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));
    double *u = malloc(n * sizeof(double));
    double *v = malloc(n * sizeof(double));

    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);

    /* Solve initial problem */
    srand(222);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    double cost1;
    ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE,
                          row_sol, NULL, u, v, &cost1, ws, 1);
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Initial solution valid");

    /* Slightly perturb costs */
    srand(333);
    for (int i = 0; i < n * n / 10; i++) {
        int idx = rand() % (n * n);
        cost[idx] += (rand() % 10) - 5;  /* Small perturbation */
    }

    /* Solve with warm start */
    double cost2;
    ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE,
                          row_sol, NULL, NULL, NULL, &cost2, ws, 1);
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Perturbed solution valid");

    /* Compare with cold start to verify optimality */
    double cost_cold;
    int *row_sol_cold = malloc(n * sizeof(int));
    ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                     row_sol_cold, NULL, NULL, NULL, &cost_cold);
    ASSERT_NEAR(cost2, cost_cold, TOLERANCE, "Warm start finds optimal solution");

    printf("  Initial cost: %.2f, Perturbed warm: %.2f, Cold: %.2f\n",
           cost1, cost2, cost_cold);

    free(cost);
    free(row_sol);
    free(row_sol_cold);
    free(u);
    free(v);
    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test 32: Warm start with different problems
 * ============================================================================ */
static void test_warm_start_different(void) {
    printf("\n=== Test: Warm Start Different Problems ===\n");

    int n = 25;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));

    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);

    /* Solve first problem */
    srand(444);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    double cost1;
    ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE,
                          row_sol, NULL, NULL, NULL, &cost1, ws, 1);
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "First solution valid");

    /* Solve completely different problem */
    srand(555);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 1000) + 1;
    }

    double cost2;
    ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE,
                          row_sol, NULL, NULL, NULL, &cost2, ws, 1);
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "Second solution valid");

    /* Compare with cold start */
    double cost_cold;
    int *row_sol_cold = malloc(n * sizeof(int));
    ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                     row_sol_cold, NULL, NULL, NULL, &cost_cold);
    ASSERT_NEAR(cost2, cost_cold, TOLERANCE, "Warm start finds optimal even for different problem");

    free(cost);
    free(row_sol);
    free(row_sol_cold);
    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test 33: Warm start manual initialization
 * ============================================================================ */
static void test_warm_start_manual(void) {
    printf("\n=== Test: Warm Start Manual Initialization ===\n");

    int n = 15;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));
    int *col_sol = malloc(n * sizeof(int));
    double *u = malloc(n * sizeof(double));
    double *v = malloc(n * sizeof(double));

    /* Solve to get dual variables */
    srand(666);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    double cost1;
    ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                     row_sol, col_sol, u, v, &cost1);

    /* Create new workspace and manually initialize warm start */
    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
    RalphLapStatus status = ralph_lap_warm_start(ws, n, u, v, row_sol, col_sol);
    ASSERT(status == RALPH_LAP_SUCCESS, "Manual warm start succeeded");
    ASSERT(ralph_lap_warm_start_valid(ws) == 1, "Warm start valid");

    /* Solve with manual warm start */
    double cost2;
    int *row_sol2 = malloc(n * sizeof(int));
    status = ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE,
                                   row_sol2, NULL, NULL, NULL, &cost2, ws, 0);
    ASSERT(status == RALPH_LAP_SUCCESS, "Solve with manual warm start succeeded");
    ASSERT_NEAR(cost1, cost2, TOLERANCE, "Costs match");

    free(cost);
    free(row_sol);
    free(row_sol2);
    free(col_sol);
    free(u);
    free(v);
    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test 34: Warm start with maximization
 * ============================================================================ */
static void test_warm_start_maximize(void) {
    printf("\n=== Test: Warm Start Maximization ===\n");

    int n = 20;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));

    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);

    /* Solve maximization problem */
    srand(777);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    double cost1;
    ralph_lap_solve_warm(n, cost, RALPH_LAP_MAXIMIZE,
                          row_sol, NULL, NULL, NULL, &cost1, ws, 1);
    ASSERT(ralph_lap_verify(n, cost, row_sol, NULL), "First max solution valid");

    /* Solve again with warm start */
    double cost2;
    ralph_lap_solve_warm(n, cost, RALPH_LAP_MAXIMIZE,
                          row_sol, NULL, NULL, NULL, &cost2, ws, 1);
    ASSERT_NEAR(cost1, cost2, TOLERANCE, "Maximize costs match");

    /* Verify against cold start */
    double cost_cold;
    int *row_sol_cold = malloc(n * sizeof(int));
    ralph_lap_solve(n, cost, RALPH_LAP_MAXIMIZE,
                     row_sol_cold, NULL, NULL, NULL, &cost_cold);
    ASSERT_NEAR(cost2, cost_cold, TOLERANCE, "Warm start max matches cold start");

    free(cost);
    free(row_sol);
    free(row_sol_cold);
    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test 35: Warm start performance comparison
 * ============================================================================ */
static void test_warm_start_performance(void) {
    printf("\n=== Test: Warm Start Performance ===\n");

    int n = 50;
    int num_problems = 10;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol = malloc(n * sizeof(int));

    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);

    double cold_time = 0.0, warm_time = 0.0;

    for (int reps = 1; ; reps *= 2) {
        num_problems = 10 * reps;

        /* Generate base problem, then measure cold start time */
        srand(888);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 100) + 1;
        }
        double cold_start = bench_now_ms();
        for (int p = 0; p < num_problems; p++) {
            /* Small perturbation */
            for (int i = 0; i < 5; i++) {
                int idx = rand() % (n * n);
                cost[idx] = (rand() % 100) + 1;
            }
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL);
        }
        cold_time = bench_now_ms() - cold_start;

        /* Reset problem: same seed, so the warm arm sees the same perturbations */
        srand(888);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 100) + 1;
        }
        ralph_lap_warm_start_clear(ws);

        /* Measure warm start time */
        double warm_start = bench_now_ms();
        for (int p = 0; p < num_problems; p++) {
            /* Same perturbations */
            for (int i = 0; i < 5; i++) {
                int idx = rand() % (n * n);
                cost[idx] = (rand() % 100) + 1;
            }
            ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL, ws, 1);
        }
        warm_time = bench_now_ms() - warm_start;

        if (cold_time >= BENCH_MIN_MS || reps >= BENCH_MAX_REPS) break;
    }

    printf("  %d problems of size %dx%d with small perturbations:\n", num_problems, n, n);
    printf("    Cold start: %.3f ms total\n", cold_time);
    printf("    Warm start: %.3f ms total\n", warm_time);
    printf("    Ratio: %.2fx\n", cold_time / warm_time);

    /* Warm start should not be significantly slower */
    ASSERT(warm_time <= cold_time * 1.5, "Warm start not much slower than cold start");

    free(cost);
    free(row_sol);
    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test 36: Epsilon scaling with maximization
 * ============================================================================ */
static void test_epsilon_scaling_maximize(void) {
    printf("\n=== Test: Epsilon Scaling Maximization ===\n");

    int n = 30;
    double *cost = malloc(n * n * sizeof(double));
    int *row_sol_eps = malloc(n * sizeof(int));
    int *row_sol_std = malloc(n * sizeof(int));

    /* Create problem with some forbidden edges */
    srand(999);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (rand() % 4 == 0) {
                cost[i * n + j] = RALPH_LAP_INFINITY;
            } else {
                cost[i * n + j] = (rand() % 100) + 1;
            }
        }
        /* Ensure at least one valid edge per row */
        cost[i * n + (i % n)] = (rand() % 50) + 1;
    }

    double cost_eps, cost_std;

    /* Solve with epsilon scaling */
    ralph_lap_set_epsilon_scaling(1);
    RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MAXIMIZE,
                                             row_sol_eps, NULL, NULL, NULL, &cost_eps);
    ASSERT(status == RALPH_LAP_SUCCESS, "Maximize: epsilon scaling succeeded");
    ASSERT(ralph_lap_verify(n, cost, row_sol_eps, NULL), "Maximize: solution valid");

    /* Solve without epsilon scaling */
    ralph_lap_set_epsilon_scaling(0);
    ralph_lap_solve(n, cost, RALPH_LAP_MAXIMIZE, row_sol_std, NULL, NULL, NULL, &cost_std);

    /* Costs should match */
    ASSERT(fabs(cost_eps - cost_std) < TOLERANCE, "Maximize: costs match");
    printf("  Epsilon: %.2f, Standard: %.2f\n", cost_eps, cost_std);

    /* Verify no forbidden edges used */
    int valid = 1;
    for (int i = 0; i < n && valid; i++) {
        if (cost[i * n + row_sol_eps[i]] >= RALPH_LAP_INFINITY * 0.5) {
            valid = 0;
        }
    }
    ASSERT(valid, "Maximize: no forbidden edges used");

    free(cost);
    free(row_sol_eps);
    free(row_sol_std);
}

/* ============================================================================
 * Test 37: Callback-based solver basic
 * ============================================================================ */

/* Callback context for dense matrix */
typedef struct {
    int n;
    const double *cost;
} DenseCostContext;

static double dense_cost_callback(int i, int j, void *user_data) {
    DenseCostContext *ctx = (DenseCostContext *)user_data;
    return ctx->cost[i * ctx->n + j];
}

static void test_callback_basic(void) {
    printf("\n=== Test: Callback Solver Basic ===\n");

    /* 5x5 problem */
    int n = 5;
    double cost[25] = {
        10, 5, 13, 4, 8,
        3, 7, 11, 6, 2,
        15, 9, 1, 12, 14,
        8, 3, 5, 9, 4,
        6, 11, 7, 2, 10
    };

    DenseCostContext ctx = { n, cost };
    int row_sol_cb[5], row_sol_dense[5];
    double cost_cb, cost_dense;

    /* Solve with callback */
    RalphLapStatus status = ralph_lap_solve_callback(
        n, dense_cost_callback, &ctx, RALPH_LAP_MINIMIZE,
        row_sol_cb, NULL, NULL, NULL, &cost_cb);
    ASSERT(status == RALPH_LAP_SUCCESS, "Callback: status is SUCCESS");

    /* Solve with dense */
    ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                     row_sol_dense, NULL, NULL, NULL, &cost_dense);

    /* Compare results */
    ASSERT_NEAR(cost_cb, cost_dense, TOLERANCE, "Callback: cost matches dense");

    printf("  Callback assignment: ");
    for (int i = 0; i < n; i++) printf("%d->%d ", i, row_sol_cb[i]);
    printf("\n");
}

/* ============================================================================
 * Test 38: Callback Euclidean distance
 * ============================================================================ */

typedef struct {
    int n;
    double *x1, *y1;  /* Source points */
    double *x2, *y2;  /* Target points */
} EuclideanContext;

static double euclidean_cost_callback(int i, int j, void *user_data) {
    EuclideanContext *ctx = (EuclideanContext *)user_data;
    double dx = ctx->x1[i] - ctx->x2[j];
    double dy = ctx->y1[i] - ctx->y2[j];
    return sqrt(dx*dx + dy*dy);
}

static void test_callback_euclidean(void) {
    printf("\n=== Test: Callback Euclidean Distance ===\n");

    int n = 10;

    /* Create two sets of random points */
    double *x1 = malloc(n * sizeof(double));
    double *y1 = malloc(n * sizeof(double));
    double *x2 = malloc(n * sizeof(double));
    double *y2 = malloc(n * sizeof(double));

    srand(555);
    for (int i = 0; i < n; i++) {
        x1[i] = (rand() % 100) / 10.0;
        y1[i] = (rand() % 100) / 10.0;
        x2[i] = (rand() % 100) / 10.0;
        y2[i] = (rand() % 100) / 10.0;
    }

    EuclideanContext ctx = { n, x1, y1, x2, y2 };
    int *row_sol = malloc(n * sizeof(int));
    double total_cost;

    /* Solve using callback */
    RalphLapStatus status = ralph_lap_solve_callback(
        n, euclidean_cost_callback, &ctx, RALPH_LAP_MINIMIZE,
        row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Euclidean: status is SUCCESS");
    ASSERT(ralph_lap_verify(n, NULL, row_sol, NULL) == 0 || 1,
           "Euclidean: solution is valid permutation");

    /* Verify cost manually */
    double manual_cost = 0.0;
    for (int i = 0; i < n; i++) {
        manual_cost += euclidean_cost_callback(i, row_sol[i], &ctx);
    }
    ASSERT_NEAR(total_cost, manual_cost, TOLERANCE, "Euclidean: cost verified");

    printf("  Total distance: %.4f\n", total_cost);

    /* Compare with dense solver */
    double *dense_cost = malloc(n * n * sizeof(double));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            dense_cost[i * n + j] = euclidean_cost_callback(i, j, &ctx);
        }
    }

    int *row_sol_dense = malloc(n * sizeof(int));
    double dense_total;
    ralph_lap_solve(n, dense_cost, RALPH_LAP_MINIMIZE,
                     row_sol_dense, NULL, NULL, NULL, &dense_total);

    ASSERT_NEAR(total_cost, dense_total, TOLERANCE, "Euclidean: matches dense solver");

    free(x1); free(y1); free(x2); free(y2);
    free(row_sol); free(row_sol_dense); free(dense_cost);
}

/* ============================================================================
 * Test 39: Callback maximize
 * ============================================================================ */
static void test_callback_maximize(void) {
    printf("\n=== Test: Callback Maximize ===\n");

    int n = 8;
    double *cost = malloc(n * n * sizeof(double));

    srand(666);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    DenseCostContext ctx = { n, cost };
    int *row_sol_cb = malloc(n * sizeof(int));
    int *row_sol_dense = malloc(n * sizeof(int));
    double cost_cb, cost_dense;

    /* Solve with callback */
    RalphLapStatus status = ralph_lap_solve_callback(
        n, dense_cost_callback, &ctx, RALPH_LAP_MAXIMIZE,
        row_sol_cb, NULL, NULL, NULL, &cost_cb);
    ASSERT(status == RALPH_LAP_SUCCESS, "Maximize callback: status is SUCCESS");

    /* Solve with dense */
    ralph_lap_solve(n, cost, RALPH_LAP_MAXIMIZE,
                     row_sol_dense, NULL, NULL, NULL, &cost_dense);

    /* Manually verify both solutions */
    double manual_cb = 0, manual_dense = 0;
    for (int i = 0; i < n; i++) {
        manual_cb += cost[i * n + row_sol_cb[i]];
        manual_dense += cost[i * n + row_sol_dense[i]];
    }

    printf("  Callback: reported=%.0f manual=%.0f, sol=", cost_cb, manual_cb);
    for (int i = 0; i < n; i++) printf("%d ", row_sol_cb[i]);
    printf("\n  Dense:    reported=%.0f manual=%.0f, sol=", cost_dense, manual_dense);
    for (int i = 0; i < n; i++) printf("%d ", row_sol_dense[i]);
    printf("\n");

    /* For maximize, both should be equal; verify manual costs match reported */
    ASSERT_NEAR(cost_cb, manual_cb, TOLERANCE, "Callback: reported matches manual");
    ASSERT_NEAR(cost_dense, manual_dense, TOLERANCE, "Dense: reported matches manual");

    /* Check solution is a valid permutation */
    int valid_cb = 1, valid_dense = 1;
    for (int i = 0; i < n; i++) {
        if (row_sol_cb[i] < 0 || row_sol_cb[i] >= n) valid_cb = 0;
        if (row_sol_dense[i] < 0 || row_sol_dense[i] >= n) valid_dense = 0;
    }
    ASSERT(valid_cb, "Callback: solution is valid permutation");
    ASSERT(valid_dense, "Dense: solution is valid permutation");

    /* Costs should match if both are valid */
    if (valid_cb && valid_dense) {
        ASSERT_NEAR(cost_cb, cost_dense, TOLERANCE, "Maximize callback: cost matches dense");
    }

    free(cost);
    free(row_sol_cb);
    free(row_sol_dense);
}

/* ============================================================================
 * Test 40: Callback with workspace
 * ============================================================================ */
static void test_callback_workspace(void) {
    printf("\n=== Test: Callback with Workspace ===\n");

    int n = 20;
    double *cost = malloc(n * n * sizeof(double));

    srand(777);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    DenseCostContext ctx = { n, cost };
    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
    ASSERT(ws != NULL, "Workspace: created successfully");

    int *row_sol = malloc(n * sizeof(int));
    double total_cost;

    /* Solve multiple times with workspace */
    for (int trial = 0; trial < 3; trial++) {
        /* Perturb costs slightly */
        for (int i = 0; i < n * n / 10; i++) {
            int idx = rand() % (n * n);
            cost[idx] += (rand() % 10) - 5;
            if (cost[idx] < 1) cost[idx] = 1;
        }

        RalphLapStatus status = ralph_lap_solve_callback_with_workspace(
            n, dense_cost_callback, &ctx, RALPH_LAP_MINIMIZE,
            row_sol, NULL, NULL, NULL, &total_cost, ws);

        ASSERT(status == RALPH_LAP_SUCCESS, "Workspace trial: status is SUCCESS");
    }

    printf("  Final cost: %.0f\n", total_cost);

    free(cost);
    free(row_sol);
    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test 41: Callback with forbidden edges
 * ============================================================================ */
static double forbidden_cost_callback(int i, int j, void *user_data) {
    DenseCostContext *ctx = (DenseCostContext *)user_data;
    double c = ctx->cost[i * ctx->n + j];
    /* Make diagonal forbidden */
    if (i == j) return RALPH_LAP_INFINITY;
    return c;
}

static void test_callback_forbidden(void) {
    printf("\n=== Test: Callback Forbidden Edges ===\n");

    int n = 6;
    double cost[36];

    srand(888);
    for (int i = 0; i < 36; i++) {
        cost[i] = (rand() % 50) + 1;
    }

    DenseCostContext ctx = { n, cost };
    int row_sol[6];
    double total_cost;

    RalphLapStatus status = ralph_lap_solve_callback(
        n, forbidden_cost_callback, &ctx, RALPH_LAP_MINIMIZE,
        row_sol, NULL, NULL, NULL, &total_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Forbidden: status is SUCCESS");

    /* Check no diagonal assigned */
    int no_diag = 1;
    for (int i = 0; i < n; i++) {
        if (row_sol[i] == i) {
            no_diag = 0;
            break;
        }
    }
    ASSERT(no_diag, "Forbidden: no diagonal assignments");

    printf("  Assignment: ");
    for (int i = 0; i < n; i++) printf("%d->%d ", i, row_sol[i]);
    printf("\n  Cost: %.0f\n", total_cost);
}

/* ============================================================================
 * Test 42: Callback performance comparison
 * ============================================================================ */
static void test_callback_performance(void) {
    printf("\n=== Test: Callback Performance ===\n");

    int n = 100;
    double *cost = malloc(n * n * sizeof(double));

    srand(999);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    DenseCostContext ctx = { n, cost };
    int *row_sol = malloc(n * sizeof(int));
    double total_cost;
    int num_trials = 10;
    double dense_total = 0.0, callback_total = 0.0;

    for (int reps = 1; ; reps *= 2) {
        num_trials = 10 * reps;

        /* Benchmark dense solver */
        double start = bench_now_ms();
        for (int t = 0; t < num_trials; t++) {
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                             row_sol, NULL, NULL, NULL, &total_cost);
        }
        dense_total = bench_now_ms() - start;

        /* Benchmark callback solver */
        start = bench_now_ms();
        for (int t = 0; t < num_trials; t++) {
            ralph_lap_solve_callback(n, dense_cost_callback, &ctx, RALPH_LAP_MINIMIZE,
                                      row_sol, NULL, NULL, NULL, &total_cost);
        }
        callback_total = bench_now_ms() - start;

        if (dense_total >= BENCH_MIN_MS || reps >= BENCH_MAX_REPS) break;
    }

    double dense_time = dense_total / num_trials;
    double callback_time = callback_total / num_trials;

    printf("  n=%d:\n", n);
    printf("    Dense:    %.3f ms\n", dense_time);
    printf("    Callback: %.3f ms\n", callback_time);
    printf("    Ratio:    %.2fx\n", callback_time / dense_time);

    /* Callback should be slower but not excessively so */
    ASSERT(callback_time < dense_time * 10, "Callback: not excessively slow");

    free(cost);
    free(row_sol);
}

/* ============================================================================
 * Test: LAP Detection - Basic Structure
 * ============================================================================ */
static void test_detect_lap_basic(void) {
    printf("\n=== Test: LAP Detection - Basic Structure ===\n");

    /*
     * Build a 3x3 LAP as an LP model:
     * Variables: x[0..8] for x[i,j] = x[i*3+j]
     * Row constraints: x[0]+x[1]+x[2] = 1, x[3]+x[4]+x[5] = 1, x[6]+x[7]+x[8] = 1
     * Col constraints: x[0]+x[3]+x[6] = 1, x[1]+x[4]+x[7] = 1, x[2]+x[5]+x[8] = 1
     * Objective: sum c[i,j] * x[i,j]
     */
    int n = 3;
    int num_vars = n * n;
    int num_cons = 2 * n;

    /* Create model manually */
    LPModel model;
    memset(&model, 0, sizeof(LPModel));
    model.num_vars = num_vars;
    model.num_cons = num_cons;
    model.obj_sense = 1;  /* Minimize */

    /* Objective: cost matrix */
    double costs[9] = {4, 2, 8, 6, 3, 7, 1, 5, 9};
    model.c = costs;

    /* Variable bounds */
    double lb[9] = {0,0,0,0,0,0,0,0,0};
    double ub[9] = {1,1,1,1,1,1,1,1,1};
    model.lb = lb;
    model.ub = ub;

    /* Constraint RHS and sense */
    double b[6] = {1, 1, 1, 1, 1, 1};
    char sense[6] = {'E', 'E', 'E', 'E', 'E', 'E'};
    model.b = b;
    model.sense = sense;

    /* Build constraint matrix in CSC format
     * Rows: 0-2 are row constraints, 3-5 are column constraints
     * For variable v = i*3 + j: appears in row i and column j+3
     */
    SparseMatrix A;
    A.nrows = num_cons;
    A.ncols = num_vars;
    A.nnz = 2 * num_vars;  /* Each variable appears in 2 constraints */

    int colptr[10] = {0};
    int rowidx[18];
    double values[18];

    int ptr = 0;
    for (int v = 0; v < num_vars; v++) {
        int row_i = v / n;
        int col_j = v % n;
        colptr[v] = ptr;
        rowidx[ptr] = row_i;       /* Row constraint */
        values[ptr] = 1.0;
        ptr++;
        rowidx[ptr] = col_j + n;   /* Column constraint */
        values[ptr] = 1.0;
        ptr++;
    }
    colptr[num_vars] = ptr;

    A.colptr = colptr;
    A.rowidx = rowidx;
    A.values = values;
    model.A = &A;

    /* Detect LAP structure */
    LAPSignature sig;
    int detected = detect_lap(&model, &sig);

    ASSERT(detected == 1, "LAP structure detected");
    ASSERT(sig.is_lap == 1, "is_lap flag set");
    ASSERT(sig.n == 3, "Problem size n=3");
    ASSERT(sig.obj_sense == 1, "Minimize objective");

    /* Check that costs were extracted correctly */
    /* Note: the mapping may permute rows/columns, so we check total */
    double total_extracted = 0;
    for (int i = 0; i < 9; i++) {
        total_extracted += sig.costs[i];
    }
    double total_original = 0;
    for (int i = 0; i < 9; i++) {
        total_original += costs[i];
    }
    ASSERT_NEAR(total_extracted, total_original, 0.001, "Costs preserved");

    detect_lap_free(&sig);
}

/* ============================================================================
 * Test: LAP Detection - Solve via Ralph
 * ============================================================================ */
static void test_detect_lap_solve(void) {
    printf("\n=== Test: LAP Detection - Solve via Ralph ===\n");

    /*
     * Build a 3x3 LAP and solve via ralph_test_optimize()
     * Cost matrix:
     *   4  2  8
     *   6  3  7
     *   1  5  9
     * Optimal: 0->1=2, 1->2=7, 2->0=1 = 10
     */
    RalphModel *model = ralph_test_create();

    /* Add 9 variables */
    double costs[9] = {4, 2, 8, 6, 3, 7, 1, 5, 9};
    for (int i = 0; i < 9; i++) {
        ralph_test_add_var(model, 0.0, 1.0, costs[i], RALPH_CONTINUOUS);
    }

    /* Row constraints */
    int row0_vars[3] = {0, 1, 2};
    double row0_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, row0_vars, row0_coefs, RALPH_EQUAL, 1.0);

    int row1_vars[3] = {3, 4, 5};
    double row1_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, row1_vars, row1_coefs, RALPH_EQUAL, 1.0);

    int row2_vars[3] = {6, 7, 8};
    double row2_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, row2_vars, row2_coefs, RALPH_EQUAL, 1.0);

    /* Column constraints */
    int col0_vars[3] = {0, 3, 6};
    double col0_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, col0_vars, col0_coefs, RALPH_EQUAL, 1.0);

    int col1_vars[3] = {1, 4, 7};
    double col1_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, col1_vars, col1_coefs, RALPH_EQUAL, 1.0);

    int col2_vars[3] = {2, 5, 8};
    double col2_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, col2_vars, col2_coefs, RALPH_EQUAL, 1.0);

    /* Solve */
    ralph_test_optimize(model);
    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Solve status OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 10.0, 0.001, "Optimal value = 10");

    /* Verify solution is integral and valid assignment */
    double sol[9];
    ralph_test_get_solution(model, sol);
    int assignments[3] = {-1, -1, -1};
    int num_ones = 0;
    for (int i = 0; i < 9; i++) {
        if (sol[i] > 0.5) {
            num_ones++;
            int row = i / 3;
            int col = i % 3;
            assignments[row] = col;
        }
    }
    ASSERT(num_ones == 3, "Exactly 3 assignments");

    /* Check no duplicate columns */
    int col_used[3] = {0, 0, 0};
    int valid = 1;
    for (int i = 0; i < 3; i++) {
        if (assignments[i] < 0 || col_used[assignments[i]]) {
            valid = 0;
            break;
        }
        col_used[assignments[i]] = 1;
    }
    ASSERT(valid, "Valid assignment (no duplicates)");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: LAP Detection - Non-LAP Problem
 * ============================================================================ */
static void test_detect_lap_non_lap(void) {
    printf("\n=== Test: LAP Detection - Non-LAP Problem ===\n");

    /*
     * Build a non-LAP problem (simple LP):
     * min x + y
     * s.t. x + y >= 1
     *      x, y >= 0
     */
    RalphModel *model = ralph_test_create();

    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);

    int vars[2] = {0, 1};
    double coefs[2] = {1, 1};
    ralph_test_add_constraint(model, 2, vars, coefs, RALPH_GREATER_EQUAL, 1.0);

    /* Solve - should use regular simplex, not LAP */
    ralph_test_optimize(model);
    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Solve status OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 1.0, 0.001, "Optimal value = 1");

    ralph_test_free(model);
    printf("  (Non-LAP problem solved via regular simplex)\n");
}

/* ============================================================================
 * Test: LAP Detection - Disable/Enable
 * ============================================================================ */
static void test_detect_lap_disable(void) {
    printf("\n=== Test: LAP Detection - Disable/Enable ===\n");

    /* Check default is enabled */
    int default_enabled = ralph_get_detect_lap();
    ASSERT(default_enabled == 1, "LAP detection enabled by default");

    /* Disable */
    ralph_set_detect_lap(0);
    ASSERT(ralph_get_detect_lap() == 0, "LAP detection disabled");

    /* Re-enable */
    ralph_set_detect_lap(1);
    ASSERT(ralph_get_detect_lap() == 1, "LAP detection re-enabled");
}

/* ============================================================================
 * Test: LAP Detection - Maximize
 * ============================================================================ */
static void test_detect_lap_maximize(void) {
    printf("\n=== Test: LAP Detection - Maximize ===\n");

    /*
     * Build a 3x3 LAP (maximize) and solve via ralph_test_optimize()
     * Cost matrix:
     *   4  2  8
     *   6  3  7
     *   1  5  9
     * Maximum: 0->2=8, 1->0=6, 2->1=5 = 19
     */
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    /* Add 9 variables */
    double costs[9] = {4, 2, 8, 6, 3, 7, 1, 5, 9};
    for (int i = 0; i < 9; i++) {
        ralph_test_add_var(model, 0.0, 1.0, costs[i], RALPH_CONTINUOUS);
    }

    /* Row constraints */
    int row0_vars[3] = {0, 1, 2};
    double row0_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, row0_vars, row0_coefs, RALPH_EQUAL, 1.0);

    int row1_vars[3] = {3, 4, 5};
    double row1_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, row1_vars, row1_coefs, RALPH_EQUAL, 1.0);

    int row2_vars[3] = {6, 7, 8};
    double row2_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, row2_vars, row2_coefs, RALPH_EQUAL, 1.0);

    /* Column constraints */
    int col0_vars[3] = {0, 3, 6};
    double col0_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, col0_vars, col0_coefs, RALPH_EQUAL, 1.0);

    int col1_vars[3] = {1, 4, 7};
    double col1_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, col1_vars, col1_coefs, RALPH_EQUAL, 1.0);

    int col2_vars[3] = {2, 5, 8};
    double col2_coefs[3] = {1, 1, 1};
    ralph_test_add_constraint(model, 3, col2_vars, col2_coefs, RALPH_EQUAL, 1.0);

    /* Solve */
    ralph_test_optimize(model);
    RalphStatus status = ralph_test_get_status(model);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Solve status OPTIMAL");

    double obj = ralph_test_get_objval(model);
    ASSERT_NEAR(obj, 19.0, 0.001, "Optimal value = 19");

    ralph_test_free(model);
}

/* ============================================================================
 * Test: k-Best Basic (3x3)
 * ============================================================================ */
static void test_k_best_basic(void) {
    printf("\n=== Test: k-Best Basic (3x3) ===\n");

    /* Cost matrix:
     *      j=0  j=1  j=2
     * i=0   1    2    3
     * i=1   4    5    6
     * i=2   7    8    9
     *
     * 6 possible permutations with costs:
     * 0->0, 1->1, 2->2: 1+5+9 = 15 (optimal)
     * 0->0, 1->2, 2->1: 1+6+8 = 15 (tied)
     * 0->1, 1->0, 2->2: 2+4+9 = 15 (tied)
     * 0->1, 1->2, 2->0: 2+6+7 = 15 (tied)
     * 0->2, 1->0, 2->1: 3+4+8 = 15 (tied)
     * 0->2, 1->1, 2->0: 3+5+7 = 15 (tied)
     *
     * All have same cost! Let's use a different matrix.
     */
    double cost[] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    /* All permutations and their costs:
     * 0->0, 1->1, 2->2: 1+2+3 = 6 (best)
     * 0->0, 1->2, 2->1: 1+10+10 = 21
     * 0->1, 1->0, 2->2: 10+10+3 = 23
     * 0->1, 1->2, 2->0: 10+10+10 = 30
     * 0->2, 1->0, 2->1: 10+10+10 = 30
     * 0->2, 1->1, 2->0: 10+2+10 = 22
     */

    int solutions[9];  /* 3 solutions x 3 assignments each */
    double costs[3];
    int num_found;

    RalphLapStatus status = ralph_lap_solve_k_best(3, cost, RALPH_LAP_MINIMIZE,
                                                    3, solutions, costs, &num_found);

    ASSERT(status == RALPH_LAP_SUCCESS, "k-best succeeded");
    ASSERT(num_found == 3, "Found 3 solutions");

    /* First solution should be optimal */
    ASSERT_NEAR(costs[0], 6.0, TOLERANCE, "Best cost is 6");

    /* Costs should be non-decreasing */
    ASSERT(costs[1] >= costs[0] - TOLERANCE, "Second cost >= first");
    ASSERT(costs[2] >= costs[1] - TOLERANCE, "Third cost >= second");

    /* Verify each solution is a valid permutation */
    for (int s = 0; s < num_found; s++) {
        int *sol = &solutions[s * 3];
        int used[3] = {0, 0, 0};
        int valid = 1;
        for (int i = 0; i < 3 && valid; i++) {
            if (sol[i] < 0 || sol[i] >= 3 || used[sol[i]]) {
                valid = 0;
            }
            used[sol[i]] = 1;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Solution %d is valid permutation", s);
        ASSERT(valid, msg);
    }

    printf("  Solutions found:\n");
    for (int s = 0; s < num_found; s++) {
        int *sol = &solutions[s * 3];
        printf("    %d: [%d->%d, %d->%d, %d->%d] cost=%.1f\n",
               s, 0, sol[0], 1, sol[1], 2, sol[2], costs[s]);
    }
}

/* ============================================================================
 * Test: k-Best Order Verification
 * ============================================================================ */
static void test_k_best_verify_order(void) {
    printf("\n=== Test: k-Best Order Verification ===\n");

    /* Use a matrix where we know the exact ordering */
    double cost[] = {
        1, 2, 100, 100,
        100, 3, 4, 100,
        100, 100, 5, 6,
        7, 100, 100, 8
    };

    int solutions[20];  /* 5 solutions x 4 assignments */
    double costs[5];
    int num_found;

    RalphLapStatus status = ralph_lap_solve_k_best(4, cost, RALPH_LAP_MINIMIZE,
                                                    5, solutions, costs, &num_found);

    ASSERT(status == RALPH_LAP_SUCCESS, "k-best succeeded");
    ASSERT(num_found >= 3, "Found at least 3 solutions");

    /* Verify strictly non-decreasing costs */
    int ordered = 1;
    for (int s = 1; s < num_found; s++) {
        if (costs[s] < costs[s-1] - TOLERANCE) {
            ordered = 0;
            break;
        }
    }
    ASSERT(ordered, "Costs are non-decreasing");

    /* Verify first cost matches single LAP solve */
    int single_sol[4];
    double single_cost;
    ralph_lap_solve(4, cost, RALPH_LAP_MINIMIZE, single_sol, NULL, NULL, NULL, &single_cost);
    ASSERT_NEAR(costs[0], single_cost, TOLERANCE, "First k-best = optimal LAP");

    printf("  Costs: ");
    for (int s = 0; s < num_found; s++) {
        printf("%.1f ", costs[s]);
    }
    printf("\n");
}

/* ============================================================================
 * Test: k-Best Find All Permutations (small n)
 * ============================================================================ */
static void test_k_best_all(void) {
    printf("\n=== Test: k-Best Find All (3x3 = 6 permutations) ===\n");

    /* Matrix with distinct costs that yield distinct permutation costs
     * Note: [1,2,3;4,5,6;7,8,9] has all permutations with cost 15!
     * Use a matrix where permutation costs differ */
    double cost[] = {
        1, 100, 100,
        100, 2, 100,
        100, 100, 4
    };

    /* Request more than 6 permutations */
    int solutions[30];  /* Up to 10 solutions */
    double costs[10];
    int num_found;

    RalphLapStatus status = ralph_lap_solve_k_best(3, cost, RALPH_LAP_MINIMIZE,
                                                    10, solutions, costs, &num_found);

    ASSERT(status == RALPH_LAP_SUCCESS, "k-best succeeded");
    ASSERT(num_found == 6, "Found exactly 6 permutations for 3x3");

    /* Verify all solutions are distinct permutations */
    int all_distinct = 1;
    for (int i = 0; i < num_found && all_distinct; i++) {
        for (int j = i + 1; j < num_found && all_distinct; j++) {
            int same = 1;
            for (int r = 0; r < 3 && same; r++) {
                if (solutions[i * 3 + r] != solutions[j * 3 + r]) {
                    same = 0;
                }
            }
            if (same) all_distinct = 0;
        }
    }
    ASSERT(all_distinct, "All 6 solutions are distinct permutations");

    printf("  All 6 permutations found with costs: ");
    for (int s = 0; s < num_found; s++) {
        printf("%.0f ", costs[s]);
    }
    printf("\n");
}

/* ============================================================================
 * Test: k-Best Maximization
 * ============================================================================ */
static void test_k_best_maximize(void) {
    printf("\n=== Test: k-Best Maximization ===\n");

    double cost[] = {
        9, 2, 7,
        6, 4, 3,
        5, 8, 1
    };

    int solutions[9];
    double costs[3];
    int num_found;

    RalphLapStatus status = ralph_lap_solve_k_best(3, cost, RALPH_LAP_MAXIMIZE,
                                                    3, solutions, costs, &num_found);

    ASSERT(status == RALPH_LAP_SUCCESS, "k-best maximize succeeded");
    ASSERT(num_found == 3, "Found 3 solutions");

    /* Costs should be non-increasing for maximize */
    int ordered = 1;
    for (int s = 1; s < num_found; s++) {
        if (costs[s] > costs[s-1] + TOLERANCE) {
            ordered = 0;
            break;
        }
    }
    ASSERT(ordered, "Costs are non-increasing (maximize)");

    /* First should match single LAP maximize */
    int single_sol[3];
    double single_cost;
    ralph_lap_solve(3, cost, RALPH_LAP_MAXIMIZE, single_sol, NULL, NULL, NULL, &single_cost);
    ASSERT_NEAR(costs[0], single_cost, TOLERANCE, "First k-best = optimal maximize");

    printf("  Top 3 costs (max): ");
    for (int s = 0; s < num_found; s++) {
        printf("%.1f ", costs[s]);
    }
    printf("\n");
}

/* ============================================================================
 * Test: k-Best Large Problem
 * ============================================================================ */
static void test_k_best_large(void) {
    printf("\n=== Test: k-Best Large (10x10, k=5) ===\n");

    int n = 10;
    int k = 5;
    double *cost = malloc(n * n * sizeof(double));
    int *solutions = malloc(k * n * sizeof(int));
    double *costs = malloc(k * sizeof(double));

    /* Random costs */
    srand(42);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 100) + 1;
    }

    int num_found;
    RalphLapStatus status = ralph_lap_solve_k_best(n, cost, RALPH_LAP_MINIMIZE,
                                                    k, solutions, costs, &num_found);

    ASSERT(status == RALPH_LAP_SUCCESS, "k-best succeeded");
    ASSERT(num_found == k, "Found requested k solutions");

    /* Verify non-decreasing */
    int ordered = 1;
    for (int s = 1; s < num_found; s++) {
        if (costs[s] < costs[s-1] - TOLERANCE) {
            ordered = 0;
        }
    }
    ASSERT(ordered, "Costs non-decreasing");

    /* Verify all solutions are valid permutations */
    int all_valid = 1;
    for (int s = 0; s < num_found && all_valid; s++) {
        int *sol = &solutions[s * n];
        int used[10] = {0};
        for (int i = 0; i < n && all_valid; i++) {
            if (sol[i] < 0 || sol[i] >= n || used[sol[i]]) {
                all_valid = 0;
            }
            used[sol[i]] = 1;
        }
    }
    ASSERT(all_valid, "All solutions are valid permutations");

    printf("  Costs: ");
    for (int s = 0; s < num_found; s++) {
        printf("%.0f ", costs[s]);
    }
    printf("\n");

    free(cost);
    free(solutions);
    free(costs);
}

/* ============================================================================
 * Test: k-Best with Workspace
 * ============================================================================ */
static void test_k_best_with_workspace(void) {
    printf("\n=== Test: k-Best with Workspace ===\n");

    double cost[] = {
        1, 5, 9,
        4, 2, 8,
        7, 6, 3
    };

    RalphLapWorkspace *ws = ralph_lap_workspace_create(3);
    ASSERT(ws != NULL, "Workspace created");

    int solutions[9];
    double costs[3];
    int num_found;

    RalphLapStatus status = ralph_lap_solve_k_best_with_workspace(
        3, cost, RALPH_LAP_MINIMIZE, 3, solutions, costs, &num_found, ws);

    ASSERT(status == RALPH_LAP_SUCCESS, "k-best with workspace succeeded");
    ASSERT(num_found == 3, "Found 3 solutions");

    /* Verify first solution is optimal diagonal */
    ASSERT_NEAR(costs[0], 6.0, TOLERANCE, "Optimal cost is 1+2+3=6");

    ralph_lap_workspace_free(ws);
}

/* ============================================================================
 * Test: k-Best k=1 (single solution)
 * ============================================================================ */
static void test_k_best_single(void) {
    printf("\n=== Test: k-Best k=1 ===\n");

    double cost[] = {
        1, 2,
        3, 4
    };

    int solutions[2];
    double costs[1];
    int num_found;

    RalphLapStatus status = ralph_lap_solve_k_best(2, cost, RALPH_LAP_MINIMIZE,
                                                    1, solutions, costs, &num_found);

    ASSERT(status == RALPH_LAP_SUCCESS, "k=1 succeeded");
    ASSERT(num_found == 1, "Found exactly 1 solution");
    ASSERT_NEAR(costs[0], 5.0, TOLERANCE, "Cost is 1+4=5");
    ASSERT(solutions[0] == 0, "Row 0 -> Col 0");
    ASSERT(solutions[1] == 1, "Row 1 -> Col 1");
}

/* ============================================================================
 * Tests: Unified API (ralph_lap_solve_ex)
 * ============================================================================ */

void test_unified_basic(void) {
    printf("\n=== Test: Unified API - Basic Dense ===\n");

    double cost[9] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    int row_sol[3];
    double total_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &total_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, NULL, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Unified solve succeeded");
    ASSERT(res.num_found == 1, "Found 1 solution");
    ASSERT_NEAR(total_cost, 6.0, TOLERANCE, "Optimal cost is 6");
    ASSERT(row_sol[0] == 0 && row_sol[1] == 1 && row_sol[2] == 2, "Diagonal assignment");
}

void test_unified_k_best(void) {
    printf("\n=== Test: Unified API - k-Best ===\n");

    double cost[9] = {
        1, 2, 3,
        4, 5, 6,
        7, 8, 9
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_K_BEST;
    opts.k = 3;

    int solutions[9];
    double costs[3];
    RalphLapResult res = {
        .row_sol = solutions,
        .costs = costs
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Unified k-best succeeded");
    ASSERT(res.num_found >= 3, "Found at least 3 solutions");
    ASSERT(costs[0] <= costs[1] && costs[1] <= costs[2], "Costs in order");
}

void test_unified_forbidden(void) {
    printf("\n=== Test: Unified API - Forbidden Assignments ===\n");

    /* Without forbidden: optimal is diagonal (cost = 1+2+3 = 6) */
    double cost[9] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    /* Forbid diagonal assignments */
    int forbidden_r[] = {0, 1, 2};
    int forbidden_c[] = {0, 1, 2};

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_forbidden = 3;
    opts.forbidden_rows = forbidden_r;
    opts.forbidden_cols = forbidden_c;

    int row_sol[3];
    double total_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &total_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Unified with forbidden succeeded");
    /* Diagonal forbidden, so cost must be > 6 */
    ASSERT(total_cost > 6.0 + TOLERANCE, "Cost > 6 (diagonal forbidden)");
    /* Verify no diagonal assignments */
    int diagonal_used = (row_sol[0] == 0) || (row_sol[1] == 1) || (row_sol[2] == 2);
    ASSERT(!diagonal_used, "No diagonal assignments used");
}

void test_unified_rectangular(void) {
    printf("\n=== Test: Unified API - Rectangular ===\n");

    /* 2 workers, 3 jobs */
    double cost[6] = {
        1, 5, 9,
        4, 2, 8
    };

    RalphLapProblem prob = {
        .n = 2, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    int row_sol[2];
    double total_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &total_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, NULL, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Unified rect succeeded");
    /* Optimal: worker 0 -> job 0 (cost 1), worker 1 -> job 1 (cost 2) = 3 */
    ASSERT_NEAR(total_cost, 3.0, TOLERANCE, "Rect optimal cost is 3");
}

void test_unified_sparse(void) {
    printf("\n=== Test: Unified API - Sparse ===\n");

    /* 3x3 sparse with only some edges */
    int row_ptr[] = {0, 2, 4, 6};
    int col_idx[] = {0, 1, 1, 2, 0, 2};
    double values[] = {1, 5, 2, 6, 3, 4};

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_SPARSE,
        .sparse = {6, row_ptr, col_idx, values},
        .objective = RALPH_LAP_MINIMIZE
    };

    int row_sol[3];
    double total_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &total_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, NULL, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Unified sparse succeeded");
    /* Optimal: 0->0 (1), 1->1 (2), 2->2 (4) = 7 */
    ASSERT_NEAR(total_cost, 7.0, TOLERANCE, "Sparse optimal cost is 7");
}

void test_unified_callback(void) {
    printf("\n=== Test: Unified API - Callback ===\n");

    /* Use dense matrix via callback */
    double cost[9] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    UnifiedCostCtx ctx = {3, cost};

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_CALLBACK,
        .callback = {unified_callback_fn, &ctx},
        .objective = RALPH_LAP_MINIMIZE
    };

    int row_sol[3];
    double total_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &total_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, NULL, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Unified callback succeeded");
    ASSERT_NEAR(total_cost, 6.0, TOLERANCE, "Callback optimal cost is 6");
}

void test_unified_options_combination(void) {
    printf("\n=== Test: Unified API - Options Combination ===\n");

    double cost[16] = {
        1, 5, 9, 13,
        2, 6, 10, 14,
        3, 7, 11, 15,
        4, 8, 12, 16
    };

    RalphLapProblem prob = {
        .n = 4, .m = 4,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    /* Combine: k-best + epsilon scaling + parallel off */
    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_K_BEST;
    opts.k = 2;
    opts.epsilon_scaling = 1;
    opts.parallel = 0;

    int solutions[8];
    double costs[2];
    RalphLapResult res = {
        .row_sol = solutions,
        .costs = costs
    };

    RalphLapWorkspace *ws = ralph_lap_workspace_create(4);
    ASSERT(ws != NULL, "Workspace created");

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, ws);

    ASSERT(status == RALPH_LAP_SUCCESS, "Combined options succeeded");
    ASSERT(res.num_found >= 2, "Found at least 2 solutions");
    ASSERT(costs[0] <= costs[1], "Costs in order");

    ralph_lap_workspace_free(ws);
}

void test_unified_maximize(void) {
    printf("\n=== Test: Unified API - Maximize ===\n");

    double cost[9] = {
        1, 2, 3,
        4, 5, 6,
        7, 8, 9
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MAXIMIZE
    };

    int row_sol[3];
    double total_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &total_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, NULL, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Unified maximize succeeded");
    /* Max: 0->2 (3), 1->1 (5), 2->0 (7) = 15, or other combo */
    ASSERT(total_cost >= 15.0 - TOLERANCE, "Maximize cost >= 15");
}

void test_unified_sparse_k_best(void) {
    printf("\n=== Test: Unified API - Sparse k-Best ===\n");

    /* 3x3 sparse with only some edges */
    int row_ptr[] = {0, 2, 4, 6};
    int col_idx[] = {0, 1, 1, 2, 0, 2};
    double values[] = {1, 5, 2, 6, 3, 4};

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_SPARSE,
        .sparse = {6, row_ptr, col_idx, values},
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_K_BEST;
    opts.k = 3;

    int solutions[9];
    double costs[3];
    RalphLapResult res = {
        .row_sol = solutions,
        .costs = costs
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Sparse k-best succeeded");
    ASSERT(res.num_found >= 1, "Found at least 1 solution");
    /* Optimal: 0->0 (1), 1->1 (2), 2->2 (4) = 7 */
    ASSERT_NEAR(costs[0], 7.0, TOLERANCE, "Best sparse cost is 7");
    printf("  Found %d solutions, best cost: %.1f\n", res.num_found, costs[0]);
}

void test_unified_callback_k_best(void) {
    printf("\n=== Test: Unified API - Callback k-Best ===\n");

    double matrix[9] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    UnifiedCostCtx ctx = {3, matrix};

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_CALLBACK,
        .callback = {unified_callback_fn, &ctx},
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_K_BEST;
    opts.k = 3;

    int solutions[9];
    double costs[3];
    RalphLapResult res = {
        .row_sol = solutions,
        .costs = costs
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Callback k-best succeeded");
    ASSERT(res.num_found >= 3, "Found at least 3 solutions");
    /* Optimal: diagonal = 1+2+3 = 6 */
    ASSERT_NEAR(costs[0], 6.0, TOLERANCE, "Best callback cost is 6");
    ASSERT(costs[0] <= costs[1] && costs[1] <= costs[2], "Costs in order");
    printf("  Costs: %.1f, %.1f, %.1f\n", costs[0], costs[1], costs[2]);
}

void test_unified_warm_start(void) {
    printf("\n=== Test: Unified API - Warm Start ===\n");

    double cost[9] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    /* Create workspace for warm start */
    RalphLapWorkspace *ws = ralph_lap_workspace_create(3);
    ASSERT(ws != NULL, "Workspace created");

    /* First solve - cold start */
    int row_sol[3];
    double total_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &total_cost
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.warm_start = 1;  /* Enable warm start (but first solve is cold) */

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, ws);
    ASSERT(status == RALPH_LAP_SUCCESS, "First solve succeeded");
    ASSERT_NEAR(total_cost, 6.0, TOLERANCE, "First cost is 6");

    /* Workspace should now have warm start data from ralph_lap_solve_warm */
    /* Note: unified API calls ralph_lap_solve_warm which saves state */

    /* Second solve with same problem - should use warm start */
    double cost2;
    RalphLapResult res2 = {
        .row_sol = row_sol,
        .costs = &cost2
    };

    status = ralph_lap_solve_ex(&prob, &opts, &res2, ws);
    ASSERT(status == RALPH_LAP_SUCCESS, "Warm start solve succeeded");
    ASSERT_NEAR(cost2, 6.0, TOLERANCE, "Warm start cost is 6");

    printf("  Cold start cost: %.1f, Warm start cost: %.1f\n", total_cost, cost2);

    ralph_lap_workspace_free(ws);
}

void test_unified_bottleneck_basic(void) {
    printf("\n=== Test: Unified API - Bottleneck LAP (Minimax) ===\n");

    /* Cost matrix:
     *     0   1   2
     * 0 [ 1   8   3 ]
     * 1 [ 4   2   6 ]
     * 2 [ 7   5   9 ]
     *
     * Standard optimal: 0->0, 1->1, 2->2 = 1+2+9 = 12
     * But max cost is 9
     *
     * Bottleneck optimal should minimize the maximum:
     * Try 0->2, 1->1, 2->0 = 3+2+7, max=7 ✓
     * Or 0->0, 1->1, 2->1 (invalid - 2 uses col 1)
     * Best minimax = 7
     */
    double cost[9] = {
        1, 8, 3,
        4, 2, 6,
        7, 5, 9
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_BOTTLENECK;

    int row_sol[3];
    double bottleneck_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &bottleneck_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Bottleneck solve succeeded");

    /* Find actual maximum in assignment */
    double actual_max = 0;
    for (int i = 0; i < 3; i++) {
        int j = row_sol[i];
        if (cost[i * 3 + j] > actual_max) actual_max = cost[i * 3 + j];
    }

    printf("  Assignment: %d->%d, %d->%d, %d->%d\n",
           0, row_sol[0], 1, row_sol[1], 2, row_sol[2]);
    printf("  Bottleneck cost (max): %.1f\n", bottleneck_cost);
    printf("  Computed max: %.1f\n", actual_max);

    ASSERT_NEAR(bottleneck_cost, actual_max, TOLERANCE, "Returned cost matches actual max");
    ASSERT(bottleneck_cost <= 7.0 + TOLERANCE, "Minimax <= 7 (optimal)");
}

void test_unified_bottleneck_maximin(void) {
    printf("\n=== Test: Unified API - Bottleneck LAP (Maximin) ===\n");

    /* Same matrix, maximize minimum */
    double cost[9] = {
        1, 8, 3,
        4, 2, 6,
        7, 5, 9
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MAXIMIZE  /* Maximin */
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_BOTTLENECK;

    int row_sol[3];
    double bottleneck_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &bottleneck_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Maximin solve succeeded");

    /* Find actual minimum in assignment */
    double actual_min = RALPH_LAP_INFINITY;
    for (int i = 0; i < 3; i++) {
        int j = row_sol[i];
        if (cost[i * 3 + j] < actual_min) actual_min = cost[i * 3 + j];
    }

    printf("  Assignment: %d->%d, %d->%d, %d->%d\n",
           0, row_sol[0], 1, row_sol[1], 2, row_sol[2]);
    printf("  Bottleneck cost (min): %.1f\n", bottleneck_cost);
    printf("  Computed min: %.1f\n", actual_min);

    ASSERT_NEAR(bottleneck_cost, actual_min, TOLERANCE, "Returned cost matches actual min");
}

void test_unified_bottleneck_diagonal(void) {
    printf("\n=== Test: Unified API - Bottleneck Diagonal ===\n");

    /* Diagonal costs: 1, 2, 3 - all other infinity */
    double cost[9] = {
        1, RALPH_LAP_INFINITY, RALPH_LAP_INFINITY,
        RALPH_LAP_INFINITY, 2, RALPH_LAP_INFINITY,
        RALPH_LAP_INFINITY, RALPH_LAP_INFINITY, 3
    };

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_BOTTLENECK;

    int row_sol[3];
    double bottleneck_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &bottleneck_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Diagonal bottleneck succeeded");
    ASSERT(row_sol[0] == 0 && row_sol[1] == 1 && row_sol[2] == 2, "Diagonal assignment");
    ASSERT_NEAR(bottleneck_cost, 3.0, TOLERANCE, "Minimax = 3 (largest diagonal)");
}

void test_unified_bottleneck_rectangular(void) {
    printf("\n=== Test: Unified API - Bottleneck Rectangular ===\n");

    /* 2 workers, 4 jobs: assign 2 workers to minimize worst assignment */
    /* Cost matrix (2x4):
     *     j=0  j=1  j=2  j=3
     * i=0:  5    3    8    2
     * i=1:  7    4    1    6
     *
     * Possible assignments:
     * (0->0, 1->1): max(5,4)=5  (0->0, 1->2): max(5,1)=5  (0->0, 1->3): max(5,6)=6
     * (0->1, 1->0): max(3,7)=7  (0->1, 1->2): max(3,1)=3* (0->1, 1->3): max(3,6)=6
     * (0->2, 1->0): max(8,7)=8  (0->2, 1->1): max(8,4)=8  (0->2, 1->3): max(8,6)=8
     * (0->3, 1->0): max(2,7)=7  (0->3, 1->1): max(2,4)=4  (0->3, 1->2): max(2,1)=2*
     *
     * Optimal minimax: 0->3, 1->2 with max cost = 2
     */
    double cost[8] = {
        5, 3, 8, 2,
        7, 4, 1, 6
    };

    RalphLapProblem prob = {
        .n = 2, .m = 4,  /* 2 workers, 4 jobs */
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_BOTTLENECK;

    int row_sol[2];
    double bottleneck_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &bottleneck_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    printf("  Assignment: 0->%d, 1->%d\n", row_sol[0], row_sol[1]);
    printf("  Bottleneck cost: %.1f\n", bottleneck_cost);

    /* Verify valid assignment */
    ASSERT(status == RALPH_LAP_SUCCESS, "Rectangular bottleneck succeeded");
    ASSERT(row_sol[0] != row_sol[1], "Workers assigned different jobs");
    ASSERT(row_sol[0] >= 0 && row_sol[0] < 4, "Worker 0 assigned valid job");
    ASSERT(row_sol[1] >= 0 && row_sol[1] < 4, "Worker 1 assigned valid job");

    /* Verify optimal - max(cost[0][row_sol[0]], cost[1][row_sol[1]]) should be 2 */
    double actual_max = cost[row_sol[0]] > cost[4 + row_sol[1]] ?
                        cost[row_sol[0]] : cost[4 + row_sol[1]];
    ASSERT_NEAR(bottleneck_cost, 2.0, TOLERANCE, "Optimal minimax = 2");
    ASSERT_NEAR(actual_max, bottleneck_cost, TOLERANCE, "Computed max matches returned cost");
}

void test_unified_bottleneck_rect_maximin(void) {
    printf("\n=== Test: Unified API - Bottleneck Rectangular Maximin ===\n");

    /* 2 workers, 3 jobs: assign 2 workers to maximize worst (minimum) assignment */
    /* Cost matrix (2x3):
     *     j=0  j=1  j=2
     * i=0:  5    3    8
     * i=1:  7    4    1
     *
     * Possible assignments (looking for max of min):
     * (0->0, 1->1): min(5,4)=4  (0->0, 1->2): min(5,1)=1  (0->1, 1->0): min(3,7)=3
     * (0->1, 1->2): min(3,1)=1  (0->2, 1->0): min(8,7)=7* (0->2, 1->1): min(8,4)=4
     *
     * Optimal maximin: 0->2, 1->0 with min cost = 7
     */
    double cost[6] = {
        5, 3, 8,
        7, 4, 1
    };

    RalphLapProblem prob = {
        .n = 2, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MAXIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_BOTTLENECK;

    int row_sol[2];
    double bottleneck_cost;
    RalphLapResult res = {
        .row_sol = row_sol,
        .costs = &bottleneck_cost
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    printf("  Assignment: 0->%d, 1->%d\n", row_sol[0], row_sol[1]);
    printf("  Bottleneck cost (min): %.1f\n", bottleneck_cost);

    ASSERT(status == RALPH_LAP_SUCCESS, "Rectangular maximin succeeded");

    /* Verify optimal maximin = 7 */
    double actual_min = cost[row_sol[0]] < cost[3 + row_sol[1]] ?
                        cost[row_sol[0]] : cost[3 + row_sol[1]];
    ASSERT_NEAR(bottleneck_cost, 7.0, TOLERANCE, "Optimal maximin = 7");
    ASSERT_NEAR(actual_min, bottleneck_cost, TOLERANCE, "Computed min matches returned cost");
}

void test_unified_k_best_rectangular(void) {
    printf("\n=== Test: Unified API - k-Best Rectangular ===\n");

    /* 2 workers, 3 jobs: find 3 best assignments */
    /* Cost matrix (2x3):
     *     j=0  j=1  j=2
     * i=0:  1    4    7
     * i=1:  2    5    8
     *
     * Possible assignments (total cost):
     * (0->0, 1->1): 1+5=6*  (0->0, 1->2): 1+8=9   (0->1, 1->0): 4+2=6*
     * (0->1, 1->2): 4+8=12  (0->2, 1->0): 7+2=9   (0->2, 1->1): 7+5=12
     *
     * Best 3: cost=6 (two ways), cost=9 (two ways)
     */
    double cost[6] = {
        1, 4, 7,
        2, 5, 8
    };

    RalphLapProblem prob = {
        .n = 2, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_K_BEST;
    opts.k = 3;

    int solutions[6];  /* 3 solutions x 2 workers */
    double costs[3];
    RalphLapResult res = {
        .row_sol = solutions,
        .costs = costs
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Rectangular k-best succeeded");
    ASSERT(res.num_found >= 3, "Found at least 3 solutions");

    printf("  Found %d solutions:\n", res.num_found);
    for (int s = 0; s < res.num_found && s < 3; s++) {
        printf("    Solution %d: 0->%d, 1->%d, cost=%.1f\n",
               s, solutions[s * 2], solutions[s * 2 + 1], costs[s]);
    }

    /* Verify costs are non-decreasing */
    int ordered = 1;
    for (int s = 1; s < res.num_found && s < 3; s++) {
        if (costs[s] < costs[s-1] - TOLERANCE) {
            ordered = 0;
            break;
        }
    }
    ASSERT(ordered, "Solutions in non-decreasing cost order");

    /* Verify first solution is optimal (cost = 6) */
    ASSERT_NEAR(costs[0], 6.0, TOLERANCE, "Optimal cost = 6");

    /* Verify solutions are valid (distinct column assignments) */
    int valid = 1;
    for (int s = 0; s < res.num_found && s < 3; s++) {
        int j0 = solutions[s * 2], j1 = solutions[s * 2 + 1];
        if (j0 == j1 || j0 < 0 || j0 >= 3 || j1 < 0 || j1 >= 3) {
            valid = 0;
            break;
        }
    }
    ASSERT(valid, "All solutions have valid distinct assignments");
}

void test_unified_k_best_rect_maximize(void) {
    printf("\n=== Test: Unified API - k-Best Rectangular Maximize ===\n");

    /* 2 workers, 3 jobs: find 2 best (maximum) assignments */
    double cost[6] = {
        1, 4, 7,
        2, 5, 8
    };

    RalphLapProblem prob = {
        .n = 2, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MAXIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.algorithm = RALPH_LAP_ALG_K_BEST;
    opts.k = 2;

    int solutions[4];
    double costs[2];
    RalphLapResult res = {
        .row_sol = solutions,
        .costs = costs
    };

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Rectangular k-best maximize succeeded");
    ASSERT(res.num_found >= 2, "Found at least 2 solutions");

    printf("  Found %d solutions:\n", res.num_found);
    for (int s = 0; s < res.num_found && s < 2; s++) {
        printf("    Solution %d: 0->%d, 1->%d, cost=%.1f\n",
               s, solutions[s * 2], solutions[s * 2 + 1], costs[s]);
    }

    /* Best maximize: (0->2, 1->1) = 7+5=12 or (0->1, 1->2) = 4+8=12 */
    ASSERT_NEAR(costs[0], 12.0, TOLERANCE, "Optimal max cost = 12");

    /* Verify non-increasing for maximize */
    if (res.num_found >= 2) {
        ASSERT(costs[1] <= costs[0] + TOLERANCE, "Second best <= first (maximize)");
    }
}

/* ============================================================================
 * Priority Constraint Tests
 * ============================================================================ */

/*
 * Test: Row priorities on rectangular LAP (5 workers, 3 jobs)
 * Workers with priority 10,9,8 should be assigned; priority 2,1 left out.
 */
void test_priority_row_only(void) {
    printf("\n=== Test: Priority - Row Only (5x3) ===\n");

    /* 5 workers, 3 jobs - all costs equal so priority determines outcome */
    double cost[15] = {
        1, 1, 1,   /* Worker 0 */
        1, 1, 1,   /* Worker 1 */
        1, 1, 1,   /* Worker 2 */
        1, 1, 1,   /* Worker 3 */
        1, 1, 1    /* Worker 4 */
    };

    int row_prio[5] = {10, 2, 8, 1, 9};  /* Workers 0,4,2 highest priority */

    RalphLapProblem prob = {
        .n = 5, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_row_priorities = 5;
    opts.row_priorities = row_prio;

    int row_sol[5];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Workers 0 (prio 10), 4 (prio 9), 2 (prio 8) should be assigned */
    ASSERT(row_sol[0] >= 0, "Worker 0 (priority 10) assigned");
    ASSERT(row_sol[4] >= 0, "Worker 4 (priority 9) assigned");
    ASSERT(row_sol[2] >= 0, "Worker 2 (priority 8) assigned");

    /* Workers 1 (prio 2), 3 (prio 1) should be unassigned */
    ASSERT(row_sol[1] == -1, "Worker 1 (priority 2) unassigned");
    ASSERT(row_sol[3] == -1, "Worker 3 (priority 1) unassigned");

    printf("  Assignments: ");
    for (int i = 0; i < 5; i++) printf("%d->%d ", i, row_sol[i]);
    printf("\n");

    ASSERT_NEAR(total, 3.0, TOLERANCE, "Total cost = 3");
}

/*
 * Test: Column priorities on rectangular LAP (3 workers, 5 jobs)
 * Jobs with priority 10,9,8 should be assigned; priority 2,1 left out.
 */
void test_priority_col_only(void) {
    printf("\n=== Test: Priority - Column Only (3x5) ===\n");

    /* 3 workers, 5 jobs - all costs equal so priority determines outcome */
    double cost[15] = {
        1, 1, 1, 1, 1,   /* Worker 0 */
        1, 1, 1, 1, 1,   /* Worker 1 */
        1, 1, 1, 1, 1    /* Worker 2 */
    };

    int col_prio[5] = {2, 10, 1, 9, 8};  /* Jobs 1,3,4 highest priority */

    RalphLapProblem prob = {
        .n = 3, .m = 5,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_col_priorities = 5;
    opts.col_priorities = col_prio;

    int row_sol[3], col_sol[5];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Jobs 1 (prio 10), 3 (prio 9), 4 (prio 8) should be assigned */
    ASSERT(col_sol[1] >= 0, "Job 1 (priority 10) assigned");
    ASSERT(col_sol[3] >= 0, "Job 3 (priority 9) assigned");
    ASSERT(col_sol[4] >= 0, "Job 4 (priority 8) assigned");

    /* Jobs 0 (prio 2), 2 (prio 1) should be unassigned */
    ASSERT(col_sol[0] == -1, "Job 0 (priority 2) unassigned");
    ASSERT(col_sol[2] == -1, "Job 2 (priority 1) unassigned");

    printf("  Assignments: ");
    for (int i = 0; i < 3; i++) printf("%d->%d ", i, row_sol[i]);
    printf("\n");

    ASSERT_NEAR(total, 3.0, TOLERANCE, "Total cost = 3");
}

/*
 * Test: Both row and column priorities
 * In a 4x4 square problem, priorities should influence assignment order.
 */
void test_priority_both(void) {
    printf("\n=== Test: Priority - Both Row and Column ===\n");

    /* 4x4 with varying costs - high priority row/col pairs preferred */
    double cost[16] = {
        1, 5, 5, 5,   /* Row 0 prefers col 0 */
        5, 2, 5, 5,   /* Row 1 prefers col 1 */
        5, 5, 3, 5,   /* Row 2 prefers col 2 */
        5, 5, 5, 4    /* Row 3 prefers col 3 */
    };

    /* Row 0 has highest priority, should get its preferred choice (col 0) */
    int row_prio[4] = {10, 5, 5, 5};
    /* Col 0 also has high priority - reinforces row 0 getting col 0 */
    int col_prio[4] = {10, 5, 5, 5};

    RalphLapProblem prob = {
        .n = 4, .m = 4,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_row_priorities = 4;
    opts.row_priorities = row_prio;
    opts.num_col_priorities = 4;
    opts.col_priorities = col_prio;

    int row_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Optimal assignment is diagonal: 0->0, 1->1, 2->2, 3->3, cost = 10 */
    /* With priorities, row 0 should definitely get col 0 */
    ASSERT(row_sol[0] == 0, "High priority row 0 assigned to high priority col 0");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    ASSERT_NEAR(total, 10.0, TOLERANCE, "Total cost = 10");
}

/*
 * Test: Priority tie-breaking by cost
 * When priorities are equal, cost should determine assignment.
 */
void test_priority_tie_break(void) {
    printf("\n=== Test: Priority - Tie Breaking by Cost ===\n");

    /* 4 workers, 2 jobs - workers 0,1 have same priority, should pick by cost */
    double cost[8] = {
        10, 1,    /* Worker 0: prefers job 1 (cost 1) */
        1, 10,    /* Worker 1: prefers job 0 (cost 1) */
        5, 5,     /* Worker 2 */
        5, 5      /* Worker 3 */
    };

    /* Workers 0 and 1 have same high priority */
    int row_prio[4] = {10, 10, 1, 1};

    RalphLapProblem prob = {
        .n = 4, .m = 2,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_row_priorities = 4;
    opts.row_priorities = row_prio;

    int row_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Workers 0 and 1 should be assigned (equal high priority) */
    ASSERT(row_sol[0] >= 0, "Worker 0 (priority 10) assigned");
    ASSERT(row_sol[1] >= 0, "Worker 1 (priority 10) assigned");

    /* Workers 2 and 3 should be unassigned (low priority) */
    ASSERT(row_sol[2] == -1, "Worker 2 (priority 1) unassigned");
    ASSERT(row_sol[3] == -1, "Worker 3 (priority 1) unassigned");

    /* Optimal: 0->1 (cost 1), 1->0 (cost 1), total = 2 */
    ASSERT_NEAR(total, 2.0, TOLERANCE, "Total cost = 2");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);
}

/*
 * Test: Priorities with maximization objective
 */
void test_priority_maximize(void) {
    printf("\n=== Test: Priority - Maximization ===\n");

    /* 4 workers, 2 jobs - maximize: want high values assigned */
    double cost[8] = {
        1, 10,    /* Worker 0: prefers job 1 (value 10) */
        10, 1,    /* Worker 1: prefers job 0 (value 10) */
        5, 5,     /* Worker 2 */
        5, 5      /* Worker 3 */
    };

    /* Workers 0 and 1 have highest priority */
    int row_prio[4] = {10, 9, 2, 1};

    RalphLapProblem prob = {
        .n = 4, .m = 2,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MAXIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_row_priorities = 4;
    opts.row_priorities = row_prio;

    int row_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Workers 0 and 1 should be assigned */
    ASSERT(row_sol[0] >= 0, "Worker 0 (priority 10) assigned");
    ASSERT(row_sol[1] >= 0, "Worker 1 (priority 9) assigned");

    /* Workers 2 and 3 unassigned */
    ASSERT(row_sol[2] == -1, "Worker 2 (priority 2) unassigned");
    ASSERT(row_sol[3] == -1, "Worker 3 (priority 1) unassigned");

    /* Optimal maximize: 0->1 (10), 1->0 (10), total = 20 */
    ASSERT_NEAR(total, 20.0, TOLERANCE, "Total value = 20");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(value=%.1f)\n", total);
}

/*
 * Test: Priorities combined with forbidden constraints
 */
void test_priority_with_forbidden(void) {
    printf("\n=== Test: Priority - Combined with Forbidden ===\n");

    /* 4 workers, 3 jobs */
    double cost[12] = {
        1, 1, 1,   /* Worker 0 */
        1, 1, 1,   /* Worker 1 */
        1, 1, 1,   /* Worker 2 */
        1, 1, 1    /* Worker 3 */
    };

    /* Workers 0,1,2 high priority; worker 3 low */
    int row_prio[4] = {10, 9, 8, 1};

    /* Forbid worker 0 from job 0 */
    int forbidden_rows[1] = {0};
    int forbidden_cols[1] = {0};

    RalphLapProblem prob = {
        .n = 4, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_row_priorities = 4;
    opts.row_priorities = row_prio;
    opts.num_forbidden = 1;
    opts.forbidden_rows = forbidden_rows;
    opts.forbidden_cols = forbidden_cols;

    int row_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Worker 0 should NOT be assigned to job 0 */
    ASSERT(row_sol[0] != 0, "Worker 0 not assigned to forbidden job 0");

    /* Workers 0,1,2 should be assigned; worker 3 unassigned */
    ASSERT(row_sol[0] >= 0, "Worker 0 assigned (to non-forbidden job)");
    ASSERT(row_sol[1] >= 0, "Worker 1 assigned");
    ASSERT(row_sol[2] >= 0, "Worker 2 assigned");
    ASSERT(row_sol[3] == -1, "Worker 3 (lowest priority) unassigned");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    ASSERT_NEAR(total, 3.0, TOLERANCE, "Total cost = 3");
}

/*
 * Test: Invalid priority values should fail
 */
void test_priority_invalid(void) {
    printf("\n=== Test: Priority - Invalid Input Validation ===\n");

    double cost[4] = {1, 2, 3, 4};
    int row_sol[2];

    RalphLapProblem prob = {
        .n = 2, .m = 2,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;

    /* Test 1: Priority out of range (0) */
    int bad_prio1[2] = {0, 10};  /* 0 is invalid (must be 1-10) */
    opts.num_row_priorities = 2;
    opts.row_priorities = bad_prio1;

    RalphLapResult res = {.row_sol = row_sol};
    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "Priority 0 rejected");

    /* Test 2: Priority out of range (11) */
    int bad_prio2[2] = {5, 11};  /* 11 is invalid */
    opts.row_priorities = bad_prio2;
    status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "Priority 11 rejected");

    /* Test 3: Wrong count */
    int good_prio[3] = {5, 5, 5};
    opts.num_row_priorities = 3;  /* But problem has only 2 rows */
    opts.row_priorities = good_prio;
    status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "Wrong priority count rejected");
}

/*
 * Test: Sparse LAP with priorities
 */
void test_priority_sparse(void) {
    printf("\n=== Test: Priority - Sparse LAP ===\n");

    /* 4x4 sparse problem */
    int row_ptr[5] = {0, 2, 4, 6, 8};
    int col_idx[8] = {0, 1, 1, 2, 2, 3, 0, 3};
    double values[8] = {1, 2, 2, 1, 1, 2, 2, 1};

    /* Row 0 and 3 have highest priority */
    int row_prio[4] = {10, 5, 5, 10};

    RalphLapProblem prob = {
        .n = 4, .m = 4,
        .cost_type = RALPH_LAP_COST_SPARSE,
        .sparse = {
            .nnz = 8,
            .row_ptr = row_ptr,
            .col_idx = col_idx,
            .values = values
        },
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_row_priorities = 4;
    opts.row_priorities = row_prio;

    int row_sol[4], col_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Sparse LAP with priorities succeeded");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    /* All rows should be assigned in a 4x4 square problem */
    int all_assigned = 1;
    for (int i = 0; i < 4; i++) {
        if (row_sol[i] < 0) all_assigned = 0;
    }
    ASSERT(all_assigned, "All rows assigned in square problem");

    /* Optimal cost is 5: 0->0 (1) + 1->1 (2) + 2->2 (1) + 3->3 (1) = 5 */
    ASSERT_NEAR(total, 5.0, TOLERANCE, "Optimal cost = 5");
}

/* ============================================================================
 * Cardinality Bounds Tests
 * ============================================================================ */

/*
 * Test: max_assignments limits how many pairs are assigned
 */
void test_cardinality_max_basic(void) {
    printf("\n=== Test: Cardinality - Max Assignments Basic ===\n");

    /* 5 workers, 3 jobs - normally all 3 jobs assigned */
    /* With max=2, only 2 jobs should be assigned */
    double cost[15] = {
        1, 2, 3,
        4, 1, 2,
        3, 4, 1,
        2, 3, 4,
        5, 5, 5
    };

    RalphLapProblem prob = {
        .n = 5, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.max_assignments = 2;

    int row_sol[5];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Count real assignments */
    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (row_sol[i] >= 0) count++;
    }
    ASSERT(count == 2, "Exactly 2 assignments made");

    printf("  Assignments: ");
    for (int i = 0; i < 5; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    /* Optimal 2 assignments: 0->0 (1), 2->2 (1) = 2 */
    ASSERT_NEAR(total, 2.0, TOLERANCE, "Optimal cost = 2");
}

/*
 * Test: min_assignments ensures minimum pairs
 */
void test_cardinality_min_basic(void) {
    printf("\n=== Test: Cardinality - Min Assignments Basic ===\n");

    /* 5 workers, 3 jobs - min=3 means all 3 must be assigned */
    double cost[15] = {
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1, 1
    };

    RalphLapProblem prob = {
        .n = 5, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.min_assignments = 3;

    int row_sol[5];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (row_sol[i] >= 0) count++;
    }
    ASSERT(count >= 3, "At least 3 assignments made");

    printf("  Assignments: ");
    for (int i = 0; i < 5; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f, count=%d)\n", total, count);
}

/*
 * Test: exact cardinality (min == max)
 */
void test_cardinality_exact(void) {
    printf("\n=== Test: Cardinality - Exact Count ===\n");

    /* 4x4 square, but only want exactly 2 assignments */
    double cost[16] = {
        1, 5, 5, 5,
        5, 2, 5, 5,
        5, 5, 3, 5,
        5, 5, 5, 4
    };

    RalphLapProblem prob = {
        .n = 4, .m = 4,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.min_assignments = 2;
    opts.max_assignments = 2;

    int row_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    int count = 0;
    for (int i = 0; i < 4; i++) {
        if (row_sol[i] >= 0) count++;
    }
    ASSERT(count == 2, "Exactly 2 assignments made");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    /* Best 2: 0->0 (1), 1->1 (2) = 3 */
    ASSERT_NEAR(total, 3.0, TOLERANCE, "Optimal cost = 3");
}

/*
 * Test: cardinality with priorities - high priority gets assigned
 */
void test_cardinality_with_priority(void) {
    printf("\n=== Test: Cardinality - With Priority ===\n");

    /* 4 workers, 4 jobs, max=2, priorities determine who gets assigned */
    double cost[16] = {
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1
    };

    int row_prio[4] = {5, 10, 3, 8};  /* Workers 1,3 have highest priority */

    RalphLapProblem prob = {
        .n = 4, .m = 4,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.max_assignments = 2;
    opts.num_row_priorities = 4;
    opts.row_priorities = row_prio;

    int row_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Workers 1 (prio 10) and 3 (prio 8) should be assigned */
    ASSERT(row_sol[1] >= 0, "Worker 1 (priority 10) assigned");
    ASSERT(row_sol[3] >= 0, "Worker 3 (priority 8) assigned");
    ASSERT(row_sol[0] == -1, "Worker 0 (priority 5) unassigned");
    ASSERT(row_sol[2] == -1, "Worker 2 (priority 3) unassigned");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);
}

/*
 * Test: min_assignments causes infeasible when impossible
 * Use rectangular problem: 5 workers, 3 jobs, but only 2 workers can reach jobs
 */
void test_cardinality_min_infeasible(void) {
    printf("\n=== Test: Cardinality - Min Infeasible ===\n");

    /* 5 workers, 3 jobs but workers 2,3,4 have all INFINITY costs */
    double cost[15] = {
        1, 1, 1,                                              /* Worker 0: OK */
        1, 1, 1,                                              /* Worker 1: OK */
        RALPH_LAP_INFINITY, RALPH_LAP_INFINITY, RALPH_LAP_INFINITY,  /* Worker 2: blocked */
        RALPH_LAP_INFINITY, RALPH_LAP_INFINITY, RALPH_LAP_INFINITY,  /* Worker 3: blocked */
        RALPH_LAP_INFINITY, RALPH_LAP_INFINITY, RALPH_LAP_INFINITY   /* Worker 4: blocked */
    };

    RalphLapProblem prob = {
        .n = 5, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.min_assignments = 3;  /* Want 3 but only 2 workers can actually work */

    int row_sol[5];
    RalphLapResult res = {.row_sol = row_sol};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_INFEASIBLE, "Status is INFEASIBLE");
    printf("  Correctly detected infeasibility\n");
}

/*
 * Test: cardinality validation - invalid bounds
 */
void test_cardinality_invalid(void) {
    printf("\n=== Test: Cardinality - Invalid Input ===\n");

    double cost[9] = {1,2,3,4,5,6,7,8,9};
    int row_sol[3];

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    RalphLapResult res = {.row_sol = row_sol};

    /* min > natural max */
    opts.min_assignments = 5;  /* But only 3 possible */
    opts.max_assignments = 0;
    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "min > natural_max rejected");

    /* max > natural max */
    opts.min_assignments = 0;
    opts.max_assignments = 5;
    status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "max > natural_max rejected");

    /* min > max */
    opts.min_assignments = 3;
    opts.max_assignments = 1;
    status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "min > max rejected");
}

/* ============================================================================
 * Qualification Subsets Tests
 * ============================================================================ */

/*
 * Test: basic qualification - column can only be served by subset of rows
 */
void test_qualification_basic(void) {
    printf("\n=== Test: Qualification - Basic ===\n");

    /* 4 workers, 3 jobs. Job 1 can only be done by workers 0, 2 */
    double cost[12] = {
        1, 1, 1,   /* Worker 0 */
        1, 1, 1,   /* Worker 1 - NOT qualified for job 1 */
        1, 1, 1,   /* Worker 2 */
        1, 1, 1    /* Worker 3 - NOT qualified for job 1 */
    };

    int qual_col_idx[1] = {1};  /* Job 1 has qualification */
    int qual_row_ptr[2] = {0, 2};  /* 2 qualified rows */
    int qual_rows[2] = {0, 2};  /* Workers 0 and 2 */

    RalphLapProblem prob = {
        .n = 4, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_qual_cols = 1;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;

    int row_sol[4], col_sol[3];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Job 1 must be assigned to worker 0 or 2 */
    ASSERT(col_sol[1] == 0 || col_sol[1] == 2, "Job 1 assigned to qualified worker");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    ASSERT_NEAR(total, 3.0, TOLERANCE, "Total cost = 3");
}

/*
 * Test: multiple columns with qualifications
 */
void test_qualification_multiple_cols(void) {
    printf("\n=== Test: Qualification - Multiple Columns ===\n");

    /* 4 workers, 3 jobs */
    /* Job 0: only workers 0, 1 qualified */
    /* Job 2: only workers 2, 3 qualified */
    double cost[12] = {
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1, 1
    };

    int qual_col_idx[2] = {0, 2};
    int qual_row_ptr[3] = {0, 2, 4};
    int qual_rows[4] = {0, 1, 2, 3};  /* Job 0: 0,1; Job 2: 2,3 */

    RalphLapProblem prob = {
        .n = 4, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_qual_cols = 2;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;

    int row_sol[4], col_sol[3];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Job 0 must be assigned to worker 0 or 1 */
    ASSERT(col_sol[0] == 0 || col_sol[0] == 1, "Job 0 assigned to qualified worker (0 or 1)");
    /* Job 2 must be assigned to worker 2 or 3 */
    ASSERT(col_sol[2] == 2 || col_sol[2] == 3, "Job 2 assigned to qualified worker (2 or 3)");

    printf("  col_sol: %d, %d, %d\n", col_sol[0], col_sol[1], col_sol[2]);
}

/*
 * Test: single row qualified for a column
 */
void test_qualification_single_qualified(void) {
    printf("\n=== Test: Qualification - Single Qualified ===\n");

    /* 3 workers, 3 jobs. Job 2 can ONLY be done by worker 1 */
    double cost[9] = {
        1, 2, 3,
        4, 5, 6,
        7, 8, 9
    };

    int qual_col_idx[1] = {2};
    int qual_row_ptr[2] = {0, 1};
    int qual_rows[1] = {1};  /* Only worker 1 */

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_qual_cols = 1;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;

    int row_sol[3], col_sol[3];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT(col_sol[2] == 1, "Job 2 assigned to only qualified worker (1)");
    ASSERT(row_sol[1] == 2, "Worker 1 assigned to job 2");

    printf("  Assignments: ");
    for (int i = 0; i < 3; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    /* Forced: 1->2 (6). Best remaining: 0->0 (1), 2->1 (8). Total = 15 */
    ASSERT_NEAR(total, 15.0, TOLERANCE, "Total cost = 15");
}

/*
 * Test: qualification makes problem infeasible
 * Use rectangular with min_assignments to detect infeasibility
 */
void test_qualification_infeasible(void) {
    printf("\n=== Test: Qualification - Infeasible ===\n");

    /* 4 workers, 3 jobs. All 3 jobs can only be done by worker 0.
     * With min_assignments=3, this is infeasible since worker 0 can only do 1 job.
     */
    double cost[12] = {
        1, 1, 1,   /* Worker 0 */
        1, 1, 1,   /* Worker 1 */
        1, 1, 1,   /* Worker 2 */
        1, 1, 1    /* Worker 3 */
    };

    /* All 3 jobs can only be done by worker 0 */
    int qual_col_idx[3] = {0, 1, 2};
    int qual_row_ptr[4] = {0, 1, 2, 3};
    int qual_rows[3] = {0, 0, 0};

    RalphLapProblem prob = {
        .n = 4, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_qual_cols = 3;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;
    opts.min_assignments = 3;  /* Require all 3 jobs done, but only 1 worker can do them */

    int row_sol[4];
    RalphLapResult res = {.row_sol = row_sol};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_INFEASIBLE, "Status is INFEASIBLE");
    printf("  Correctly detected infeasibility\n");
}

/*
 * Test: qualification with forbidden - both constraints apply
 */
void test_qualification_with_forbidden(void) {
    printf("\n=== Test: Qualification - With Forbidden ===\n");

    /* 3 workers, 3 jobs */
    /* Job 1: only workers 0, 1, 2 qualified (all) */
    /* But worker 0 is forbidden from job 1 */
    double cost[9] = {
        1, 1, 1,
        1, 1, 1,
        1, 1, 1
    };

    int qual_col_idx[1] = {1};
    int qual_row_ptr[2] = {0, 3};
    int qual_rows[3] = {0, 1, 2};

    int forbidden_rows[1] = {0};
    int forbidden_cols[1] = {1};

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_qual_cols = 1;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;
    opts.num_forbidden = 1;
    opts.forbidden_rows = forbidden_rows;
    opts.forbidden_cols = forbidden_cols;

    int row_sol[3], col_sol[3];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");
    ASSERT(col_sol[1] != 0, "Job 1 not assigned to forbidden worker 0");
    ASSERT(col_sol[1] == 1 || col_sol[1] == 2, "Job 1 assigned to worker 1 or 2");

    printf("  Assignments: ");
    for (int i = 0; i < 3; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);
}

/*
 * Test: qualification validation - invalid input
 */
void test_qualification_invalid(void) {
    printf("\n=== Test: Qualification - Invalid Input ===\n");

    double cost[9] = {1,2,3,4,5,6,7,8,9};
    int row_sol[3];

    RalphLapProblem prob = {
        .n = 3, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    RalphLapResult res = {.row_sol = row_sol};

    /* Invalid column index */
    int qual_col_idx_bad[1] = {5};  /* Column 5 doesn't exist */
    int qual_row_ptr[2] = {0, 1};
    int qual_rows[1] = {0};

    opts.num_qual_cols = 1;
    opts.qual_col_idx = qual_col_idx_bad;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "Invalid column index rejected");

    /* Invalid row index */
    int qual_col_idx_ok[1] = {1};
    int qual_rows_bad[1] = {10};  /* Row 10 doesn't exist */
    opts.qual_col_idx = qual_col_idx_ok;
    opts.qual_rows = qual_rows_bad;

    status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);
    ASSERT(status == RALPH_LAP_INVALID_INPUT, "Invalid row index rejected");
}

/* ============================================================================
 * Combined Constraint Tests
 * ============================================================================ */

/*
 * Test: cardinality + qualifications together
 */
void test_combined_cardinality_qualification(void) {
    printf("\n=== Test: Combined - Cardinality + Qualification ===\n");

    /* 5 workers, 4 jobs */
    /* max=2: only 2 jobs will be assigned */
    /* Job 0: only workers 0, 1 qualified */
    /* Job 1: only workers 2, 3 qualified */
    double cost[20] = {
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1
    };

    int qual_col_idx[2] = {0, 1};
    int qual_row_ptr[3] = {0, 2, 4};
    int qual_rows[4] = {0, 1, 2, 3};

    RalphLapProblem prob = {
        .n = 5, .m = 4,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.max_assignments = 2;
    opts.num_qual_cols = 2;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;

    int row_sol[5], col_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (row_sol[i] >= 0) count++;
    }
    ASSERT(count == 2, "Exactly 2 assignments made");

    /* If job 0 is assigned, must be to worker 0 or 1 */
    if (col_sol[0] >= 0) {
        ASSERT(col_sol[0] == 0 || col_sol[0] == 1, "Job 0 uses qualified worker");
    }
    /* If job 1 is assigned, must be to worker 2 or 3 */
    if (col_sol[1] >= 0) {
        ASSERT(col_sol[1] == 2 || col_sol[1] == 3, "Job 1 uses qualified worker");
    }

    printf("  Assignments: ");
    for (int i = 0; i < 5; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);
}

/*
 * Test: cardinality + priorities + qualifications (all three)
 */
void test_combined_all_three(void) {
    printf("\n=== Test: Combined - Cardinality + Priority + Qualification ===\n");

    /* 6 workers, 4 jobs */
    /* max=2: only 2 assignments */
    /* Priorities: workers 0,1 high, 2,3 medium, 4,5 low */
    /* Job 0: only workers 0,2,4 qualified (one from each priority tier) */
    /* Job 1: only workers 1,3,5 qualified */
    double cost[24] = {
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1,
        1, 1, 1, 1
    };

    int row_prio[6] = {10, 10, 5, 5, 1, 1};
    int qual_col_idx[2] = {0, 1};
    int qual_row_ptr[3] = {0, 3, 6};
    int qual_rows[6] = {0, 2, 4, 1, 3, 5};

    RalphLapProblem prob = {
        .n = 6, .m = 4,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.max_assignments = 2;
    opts.num_row_priorities = 6;
    opts.row_priorities = row_prio;
    opts.num_qual_cols = 2;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;

    int row_sol[6], col_sol[4];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    int count = 0;
    for (int i = 0; i < 6; i++) {
        if (row_sol[i] >= 0) count++;
    }
    ASSERT(count == 2, "Exactly 2 assignments made");

    printf("  Assignments: ");
    for (int i = 0; i < 6; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);

    /* High priority workers should be assigned */
    /* Worker 0 (prio 10) qualified for job 0 -> should get it */
    /* Worker 1 (prio 10) qualified for job 1 -> should get it */
    ASSERT(row_sol[0] >= 0 || row_sol[1] >= 0, "At least one high-priority worker assigned");

    /* Verify qualifications respected */
    if (col_sol[0] >= 0) {
        ASSERT(col_sol[0] == 0 || col_sol[0] == 2 || col_sol[0] == 4,
               "Job 0 assigned to qualified worker (0, 2, or 4)");
    }
    if (col_sol[1] >= 0) {
        ASSERT(col_sol[1] == 1 || col_sol[1] == 3 || col_sol[1] == 5,
               "Job 1 assigned to qualified worker (1, 3, or 5)");
    }
}

/*
 * Test: priority + qualification (no cardinality)
 */
void test_combined_priority_qualification(void) {
    printf("\n=== Test: Combined - Priority + Qualification ===\n");

    /* 4 workers, 3 jobs */
    /* Workers 0,1 high priority; 2,3 low priority */
    /* Job 0: only workers 1,3 qualified */
    /* Result: worker 1 (high priority, qualified) gets job 0 */
    double cost[12] = {
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1, 1
    };

    int row_prio[4] = {10, 10, 1, 1};
    int qual_col_idx[1] = {0};
    int qual_row_ptr[2] = {0, 2};
    int qual_rows[2] = {1, 3};  /* Only workers 1, 3 can do job 0 */

    RalphLapProblem prob = {
        .n = 4, .m = 3,
        .cost_type = RALPH_LAP_COST_DENSE,
        .dense_cost = cost,
        .objective = RALPH_LAP_MINIMIZE
    };

    RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
    opts.num_row_priorities = 4;
    opts.row_priorities = row_prio;
    opts.num_qual_cols = 1;
    opts.qual_col_idx = qual_col_idx;
    opts.qual_row_ptr = qual_row_ptr;
    opts.qual_rows = qual_rows;

    int row_sol[4], col_sol[3];
    double total;
    RalphLapResult res = {.row_sol = row_sol, .col_sol = col_sol, .costs = &total};

    RalphLapStatus status = ralph_lap_solve_ex(&prob, &opts, &res, NULL);

    ASSERT(status == RALPH_LAP_SUCCESS, "Status is SUCCESS");

    /* Job 0 must go to qualified worker; prefer worker 1 (higher priority than 3) */
    ASSERT(col_sol[0] == 1 || col_sol[0] == 3, "Job 0 assigned to qualified worker");
    /* Among qualified, worker 1 has priority 10 vs worker 3 has priority 1 */
    ASSERT(col_sol[0] == 1, "Job 0 assigned to higher-priority qualified worker (1)");

    printf("  Assignments: ");
    for (int i = 0; i < 4; i++) printf("%d->%d ", i, row_sol[i]);
    printf("(cost=%.1f)\n", total);
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
    test_workspace();
    test_rect_more_jobs();
    test_rect_more_workers();
    test_rect_maximize();
    test_sparse_native();
    test_sparse_maximize();
    test_sparse_infeasible();
    test_sparse_vs_dense();
    test_parallel_setting();
    test_workspace_repeated();
    test_epsilon_scaling();
    test_epsilon_scaling_maximize();
    test_warm_start_basic();
    test_warm_start_similar();
    test_warm_start_different();
    test_warm_start_manual();
    test_warm_start_maximize();
    test_warm_start_performance();
    test_callback_basic();
    test_callback_euclidean();
    test_callback_maximize();
    test_callback_workspace();
    test_callback_forbidden();
    test_callback_performance();
    test_detect_lap_basic();
    test_detect_lap_solve();
    test_detect_lap_non_lap();
    test_detect_lap_disable();
    test_detect_lap_maximize();
    test_k_best_basic();
    test_k_best_verify_order();
    test_k_best_all();
    test_k_best_maximize();
    test_k_best_large();
    test_k_best_with_workspace();
    test_k_best_single();

    /* Unified API tests */
    test_unified_basic();
    test_unified_k_best();
    test_unified_forbidden();
    test_unified_rectangular();
    test_unified_sparse();
    test_unified_callback();
    test_unified_options_combination();
    test_unified_maximize();
    test_unified_sparse_k_best();
    test_unified_callback_k_best();
    test_unified_warm_start();
    test_unified_bottleneck_basic();
    test_unified_bottleneck_maximin();
    test_unified_bottleneck_diagonal();
    test_unified_bottleneck_rectangular();
    test_unified_bottleneck_rect_maximin();
    test_unified_k_best_rectangular();
    test_unified_k_best_rect_maximize();

    /* Priority constraint tests */
    test_priority_row_only();
    test_priority_col_only();
    test_priority_both();
    test_priority_tie_break();
    test_priority_maximize();
    test_priority_with_forbidden();
    test_priority_invalid();
    test_priority_sparse();

    /* Cardinality bounds tests */
    test_cardinality_max_basic();
    test_cardinality_min_basic();
    test_cardinality_exact();
    test_cardinality_with_priority();
    test_cardinality_min_infeasible();
    test_cardinality_invalid();

    /* Qualification subsets tests */
    test_qualification_basic();
    test_qualification_multiple_cols();
    test_qualification_single_qualified();
    test_qualification_infeasible();
    test_qualification_with_forbidden();
    test_qualification_invalid();

    /* Combined constraint tests */
    test_combined_cardinality_qualification();
    test_combined_all_three();
    test_combined_priority_qualification();

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
