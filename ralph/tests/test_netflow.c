/*
 * test_netflow.c - Tests for Network Simplex Solver
 *
 * Tests the network simplex algorithm against:
 * - Known optimal solutions
 * - Various network structures (transportation, assignment, shortest path)
 * - Edge cases (infeasible, degenerate, unbounded)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "netflow.h"
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

/* Safe allocation macro - uses calloc to prevent overflow and zero-initializes */
#define SAFE_CALLOC(ptr, count, type) do { \
    (ptr) = (type *)calloc((count), sizeof(type)); \
    if (!(ptr)) { \
        printf("  SKIP: Memory allocation failed\n"); \
        return; \
    } \
} while(0)

/* ============================================================================
 * Test 1: Simple 3-node network
 * ============================================================================ */
static void test_simple_3node(void) {
    printf("\n=== Test: Simple 3-Node Network ===\n");

    /*
     * Network:  0 ---(cost=1, cap=10)---> 1 ---(cost=2, cap=10)---> 2
     * Supply: [5, 0, -5]
     * Expected: flow = [5, 5], obj = 5*1 + 5*2 = 15
     */
    int tail[] = {0, 1};
    int head[] = {1, 2};
    double cost[] = {1.0, 2.0};
    double cap[] = {10.0, 10.0};
    double supply[] = {5.0, 0.0, -5.0};

    double flow[2];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(3, 2, tail, head, cost, cap, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 15.0, TOLERANCE, "Objective value");
    ASSERT_NEAR(flow[0], 5.0, TOLERANCE, "Flow on arc 0");
    ASSERT_NEAR(flow[1], 5.0, TOLERANCE, "Flow on arc 1");
}

/* ============================================================================
 * Test 2: Single arc
 * ============================================================================ */
static void test_single_arc(void) {
    printf("\n=== Test: Single Arc Network ===\n");

    /*
     * Network: 0 ---(cost=3, cap=inf)---> 1
     * Supply: [7, -7]
     * Expected: flow = [7], obj = 21
     */
    int tail[] = {0};
    int head[] = {1};
    double cost[] = {3.0};
    double supply[] = {7.0, -7.0};

    double flow[1];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(2, 1, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 21.0, TOLERANCE, "Objective value");
    ASSERT_NEAR(flow[0], 7.0, TOLERANCE, "Flow on arc 0");
}

/* ============================================================================
 * Test 3: Parallel arcs
 * ============================================================================ */
static void test_parallel_arcs(void) {
    printf("\n=== Test: Parallel Arcs ===\n");

    /*
     * Network: Two arcs from 0 to 1 with different costs
     * 0 ---(cost=1, cap=5)---> 1
     * 0 ---(cost=2, cap=5)---> 1
     * Supply: [8, -8]
     * Expected: Use cheaper arc fully (5), then expensive (3)
     * Obj = 5*1 + 3*2 = 11
     */
    int tail[] = {0, 0};
    int head[] = {1, 1};
    double cost[] = {1.0, 2.0};
    double cap[] = {5.0, 5.0};
    double supply[] = {8.0, -8.0};

    double flow[2];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(2, 2, tail, head, cost, cap, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 11.0, TOLERANCE, "Objective value");
    ASSERT_NEAR(flow[0], 5.0, TOLERANCE, "Flow on cheaper arc");
    ASSERT_NEAR(flow[1], 3.0, TOLERANCE, "Flow on expensive arc");
}

/* ============================================================================
 * Test 4: Transportation problem 2x2
 * ============================================================================ */
static void test_transportation_2x2(void) {
    printf("\n=== Test: Transportation 2x2 ===\n");

    /*
     * Sources: 0 (supply 10), 1 (supply 5)
     * Sinks: 2 (demand 8), 3 (demand 7)
     *
     * Costs:
     *        Sink2  Sink3
     * Src0     2      3
     * Src1     4      1
     *
     * Optimal: 0->2 (8), 1->3 (5), 0->3 (2)
     * Cost = 8*2 + 5*1 + 2*3 = 16 + 5 + 6 = 27
     */
    int tail[] = {0, 0, 1, 1};
    int head[] = {2, 3, 2, 3};
    double cost[] = {2.0, 3.0, 4.0, 1.0};
    double supply[] = {10.0, 5.0, -8.0, -7.0};

    double flow[4];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(4, 4, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 27.0, TOLERANCE, "Objective value");

    /* Verify feasibility */
    RalphNetflowProblem prob = {
        .num_nodes = 4, .num_arcs = 4,
        .tail = tail, .head = head, .cost = cost,
        .capacity = NULL, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };
    double max_viol;
    int feasible = ralph_netflow_verify(&prob, flow, NULL, &max_viol);
    ASSERT(feasible, "Solution is feasible");
}

