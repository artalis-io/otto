/*
 * Optional integration tests for GLPK out-of-process adapter.
 *
 * This test runs only when `glpsol` is available in PATH.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ralph_test_mod_api.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_skipped = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

#define ASSERT_DBL_CLOSE(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) <= (tol)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12g != %.12g)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

static int glpsol_available(void) {
    int rc = system("which glpsol >/dev/null 2>&1");
    return rc == 0 ? 1 : 0;
}

static RalphModel* build_small_lp(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "detect_special", 0);
    ralph_test_set_int_param(model, "presolve", 0);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    {
        int idx[] = {0};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }
    return model;
}

static int configure_external_glpk(RalphModel *model, int algorithm) {
    if (!model) return -1;
    if (ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM, algorithm) != 0) return -1;
    if (ralph_core_set_int_param_id(model,
                               RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                               (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK) != 0) return -1;
    return 0;
}

static void test_optimal_duals_and_rc(void) {
    RalphModel *model = build_small_lp();
    double x = 0.0;
    double y = 0.0;
    double rc = 0.0;

    ASSERT_TRUE(model != NULL, "integration/optimal: model created");
    if (!model) return;

    ASSERT_INT_EQ(configure_external_glpk(model, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "integration/optimal: configure external primal");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "integration/optimal: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "integration/optimal: status optimal");
    ASSERT_DBL_CLOSE(ralph_test_get_objval(model), 1.0, 1e-9,
                     "integration/optimal: objective");
    ASSERT_INT_EQ(ralph_test_get_solution(model, &x), 0,
                  "integration/optimal: primal solution available");
    ASSERT_DBL_CLOSE(x, 1.0, 1e-9,
                     "integration/optimal: x value");
    ASSERT_INT_EQ(ralph_test_get_dual_solution(model, &y), 0,
                  "integration/optimal: dual solution available");
    ASSERT_DBL_CLOSE(y, 1.0, 1e-7,
                     "integration/optimal: dual value");
    ASSERT_INT_EQ(ralph_test_get_reduced_costs(model, &rc), 0,
                  "integration/optimal: reduced costs available");
    ASSERT_DBL_CLOSE(rc, 0.0, 1e-7,
                     "integration/optimal: reduced cost value");

    ralph_test_free(model);
}

static void test_dual_external_route(void) {
    RalphModel *model = build_small_lp();
    ASSERT_TRUE(model != NULL, "integration/dual-route: model created");
    if (!model) return;

    ASSERT_INT_EQ(configure_external_glpk(model, (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL),
                  0,
                  "integration/dual-route: configure external dual");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "integration/dual-route: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "integration/dual-route: status optimal");

    ralph_test_free(model);
}

static void test_column_order_roundtrip(void) {
    RalphModel *model = ralph_test_create();
    double x[12];

    ASSERT_TRUE(model != NULL, "integration/column-order: model created");
    if (!model) return;

    memset(x, 0, sizeof(x));
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "detect_special", 0);
    ralph_test_set_int_param(model, "presolve", 0);
    for (int j = 0; j < 12; j++) {
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, (double)(j + 1), RALPH_CONTINUOUS);
    }
    for (int j = 0; j < 12; j++) {
        int idx[] = {j};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_EQUAL, (double)(j + 1));
    }

    ASSERT_INT_EQ(configure_external_glpk(model, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "integration/column-order: configure external primal");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "integration/column-order: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "integration/column-order: status optimal");
    ASSERT_INT_EQ(ralph_test_get_solution(model, x), 0,
                  "integration/column-order: primal solution available");
    for (int j = 0; j < 12; j++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "integration/column-order: x[%d]", j);
        ASSERT_DBL_CLOSE(x[j], (double)(j + 1), 1e-9, msg);
    }

    ralph_test_free(model);
}

static void test_infeasible_mapping(void) {
    RalphModel *model = ralph_test_create();
    ASSERT_TRUE(model != NULL, "integration/infeasible: model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "detect_special", 0);
    ralph_test_set_int_param(model, "presolve", 0);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    {
        int idx[] = {0};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 2.0);
        ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 1.0);
    }

    ASSERT_INT_EQ(configure_external_glpk(model, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "integration/infeasible: configure external primal");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "integration/infeasible: solve returns status");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_INFEASIBLE,
                  "integration/infeasible: mapped status");

    ralph_test_free(model);
}

static void test_unbounded_mapping(void) {
    RalphModel *model = ralph_test_create();
    ASSERT_TRUE(model != NULL, "integration/unbounded: model created");
    if (!model) return;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "detect_special", 0);
    ralph_test_set_int_param(model, "presolve", 0);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    {
        int idx[] = {0};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0);
    }

    ASSERT_INT_EQ(configure_external_glpk(model, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "integration/unbounded: configure external primal");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "integration/unbounded: solve returns status");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_UNBOUNDED,
                  "integration/unbounded: mapped status");

    ralph_test_free(model);
}

int main(void) {
    printf("=== LP External GLPK OOP Integration Tests ===\n");

    if (!glpsol_available()) {
        tests_skipped = 1;
        printf("SKIP: glpsol not found in PATH\n");
        return 0;
    }

    ralph_lp_external_unregister_all_adapters();
    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(NULL), 0,
                  "integration: register GLPK OOP with PATH lookup");

    test_optimal_duals_and_rc();
    test_dual_external_route();
    test_column_order_roundtrip();
    test_infeasible_mapping();
    test_unbounded_mapping();

    ralph_lp_external_unregister_all_adapters();
    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_run == tests_passed) ? 0 : 1;
}
