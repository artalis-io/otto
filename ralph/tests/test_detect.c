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
 * Main
 * ============================================================================ */
int main(void) {
    printf("Network Detection Tests\n");
    printf("=======================\n");

    /* Detection tests */
    test_detect_simple_network();
    test_detect_transportation();
    test_detect_assignment();
    test_detect_shortest_path();

    /* Rejection tests */
    test_reject_non_network_coef();
    test_reject_non_network_count();
    test_reject_non_network_inequality();

    /* Solve tests */
    test_solve_via_detection();
    test_solve_assignment_delegation();

    /* Edge cases */
    test_empty_model();
    test_null_inputs();

    /* Summary */
    printf("\n=======================\n");
    printf("Tests: %d/%d passed\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}
