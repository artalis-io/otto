/*
 * Tests for fixed-basis LP sensitivity/ranging public APIs.
 *
 * This module is intentionally separate from telemetry/logging tests.
 */

#include <stdio.h>
#include <math.h>
#include "ralph_test_mod_api.h"

static int tests_run = 0;
static int tests_passed = 0;

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

#define ASSERT_DBL_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((double)(a) - (double)(b)) <= (tol)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12f != %.12f)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

#define ASSERT_INF_POS(v, msg) do { \
    tests_run++; \
    if ((double)(v) >= 0.9 * RALPH_INFINITY) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12f not +INF)\n", msg, (double)(v)); \
    } \
} while (0)

#define ASSERT_INF_NEG(v, msg) do { \
    tests_run++; \
    if ((double)(v) <= -0.9 * RALPH_INFINITY) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12f not -INF)\n", msg, (double)(v)); \
    } \
} while (0)

static RalphModel* build_nonbasic_case(int maximize_mode) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, maximize_mode ? RALPH_MAXIMIZE : RALPH_MINIMIZE);
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, maximize_mode ? -1.0 : 1.0, RALPH_CONTINUOUS); /* x1 */
    ralph_test_add_var(model, 0.0, 3.0, 0.0, RALPH_CONTINUOUS);                                     /* x2 */

    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_EQUAL, 2.0);
    }
    return model;
}

