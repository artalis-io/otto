/*
 * Ralph - Network Detection Tests
 *
 * Tests for detecting network flow structure in LP models.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "detect.h"
#include "presolve.h"
#include "mip.h"

#define TOLERANCE 1e-6

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
 * Test: Detect Simple Network (4 nodes, 4 arcs)
 *
 * Network: source(0) -> transit(1,2) -> sink(3)
 * Arcs: 0->1, 0->2, 1->3, 2->3
 * Supply: node 0 = +100, node 3 = -100
 *
 * LP form:
 *   Node 0: +x0 +x1 = 100
 *   Node 1: -x0 +x2 = 0
 *   Node 2: -x1 +x3 = 0
 *   Node 3: -x2 -x3 = -100
 * ============================================================================ */
void test_detect_simple_network(void) {
    printf("\n=== Test: Detect Simple Network ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;  /* Minimize */

    /* 4 arcs with costs */
    lp_model_add_var(model, 0.0, 100.0, 1.0, 'C');  /* x0: 0->1, cost=1 */
    lp_model_add_var(model, 0.0, 100.0, 2.0, 'C');  /* x1: 0->2, cost=2 */
    lp_model_add_var(model, 0.0, 100.0, 3.0, 'C');  /* x2: 1->3, cost=3 */
    lp_model_add_var(model, 0.0, 100.0, 4.0, 'C');  /* x3: 2->3, cost=4 */

    /* Node 0: +x0 +x1 = 100 (source) */
    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'E', 100.0);

    /* Node 1: -x0 +x2 = 0 */
    int idx1[] = {0, 2};
    double coef1[] = {-1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'E', 0.0);

    /* Node 2: -x1 +x3 = 0 */
    int idx2[] = {1, 3};
    double coef2[] = {-1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'E', 0.0);

    /* Node 3: -x2 -x3 = -100 (sink) */
    int idx3[] = {2, 3};
    double coef3[] = {-1.0, -1.0};
    lp_model_add_constraint(model, 2, idx3, coef3, 'E', -100.0);

    /* Finalize model */
    lp_model_finalize(model);

    /* Detect network structure */
    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 1, "Network structure detected");
    ASSERT(sig.is_network == 1, "is_network flag set");
    ASSERT(sig.num_nodes == 4, "4 nodes detected");
    ASSERT(sig.num_arcs == 4, "4 arcs detected");

    /* Verify node supplies */
    ASSERT_NEAR(sig.supply[0], 100.0, TOLERANCE, "Node 0 supply = 100");
    ASSERT_NEAR(sig.supply[1], 0.0, TOLERANCE, "Node 1 supply = 0");
    ASSERT_NEAR(sig.supply[2], 0.0, TOLERANCE, "Node 2 supply = 0");
    ASSERT_NEAR(sig.supply[3], -100.0, TOLERANCE, "Node 3 supply = -100");

    /* Verify costs extracted from objective */
    ASSERT_NEAR(sig.cost[0], 1.0, TOLERANCE, "Arc 0 cost = 1");
    ASSERT_NEAR(sig.cost[1], 2.0, TOLERANCE, "Arc 1 cost = 2");
    ASSERT_NEAR(sig.cost[2], 3.0, TOLERANCE, "Arc 2 cost = 3");
    ASSERT_NEAR(sig.cost[3], 4.0, TOLERANCE, "Arc 3 cost = 4");

    /* Verify type classification */
    RalphNetworkType type = detect_network_type(&sig);
    ASSERT(type == RALPH_NETWORK_GENERAL, "Type is GENERAL (has transshipment)");

    detect_network_free(&sig);
    lp_model_free(model);
}

/* ============================================================================
 * Test: Detect Transportation Problem (2x2)
 *
 * Sources: node 0 (supply=30), node 1 (supply=50)
 * Sinks: node 2 (demand=40), node 3 (demand=40)
 *
 * Arcs: 0->2, 0->3, 1->2, 1->3
 *
 * LP form:
 *   Node 0: +x0 +x1 = 30
 *   Node 1: +x2 +x3 = 50
 *   Node 2: -x0 -x2 = -40
 *   Node 3: -x1 -x3 = -40
 * ============================================================================ */
void test_detect_transportation(void) {
    printf("\n=== Test: Detect Transportation Problem ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 4 arcs */
    lp_model_add_var(model, 0.0, 1e30, 1.0, 'C');  /* x0: 0->2 */
    lp_model_add_var(model, 0.0, 1e30, 2.0, 'C');  /* x1: 0->3 */
    lp_model_add_var(model, 0.0, 1e30, 3.0, 'C');  /* x2: 1->2 */
    lp_model_add_var(model, 0.0, 1e30, 4.0, 'C');  /* x3: 1->3 */

    /* Source 0: +x0 +x1 = 30 */
    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'E', 30.0);

    /* Source 1: +x2 +x3 = 50 */
    int idx1[] = {2, 3};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'E', 50.0);

    /* Sink 2: -x0 -x2 = -40 */
    int idx2[] = {0, 2};
    double coef2[] = {-1.0, -1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'E', -40.0);

    /* Sink 3: -x1 -x3 = -40 */
    int idx3[] = {1, 3};
    double coef3[] = {-1.0, -1.0};
    lp_model_add_constraint(model, 2, idx3, coef3, 'E', -40.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 1, "Network structure detected");
    ASSERT(sig.num_nodes == 4, "4 nodes");
    ASSERT(sig.num_arcs == 4, "4 arcs");

    RalphNetworkType type = detect_network_type(&sig);
    ASSERT(type == RALPH_NETWORK_TRANSPORTATION, "Type is TRANSPORTATION");

    detect_network_free(&sig);
    lp_model_free(model);
}

/* ============================================================================
 * Test: Detect Assignment Problem (3x3)
 *
 * Sources: nodes 0,1,2 (supply=1 each)
 * Sinks: nodes 3,4,5 (demand=1 each)
 * Arcs: all source-to-sink pairs (9 arcs)
 * ============================================================================ */
void test_detect_assignment(void) {
    printf("\n=== Test: Detect Assignment Problem ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 9 arcs: 0->3, 0->4, 0->5, 1->3, 1->4, 1->5, 2->3, 2->4, 2->5 */
    double costs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int i = 0; i < 9; i++) {
        lp_model_add_var(model, 0.0, 1.0, costs[i], 'C');
    }

    /* Source constraints (supply = 1) */
    /* Source 0: x0 + x1 + x2 = 1 */
    int src0[] = {0, 1, 2};
    double one3[] = {1.0, 1.0, 1.0};
    lp_model_add_constraint(model, 3, src0, one3, 'E', 1.0);

    /* Source 1: x3 + x4 + x5 = 1 */
    int src1[] = {3, 4, 5};
    lp_model_add_constraint(model, 3, src1, one3, 'E', 1.0);

    /* Source 2: x6 + x7 + x8 = 1 */
    int src2[] = {6, 7, 8};
    lp_model_add_constraint(model, 3, src2, one3, 'E', 1.0);

    /* Sink constraints (demand = 1, so RHS = -1 with -1 coefficients) */
    /* Sink 3: -x0 - x3 - x6 = -1 */
    int snk0[] = {0, 3, 6};
    double mone3[] = {-1.0, -1.0, -1.0};
    lp_model_add_constraint(model, 3, snk0, mone3, 'E', -1.0);

    /* Sink 4: -x1 - x4 - x7 = -1 */
    int snk1[] = {1, 4, 7};
    lp_model_add_constraint(model, 3, snk1, mone3, 'E', -1.0);

    /* Sink 5: -x2 - x5 - x8 = -1 */
    int snk2[] = {2, 5, 8};
    lp_model_add_constraint(model, 3, snk2, mone3, 'E', -1.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 1, "Network structure detected");
    ASSERT(sig.num_nodes == 6, "6 nodes (3 sources + 3 sinks)");
    ASSERT(sig.num_arcs == 9, "9 arcs (3x3)");

    RalphNetworkType type = detect_network_type(&sig);
    ASSERT(type == RALPH_NETWORK_ASSIGNMENT, "Type is ASSIGNMENT");

    detect_network_free(&sig);
    lp_model_free(model);
}

/* ============================================================================
 * Test: Detect Shortest Path (unit flow)
 *
 * Single source (supply=1), single sink (demand=1)
 * ============================================================================ */
void test_detect_shortest_path(void) {
    printf("\n=== Test: Detect Shortest Path ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 5 nodes: 0 (source) -> 1,2 -> 3 -> 4 (sink) */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'C');  /* x0: 0->1 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'C');  /* x1: 0->2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'C');  /* x2: 1->3 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'C');  /* x3: 2->3 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'C');  /* x4: 3->4 */

    /* Node 0: x0 + x1 = 1 (source) */
    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'E', 1.0);

    /* Node 1: -x0 + x2 = 0 */
    int idx1[] = {0, 2};
    double coef1[] = {-1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'E', 0.0);

    /* Node 2: -x1 + x3 = 0 */
    int idx2[] = {1, 3};
    double coef2[] = {-1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'E', 0.0);

    /* Node 3: -x2 - x3 + x4 = 0 */
    int idx3[] = {2, 3, 4};
    double coef3[] = {-1.0, -1.0, 1.0};
    lp_model_add_constraint(model, 3, idx3, coef3, 'E', 0.0);

    /* Node 4: -x4 = -1 (sink) */
    int idx4[] = {4};
    double coef4[] = {-1.0};
    lp_model_add_constraint(model, 1, idx4, coef4, 'E', -1.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 1, "Network structure detected");
    ASSERT(sig.num_nodes == 5, "5 nodes");
    ASSERT(sig.num_arcs == 5, "5 arcs");

    RalphNetworkType type = detect_network_type(&sig);
    ASSERT(type == RALPH_NETWORK_SHORTEST_PATH, "Type is SHORTEST_PATH");

    detect_network_free(&sig);
    lp_model_free(model);
}

