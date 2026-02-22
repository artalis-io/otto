/*
 * Tests for LP determinism/reproducibility controls.
 *
 * This test module is intentionally separate from telemetry/logging tests.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"

static int tests_run = 0;
static int tests_passed = 0;

/* Internal diagnostics helper exported by ralph.c for test/bench use. */
SimplexSolver* ralph_get_lp_solver(const RalphModel *model);

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

static RalphModel* build_repeatable_lp(void) {
    RalphModel *model = ralph_create();
    if (!model) return NULL;

    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_add_var(model, 0.0, RALPH_INFINITY, -3.0, RALPH_CONTINUOUS);
    ralph_add_var(model, 0.0, RALPH_INFINITY, -2.0, RALPH_CONTINUOUS);

    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        ralph_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 4.0);
    }
    {
        int idx[] = {0, 1};
        double val[] = {2.0, 1.0};
        ralph_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 5.0);
    }

    return model;
}

static void test_determinism_metadata_and_lookup(void) {
    RalphParamMeta meta;
    RalphParamId id = RALPH_PARAM_COUNT;

    ASSERT_TRUE(ralph_find_param_by_name("deterministic", &id) == 0 &&
                id == RALPH_PARAM_DETERMINISTIC,
                "deterministic canonical name lookup");
    ASSERT_TRUE(ralph_find_param_by_name("RandomSeed", &id) == 0 &&
                id == RALPH_PARAM_RANDOM_SEED,
                "random_seed alias lookup");
    ASSERT_TRUE(ralph_find_param_by_name("LPThreads", &id) == 0 &&
                id == RALPH_PARAM_LP_THREADS,
                "lp_threads alias lookup");

    ASSERT_TRUE(ralph_get_param_meta(RALPH_PARAM_DETERMINISTIC, &meta) == 0,
                "deterministic metadata available");
    ASSERT_TRUE(meta.scope == RALPH_PARAM_SCOPE_LP, "deterministic scope is LP");
    ASSERT_TRUE(meta.value_type == RALPH_PARAM_VALUE_INT, "deterministic type is int");
    ASSERT_TRUE(meta.has_min == 1 && meta.min_value == 0.0, "deterministic min=0");
    ASSERT_TRUE(meta.has_max == 1 && meta.max_value == 1.0, "deterministic max=1");

    ASSERT_TRUE(ralph_get_param_meta(RALPH_PARAM_RANDOM_SEED, &meta) == 0,
                "random_seed metadata available");
    ASSERT_TRUE(meta.scope == RALPH_PARAM_SCOPE_LP, "random_seed scope is LP");
    ASSERT_TRUE(meta.value_type == RALPH_PARAM_VALUE_INT, "random_seed type is int");
    ASSERT_TRUE(meta.has_min == 1 && meta.min_value == 0.0, "random_seed min=0");

    ASSERT_TRUE(ralph_get_param_meta(RALPH_PARAM_LP_THREADS, &meta) == 0,
                "lp_threads metadata available");
    ASSERT_TRUE(meta.scope == RALPH_PARAM_SCOPE_LP, "lp_threads scope is LP");
    ASSERT_TRUE(meta.value_type == RALPH_PARAM_VALUE_INT, "lp_threads type is int");
    ASSERT_TRUE(meta.has_min == 1 && meta.min_value == 0.0, "lp_threads min=0");
}

static void test_determinism_helpers(void) {
    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));

    ASSERT_INT_EQ(lp_determinism_effective_threads(&solver), 0,
                  "effective threads default to runtime auto");

    solver.deterministic = 1;
    ASSERT_INT_EQ(lp_determinism_effective_threads(&solver), 1,
                  "deterministic defaults to single LP thread");

    solver.lp_threads = 3;
    ASSERT_INT_EQ(lp_determinism_effective_threads(&solver), 3,
                  "explicit lp_threads overrides deterministic default");

    solver.deterministic = 0;
    solver.random_seed = 123U;
    ASSERT_INT_EQ((int)lp_determinism_seed_offset(&solver, 19, 1009U), 0,
                  "seed offset disabled when deterministic=0");

    solver.deterministic = 1;
    solver.random_seed = 0U;
    ASSERT_INT_EQ((int)lp_determinism_seed_offset(&solver, 19, 1009U), 0,
                  "seed offset disabled when random_seed=0");

    solver.random_seed = 11U;
    {
        unsigned int a1 = lp_determinism_seed_offset(&solver, 19, 1009U);
        unsigned int a2 = lp_determinism_seed_offset(&solver, 23, 1009U);
        solver.random_seed = 97U;
        unsigned int b1 = lp_determinism_seed_offset(&solver, 19, 1009U);
        unsigned int b2 = lp_determinism_seed_offset(&solver, 23, 1009U);
        ASSERT_TRUE((a1 != b1) || (a2 != b2),
                    "changing seed changes deterministic perturbation offset");
    }
}