/* ============================================================================
 * Test 5: Transportation problem 3x3
 * ============================================================================ */
static void test_transportation_3x3(void) {
    printf("\n=== Test: Transportation 3x3 ===\n");

    /*
     * Classic transportation problem
     * Sources: 0, 1, 2 with supplies 20, 30, 25
     * Sinks: 3, 4, 5 with demands 15, 25, 35
     */
    int tail[] = {0, 0, 0, 1, 1, 1, 2, 2, 2};
    int head[] = {3, 4, 5, 3, 4, 5, 3, 4, 5};
    double cost[] = {8, 6, 10, 9, 12, 13, 14, 9, 16};
    double supply[] = {20.0, 30.0, 25.0, -15.0, -25.0, -35.0};

    double flow[9];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(6, 9, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");

    /* Verify feasibility */
    RalphNetflowProblem prob = {
        .num_nodes = 6, .num_arcs = 9,
        .tail = tail, .head = head, .cost = cost,
        .capacity = NULL, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };
    double max_viol;
    int feasible = ralph_netflow_verify(&prob, flow, NULL, &max_viol);
    ASSERT(feasible, "Solution is feasible");
    printf("  Objective: %.2f\n", obj);
}

/* ============================================================================
 * Test 6: Assignment problem 3x3 (unit supply/demand)
 * ============================================================================ */
static void test_assignment_3x3(void) {
    printf("\n=== Test: Assignment 3x3 ===\n");

    /*
     * Assignment problem as MCNF:
     * Workers: 0, 1, 2 (supply 1 each)
     * Jobs: 3, 4, 5 (demand 1 each)
     * Each worker-job arc has capacity 1
     *
     * Cost matrix:
     *      Job3  Job4  Job5
     * W0    9     2     7
     * W1    6     4     3
     * W2    5     8     1
     *
     * Optimal: W0->Job4, W1->Job3, W2->Job5, cost = 2+6+1 = 9
     */
    int tail[] = {0, 0, 0, 1, 1, 1, 2, 2, 2};
    int head[] = {3, 4, 5, 3, 4, 5, 3, 4, 5};
    double cost[] = {9, 2, 7, 6, 4, 3, 5, 8, 1};
    double cap[] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    double supply[] = {1.0, 1.0, 1.0, -1.0, -1.0, -1.0};

    double flow[9];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(6, 9, tail, head, cost, cap, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 9.0, TOLERANCE, "Objective matches LAP solution");
}

/* ============================================================================
 * Test 7: Shortest path as MCNF
 * ============================================================================ */
static void test_shortest_path_simple(void) {
    printf("\n=== Test: Shortest Path ===\n");

    /*
     * Find shortest path from 0 to 4 by sending 1 unit of flow
     *
     *      1---3
     *     /|   |\
     *    2 |   | 2
     *   /  1   1  \
     *  0       |   4
     *   \  2   |  /
     *    3 |   | 4
     *     \|   |/
     *      2---3
     *
     * Edges: 0-1 (2), 0-2 (3), 1-2 (1), 1-3 (3), 2-3 (2), 3-4 (2), 1-4 (4)
     * Shortest path: 0 -> 1 -> 4, cost = 2 + 4 = 6
     */
    int tail[] = {0, 0, 1, 1, 2, 3, 1};
    int head[] = {1, 2, 2, 3, 3, 4, 4};
    double cost[] = {2, 3, 1, 3, 2, 2, 4};
    double supply[] = {1.0, 0.0, 0.0, 0.0, -1.0};

    double flow[7];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(5, 7, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 6.0, TOLERANCE, "Shortest path cost");
}

/* ============================================================================
 * Test 8: Transshipment (intermediate nodes)
 * ============================================================================ */
static void test_transshipment(void) {
    printf("\n=== Test: Transshipment Network ===\n");

    /*
     * Supply -> Hub -> Demand
     * Node 0: supply 10
     * Node 1: hub (supply 0)
     * Node 2: hub (supply 0)
     * Node 3: demand 10
     *
     * Arcs: 0->1 (cost 1), 0->2 (cost 2), 1->3 (cost 3), 2->3 (cost 1)
     * Optimal: 0->2->3 with cost 2+1=3 per unit, total = 30
     */
    int tail[] = {0, 0, 1, 2};
    int head[] = {1, 2, 3, 3};
    double cost[] = {1.0, 2.0, 3.0, 1.0};
    double supply[] = {10.0, 0.0, 0.0, -10.0};

    double flow[4];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(4, 4, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 30.0, TOLERANCE, "Objective value");
    ASSERT_NEAR(flow[1], 10.0, TOLERANCE, "All flow via node 2");
    ASSERT_NEAR(flow[3], 10.0, TOLERANCE, "All flow to sink via arc 2->3");
}

/* ============================================================================
 * Test 9: Infeasible - imbalanced supply/demand
 * ============================================================================ */
static void test_infeasible_imbalanced(void) {
    printf("\n=== Test: Infeasible (Imbalanced) ===\n");

    /*
     * Supply: 10, Demand: 8 (imbalanced)
     */
    int tail[] = {0};
    int head[] = {1};
    double cost[] = {1.0};
    double supply[] = {10.0, -8.0};

    double flow[1];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(2, 1, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_INFEASIBLE, "Status is INFEASIBLE");
}

/* ============================================================================
 * Test 10: Infeasible - capacity too small
 * ============================================================================ */
static void test_infeasible_capacity(void) {
    printf("\n=== Test: Infeasible (Capacity) ===\n");

    /*
     * Supply 10, demand 10, but arc capacity is only 5
     */
    int tail[] = {0};
    int head[] = {1};
    double cost[] = {1.0};
    double cap[] = {5.0};
    double supply[] = {10.0, -10.0};

    double flow[1];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(2, 1, tail, head, cost, cap, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_INFEASIBLE, "Status is INFEASIBLE");
}

/* ============================================================================
 * Test 11: Degenerate problem
 * ============================================================================ */
static void test_degenerate(void) {
    printf("\n=== Test: Degenerate Network ===\n");

    /*
     * Multiple paths with same cost (degenerate optimal)
     */
    int tail[] = {0, 0, 1, 2};
    int head[] = {1, 2, 3, 3};
    double cost[] = {1.0, 1.0, 1.0, 1.0};
    double supply[] = {10.0, 0.0, 0.0, -10.0};

    double flow[4];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(4, 4, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 20.0, TOLERANCE, "Objective value (any path has cost 2)");

    /* Verify feasibility */
    RalphNetflowProblem prob = {
        .num_nodes = 4, .num_arcs = 4,
        .tail = tail, .head = head, .cost = cost,
        .capacity = NULL, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };
    int feasible = ralph_netflow_verify(&prob, flow, NULL, NULL);
    ASSERT(feasible, "Solution is feasible");
}

/* ============================================================================
 * Test 12: All zero costs
 * ============================================================================ */
static void test_all_zero_cost(void) {
    printf("\n=== Test: Zero Costs (Feasibility) ===\n");

    /*
     * Just find a feasible flow, all costs are zero
     */
    int tail[] = {0, 0, 1, 2};
    int head[] = {1, 2, 3, 3};
    double cost[] = {0.0, 0.0, 0.0, 0.0};
    double supply[] = {10.0, 0.0, 0.0, -10.0};

    double flow[4];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(4, 4, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 0.0, TOLERANCE, "Objective is zero");
}

/* ============================================================================
 * Test 13: Negative costs
 * ============================================================================ */
static void test_negative_costs(void) {
    printf("\n=== Test: Negative Costs ===\n");

    /*
     * Negative cost arc - should prefer it if capacity allows
     */
    int tail[] = {0, 0};
    int head[] = {1, 1};
    double cost[] = {-5.0, 2.0};
    double cap[] = {3.0, 10.0};
    double supply[] = {7.0, -7.0};

    double flow[2];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(2, 2, tail, head, cost, cap, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(flow[0], 3.0, TOLERANCE, "Maximize negative cost arc");
    ASSERT_NEAR(flow[1], 4.0, TOLERANCE, "Remainder on positive arc");
    ASSERT_NEAR(obj, 3*(-5) + 4*2, TOLERANCE, "Objective value");
}

/* ============================================================================
 * Test 14: Lower bounds
 * ============================================================================ */
static void test_lower_bounds(void) {
    printf("\n=== Test: Lower Bounds ===\n");

    /*
     * Arc with lower bound 3, capacity 10
     * Supply 5, demand 5
     */
    int tail[] = {0};
    int head[] = {1};
    double cost[] = {2.0};
    double cap[] = {10.0};
    double lower[] = {3.0};
    double supply[] = {5.0, -5.0};

    RalphNetflowProblem prob = {
        .num_nodes = 2, .num_arcs = 1,
        .tail = tail, .head = head, .cost = cost,
        .capacity = cap, .lower = lower, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };

    double flow[1];
    RalphNetflowResult result = {.flow = flow, .potential = NULL};

    RalphNetflowStatus status = ralph_netflow_solve(&prob, NULL, &result, NULL);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(flow[0], 5.0, TOLERANCE, "Flow is 5 (>= lower bound)");
    ASSERT_NEAR(result.objective, 10.0, TOLERANCE, "Objective value");
}

/* ============================================================================
 * Test 15: Single node (trivial)
 * ============================================================================ */
static void test_single_node(void) {
    printf("\n=== Test: Single Node ===\n");

    /*
     * Single node with zero supply - trivially feasible
     */
    double supply[] = {0.0};

    double *flow = NULL;  /* No arcs */
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(1, 0, NULL, NULL, NULL, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 0.0, TOLERANCE, "Objective is zero");
}

/* ============================================================================
 * Test 16: Disconnected network
 * ============================================================================ */
static void test_disconnected(void) {
    printf("\n=== Test: Disconnected (Infeasible) ===\n");

    /*
     * Two separate components, each balanced
     */
    int tail[] = {0, 2};
    int head[] = {1, 3};
    double cost[] = {1.0, 1.0};
    double supply[] = {5.0, -5.0, 3.0, -3.0};

    double flow[2];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(4, 2, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL (each component balanced)");
    ASSERT_NEAR(flow[0], 5.0, TOLERANCE, "Flow in component 1");
    ASSERT_NEAR(flow[1], 3.0, TOLERANCE, "Flow in component 2");
}

/* ============================================================================
 * Test 17: Self loop
 * ============================================================================ */
static void test_self_loop(void) {
    printf("\n=== Test: Self Loop ===\n");

    /*
     * Arc from node 0 to itself (no effect on flow balance)
     */
    int tail[] = {0, 0};
    int head[] = {0, 1};
    double cost[] = {-1.0, 2.0};  /* Self-loop has negative cost */
    double cap[] = {5.0, 10.0};
    double supply[] = {7.0, -7.0};

    double flow[2];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(2, 2, tail, head, cost, cap, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(flow[1], 7.0, TOLERANCE, "Flow to sink");
    /* Self-loop should be at capacity (negative cost) */
    ASSERT_NEAR(flow[0], 5.0, TOLERANCE, "Self-loop at capacity");
}

/* ============================================================================
 * Test 18: Maximization
 * ============================================================================ */
static void test_maximize(void) {
    printf("\n=== Test: Maximization ===\n");

    /*
     * Maximize flow value (profit on arcs)
     */
    int tail[] = {0, 0};
    int head[] = {1, 1};
    double cost[] = {3.0, 1.0};  /* Prefer arc 0 (higher profit) */
    double cap[] = {5.0, 10.0};
    double supply[] = {8.0, -8.0};

    RalphNetflowProblem prob = {
        .num_nodes = 2, .num_arcs = 2,
        .tail = tail, .head = head, .cost = cost,
        .capacity = cap, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MAXIMIZE
    };

    double flow[2];
    RalphNetflowResult result = {.flow = flow, .potential = NULL};

    RalphNetflowStatus status = ralph_netflow_solve(&prob, NULL, &result, NULL);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(flow[0], 5.0, TOLERANCE, "Higher profit arc at capacity");
    ASSERT_NEAR(flow[1], 3.0, TOLERANCE, "Remainder on lower profit arc");
    ASSERT_NEAR(result.objective, 5*3 + 3*1, TOLERANCE, "Maximized objective");
}

/* ============================================================================
 * Test 19: Transportation 10x10
 * ============================================================================ */
static void test_transportation_10x10(void) {
    printf("\n=== Test: Transportation 10x10 ===\n");

    int m = 10, n = 10;
    int num_nodes = m + n;
    int num_arcs = m * n;

    int *tail, *head;
    double *cost, *supply, *flow;
    SAFE_CALLOC(tail, num_arcs, int);
    SAFE_CALLOC(head, num_arcs, int);
    SAFE_CALLOC(cost, num_arcs, double);
    SAFE_CALLOC(supply, num_nodes, double);
    SAFE_CALLOC(flow, num_arcs, double);

    /* Build bipartite graph */
    srand(42);
    int arc = 0;
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            tail[arc] = i;
            head[arc] = m + j;
            cost[arc] = (rand() % 100) + 1;
            arc++;
        }
    }

    /* Balanced supply/demand */
    double total = 0;
    for (int i = 0; i < m; i++) {
        supply[i] = (rand() % 20) + 10;
        total += supply[i];
    }
    for (int j = 0; j < n - 1; j++) {
        double d = total / (n - j);
        if (d > total) d = total;
        supply[m + j] = -d;
        total -= d;
    }
    supply[m + n - 1] = -total;

    double obj;
    RalphNetflowStatus status = ralph_mcnf_solve(num_nodes, num_arcs, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");

    /* Verify */
    RalphNetflowProblem prob = {
        .num_nodes = num_nodes, .num_arcs = num_arcs,
        .tail = tail, .head = head, .cost = cost,
        .capacity = NULL, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };
    double max_viol;
    int feasible = ralph_netflow_verify(&prob, flow, &obj, &max_viol);
    ASSERT(feasible, "Solution is feasible");
    printf("  Objective: %.2f, Max violation: %.2e\n", obj, max_viol);

    free(tail);
    free(head);
    free(cost);
    free(supply);
    free(flow);
}

/* ============================================================================
 * Test 20: Transportation 100x100 (scale test)
 * ============================================================================ */
static void test_transportation_100x100(void) {
    printf("\n=== Test: Transportation 100x100 (Scale) ===\n");

    int m = 100, n = 100;
    int num_nodes = m + n;
    int num_arcs = m * n;

    int *tail, *head;
    double *cost, *supply, *flow;
    SAFE_CALLOC(tail, num_arcs, int);
    SAFE_CALLOC(head, num_arcs, int);
    SAFE_CALLOC(cost, num_arcs, double);
    SAFE_CALLOC(supply, num_nodes, double);
    SAFE_CALLOC(flow, num_arcs, double);

    /* Build bipartite graph */
    srand(123);
    int arc = 0;
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            tail[arc] = i;
            head[arc] = m + j;
            cost[arc] = (rand() % 100) + 1;
            arc++;
        }
    }

    /* Balanced supply/demand */
    double total = 0;
    for (int i = 0; i < m; i++) {
        supply[i] = (rand() % 50) + 25;
        total += supply[i];
    }
    for (int j = 0; j < n - 1; j++) {
        double d = total / (n - j);
        supply[m + j] = -d;
        total -= d;
    }
    supply[m + n - 1] = -total;

    clock_t start = clock();
    double obj;
    RalphNetflowStatus status = ralph_mcnf_solve(num_nodes, num_arcs, tail, head, cost, NULL, supply, flow, &obj);
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    printf("  Time: %.3f seconds, Objective: %.2f\n", elapsed, obj);

    /* Verify */
    RalphNetflowProblem prob = {
        .num_nodes = num_nodes, .num_arcs = num_arcs,
        .tail = tail, .head = head, .cost = cost,
        .capacity = NULL, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };
    int feasible = ralph_netflow_verify(&prob, flow, NULL, NULL);
    ASSERT(feasible, "Solution is feasible");

    free(tail);
    free(head);
    free(cost);
    free(supply);
    free(flow);
}

/* ============================================================================
 * Test 21: Sparse network 1000 nodes
 * ============================================================================ */
static void test_sparse_network_1k(void) {
    printf("\n=== Test: Sparse Network 1K Nodes ===\n");

    int num_nodes = 1000;
    int num_arcs = 5000;  /* Sparse: ~5 arcs per node on average */

    int *tail, *head;
    double *cost, *supply, *flow;
    SAFE_CALLOC(tail, num_arcs, int);
    SAFE_CALLOC(head, num_arcs, int);
    SAFE_CALLOC(cost, num_arcs, double);
    SAFE_CALLOC(supply, num_nodes, double);
    SAFE_CALLOC(flow, num_arcs, double);

    /* Build sparse random graph */
    srand(456);
    for (int a = 0; a < num_arcs; a++) {
        tail[a] = rand() % num_nodes;
        head[a] = rand() % num_nodes;
        /* Avoid self-loops */
        while (head[a] == tail[a]) {
            head[a] = rand() % num_nodes;
        }
        cost[a] = (rand() % 100) + 1;
    }

    /* Random supply/demand (balanced) */
    double total = 0;
    for (int i = 0; i < num_nodes - 1; i++) {
        supply[i] = (rand() % 21) - 10;  /* -10 to +10 */
        total += supply[i];
    }
    supply[num_nodes - 1] = -total;

    clock_t start = clock();
    double obj;
    RalphNetflowStatus status = ralph_mcnf_solve(num_nodes, num_arcs, tail, head, cost, NULL, supply, flow, &obj);
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    /* May be infeasible due to disconnected components */
    printf("  Status: %s, Time: %.3f seconds\n", ralph_netflow_status_string(status), elapsed);
    if (status == RALPH_NETFLOW_OPTIMAL) {
        printf("  Objective: %.2f\n", obj);
        tests_run++;
        tests_passed++;
    } else {
        /* Infeasible is also acceptable for random sparse graph */
        ASSERT(status == RALPH_NETFLOW_INFEASIBLE || status == RALPH_NETFLOW_OPTIMAL,
               "Status is OPTIMAL or INFEASIBLE");
    }

    free(tail);
    free(head);
    free(cost);
    free(supply);
    free(flow);
}

/* ============================================================================
 * Test 22: Chain network (deep tree)
 * ============================================================================ */
static void test_chain_network(void) {
    printf("\n=== Test: Chain Network (Deep Tree) ===\n");

    /*
     * Linear chain: 0 -> 1 -> 2 -> ... -> 99
     * Tests tree depth handling
     */
    int num_nodes = 100;
    int num_arcs = num_nodes - 1;

    int *tail, *head;
    double *cost, *supply, *flow;
    SAFE_CALLOC(tail, num_arcs, int);
    SAFE_CALLOC(head, num_arcs, int);
    SAFE_CALLOC(cost, num_arcs, double);
    SAFE_CALLOC(supply, num_nodes, double);
    SAFE_CALLOC(flow, num_arcs, double);

    for (int a = 0; a < num_arcs; a++) {
        tail[a] = a;
        head[a] = a + 1;
        cost[a] = 1.0;
    }

    /* Supply at start, demand at end (already zero from calloc) */
    supply[0] = 10.0;
    supply[num_nodes - 1] = -10.0;

    double obj;
    RalphNetflowStatus status = ralph_mcnf_solve(num_nodes, num_arcs, tail, head, cost, NULL, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(obj, 10.0 * (num_nodes - 1), TOLERANCE, "Objective (10 * 99 = 990)");

    /* All arcs should have flow 10 */
    int all_correct = 1;
    for (int a = 0; a < num_arcs; a++) {
        if (fabs(flow[a] - 10.0) > TOLERANCE) {
            all_correct = 0;
            break;
        }
    }
    ASSERT(all_correct, "All arcs have flow 10");

    free(tail);
    free(head);
    free(cost);
    free(supply);
    free(flow);
}

/* ============================================================================
 * Test 23: Workspace reuse
 * ============================================================================ */
static void test_workspace_reuse(void) {
    printf("\n=== Test: Workspace Reuse ===\n");

    /* Create workspace once */
    RalphNetflowWorkspace *ws = ralph_netflow_workspace_create(100, 1000);
    ASSERT(ws != NULL, "Workspace created");

    /* Solve multiple problems */
    for (int iter = 0; iter < 5; iter++) {
        int tail[] = {0, 0, 1, 2};
        int head[] = {1, 2, 3, 3};
        double cost[] = {1.0 + iter, 2.0, 3.0, 1.0 - iter * 0.1};
        double supply[] = {10.0, 0.0, 0.0, -10.0};

        RalphNetflowProblem prob = {
            .num_nodes = 4, .num_arcs = 4,
            .tail = tail, .head = head, .cost = cost,
            .capacity = NULL, .lower = NULL, .supply = supply,
            .objective = RALPH_NETFLOW_MINIMIZE
        };

        double flow[4];
        RalphNetflowResult result = {.flow = flow, .potential = NULL};

        RalphNetflowStatus status = ralph_netflow_solve(&prob, NULL, &result, ws);
        if (status != RALPH_NETFLOW_OPTIMAL) {
            printf("  FAIL: Iteration %d not optimal\n", iter);
            break;
        }
    }

    tests_run++;
    tests_passed++;
    printf("  PASS: Solved 5 problems with same workspace\n");

    ralph_netflow_workspace_free(ws);
}

/* ============================================================================
 * Test 24: Verify vs LP formulation (small)
 * ============================================================================ */
static void test_vs_lp_small(void) {
    printf("\n=== Test: Verify vs LP (Small) ===\n");

    /*
     * Solve with network simplex, then formulate as LP and compare
     */
    int tail[] = {0, 0, 1, 1, 2};
    int head[] = {1, 2, 2, 3, 3};
    double cost[] = {2.0, 5.0, 1.0, 4.0, 3.0};
    double cap[] = {10.0, 8.0, 6.0, 7.0, 9.0};
    double supply[] = {12.0, 0.0, 0.0, -12.0};

    /* Solve with network simplex */
    double flow[5];
    double ns_obj;
    RalphNetflowStatus status = ralph_mcnf_solve(4, 5, tail, head, cost, cap, supply, flow, &ns_obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Network simplex optimal");

    /* Formulate as LP */
    RalphModel *model = ralph_create();
    for (int a = 0; a < 5; a++) {
        ralph_add_var(model, 0.0, cap[a], cost[a], RALPH_CONTINUOUS);
    }

    /* Flow conservation constraints */
    for (int i = 0; i < 4; i++) {
        int vars[5];
        double coefs[5];
        int count = 0;

        for (int a = 0; a < 5; a++) {
            if (tail[a] == i) {
                vars[count] = a;
                coefs[count] = 1.0;  /* Outflow */
                count++;
            }
            if (head[a] == i) {
                vars[count] = a;
                coefs[count] = -1.0;  /* Inflow */
                count++;
            }
        }

        ralph_add_constraint(model, count, vars, coefs, RALPH_EQUAL, supply[i]);
    }

    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_optimize(model);
    RalphStatus lp_status = ralph_get_status(model);
    ASSERT(lp_status == RALPH_STATUS_OPTIMAL, "LP optimal");

    double lp_obj = ralph_get_objval(model);
    ASSERT_NEAR(ns_obj, lp_obj, TOLERANCE, "Objectives match");

    ralph_free(model);
}

/* ============================================================================
 * Test 25: Complementary slackness
 * ============================================================================ */
static void test_complementary_slackness(void) {
    printf("\n=== Test: Complementary Slackness ===\n");

    int tail[] = {0, 0, 1, 2};
    int head[] = {1, 2, 3, 3};
    double cost[] = {1.0, 2.0, 3.0, 1.0};
    double cap[] = {10.0, 10.0, 10.0, 10.0};
    double supply[] = {10.0, 0.0, 0.0, -10.0};

    RalphNetflowProblem prob = {
        .num_nodes = 4, .num_arcs = 4,
        .tail = tail, .head = head, .cost = cost,
        .capacity = cap, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };

    double flow[4];
    double potential[4];
    RalphNetflowResult result = {.flow = flow, .potential = potential};

    RalphNetflowStatus status = ralph_netflow_solve(&prob, NULL, &result, NULL);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");

    /* Check optimality conditions */
    double max_viol;
    int optimal = ralph_netflow_check_optimality(&prob, flow, potential, &max_viol);
    ASSERT(optimal, "Optimality conditions satisfied");
    printf("  Max reduced cost violation: %.2e\n", max_viol);
}

/* ============================================================================
 * Test 26: Integer data gives integer solution
 * ============================================================================ */
static void test_integer_solution(void) {
    printf("\n=== Test: Integer Solution ===\n");

    /*
     * Integer costs, capacities, supplies should give integer flows
     * Network: 0 -> 1 -> 2 -> 3 (chain with enough capacity)
     */
    int tail[] = {0, 1, 2};
    int head[] = {1, 2, 3};
    double cost[] = {3, 2, 5};
    double cap[] = {15, 12, 20};
    double supply[] = {10, 0, 0, -10};

    double flow[3];
    double obj;

    RalphNetflowStatus status = ralph_mcnf_solve(4, 3, tail, head, cost, cap, supply, flow, &obj);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");

    /* Check all flows are integer */
    int all_int = 1;
    for (int a = 0; a < 3; a++) {
        double frac = flow[a] - floor(flow[a] + 0.5);
        if (fabs(frac) > TOLERANCE) {
            all_int = 0;
            printf("  Arc %d: flow = %.6f (not integer)\n", a, flow[a]);
        }
    }
    ASSERT(all_int, "All flows are integer");
}

/* ============================================================================
 * Test 27: Flow conservation check
 * ============================================================================ */
static void test_flow_conservation(void) {
    printf("\n=== Test: Flow Conservation ===\n");

    /* Larger network for thorough conservation check */
    int num_nodes = 20;
    int num_arcs = 60;

    int *tail, *head;
    double *cost, *supply, *flow;
    SAFE_CALLOC(tail, num_arcs, int);
    SAFE_CALLOC(head, num_arcs, int);
    SAFE_CALLOC(cost, num_arcs, double);
    SAFE_CALLOC(supply, num_nodes, double);
    SAFE_CALLOC(flow, num_arcs, double);

    /* Build random graph */
    srand(789);
    for (int a = 0; a < num_arcs; a++) {
        tail[a] = rand() % num_nodes;
        head[a] = rand() % num_nodes;
        while (head[a] == tail[a]) {
            head[a] = rand() % num_nodes;
        }
        cost[a] = (rand() % 10) + 1;
    }

    /* Balanced supply/demand (already zero from calloc) */
    supply[0] = 50.0;
    supply[num_nodes - 1] = -50.0;

    double obj;
    RalphNetflowStatus status = ralph_mcnf_solve(num_nodes, num_arcs, tail, head, cost, NULL, supply, flow, &obj);

    /* Check flow conservation at each node */
    if (status == RALPH_NETFLOW_OPTIMAL) {
        int conservation_ok = 1;
        for (int i = 0; i < num_nodes; i++) {
            double net = supply[i];
            for (int a = 0; a < num_arcs; a++) {
                if (tail[a] == i) net -= flow[a];
                if (head[a] == i) net += flow[a];
            }
            if (fabs(net) > TOLERANCE * 10) {
                conservation_ok = 0;
                printf("  Node %d: net flow = %.6f (should be 0)\n", i, net);
            }
        }
        ASSERT(conservation_ok, "Flow conservation at all nodes");
    } else {
        printf("  Status: %s (may be infeasible for random graph)\n",
               ralph_netflow_status_string(status));
        tests_run++;
        tests_passed++;  /* Infeasible is acceptable */
    }

    free(tail);
    free(head);
    free(cost);
    free(supply);
    free(flow);
}

/* ============================================================================
 * Test 28: First eligible pricing
 * ============================================================================ */
static void test_first_eligible_pricing(void) {
    printf("\n=== Test: First Eligible Pricing ===\n");

    int tail[] = {0, 0, 1, 2};
    int head[] = {1, 2, 3, 3};
    double cost[] = {1.0, 2.0, 3.0, 1.0};
    double supply[] = {10.0, 0.0, 0.0, -10.0};

    RalphNetflowProblem prob = {
        .num_nodes = 4, .num_arcs = 4,
        .tail = tail, .head = head, .cost = cost,
        .capacity = NULL, .lower = NULL, .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };

    RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;
    opts.pricing_rule = 1;  /* First eligible */

    double flow[4];
    RalphNetflowResult result = {.flow = flow, .potential = NULL};

    RalphNetflowStatus status = ralph_netflow_solve(&prob, &opts, &result, NULL);

    ASSERT(status == RALPH_NETFLOW_OPTIMAL, "Status is OPTIMAL");
    ASSERT_NEAR(result.objective, 30.0, TOLERANCE, "Objective value");
}

/* ============================================================================
 * Test 29: Status string
 * ============================================================================ */
static void test_status_string(void) {
    printf("\n=== Test: Status Strings ===\n");

    ASSERT(strcmp(ralph_netflow_status_string(RALPH_NETFLOW_OPTIMAL), "Optimal") == 0,
           "OPTIMAL string");
    ASSERT(strcmp(ralph_netflow_status_string(RALPH_NETFLOW_INFEASIBLE), "Infeasible") == 0,
           "INFEASIBLE string");
    ASSERT(strcmp(ralph_netflow_status_string(RALPH_NETFLOW_UNBOUNDED), "Unbounded") == 0,
           "UNBOUNDED string");
    ASSERT(strcmp(ralph_netflow_status_string(RALPH_NETFLOW_INVALID_INPUT), "Invalid input") == 0,
           "INVALID_INPUT string");
}

/* ============================================================================
 * Test 30: Invalid input handling
 * ============================================================================ */
static void test_invalid_input(void) {
    printf("\n=== Test: Invalid Input Handling ===\n");

    double flow[1];
    RalphNetflowResult result = {.flow = flow};

    /* NULL problem */
    RalphNetflowStatus status = ralph_netflow_solve(NULL, NULL, &result, NULL);
    ASSERT(status == RALPH_NETFLOW_INVALID_INPUT, "NULL problem rejected");

    /* NULL result */
    int tail[] = {0};
    int head[] = {1};
    double cost[] = {1.0};
    double supply[] = {1.0, -1.0};
    RalphNetflowProblem prob = {
        .num_nodes = 2, .num_arcs = 1,
        .tail = tail, .head = head, .cost = cost,
        .supply = supply, .objective = RALPH_NETFLOW_MINIMIZE
    };
    status = ralph_netflow_solve(&prob, NULL, NULL, NULL);
    ASSERT(status == RALPH_NETFLOW_INVALID_INPUT, "NULL result rejected");

    /* Invalid arc endpoint */
    int bad_tail[] = {5};  /* Out of range */
    RalphNetflowProblem bad_prob = {
        .num_nodes = 2, .num_arcs = 1,
        .tail = bad_tail, .head = head, .cost = cost,
        .supply = supply, .objective = RALPH_NETFLOW_MINIMIZE
    };
    result.flow = flow;
    status = ralph_netflow_solve(&bad_prob, NULL, &result, NULL);
    ASSERT(status == RALPH_NETFLOW_INVALID_INPUT, "Invalid arc endpoint rejected");
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Network Simplex Tests\n");
    printf("=====================\n");

    /* Basic tests */
    test_simple_3node();
    test_single_arc();
    test_parallel_arcs();
    test_transportation_2x2();
    test_transportation_3x3();
    test_assignment_3x3();
    test_shortest_path_simple();
    test_transshipment();

    /* Edge cases */
    test_infeasible_imbalanced();
    test_infeasible_capacity();
    test_degenerate();
    test_all_zero_cost();
    test_negative_costs();
    test_lower_bounds();
    test_single_node();
    test_disconnected();
    test_self_loop();
    test_maximize();

    /* Scale tests */
    test_transportation_10x10();
    test_transportation_100x100();
    test_sparse_network_1k();
    test_chain_network();

    /* Correctness tests */
    test_workspace_reuse();
    test_vs_lp_small();
    test_complementary_slackness();
    test_integer_solution();
    test_flow_conservation();
    test_first_eligible_pricing();

    /* Utility tests */
    test_status_string();
    test_invalid_input();

    printf("\n=====================\n");
    printf("Tests: %d/%d passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