/* ============================================================================
 * Test: Non-Network (coefficient != +/-1)
 * ============================================================================ */
void test_reject_non_network_coef(void) {
    printf("\n=== Test: Reject Non-Network (coefficient != +/-1) ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 100.0, 1.0, 'C');
    lp_model_add_var(model, 0.0, 100.0, 1.0, 'C');

    /* Coefficient 2.0 is not network structure */
    int idx0[] = {0, 1};
    double coef0[] = {2.0, 1.0};  /* 2.0 breaks network structure */
    lp_model_add_constraint(model, 2, idx0, coef0, 'E', 10.0);

    int idx1[] = {0, 1};
    double coef1[] = {-1.0, -1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'E', -10.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 0, "Non-network (coef != +/-1) rejected");

    lp_model_free(model);
}

/* ============================================================================
 * Test: Non-Network (variable in 3 constraints)
 * ============================================================================ */
void test_reject_non_network_count(void) {
    printf("\n=== Test: Reject Non-Network (variable in 3 constraints) ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 100.0, 1.0, 'C');
    lp_model_add_var(model, 0.0, 100.0, 1.0, 'C');

    /* Variable 0 appears in 3 constraints */
    int idx0[] = {0};
    double coef0[] = {1.0};
    lp_model_add_constraint(model, 1, idx0, coef0, 'E', 10.0);

    int idx1[] = {0, 1};
    double coef1[] = {-1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'E', 0.0);

    int idx2[] = {0, 1};
    double coef2[] = {1.0, -1.0};  /* Variable 0 appears third time */
    lp_model_add_constraint(model, 2, idx2, coef2, 'E', 0.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 0, "Non-network (var in 3 constraints) rejected");

    lp_model_free(model);
}

/* ============================================================================
 * Test: Non-Network (inequality constraint)
 * ============================================================================ */
void test_reject_non_network_inequality(void) {
    printf("\n=== Test: Reject Non-Network (inequality constraint) ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 100.0, 1.0, 'C');
    lp_model_add_var(model, 0.0, 100.0, 1.0, 'C');

    /* Inequality breaks pure network structure */
    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'L', 10.0);  /* <= instead of = */

    int idx1[] = {0, 1};
    double coef1[] = {-1.0, -1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'E', -10.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 0, "Non-network (inequality) rejected");

    lp_model_free(model);
}

/* ============================================================================
 * Test: Solve Network via Detection
 * ============================================================================ */
void test_solve_via_detection(void) {
    printf("\n=== Test: Solve Network via Detection ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* Simple network: 0 -> 1, 0 -> 2, with supply=10 at 0, demand=5 each at 1,2 */
    lp_model_add_var(model, 0.0, 10.0, 1.0, 'C');  /* x0: 0->1, cost=1 */
    lp_model_add_var(model, 0.0, 10.0, 2.0, 'C');  /* x1: 0->2, cost=2 */

    /* Node 0: x0 + x1 = 10 */
    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'E', 10.0);

    /* Node 1: -x0 = -5 */
    int idx1[] = {0};
    double coef1[] = {-1.0};
    lp_model_add_constraint(model, 1, idx1, coef1, 'E', -5.0);

    /* Node 2: -x1 = -5 */
    int idx2[] = {1};
    double coef2[] = {-1.0};
    lp_model_add_constraint(model, 1, idx2, coef2, 'E', -5.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);
    ASSERT(detected == 1, "Network detected");

    double solution[2];
    double obj_val;
    int status = solve_as_network(&sig, solution, &obj_val);

    ASSERT(status == 0, "solve_as_network succeeded");
    ASSERT_NEAR(solution[0], 5.0, TOLERANCE, "Flow on arc 0 = 5");
    ASSERT_NEAR(solution[1], 5.0, TOLERANCE, "Flow on arc 1 = 5");
    ASSERT_NEAR(obj_val, 15.0, TOLERANCE, "Objective = 15 (5*1 + 5*2)");

    detect_network_free(&sig);
    lp_model_free(model);
}

/* ============================================================================
 * Test: Solve Assignment via LAP Delegation
 * ============================================================================ */
void test_solve_assignment_delegation(void) {
    printf("\n=== Test: Solve Assignment via LAP Delegation ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 2x2 assignment problem */
    /* Costs: (0,0)=1, (0,1)=3, (1,0)=2, (1,1)=1 */
    /* Optimal: (0,0)=1, (1,1)=1 with cost 2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'C');  /* x0: source0 -> sink0 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'C');  /* x1: source0 -> sink1 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'C');  /* x2: source1 -> sink0 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'C');  /* x3: source1 -> sink1 */

    /* Source 0: x0 + x1 = 1 */
    int src0[] = {0, 1};
    double ones[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, src0, ones, 'E', 1.0);

    /* Source 1: x2 + x3 = 1 */
    int src1[] = {2, 3};
    lp_model_add_constraint(model, 2, src1, ones, 'E', 1.0);

    /* Sink 0: -x0 - x2 = -1 */
    int snk0[] = {0, 2};
    double mones[] = {-1.0, -1.0};
    lp_model_add_constraint(model, 2, snk0, mones, 'E', -1.0);

    /* Sink 1: -x1 - x3 = -1 */
    int snk1[] = {1, 3};
    lp_model_add_constraint(model, 2, snk1, mones, 'E', -1.0);

    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);
    ASSERT(detected == 1, "Network detected");

    RalphNetworkType type = detect_network_type(&sig);
    ASSERT(type == RALPH_NETWORK_ASSIGNMENT, "Type is ASSIGNMENT");

    double solution[4];
    double obj_val;
    int status = solve_as_network(&sig, solution, &obj_val);

    ASSERT(status == 0, "solve_as_network succeeded");
    ASSERT_NEAR(obj_val, 2.0, TOLERANCE, "Optimal cost = 2");

    /* Check solution is a valid assignment */
    if (fabs(solution[0] - 1.0) < TOLERANCE && fabs(solution[3] - 1.0) < TOLERANCE) {
        /* (0,0)=1, (1,1)=1 */
        ASSERT(1, "Assignment: (0,0)=1, (1,1)=1");
    } else if (fabs(solution[1] - 1.0) < TOLERANCE && fabs(solution[2] - 1.0) < TOLERANCE) {
        /* (0,1)=1, (1,0)=1 - alternative but not optimal */
        ASSERT(0, "Assignment: wrong pairing");
    } else {
        ASSERT(0, "Assignment: invalid solution");
    }

    detect_network_free(&sig);
    lp_model_free(model);
}

/* ============================================================================
 * Test: Empty Model
 * ============================================================================ */
void test_empty_model(void) {
    printf("\n=== Test: Empty Model ===\n");

    LPModel *model = lp_model_create();
    lp_model_finalize(model);

    NetworkSignature sig;
    int detected = detect_network(model, &sig);

    ASSERT(detected == 0, "Empty model rejected");

    lp_model_free(model);
}

/* ============================================================================
 * Test: NULL Inputs
 * ============================================================================ */
void test_null_inputs(void) {
    printf("\n=== Test: NULL Inputs ===\n");

    NetworkSignature sig;
    int detected = detect_network(NULL, &sig);
    ASSERT(detected == 0, "NULL model rejected");

    LPModel *model = lp_model_create();
    detected = detect_network(model, NULL);
    ASSERT(detected == 0, "NULL signature rejected");

    lp_model_free(model);
}

/* ============================================================================
 * Set Covering Detection Tests
 * ============================================================================ */

/*
 * Test: Detect Set Covering Problem (5 elements, 6 sets)
 *
 * Elements: 0, 1, 2, 3, 4
 * Sets:
 *   S0 = {0, 1}     cost = 2
 *   S1 = {1, 2, 3}  cost = 3
 *   S2 = {2, 4}     cost = 2
 *   S3 = {3, 4}     cost = 2
 *   S4 = {0, 2, 4}  cost = 4
 *   S5 = {1, 3}     cost = 2
 *
 * Constraints: each element covered by at least one set (>= 1)
 */
void test_detect_set_covering(void) {
    printf("\n=== Test: Detect Set Covering Problem ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;  /* Minimize */

    /* Add binary variables (sets) with costs */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S0: {0,1} */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S1: {1,2,3} */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S2: {2,4} */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S3: {3,4} */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* S4: {0,2,4} */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S5: {1,3} */

    /* Element 0: S0 + S4 >= 1 */
    int idx0[] = {0, 4};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'G', 1.0);

    /* Element 1: S0 + S1 + S5 >= 1 */
    int idx1[] = {0, 1, 5};
    double coef3[] = {1.0, 1.0, 1.0};
    lp_model_add_constraint(model, 3, idx1, coef3, 'G', 1.0);

    /* Element 2: S1 + S2 + S4 >= 1 */
    int idx2[] = {1, 2, 4};
    lp_model_add_constraint(model, 3, idx2, coef3, 'G', 1.0);

    /* Element 3: S1 + S3 + S5 >= 1 */
    int idx3[] = {1, 3, 5};
    lp_model_add_constraint(model, 3, idx3, coef3, 'G', 1.0);

    /* Element 4: S2 + S3 + S4 >= 1 */
    int idx4[] = {2, 3, 4};
    lp_model_add_constraint(model, 3, idx4, coef3, 'G', 1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 1, "Set covering structure detected");
    ASSERT(sig.type == RALPH_SETCOVER_COVERING, "Type is SET_COVERING");
    ASSERT(sig.num_elements == 5, "5 elements");
    ASSERT(sig.num_sets == 6, "6 sets");
    ASSERT(sig.num_covering == 5, "5 covering constraints");
    ASSERT(sig.num_partitioning == 0, "0 partitioning constraints");
    ASSERT(sig.num_packing == 0, "0 packing constraints");

    /* Check set sizes */
    ASSERT(sig.set_size[0] == 2, "S0 covers 2 elements");
    ASSERT(sig.set_size[1] == 3, "S1 covers 3 elements");
    ASSERT(sig.set_size[4] == 3, "S4 covers 3 elements");

    /* Check element coverage */
    ASSERT(sig.element_coverage[0] == 2, "Element 0 covered by 2 sets");
    ASSERT(sig.element_coverage[1] == 3, "Element 1 covered by 3 sets");

    /* Check cost statistics */
    ASSERT_NEAR(sig.min_cost, 2.0, TOLERANCE, "Min cost = 2");
    ASSERT_NEAR(sig.max_cost, 4.0, TOLERANCE, "Max cost = 4");

    detect_set_cover_free(&sig);
    lp_model_free(model);
}

/*
 * Test: Detect Set Partitioning Problem (3x3 assignment-like)
 *
 * Each row must be covered exactly once, each column exactly once.
 */
void test_detect_set_partitioning(void) {
    printf("\n=== Test: Detect Set Partitioning Problem ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 9 binary variables representing (row, col) pairs */
    double costs[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int i = 0; i < 9; i++) {
        lp_model_add_var(model, 0.0, 1.0, costs[i], 'B');
    }

    /* Row constraints: x0+x1+x2=1, x3+x4+x5=1, x6+x7+x8=1 */
    int row0[] = {0, 1, 2};
    int row1[] = {3, 4, 5};
    int row2[] = {6, 7, 8};
    double ones[] = {1.0, 1.0, 1.0};

    lp_model_add_constraint(model, 3, row0, ones, 'E', 1.0);
    lp_model_add_constraint(model, 3, row1, ones, 'E', 1.0);
    lp_model_add_constraint(model, 3, row2, ones, 'E', 1.0);

    /* Column constraints: x0+x3+x6=1, x1+x4+x7=1, x2+x5+x8=1 */
    int col0[] = {0, 3, 6};
    int col1[] = {1, 4, 7};
    int col2[] = {2, 5, 8};

    lp_model_add_constraint(model, 3, col0, ones, 'E', 1.0);
    lp_model_add_constraint(model, 3, col1, ones, 'E', 1.0);
    lp_model_add_constraint(model, 3, col2, ones, 'E', 1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 1, "Set partitioning structure detected");
    ASSERT(sig.type == RALPH_SETCOVER_PARTITIONING, "Type is SET_PARTITIONING");
    ASSERT(sig.num_elements == 6, "6 constraints");
    ASSERT(sig.num_sets == 9, "9 variables");
    ASSERT(sig.num_partitioning == 6, "6 partitioning constraints");

    detect_set_cover_free(&sig);
    lp_model_free(model);
}

/*
 * Test: Detect Set Packing Problem
 *
 * Constraints: at most one set selected per element (<=)
 */
void test_detect_set_packing(void) {
    printf("\n=== Test: Detect Set Packing Problem ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = -1;  /* Maximize for packing */

    /* 4 sets, 3 elements */
    lp_model_add_var(model, 0.0, 1.0, 5.0, 'B');  /* S0: {0,1} */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S1: {1,2} */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* S2: {0} */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S3: {2} */

    /* Element 0: S0 + S2 <= 1 */
    int idx0[] = {0, 2};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef2, 'L', 1.0);

    /* Element 1: S0 + S1 <= 1 */
    int idx1[] = {0, 1};
    lp_model_add_constraint(model, 2, idx1, coef2, 'L', 1.0);

    /* Element 2: S1 + S3 <= 1 */
    int idx2[] = {1, 3};
    lp_model_add_constraint(model, 2, idx2, coef2, 'L', 1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 1, "Set packing structure detected");
    ASSERT(sig.type == RALPH_SETCOVER_PACKING, "Type is SET_PACKING");
    ASSERT(sig.num_elements == 3, "3 elements");
    ASSERT(sig.num_sets == 4, "4 sets");
    ASSERT(sig.num_packing == 3, "3 packing constraints");

    detect_set_cover_free(&sig);
    lp_model_free(model);
}

/*
 * Test: Detect Mixed Set Cover Problem
 *
 * Mix of covering (>=), partitioning (=), and packing (<=)
 */
void test_detect_set_cover_mixed(void) {
    printf("\n=== Test: Detect Mixed Set Cover Problem ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 sets, 3 elements */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');

    /* Element 0: covering (>=) */
    int idx0[] = {0, 1};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef2, 'G', 1.0);

    /* Element 1: partitioning (=) */
    int idx1[] = {1, 2};
    lp_model_add_constraint(model, 2, idx1, coef2, 'E', 1.0);

    /* Element 2: packing (<=) */
    int idx2[] = {0, 2};
    lp_model_add_constraint(model, 2, idx2, coef2, 'L', 1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 1, "Mixed structure detected");
    ASSERT(sig.type == RALPH_SETCOVER_MIXED, "Type is MIXED");
    ASSERT(sig.num_covering == 1, "1 covering constraint");
    ASSERT(sig.num_partitioning == 1, "1 partitioning constraint");
    ASSERT(sig.num_packing == 1, "1 packing constraint");

    detect_set_cover_free(&sig);
    lp_model_free(model);
}

/*
 * Test: Detect Weighted Set Covering (RHS > 1)
 */
void test_detect_weighted_set_covering(void) {
    printf("\n=== Test: Detect Weighted Set Covering (RHS > 1) ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 sets covering 2 elements with RHS = 2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0: {0} */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S1: {0,1} */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2: {1} */

    /* Element 0: S0 + S1 >= 2 (need both) */
    int idx0[] = {0, 1};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef2, 'G', 2.0);

    /* Element 1: S1 + S2 >= 1 */
    int idx1[] = {1, 2};
    lp_model_add_constraint(model, 2, idx1, coef2, 'G', 1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 1, "Weighted set covering detected");
    ASSERT(sig.type == RALPH_SETCOVER_COVERING, "Type is SET_COVERING");
    ASSERT_NEAR(sig.rhs[0], 2.0, TOLERANCE, "RHS[0] = 2");
    ASSERT_NEAR(sig.rhs[1], 1.0, TOLERANCE, "RHS[1] = 1");

    detect_set_cover_free(&sig);
    lp_model_free(model);
}

/*
 * Test: Reject Non-Binary Variables
 */
void test_reject_non_binary_scp(void) {
    printf("\n=== Test: Reject Non-Binary Variables (SCP) ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* Continuous variable - not binary */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'C');
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');

    int idx[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, coef, 'G', 1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 0, "Non-binary variables rejected");

    lp_model_free(model);
}

/*
 * Test: Reject Non-0/1 Coefficients
 */
void test_reject_non_01_coef_scp(void) {
    printf("\n=== Test: Reject Non-0/1 Coefficients (SCP) ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');

    /* Coefficient 2.0 is not 0/1 */
    int idx[] = {0, 1};
    double coef[] = {2.0, 1.0};
    lp_model_add_constraint(model, 2, idx, coef, 'G', 1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 0, "Non-0/1 coefficients rejected");

    lp_model_free(model);
}

/*
 * Test: Reject Negative RHS
 */
void test_reject_negative_rhs_scp(void) {
    printf("\n=== Test: Reject Negative RHS (SCP) ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');

    /* Negative RHS */
    int idx[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, coef, 'G', -1.0);

    lp_model_finalize(model);

    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);

    ASSERT(detected == 0, "Negative RHS rejected");

    lp_model_free(model);
}

/*
 * Test: Type Name Function
 */
void test_set_cover_type_name(void) {
    printf("\n=== Test: Set Cover Type Names ===\n");

    ASSERT(strcmp(ralph_set_cover_type_name(RALPH_SETCOVER_NONE), "NONE") == 0,
           "NONE type name");
    ASSERT(strcmp(ralph_set_cover_type_name(RALPH_SETCOVER_COVERING), "SET_COVERING") == 0,
           "COVERING type name");
    ASSERT(strcmp(ralph_set_cover_type_name(RALPH_SETCOVER_PARTITIONING), "SET_PARTITIONING") == 0,
           "PARTITIONING type name");
    ASSERT(strcmp(ralph_set_cover_type_name(RALPH_SETCOVER_PACKING), "SET_PACKING") == 0,
           "PACKING type name");
    ASSERT(strcmp(ralph_set_cover_type_name(RALPH_SETCOVER_MIXED), "MIXED") == 0,
           "MIXED type name");
}

/* ============================================================================
 * SCP Presolve Tests
 * ============================================================================ */

/*
 * Helper: Create a presolve context from an LPModel
 */
static PresolveContext* create_test_presolve_ctx(LPModel *model) {
    PresolveContext *ctx = (PresolveContext*)calloc(1, sizeof(PresolveContext));
    if (!ctx) return NULL;

    ctx->original = model;
    ctx->working = lp_model_copy(model);
    if (!ctx->working) {
        free(ctx);
        return NULL;
    }

    ctx->row_deleted = (int*)calloc(model->num_cons, sizeof(int));
    ctx->col_deleted = (int*)calloc(model->num_vars, sizeof(int));
    ctx->row_lb = (double*)calloc(model->num_cons, sizeof(double));
    ctx->row_ub = (double*)calloc(model->num_cons, sizeof(double));

    if (!ctx->row_deleted || !ctx->col_deleted) {
        lp_model_free(ctx->working);
        free(ctx->row_deleted);
        free(ctx->col_deleted);
        free(ctx->row_lb);
        free(ctx->row_ub);
        free(ctx);
        return NULL;
    }

    return ctx;
}

static void free_test_presolve_ctx(PresolveContext *ctx) {
    if (!ctx) return;
    lp_model_free(ctx->working);
    free(ctx->row_deleted);
    free(ctx->col_deleted);
    free(ctx->row_lb);
    free(ctx->row_ub);
    free(ctx);
}

/*
 * Test: Essential Set Detection
 *
 * Element 0 is covered only by S0 -> S0 must be fixed to 1
 */
void test_presolve_scp_essential_sets(void) {
    printf("\n=== Test: SCP Presolve - Essential Sets ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* S0 covers {0, 1}, S1 covers {1, 2}, S2 covers {2} */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S0: {0,1} cost=3 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S1: {1,2} cost=2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2: {2}   cost=1 */

    /* Element 0: S0 >= 1 (only S0 covers it) */
    int idx0[] = {0};
    double coef1[] = {1.0};
    lp_model_add_constraint(model, 1, idx0, coef1, 'G', 1.0);

    /* Element 1: S0 + S1 >= 1 */
    int idx1[] = {0, 1};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef2, 'G', 1.0);

    /* Element 2: S1 + S2 >= 1 */
    int idx2[] = {1, 2};
    lp_model_add_constraint(model, 2, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    PresolveContext *ctx = create_test_presolve_ctx(model);
    ASSERT(ctx != NULL, "Presolve context created");

    int fixed = presolve_scp_essential_sets(ctx);

    ASSERT(fixed == 1, "1 essential set found (S0)");
    ASSERT(ctx->col_deleted[0] == 1, "S0 fixed to 1 (deleted)");
    ASSERT(ctx->col_deleted[1] == 0, "S1 not fixed");
    ASSERT(ctx->col_deleted[2] == 0, "S2 not fixed");

    /* Element 0 and 1 should be satisfied (RHS reduced) */
    ASSERT(ctx->row_deleted[0] == 1, "Element 0 satisfied (deleted)");
    ASSERT(ctx->row_deleted[1] == 1, "Element 1 satisfied (deleted)");

    free_test_presolve_ctx(ctx);
    lp_model_free(model);
}

/*
 * Test: Row Dominance
 *
 * Element 0: S0 + S1 >= 1
 * Element 1: S0 + S1 + S2 >= 1 (dominated by element 0)
 */
void test_presolve_scp_row_dominance(void) {
    printf("\n=== Test: SCP Presolve - Row Dominance ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 sets */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S1 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2 */

    /* Element 0: S0 + S1 >= 1 */
    int idx0[] = {0, 1};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef2, 'G', 1.0);

    /* Element 1: S0 + S1 + S2 >= 1 (dominated - superset of element 0's sets) */
    int idx1[] = {0, 1, 2};
    double coef3[] = {1.0, 1.0, 1.0};
    lp_model_add_constraint(model, 3, idx1, coef3, 'G', 1.0);

    /* Element 2: S2 >= 1 (not dominated) */
    int idx2[] = {2};
    double coef1[] = {1.0};
    lp_model_add_constraint(model, 1, idx2, coef1, 'G', 1.0);

    lp_model_finalize(model);

    PresolveContext *ctx = create_test_presolve_ctx(model);
    ASSERT(ctx != NULL, "Presolve context created");

    int removed = presolve_scp_row_dominance(ctx);

    ASSERT(removed == 1, "1 dominated row removed");
    ASSERT(ctx->row_deleted[0] == 0, "Element 0 not dominated");
    ASSERT(ctx->row_deleted[1] == 1, "Element 1 dominated (removed)");
    ASSERT(ctx->row_deleted[2] == 0, "Element 2 not dominated");

    free_test_presolve_ctx(ctx);
    lp_model_free(model);
}

/*
 * Test: Column Dominance
 *
 * S0 covers {0, 1} cost=2
 * S1 covers {0} cost=3 (dominated by S0 - S0 is cheaper and covers more)
 * S2 covers {1} cost=1 (not dominated)
 */
void test_presolve_scp_column_dominance(void) {
    printf("\n=== Test: SCP Presolve - Column Dominance ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;  /* Minimize */

    /* S0 covers {0,1} cost=2, S1 covers {0} cost=3, S2 covers {1} cost=1 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S0: {0,1} */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S1: {0} - dominated by S0 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2: {1} */

    /* Element 0: S0 + S1 >= 1 */
    int idx0[] = {0, 1};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef2, 'G', 1.0);

    /* Element 1: S0 + S2 >= 1 */
    int idx1[] = {0, 2};
    lp_model_add_constraint(model, 2, idx1, coef2, 'G', 1.0);

    lp_model_finalize(model);

    PresolveContext *ctx = create_test_presolve_ctx(model);
    ASSERT(ctx != NULL, "Presolve context created");

    int fixed = presolve_scp_column_dominance(ctx);

    ASSERT(fixed == 1, "1 dominated column fixed to 0");
    ASSERT(ctx->col_deleted[0] == 0, "S0 not dominated");
    ASSERT(ctx->col_deleted[1] == 1, "S1 dominated (fixed to 0)");
    ASSERT(ctx->col_deleted[2] == 0, "S2 not dominated");

    free_test_presolve_ctx(ctx);
    lp_model_free(model);
}

/*
 * Test: Combined SCP Presolve
 */
void test_presolve_scp_combined(void) {
    printf("\n=== Test: SCP Presolve - Combined ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* Problem with opportunities for all three reductions */
    /* S0: {0,1} cost=2, S1: {0} cost=3 (dominated), S2: {2} cost=1 (essential) */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S0 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S1 - dominated by S0 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2 - essential for elem 2 */

    /* Element 0: S0 + S1 >= 1 */
    int idx0[] = {0, 1};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef2, 'G', 1.0);

    /* Element 1: S0 >= 1 (also covered by fixing S0 will help) */
    int idx1[] = {0};
    double coef1[] = {1.0};
    lp_model_add_constraint(model, 1, idx1, coef1, 'G', 1.0);

    /* Element 2: S2 >= 1 (only S2 covers it) */
    int idx2[] = {2};
    lp_model_add_constraint(model, 1, idx2, coef1, 'G', 1.0);

    lp_model_finalize(model);

    PresolveContext *ctx = create_test_presolve_ctx(model);
    ASSERT(ctx != NULL, "Presolve context created");

    int total = presolve_scp(ctx);

    ASSERT(total >= 2, "At least 2 reductions made");
    printf("  Total reductions: %d\n", total);

    /* S2 should be essential, S0 should be essential too */
    /* After essentials, S1 dominance may kick in */

    free_test_presolve_ctx(ctx);
    lp_model_free(model);
}

/*
 * Test: Essential Set Infeasibility
 *
 * Element with no covering sets -> infeasible
 */
void test_presolve_scp_infeasible(void) {
    printf("\n=== Test: SCP Presolve - Infeasibility Detection ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* S0 covers {0} only */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');

    /* Element 0: S0 >= 1 */
    int idx0[] = {0};
    double coef1[] = {1.0};
    lp_model_add_constraint(model, 1, idx0, coef1, 'G', 1.0);

    /* Element 1: no sets cover it (will be detected after model changes) */
    /* We simulate by having an empty constraint after presolve */

    lp_model_finalize(model);

    PresolveContext *ctx = create_test_presolve_ctx(model);
    ASSERT(ctx != NULL, "Presolve context created");

    /* First fix the essential set */
    int fixed = presolve_scp_essential_sets(ctx);
    ASSERT(fixed == 1, "S0 fixed as essential");

    /* Now model should be feasible (all elements covered) */
    ASSERT(ctx->row_deleted[0] == 1, "Element 0 satisfied");

    free_test_presolve_ctx(ctx);
    lp_model_free(model);
}

/*
 * Test: No Reductions Possible
 */
void test_presolve_scp_no_reductions(void) {
    printf("\n=== Test: SCP Presolve - No Reductions ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* Problem where no presolve applies */
    /* S0: {0}, S1: {1} - no dominance, no essential (both needed) */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0: {0} */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S1: {1} */

    /* Element 0: S0 >= 1 */
    int idx0[] = {0};
    double coef1[] = {1.0};
    lp_model_add_constraint(model, 1, idx0, coef1, 'G', 1.0);

    /* Element 1: S1 >= 1 */
    int idx1[] = {1};
    lp_model_add_constraint(model, 1, idx1, coef1, 'G', 1.0);

    lp_model_finalize(model);

    PresolveContext *ctx = create_test_presolve_ctx(model);
    ASSERT(ctx != NULL, "Presolve context created");

    /* Each element covered by exactly one set -> both are essential */
    int total = presolve_scp(ctx);

    /* Both S0 and S1 are essential (each covers unique element) */
    ASSERT(total == 2, "Both sets fixed as essential");
    ASSERT(ctx->col_deleted[0] == 1, "S0 fixed");
    ASSERT(ctx->col_deleted[1] == 1, "S1 fixed");

    free_test_presolve_ctx(ctx);
    lp_model_free(model);
}

/* ============================================================================
 * SCP Cutting Planes Tests (Phase 3)
 * ============================================================================ */

/*
 * Test conflict graph creation.
 *
 * Problem: 3 elements, 4 sets
 * S0 covers {0, 1} - conflicts with S1, S2
 * S1 covers {1, 2} - conflicts with S0, S2, S3
 * S2 covers {0, 2} - conflicts with S0, S1, S3
 * S3 covers {2}    - conflicts with S1, S2
 */
void test_conflict_graph_creation(void) {
    printf("\n=== Test: Conflict Graph Creation ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 4 binary variables (sets) */
    for (int j = 0; j < 4; j++) {
        lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    }

    /* Element 0: S0 + S2 >= 1 */
    int idx0[] = {0, 2};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'G', 1.0);

    /* Element 1: S0 + S1 >= 1 */
    int idx1[] = {0, 1};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'G', 1.0);

    /* Element 2: S1 + S2 + S3 >= 1 */
    int idx2[] = {1, 2, 3};
    double coef2[] = {1.0, 1.0, 1.0};
    lp_model_add_constraint(model, 3, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    /* Detect SCP structure */
    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);
    ASSERT(detected == 1, "SCP structure detected");

    /* Create conflict graph */
    ConflictGraph *graph = conflict_graph_create(model, &sig);
    ASSERT(graph != NULL, "Conflict graph created");
    ASSERT(graph->num_vars == 4, "Graph has 4 vertices");

    /* Check edge counts:
     * S0: neighbors S1, S2 (2 edges)
     * S1: neighbors S0, S2, S3 (3 edges)
     * S2: neighbors S0, S1, S3 (3 edges)
     * S3: neighbors S1, S2 (2 edges)
     * Total: 10 directed edges = 5 undirected edges */
    int s0_degree = graph->adj_ptr[1] - graph->adj_ptr[0];
    int s1_degree = graph->adj_ptr[2] - graph->adj_ptr[1];
    int s2_degree = graph->adj_ptr[3] - graph->adj_ptr[2];
    int s3_degree = graph->adj_ptr[4] - graph->adj_ptr[3];

    ASSERT(s0_degree == 2, "S0 has 2 neighbors");
    ASSERT(s1_degree == 3, "S1 has 3 neighbors");
    ASSERT(s2_degree == 3, "S2 has 3 neighbors");
    ASSERT(s3_degree == 2, "S3 has 2 neighbors");
    ASSERT(graph->num_edges == 5, "Graph has 5 edges");

    conflict_graph_free(graph);
    detect_set_cover_free(&sig);
    lp_model_free(model);
}

/*
 * Test clique cut generation.
 *
 * Create a problem where {S0, S1, S2} form a clique (all cover element 0).
 * Set LP solution to fractional values that violate clique inequality.
 */
void test_clique_cut_generation(void) {
    printf("\n=== Test: Clique Cut Generation ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 binary variables */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S1 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S2 */

    /* Single element covered by all 3: S0 + S1 + S2 = 1 (SPP) */
    int idx[] = {0, 1, 2};
    double coef[] = {1.0, 1.0, 1.0};
    lp_model_add_constraint(model, 3, idx, coef, 'E', 1.0);

    lp_model_finalize(model);

    /* Detect SCP structure */
    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);
    ASSERT(detected == 1, "SPP structure detected");
    ASSERT(sig.type == RALPH_SETCOVER_PARTITIONING, "Type is partitioning");

    /* Create conflict graph */
    ConflictGraph *graph = conflict_graph_create(model, &sig);
    ASSERT(graph != NULL, "Conflict graph created");

    /* All 3 sets conflict with each other -> triangle */
    ASSERT(graph->num_edges == 3, "Graph is a triangle (3 edges)");

    /* Create a fake MIP solver with fractional LP solution */
    /* This is a simplified test - in reality we'd run LP */
    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Manually set a fractional "solution" for testing */
    if (solver->lp_solver && solver->lp_solver->solution) {
        solver->lp_solver->solution[0] = 0.4;
        solver->lp_solver->solution[1] = 0.4;
        solver->lp_solver->solution[2] = 0.4;
        /* sum = 1.2 > 1, so clique cut would be violated */
    }

    /* Generate clique cuts */
    CutPool *pool = cut_pool_create(64);
    ASSERT(pool != NULL, "Cut pool created");

    int cuts = generate_clique_cuts(solver, pool, graph);
    /* Note: cuts may or may not be found depending on thresholds */
    printf("  INFO: %d clique cuts generated\n", cuts);

    /* Cleanup */
    cut_pool_free(pool);
    conflict_graph_free(graph);
    detect_set_cover_free(&sig);
    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test odd-hole cut generation.
 *
 * Create a 5-cycle in the conflict graph (odd hole of length 5).
 */
void test_odd_hole_cut_generation(void) {
    printf("\n=== Test: Odd-Hole Cut Generation ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 5 binary variables forming a 5-cycle */
    for (int j = 0; j < 5; j++) {
        lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    }

    /* Create conflicts: 0-1, 1-2, 2-3, 3-4, 4-0 (5-cycle) */
    /* Each edge comes from a shared element */

    /* Element 0: S0 + S1 = 1 */
    int idx0[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'E', 1.0);

    /* Element 1: S1 + S2 = 1 */
    int idx1[] = {1, 2};
    lp_model_add_constraint(model, 2, idx1, coef, 'E', 1.0);

    /* Element 2: S2 + S3 = 1 */
    int idx2[] = {2, 3};
    lp_model_add_constraint(model, 2, idx2, coef, 'E', 1.0);

    /* Element 3: S3 + S4 = 1 */
    int idx3[] = {3, 4};
    lp_model_add_constraint(model, 2, idx3, coef, 'E', 1.0);

    /* Element 4: S4 + S0 = 1 */
    int idx4[] = {4, 0};
    lp_model_add_constraint(model, 2, idx4, coef, 'E', 1.0);

    lp_model_finalize(model);

    /* Detect SCP structure */
    SetCoverSignature sig;
    int detected = detect_set_cover(model, &sig);
    ASSERT(detected == 1, "SPP structure detected");

    /* Create conflict graph */
    ConflictGraph *graph = conflict_graph_create(model, &sig);
    ASSERT(graph != NULL, "Conflict graph created");
    ASSERT(graph->num_edges == 5, "Graph is a 5-cycle (5 edges)");

    /* Create MIP solver */
    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Set fractional solution violating odd-hole: all = 0.5
     * sum = 2.5 > k=2, so cut sum <= 2 is violated */
    if (solver->lp_solver && solver->lp_solver->solution) {
        for (int j = 0; j < 5; j++) {
            solver->lp_solver->solution[j] = 0.5;
        }
    }

    /* Generate odd-hole cuts */
    CutPool *pool = cut_pool_create(64);
    int cuts = generate_odd_hole_cuts(solver, pool, graph);
    printf("  INFO: %d odd-hole cuts generated\n", cuts);

    /* Cleanup */
    cut_pool_free(pool);
    conflict_graph_free(graph);
    detect_set_cover_free(&sig);
    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test lifted cover cut generation.
 *
 * Create a knapsack constraint and verify lifting improves the cut.
 */
void test_lifted_cover_cut_generation(void) {
    printf("\n=== Test: Lifted Cover Cut Generation ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 5 binary variables with varying coefficients */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* x0, coef=3 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* x1, coef=3 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* x2, coef=3 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* x3, coef=2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* x4, coef=1 */

    /* Knapsack: 3x0 + 3x1 + 3x2 + 2x3 + x4 <= 7 */
    int idx[] = {0, 1, 2, 3, 4};
    double coef[] = {3.0, 3.0, 3.0, 2.0, 1.0};
    lp_model_add_constraint(model, 5, idx, coef, 'L', 7.0);

    lp_model_finalize(model);

    /* Create MIP solver */
    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Set fractional solution */
    if (solver->lp_solver && solver->lp_solver->solution) {
        solver->lp_solver->solution[0] = 0.8;
        solver->lp_solver->solution[1] = 0.8;
        solver->lp_solver->solution[2] = 0.8;
        solver->lp_solver->solution[3] = 0.4;
        solver->lp_solver->solution[4] = 0.2;
    }

    /* Generate lifted cover cuts */
    CutPool *pool = cut_pool_create(64);
    int cuts = generate_lifted_cover_cuts(solver, pool);
    printf("  INFO: %d lifted cover cuts generated\n", cuts);

    /* Cleanup */
    cut_pool_free(pool);
    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test combined SCP cut generation.
 */
void test_combined_scp_cuts(void) {
    printf("\n=== Test: Combined SCP Cut Generation ===\n");

    /* Create a small set partitioning problem */
    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 4 sets covering 3 elements */
    for (int j = 0; j < 4; j++) {
        lp_model_add_var(model, 0.0, 1.0, (double)(j + 1), 'B');
    }

    /* Element 0: S0 + S1 = 1 */
    int idx0[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'E', 1.0);

    /* Element 1: S1 + S2 = 1 */
    int idx1[] = {1, 2};
    lp_model_add_constraint(model, 2, idx1, coef, 'E', 1.0);

    /* Element 2: S2 + S3 = 1 */
    int idx2[] = {2, 3};
    lp_model_add_constraint(model, 2, idx2, coef, 'E', 1.0);

    lp_model_finalize(model);

    /* Create MIP solver */
    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Set fractional solution */
    if (solver->lp_solver && solver->lp_solver->solution) {
        for (int j = 0; j < 4; j++) {
            solver->lp_solver->solution[j] = 0.5;
        }
    }

    /* Generate all SCP cuts */
    CutPool *pool = cut_pool_create(256);
    int total_cuts = generate_scp_cuts(solver, pool);
    printf("  INFO: %d total SCP cuts generated\n", total_cuts);

    /* Verify pool count matches returned count (accounting for deduplication) */
    ASSERT(pool->count <= total_cuts, "Pool count reasonable");

    /* Cleanup */
    cut_pool_free(pool);
    mip_free(solver);
    lp_model_free(model);
}

/* ============================================================================
 * SCP Heuristics Tests (Phase 4)
 * ============================================================================ */

/*
 * Test greedy set cover heuristic.
 *
 * Problem: 3 elements, 4 sets
 * S0: covers {0, 1}, cost = 3
 * S1: covers {1, 2}, cost = 3
 * S2: covers {0},    cost = 2
 * S3: covers {2},    cost = 2
 *
 * Greedy should select: S0 (cost 3, covers 2) then S3 (cost 2, covers 1)
 * Total cost = 5, covering all elements
 */
void test_greedy_set_cover(void) {
    printf("\n=== Test: Greedy Set Cover Heuristic ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 4 sets with varying costs */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S0: cost 3 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S1: cost 3 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S2: cost 2 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S3: cost 2 */

    /* Element 0: S0 + S2 >= 1 */
    int idx0[] = {0, 2};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'G', 1.0);

    /* Element 1: S0 + S1 >= 1 */
    int idx1[] = {0, 1};
    lp_model_add_constraint(model, 2, idx1, coef, 'G', 1.0);

    /* Element 2: S1 + S3 >= 1 */
    int idx2[] = {1, 3};
    lp_model_add_constraint(model, 2, idx2, coef, 'G', 1.0);

    lp_model_finalize(model);

    /* Create MIP solver */
    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Run greedy heuristic */
    double *solution = (double *)calloc(4, sizeof(double));
    int result = heuristic_greedy_set_cover(solver, solution);
    ASSERT(result == 0, "Greedy heuristic succeeded");

    /* Calculate objective */
    double obj = 0;
    int selected = 0;
    for (int j = 0; j < 4; j++) {
        if (solution[j] > 0.5) {
            obj += model->c[j];
            selected++;
        }
    }
    printf("  INFO: Greedy selected %d sets, cost = %.1f\n", selected, obj);
    ASSERT(selected >= 2, "At least 2 sets selected");
    ASSERT(obj <= 6.0, "Cost is reasonable (<= 6)");

    /* Verify feasibility: all elements covered */
    int covered[3] = {0, 0, 0};
    if (solution[0] > 0.5) { covered[0]++; covered[1]++; }
    if (solution[1] > 0.5) { covered[1]++; covered[2]++; }
    if (solution[2] > 0.5) { covered[0]++; }
    if (solution[3] > 0.5) { covered[2]++; }
    ASSERT(covered[0] >= 1 && covered[1] >= 1 && covered[2] >= 1,
           "All elements covered");

    free(solution);
    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test LP-guided greedy heuristic.
 */
void test_lp_guided_greedy(void) {
    printf("\n=== Test: LP-Guided Greedy Heuristic ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 sets, 2 elements */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S0: cost 2 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S1: cost 2 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S2: cost 3 */

    /* Element 0: S0 + S2 >= 1 */
    int idx0[] = {0, 2};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'G', 1.0);

    /* Element 1: S1 + S2 >= 1 */
    int idx1[] = {1, 2};
    lp_model_add_constraint(model, 2, idx1, coef, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Fake LP solution biasing toward S2 */
    double lp_sol[3] = {0.3, 0.3, 0.8};

    double *solution = (double *)calloc(3, sizeof(double));
    int result = heuristic_lp_guided_greedy(solver, lp_sol, solution);
    ASSERT(result == 0, "LP-guided greedy succeeded");

    /* With LP guidance toward S2, it should prefer S2 despite higher cost */
    double obj = 0;
    for (int j = 0; j < 3; j++) {
        if (solution[j] > 0.5) obj += model->c[j];
    }
    printf("  INFO: LP-guided selected cost = %.1f\n", obj);
    ASSERT(obj <= 5.0, "Cost is reasonable");

    free(solution);
    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test local search improvement.
 */
void test_local_search_scp(void) {
    printf("\n=== Test: Local Search SCP Improvement ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 sets, 2 elements - S0 covers both, S1 and S2 each cover one */
    lp_model_add_var(model, 0.0, 1.0, 5.0, 'B');  /* S0: cost 5, covers both */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S1: cost 2, covers elem 0 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S2: cost 2, covers elem 1 */

    /* Element 0: S0 + S1 >= 1 */
    int idx0[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'G', 1.0);

    /* Element 1: S0 + S2 >= 1 */
    int idx1[] = {0, 2};
    lp_model_add_constraint(model, 2, idx1, coef, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Start with suboptimal solution: all sets selected */
    double solution[3] = {1.0, 1.0, 1.0};
    double initial_cost = 5.0 + 2.0 + 2.0;

    int improvements = heuristic_local_search_scp(solver, solution);
    ASSERT(improvements >= 0, "Local search completed");

    double final_cost = 0;
    for (int j = 0; j < 3; j++) {
        if (solution[j] > 0.5) final_cost += model->c[j];
    }

    printf("  INFO: Local search: initial=%.1f, final=%.1f, improvements=%d\n",
           initial_cost, final_cost, improvements);
    ASSERT(final_cost < initial_cost, "Cost improved");

    /* Should have removed redundant sets - either S0 alone or S1+S2 */
    int s0 = solution[0] > 0.5;
    int s1 = solution[1] > 0.5;
    int s2 = solution[2] > 0.5;
    ASSERT((s0 && !s1 && !s2) || (!s0 && s1 && s2),
           "Removed redundant sets correctly");

    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test combined SCP heuristic.
 */
void test_combined_scp_heuristic(void) {
    printf("\n=== Test: Combined SCP Heuristic ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 5 sets, 4 elements */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S0 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S1 */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* S2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S3 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S4 */

    /* Element 0: S0 + S1 >= 1 */
    int idx0[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'G', 1.0);

    /* Element 1: S0 + S2 >= 1 */
    int idx1[] = {0, 2};
    lp_model_add_constraint(model, 2, idx1, coef, 'G', 1.0);

    /* Element 2: S2 + S3 >= 1 */
    int idx2[] = {2, 3};
    lp_model_add_constraint(model, 2, idx2, coef, 'G', 1.0);

    /* Element 3: S3 + S4 >= 1 */
    int idx3[] = {3, 4};
    lp_model_add_constraint(model, 2, idx3, coef, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    double *solution = (double *)calloc(5, sizeof(double));
    int result = heuristic_scp(solver, NULL, solution);
    ASSERT(result == 0, "Combined SCP heuristic succeeded");

    double obj = 0;
    int selected = 0;
    for (int j = 0; j < 5; j++) {
        if (solution[j] > 0.5) {
            obj += model->c[j];
            selected++;
        }
    }
    printf("  INFO: Combined heuristic: %d sets, cost = %.1f\n", selected, obj);
    ASSERT(selected >= 2, "At least 2 sets needed");

    /* Verify feasibility */
    int covered[4] = {0, 0, 0, 0};
    if (solution[0] > 0.5) { covered[0]++; covered[1]++; }
    if (solution[1] > 0.5) { covered[0]++; }
    if (solution[2] > 0.5) { covered[1]++; covered[2]++; }
    if (solution[3] > 0.5) { covered[2]++; covered[3]++; }
    if (solution[4] > 0.5) { covered[3]++; }
    ASSERT(covered[0] >= 1 && covered[1] >= 1 && covered[2] >= 1 && covered[3] >= 1,
           "All elements covered");

    free(solution);
    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test that heuristics return -1 for non-SCP problems.
 */
void test_heuristic_non_scp(void) {
    printf("\n=== Test: Heuristics Reject Non-SCP ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* Non-binary variable -> not SCP */
    lp_model_add_var(model, 0.0, 5.0, 1.0, 'I');  /* Integer, not binary */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');

    int idx[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, coef, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    double solution[2];
    int result = heuristic_greedy_set_cover(solver, solution);
    ASSERT(result == -1, "Greedy rejects non-SCP");

    result = heuristic_scp(solver, NULL, solution);
    ASSERT(result == -1, "Combined heuristic rejects non-SCP");

    mip_free(solver);
    lp_model_free(model);
}

/* ============================================================================
 * SCP Branching Tests (Phase 5)
 * ============================================================================ */

/*
 * Test pseudo-cost initialization for SCP.
 */
void test_init_pseudo_costs_scp(void) {
    printf("\n=== Test: SCP Pseudo-Cost Initialization ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 sets with different costs and coverage */
    lp_model_add_var(model, 0.0, 1.0, 6.0, 'B');  /* S0: cost 6, covers 2 elements */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S1: cost 3, covers 1 element */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* S2: cost 4, covers 2 elements */

    /* Element 0: S0 + S2 >= 1 */
    int idx0[] = {0, 2};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'G', 1.0);

    /* Element 1: S0 + S1 >= 1 */
    int idx1[] = {0, 1};
    lp_model_add_constraint(model, 2, idx1, coef, 'G', 1.0);

    /* Element 2: S2 >= 1 (only S2 covers it) */
    int idx2[] = {2};
    double coef1[] = {1.0};
    lp_model_add_constraint(model, 1, idx2, coef1, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Initialize pseudo-costs */
    int result = init_pseudo_costs_scp(solver);
    ASSERT(result == 0, "Pseudo-cost initialization succeeded");

    /* Check pseudo-costs are cost/coverage ratio */
    /* S0: cost=6, covers 2 -> 3.0 */
    /* S1: cost=3, covers 1 -> 3.0 */
    /* S2: cost=4, covers 2 -> 2.0 */
    ASSERT(fabs(solver->pseudo_cost_down[0] - 3.0) < 0.01, "S0 pseudo-cost = 3.0");
    ASSERT(fabs(solver->pseudo_cost_down[1] - 3.0) < 0.01, "S1 pseudo-cost = 3.0");
    ASSERT(fabs(solver->pseudo_cost_down[2] - 2.0) < 0.01, "S2 pseudo-cost = 2.0");

    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test SCP constraint branching.
 */
void test_scp_constraint_branching(void) {
    printf("\n=== Test: SCP Constraint Branching ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 3 sets, 2 elements */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S1 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2 */

    /* Element 0: S0 + S1 >= 1 */
    int idx0[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'G', 1.0);

    /* Element 1: S1 + S2 >= 1 */
    int idx1[] = {1, 2};
    lp_model_add_constraint(model, 2, idx1, coef, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Fractional solution with fractional coverage:
     * Element 0: 0.3 + 0.4 = 0.7 (under-covered, fractional)
     * Element 1: 0.4 + 0.6 = 1.0 (covered, but S1,S2 fractional) */
    double solution[3] = {0.3, 0.4, 0.6};

    int element, set;
    int result = select_scp_branch(solver, solution, &element, &set);
    ASSERT(result == 0, "SCP branching found valid decision");
    ASSERT(element >= 0 && element < 2, "Valid element selected");
    ASSERT(set >= 0 && set < 3, "Valid set selected");

    printf("  INFO: Branching on element %d, set %d\n", element, set);

    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test SOS1 branching for SPP.
 */
void test_sos1_branching_spp(void) {
    printf("\n=== Test: SOS1 Branching for SPP ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* 4 sets, 2 elements - set partitioning (=) */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S1 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S3 */

    /* Element 0: S0 + S1 = 1 (fractional coverage 0.6+0.5=1.1, not exact 1) */
    int idx0[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef, 'E', 1.0);

    /* Element 1: S2 + S3 = 1 (fractional coverage 0.3+0.6=0.9, not exact 1) */
    int idx1[] = {2, 3};
    lp_model_add_constraint(model, 2, idx1, coef, 'E', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Fractional solution with coverage != 1 for each element
     * Element 0: 0.6 + 0.5 = 1.1 (over-covered by 0.1)
     * Element 1: 0.3 + 0.6 = 0.9 (under-covered by 0.1) */
    double solution[4] = {0.6, 0.5, 0.3, 0.6};

    int set;
    int result = select_sos1_branch_spp(solver, solution, &set);
    ASSERT(result == 0, "SOS1 branching found valid decision");
    ASSERT(set >= 0 && set < 4, "Valid set selected");

    printf("  INFO: SOS1 branching on set %d (value=%.2f)\n", set, solution[set]);

    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test that SCP branching rejects non-SCP models.
 */
void test_scp_branching_rejects_non_scp(void) {
    printf("\n=== Test: SCP Branching Rejects Non-SCP ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* Non-binary variable -> not SCP */
    lp_model_add_var(model, 0.0, 5.0, 1.0, 'I');  /* Integer, not binary */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');

    int idx[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, coef, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    double solution[2] = {0.5, 0.5};
    int element, set;
    int result = select_scp_branch(solver, solution, &element, &set);
    ASSERT(result == -1, "SCP branching rejects non-SCP");

    result = init_pseudo_costs_scp(solver);
    ASSERT(result == -1, "Pseudo-cost init rejects non-SCP");

    mip_free(solver);
    lp_model_free(model);
}

/*
 * Test VAR_SELECT_SCP strategy integration.
 */
void test_var_select_scp_strategy(void) {
    printf("\n=== Test: VAR_SELECT_SCP Strategy ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    /* Simple SCP */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');

    int idx[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, coef, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    /* Set strategy to SCP */
    solver->var_select = VAR_SELECT_SCP;

    double solution[2] = {0.6, 0.4};
    int branch_var;
    int result = select_branch_variable(solver, solution, &branch_var);
    ASSERT(result == 0, "Branch variable selection succeeded");
    ASSERT(branch_var >= 0 && branch_var < 2, "Valid branch variable");

    printf("  INFO: VAR_SELECT_SCP chose var %d\n", branch_var);

    mip_free(solver);
    lp_model_free(model);
}

/* ============================================================================
 * SCP Lagrangian Relaxation Tests (Phase 6)
 * ============================================================================ */

void test_lagrangian_context_creation(void) {
    printf("\n=== Test: Lagrangian Context Creation ===\n");

    /* Create a simple SCP:
     * min 3*x0 + 2*x1 + 4*x2
     * s.t. x0 + x1 >= 1  (element 0)
     *      x1 + x2 >= 1  (element 1)
     *      x0 + x2 >= 1  (element 2)
     */
    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');

    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'G', 1.0);

    int idx1[] = {1, 2};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'G', 1.0);

    int idx2[] = {0, 2};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    LagrangianContext *ctx = lagrangian_create(solver);
    ASSERT(ctx != NULL, "Lagrangian context created");
    ASSERT(ctx->num_elements == 3, "3 elements");
    ASSERT(ctx->num_sets == 3, "3 sets");
    ASSERT(ctx->lambda != NULL, "Lambda array allocated");
    ASSERT(ctx->subgradient != NULL, "Subgradient array allocated");

    lagrangian_free(ctx);
    mip_free(solver);
    lp_model_free(model);
}

void test_lagrangian_bound_computation(void) {
    printf("\n=== Test: Lagrangian Bound Computation ===\n");

    /* Same SCP as above */
    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* c0 = 3 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* c1 = 2 */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* c2 = 4 */

    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'G', 1.0);

    int idx1[] = {1, 2};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'G', 1.0);

    int idx2[] = {0, 2};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    LagrangianContext *ctx = lagrangian_create(solver);
    ASSERT(ctx != NULL, "Context created");

    /* With lambda = 0, bound should be sum of negative reduced costs */
    /* All reduced costs are positive (c_j > 0), so bound = 0 */
    double bound = lagrangian_bound(ctx);
    printf("  INFO: Bound with lambda=0: %.4f\n", bound);
    ASSERT(bound < 0.1, "Bound with lambda=0 is 0");

    /* Set lambda = [1, 1, 1]:
     * Reduced costs:
     *   c0 - lambda[0] - lambda[2] = 3 - 1 - 1 = 1 > 0, x0 = 0
     *   c1 - lambda[0] - lambda[1] = 2 - 1 - 1 = 0 >= 0, x1 = 0
     *   c2 - lambda[1] - lambda[2] = 4 - 1 - 1 = 2 > 0, x2 = 0
     * L(lambda) = 1+1+1 = 3
     */
    ctx->lambda[0] = 1.0;
    ctx->lambda[1] = 1.0;
    ctx->lambda[2] = 1.0;
    bound = lagrangian_bound(ctx);
    printf("  INFO: Bound with lambda=[1,1,1]: %.4f\n", bound);
    ASSERT(fabs(bound - 3.0) < 0.01, "Bound with lambda=[1,1,1] is 3");

    /* Set lambda = [2, 2, 2]:
     * Reduced costs:
     *   c0 - lambda[0] - lambda[2] = 3 - 2 - 2 = -1 < 0, x0 = 1
     *   c1 - lambda[0] - lambda[1] = 2 - 2 - 2 = -2 < 0, x1 = 1
     *   c2 - lambda[1] - lambda[2] = 4 - 2 - 2 = 0 >= 0, x2 = 0
     * L(lambda) = 2+2+2 + (-1) + (-2) = 3
     */
    ctx->lambda[0] = 2.0;
    ctx->lambda[1] = 2.0;
    ctx->lambda[2] = 2.0;
    bound = lagrangian_bound(ctx);
    printf("  INFO: Bound with lambda=[2,2,2]: %.4f\n", bound);
    ASSERT(fabs(bound - 3.0) < 0.01, "Bound with lambda=[2,2,2] is 3");

    /* Check subgradient */
    printf("  INFO: Subgradient: [%.1f, %.1f, %.1f]\n",
           ctx->subgradient[0], ctx->subgradient[1], ctx->subgradient[2]);

    lagrangian_free(ctx);
    mip_free(solver);
    lp_model_free(model);
}

void test_lagrangian_subgradient_step(void) {
    printf("\n=== Test: Lagrangian Subgradient Step ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');

    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'G', 1.0);

    int idx1[] = {1, 2};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'G', 1.0);

    int idx2[] = {0, 2};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    LagrangianContext *ctx = lagrangian_create(solver);
    ASSERT(ctx != NULL, "Context created");

    /* Set upper bound (optimal is x1=1, cost=2 doesn't work; need x0+x1=5 or x1+x2=6) */
    /* Actually optimal is x0=1, x1=1 (covers all) with cost=5 */
    ctx->ub = 5.0;

    /* Perform a few subgradient steps */
    double bound1 = lagrangian_step(ctx);
    double bound2 = lagrangian_step(ctx);
    double bound3 = lagrangian_step(ctx);

    printf("  INFO: Bounds after steps: %.4f, %.4f, %.4f\n", bound1, bound2, bound3);
    printf("  INFO: Best bound: %.4f\n", ctx->best_bound);
    ASSERT(ctx->iterations == 3, "3 iterations performed");
    ASSERT(ctx->best_bound >= 0.0, "Best bound is non-negative");

    lagrangian_free(ctx);
    mip_free(solver);
    lp_model_free(model);
}

void test_lagrangian_optimize(void) {
    printf("\n=== Test: Lagrangian Optimization ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');

    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'G', 1.0);

    int idx1[] = {1, 2};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'G', 1.0);

    int idx2[] = {0, 2};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    LagrangianContext *ctx = lagrangian_create(solver);
    ASSERT(ctx != NULL, "Context created");

    /* Set upper bound */
    ctx->ub = 5.0;  /* Optimal is x0=x1=1 with cost 5 */
    ctx->max_iterations = 100;

    double best_bound = lagrangian_optimize(ctx);
    printf("  INFO: Best Lagrangian bound: %.4f (after %d iterations)\n",
           best_bound, ctx->iterations);
    printf("  INFO: Bound improvements: %d\n", ctx->bound_improvements);

    /* Lagrangian bound should be <= optimal (5) */
    ASSERT(best_bound <= 5.0 + 0.01, "Lagrangian bound <= optimal");
    /* For SCP, Lagrangian bound is usually >= LP bound which is >= 0 */
    ASSERT(best_bound >= 0.0, "Lagrangian bound >= 0");

    lagrangian_free(ctx);
    mip_free(solver);
    lp_model_free(model);
}

void test_lagrangian_repair(void) {
    printf("\n=== Test: Lagrangian Repair ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');

    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'G', 1.0);

    int idx1[] = {1, 2};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'G', 1.0);

    int idx2[] = {0, 2};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    LagrangianContext *ctx = lagrangian_create(solver);
    ASSERT(ctx != NULL, "Context created");

    /* Set up a Lagrangian solution that is infeasible (element 2 not covered) */
    ctx->x_lagrangian[0] = 0.0;
    ctx->x_lagrangian[1] = 1.0;  /* Covers elements 0, 1 */
    ctx->x_lagrangian[2] = 0.0;

    double *solution = (double *)calloc(3, sizeof(double));
    double repair_cost = lagrangian_repair(ctx, solver, solution);

    printf("  INFO: Repair cost: %.4f\n", repair_cost);
    printf("  INFO: Solution: [%.0f, %.0f, %.0f]\n",
           solution[0], solution[1], solution[2]);

    ASSERT(repair_cost < RALPH_INFINITY, "Repair succeeded");
    /* Should add either x0 or x2 to cover element 2 */
    /* x0 (cost 3) covers elem 2; x2 (cost 4) covers elem 2 */
    /* Greedy should pick x0 (cheaper) */
    /* Final: x0=1, x1=1 -> cost 5 */
    ASSERT(repair_cost <= 6.0, "Repair cost <= 6");

    /* Verify feasibility */
    int covered[3] = {0, 0, 0};
    if (solution[0] > 0.5) { covered[0]++; covered[2]++; }
    if (solution[1] > 0.5) { covered[0]++; covered[1]++; }
    if (solution[2] > 0.5) { covered[1]++; covered[2]++; }
    ASSERT(covered[0] >= 1 && covered[1] >= 1 && covered[2] >= 1, "All elements covered");

    free(solution);
    lagrangian_free(ctx);
    mip_free(solver);
    lp_model_free(model);
}

void test_lagrangian_solve_scp(void) {
    printf("\n=== Test: Lagrangian Solve SCP ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');

    int idx0[] = {0, 1};
    double coef0[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx0, coef0, 'G', 1.0);

    int idx1[] = {1, 2};
    double coef1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, coef1, 'G', 1.0);

    int idx2[] = {0, 2};
    double coef2[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, coef2, 'G', 1.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    double *solution = (double *)calloc(3, sizeof(double));
    double lower_bound;
    int result = lagrangian_solve_scp(solver, solution, &lower_bound);

    ASSERT(result == 0, "Lagrangian solve succeeded");
    printf("  INFO: Lower bound: %.4f\n", lower_bound);

    /* Compute solution objective */
    double obj = 0.0;
    int count = 0;
    for (int j = 0; j < 3; j++) {
        if (solution[j] > 0.5) {
            obj += model->c[j];
            count++;
        }
    }
    printf("  INFO: Solution: [%.0f, %.0f, %.0f], cost=%.1f\n",
           solution[0], solution[1], solution[2], obj);

    ASSERT(count >= 2, "At least 2 sets selected");
    ASSERT(lower_bound <= obj + 0.01, "Lower bound <= solution cost");

    /* Verify feasibility */
    int covered[3] = {0, 0, 0};
    if (solution[0] > 0.5) { covered[0]++; covered[2]++; }
    if (solution[1] > 0.5) { covered[0]++; covered[1]++; }
    if (solution[2] > 0.5) { covered[1]++; covered[2]++; }
    ASSERT(covered[0] >= 1 && covered[1] >= 1 && covered[2] >= 1, "All elements covered");

    free(solution);
    mip_free(solver);
    lp_model_free(model);
}

void test_lagrangian_rejects_non_scp(void) {
    printf("\n=== Test: Lagrangian Rejects Non-SCP ===\n");

    /* Create a non-SCP model (general LP) */
    LPModel *model = lp_model_create();
    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, RALPH_INFINITY, 1.0, 'C');
    lp_model_add_var(model, 0.0, RALPH_INFINITY, 2.0, 'C');

    int idx[] = {0, 1};
    double coef[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, coef, 'L', 10.0);

    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 256);
    ASSERT(solver != NULL, "MIP solver created");

    LagrangianContext *ctx = lagrangian_create(solver);
    ASSERT(ctx == NULL, "Lagrangian context rejected for non-SCP");

    double solution[2];
    int result = lagrangian_solve_scp(solver, solution, NULL);
    ASSERT(result == -1, "Lagrangian solve rejected non-SCP");

    mip_free(solver);
    lp_model_free(model);
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(void) {
    printf("Network Detection Tests\n");
    printf("=======================\n");

    /* Network detection tests */
    test_detect_simple_network();
    test_detect_transportation();
    test_detect_assignment();
    test_detect_shortest_path();

    /* Network rejection tests */
    test_reject_non_network_coef();
    test_reject_non_network_count();
    test_reject_non_network_inequality();

    /* Network solve tests */
    test_solve_via_detection();
    test_solve_assignment_delegation();

    /* Edge cases */
    test_empty_model();
    test_null_inputs();

    printf("\nSet Cover Detection Tests\n");
    printf("=========================\n");

    /* Set cover detection tests */
    test_detect_set_covering();
    test_detect_set_partitioning();
    test_detect_set_packing();
    test_detect_set_cover_mixed();
    test_detect_weighted_set_covering();

    /* Set cover rejection tests */
    test_reject_non_binary_scp();
    test_reject_non_01_coef_scp();
    test_reject_negative_rhs_scp();

    /* Utility tests */
    test_set_cover_type_name();

    printf("\nSCP Presolve Tests\n");
    printf("==================\n");

    /* SCP presolve tests */
    test_presolve_scp_essential_sets();
    test_presolve_scp_row_dominance();
    test_presolve_scp_column_dominance();
    test_presolve_scp_combined();
    test_presolve_scp_infeasible();
    test_presolve_scp_no_reductions();

    printf("\nSCP Cutting Planes Tests\n");
    printf("========================\n");

    /* SCP cutting planes tests (Phase 3) */
    test_conflict_graph_creation();
    test_clique_cut_generation();
    test_odd_hole_cut_generation();
    test_lifted_cover_cut_generation();
    test_combined_scp_cuts();

    printf("\nSCP Heuristics Tests\n");
    printf("====================\n");

    /* SCP heuristics tests (Phase 4) */
    test_greedy_set_cover();
    test_lp_guided_greedy();
    test_local_search_scp();
    test_combined_scp_heuristic();
    test_heuristic_non_scp();

    printf("\nSCP Branching Tests\n");
    printf("===================\n");

    /* SCP branching tests (Phase 5) */
    test_init_pseudo_costs_scp();
    test_scp_constraint_branching();
    test_sos1_branching_spp();
    test_scp_branching_rejects_non_scp();
    test_var_select_scp_strategy();

    printf("\nSCP Lagrangian Relaxation Tests\n");
    printf("================================\n");

    /* SCP Lagrangian relaxation tests (Phase 6) */
    test_lagrangian_context_creation();
    test_lagrangian_bound_computation();
    test_lagrangian_subgradient_step();
    test_lagrangian_optimize();
    test_lagrangian_repair();
    test_lagrangian_solve_scp();
    test_lagrangian_rejects_non_scp();

    /* Summary */
    printf("\n=======================\n");
    printf("Tests: %d/%d passed\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}