static void test_runtime_determinism_param_propagation(void) {
    RalphModel *model = build_repeatable_lp();
    ASSERT_TRUE(model != NULL, "model created for runtime propagation");
    if (!model) return;

    ASSERT_TRUE(ralph_set_int_param(model, "telemetry", 0) == 0,
                "telemetry=0 accepted");
    ASSERT_TRUE(ralph_set_int_param(model, "deterministic", 1) == 0,
                "deterministic=1 accepted");
    ASSERT_TRUE(ralph_set_int_param(model, "random_seed", 42) == 0,
                "random_seed accepted");
    ASSERT_TRUE(ralph_set_int_param(model, "lp_threads", 2) == 0,
                "lp_threads accepted");
    ASSERT_TRUE(ralph_optimize_lp(model) == 0, "LP solve succeeds with determinism params");
    ASSERT_TRUE(ralph_get_status(model) == RALPH_STATUS_OPTIMAL, "LP solve is optimal");

    {
        SimplexSolver *solver = ralph_get_lp_solver(model);
        ASSERT_TRUE(solver != NULL, "LP solver exists after solve");
        ASSERT_TRUE(solver && solver->telemetry_enabled == 0,
                    "telemetry gate remains orthogonal to deterministic controls");
        ASSERT_TRUE(solver && solver->deterministic == 1,
                    "deterministic flag propagated to LP solver");
        ASSERT_TRUE(solver && solver->random_seed == 42U,
                    "random_seed propagated to LP solver");
        ASSERT_TRUE(solver && solver->lp_threads == 2,
                    "lp_threads propagated to LP solver");
        ASSERT_TRUE(solver && solver->determinism_effective_threads == 2,
                    "runtime effective LP threads captured");
    }

    ralph_free(model);
}

static void test_repeatability_contract(void) {
    const int runs = 5;
    RalphStatus status_ref = RALPH_STATUS_UNKNOWN;
    double obj_ref = 0.0;
    int iter_ref = 0;
    double x_ref[2] = {0.0, 0.0};

    for (int r = 0; r < runs; r++) {
        RalphModel *model = build_repeatable_lp();
        ASSERT_TRUE(model != NULL, "repeatability: model created");
        if (!model) continue;

        ASSERT_TRUE(ralph_set_int_param(model, "presolve", 0) == 0,
                    "repeatability: presolve=0 accepted");
        ASSERT_TRUE(ralph_set_int_param(model, "method", 0) == 0,
                    "repeatability: method=0 accepted");
        ASSERT_TRUE(ralph_set_int_param(model, "deterministic", 1) == 0,
                    "repeatability: deterministic=1 accepted");
        ASSERT_TRUE(ralph_set_int_param(model, "random_seed", 123) == 0,
                    "repeatability: random_seed accepted");
        ASSERT_TRUE(ralph_set_int_param(model, "lp_threads", 1) == 0,
                    "repeatability: lp_threads=1 accepted");
        ASSERT_TRUE(ralph_optimize_lp(model) == 0, "repeatability: LP solve succeeds");

        RalphStatus st = ralph_get_status(model);
        double obj = ralph_get_objval(model);
        int iter = ralph_get_iterations(model);
        double x[2] = {0.0, 0.0};
        (void)ralph_get_solution(model, x);

        if (r == 0) {
            status_ref = st;
            obj_ref = obj;
            iter_ref = iter;
            x_ref[0] = x[0];
            x_ref[1] = x[1];
        } else {
            ASSERT_INT_EQ(st, status_ref, "repeatability: status stable");
            ASSERT_DBL_NEAR(obj, obj_ref, 1e-10, "repeatability: objective stable");
            ASSERT_INT_EQ(iter, iter_ref, "repeatability: iteration count stable");
            ASSERT_DBL_NEAR(x[0], x_ref[0], 1e-10, "repeatability: x[0] stable");
            ASSERT_DBL_NEAR(x[1], x_ref[1], 1e-10, "repeatability: x[1] stable");
        }

        ralph_free(model);
    }
}

int main(void) {
    printf("=== LP Determinism Tests ===\n");

    test_determinism_metadata_and_lookup();
    test_determinism_helpers();
    test_runtime_determinism_param_propagation();
    test_repeatability_contract();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