static void test_sensitivity_guards(void) {
    RalphSensitivityRange range;
    RalphBoundSensitivityRange brange;
    RalphModel *lp = build_nonbasic_case(0);
    RalphModel *mip = NULL;

    ASSERT_TRUE(lp != NULL, "guard: LP model created");
    if (!lp) return;

    ASSERT_INT_EQ(ralph_core_get_constraint_rhs_range(lp, 0, &range), -1,
                  "guard: rhs range unavailable before solve");
    ASSERT_INT_EQ(ralph_core_get_obj_coef_range(lp, 0, &range), -1,
                  "guard: obj range unavailable before solve");
    ASSERT_INT_EQ(ralph_core_get_var_bound_range(lp, 0, &brange), -1,
                  "guard: bound range unavailable before solve");

    ASSERT_INT_EQ(ralph_test_optimize_lp(lp), 0, "guard: LP solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(lp), (int)RALPH_STATUS_OPTIMAL, "guard: LP optimal");

    ASSERT_INT_EQ(ralph_core_get_constraint_rhs_range(NULL, 0, &range), -1,
                  "guard: rhs rejects NULL model");
    ASSERT_INT_EQ(ralph_core_get_obj_coef_range(lp, 3, &range), -1,
                  "guard: obj rejects invalid var");
    ASSERT_INT_EQ(ralph_core_get_var_bound_range(lp, -1, &brange), -1,
                  "guard: bound rejects invalid var");

    mip = ralph_test_create();
    ASSERT_TRUE(mip != NULL, "guard: MIP model created");
    if (mip) {
        ralph_test_set_obj_sense(mip, RALPH_MAXIMIZE);
        ralph_test_add_var(mip, 0.0, 1.0, 1.0, RALPH_BINARY);
        ASSERT_INT_EQ(ralph_test_optimize_mip(mip), 0, "guard: MIP solve succeeds");
        ASSERT_INT_EQ(ralph_core_get_constraint_rhs_range(mip, 0, &range), -1,
                      "guard: rhs range is LP-only");
        ASSERT_INT_EQ(ralph_core_get_obj_coef_range(mip, 0, &range), -1,
                      "guard: obj range is LP-only");
        ASSERT_INT_EQ(ralph_core_get_var_bound_range(mip, 0, &brange), -1,
                      "guard: bound range is LP-only");
    }

    ralph_test_free(lp);
    ralph_test_free(mip);
}

static void test_nonbasic_ranges_min(void) {
    RalphModel *model = build_nonbasic_case(0);
    RalphSensitivityRange rhs_range;
    RalphSensitivityRange obj_x1;
    RalphSensitivityRange obj_x2;
    RalphBoundSensitivityRange bound_x1;
    RalphBoundSensitivityRange bound_x2;
    RalphBasisStatus col_status[2];

    ASSERT_TRUE(model != NULL, "nonbasic/min: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0, "nonbasic/min: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL, "nonbasic/min: optimal");

    ASSERT_INT_EQ(ralph_test_get_basis_status(model, col_status, NULL), 0,
                  "nonbasic/min: basis status available");
    ASSERT_INT_EQ((int)col_status[0], (int)RALPH_BASIS_STATUS_AT_LOWER,
                  "nonbasic/min: x1 at lower");
    ASSERT_INT_EQ((int)col_status[1], (int)RALPH_BASIS_STATUS_BASIC,
                  "nonbasic/min: x2 basic");

    ASSERT_INT_EQ(ralph_core_get_constraint_rhs_range(model, 0, &rhs_range), 0,
                  "nonbasic/min: rhs range available");
    ASSERT_DBL_NEAR(rhs_range.current, 2.0, 1e-9, "nonbasic/min: rhs current");
    ASSERT_DBL_NEAR(rhs_range.lower, 0.0, 1e-7, "nonbasic/min: rhs lower");
    ASSERT_DBL_NEAR(rhs_range.upper, 3.0, 1e-7, "nonbasic/min: rhs upper");

    ASSERT_INT_EQ(ralph_core_get_obj_coef_range(model, 0, &obj_x1), 0,
                  "nonbasic/min: obj range x1");
    ASSERT_DBL_NEAR(obj_x1.current, 1.0, 1e-9, "nonbasic/min: obj x1 current");
    ASSERT_DBL_NEAR(obj_x1.lower, 0.0, 1e-7, "nonbasic/min: obj x1 lower");
    ASSERT_INF_POS(obj_x1.upper, "nonbasic/min: obj x1 upper = +inf");

    ASSERT_INT_EQ(ralph_core_get_obj_coef_range(model, 1, &obj_x2), 0,
                  "nonbasic/min: obj range x2");
    ASSERT_DBL_NEAR(obj_x2.current, 0.0, 1e-9, "nonbasic/min: obj x2 current");
    ASSERT_INF_NEG(obj_x2.lower, "nonbasic/min: obj x2 lower = -inf");
    ASSERT_DBL_NEAR(obj_x2.upper, 1.0, 1e-6, "nonbasic/min: obj x2 upper");

    ASSERT_INT_EQ(ralph_core_get_var_bound_range(model, 0, &bound_x1), 0,
                  "nonbasic/min: bound range x1");
    ASSERT_DBL_NEAR(bound_x1.lower_current, 0.0, 1e-9, "nonbasic/min: lb current");
    ASSERT_DBL_NEAR(bound_x1.lower_min, -1.0, 1e-6, "nonbasic/min: lb min");
    ASSERT_DBL_NEAR(bound_x1.lower_max, 2.0, 1e-6, "nonbasic/min: lb max");
    ASSERT_DBL_NEAR(bound_x1.upper_min, 0.0, 1e-9, "nonbasic/min: ub min");
    ASSERT_INF_POS(bound_x1.upper_max, "nonbasic/min: ub max = +inf");

    ASSERT_INT_EQ(ralph_core_get_var_bound_range(model, 1, &bound_x2), 0,
                  "nonbasic/min: bound range x2");
    ASSERT_DBL_NEAR(bound_x2.lower_current, 0.0, 1e-9, "nonbasic/min: x2 lb current");
    ASSERT_DBL_NEAR(bound_x2.upper_current, 3.0, 1e-9, "nonbasic/min: x2 ub current");
    ASSERT_INF_NEG(bound_x2.lower_min, "nonbasic/min: x2 lb min = -inf");
    ASSERT_DBL_NEAR(bound_x2.lower_max, 2.0, 1e-6, "nonbasic/min: x2 lb max");
    ASSERT_DBL_NEAR(bound_x2.upper_min, 2.0, 1e-6, "nonbasic/min: x2 ub min");
    ASSERT_INF_POS(bound_x2.upper_max, "nonbasic/min: x2 ub max = +inf");

    ralph_test_free(model);
}

static void test_nonbasic_ranges_max_obj_mapping(void) {
    RalphModel *model = build_nonbasic_case(1);
    RalphSensitivityRange obj_x1;

    ASSERT_TRUE(model != NULL, "nonbasic/max: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0, "nonbasic/max: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL, "nonbasic/max: optimal");

    ASSERT_INT_EQ(ralph_core_get_obj_coef_range(model, 0, &obj_x1), 0,
                  "nonbasic/max: obj range x1 available");
    ASSERT_DBL_NEAR(obj_x1.current, -1.0, 1e-9, "nonbasic/max: obj current");
    ASSERT_INF_NEG(obj_x1.lower, "nonbasic/max: obj lower = -inf");
    ASSERT_DBL_NEAR(obj_x1.upper, 0.0, 1e-7, "nonbasic/max: obj upper");

    ralph_test_free(model);
}

int main(void) {
    printf("=== LP Sensitivity/Ranging Tests ===\n");

    test_sensitivity_guards();
    test_nonbasic_ranges_min();
    test_nonbasic_ranges_max_obj_mapping();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
