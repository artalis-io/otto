#include <stdio.h>
#include <math.h>
#include "lp.h"

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

static int build_flip_fixture(SimplexSolver **solver_out,
                              SimplexTableau **tab_out,
                              LPModel **model_out) {
    LPModel *model = NULL;
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    int idx[2] = {0, 1};
    double val[2] = {-1.0, -1.0};

    if (!solver_out || !tab_out || !model_out) return -1;
    *solver_out = NULL;
    *tab_out = NULL;
    *model_out = NULL;

    model = lp_model_create();
    if (!model) return -1;
    model->obj_sense = RALPH_MINIMIZE;

    if (lp_model_add_var(model, 0.0, 1.0, 1e-8, 'C') < 0) goto fail;
    if (lp_model_add_var(model, 0.0, 1.0, 2.0, 'C') < 0) goto fail;
    if (lp_model_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, -1.0) < 0) goto fail;
    if (lp_model_finalize(model) != 0) goto fail;

    solver = simplex_create(model);
    if (!solver) goto fail;
    tab = tableau_create(model);
    if (!tab) goto fail;

    solver->tableau = tab;
    tab->owner = solver;
    solver->telemetry_enabled = 1;
    solver->use_dual_bound_flip = 1;
    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_FLIP;

    if (tableau_refactorize(tab) != 0) goto fail;
    if (tableau_compute_solution(tab) != 0) goto fail;
    if (tableau_compute_reduced_costs(tab) != 0) goto fail;

    /* Seed a deterministic dual-ratio scenario:
     * - Leaving basic row is infeasible below its lower bound.
     * - Both structural vars stay non-basic at lower bounds.
     * - rc[0] is tiny-positive (eligible for flip window), rc[1] is large.
     */
    {
        int leaving_var = tab->basis[0];
        tab->ub_ext[leaving_var] = 0.0;
        tab->x[leaving_var] = tab->ub_ext[leaving_var] + 0.5;
    }
    tab->var_status[0] = RALPH_NONBASIC_LOWER;
    tab->var_status[1] = RALPH_NONBASIC_LOWER;
    tab->x[0] = tab->lb_ext[0];
    tab->x[1] = tab->lb_ext[1];
    tab->rc[0] = 1e-8;
    tab->rc[1] = 2.0;

    *solver_out = solver;
    *tab_out = tab;
    *model_out = model;
    return 0;

fail:
    if (solver) simplex_free(solver);
    else if (tab) tableau_free(tab);
    if (model) lp_model_free(model);
    return -1;
}

static void free_flip_fixture(SimplexSolver *solver, LPModel *model) {
    if (solver) simplex_free(solver);
    if (model) lp_model_free(model);
}

static void test_flip_mode_applies_flip_only_step(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    /* One constraint => leaving row is position 0. */
    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test succeeds in flip mode");
    ASSERT_TRUE(entering == -2, "flip mode returns entering=-2 for flip-only step");
    ASSERT_TRUE(fabs(theta) < 1e-12, "flip-only step returns theta=0");
    ASSERT_TRUE(tab->var_status[0] == RALPH_NONBASIC_UPPER, "boxed candidate flips to upper bound");
    ASSERT_TRUE(fabs(tab->x[0] - tab->ub_ext[0]) < 1e-12, "flipped candidate value snapped to upper bound");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_applied >= 1,
                "dual flip telemetry increments");

    free_flip_fixture(solver, model);
}

static void test_harris_mode_keeps_regular_entering(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_HARRIS;
    solver->use_dual_bound_flip = 1;
    solver->telemetry.perf_dual_bound_flip_applied = 0;

    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test succeeds in Harris mode");
    ASSERT_TRUE(entering >= 0, "Harris mode returns a normal entering column");
    ASSERT_TRUE(entering != -2, "Harris mode does not produce flip-only step");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_applied == 0,
                "Harris mode leaves dual flip telemetry unchanged");

    free_flip_fixture(solver, model);
}

static void test_flip_mode_respects_runtime_disable(void) {
    SimplexSolver *solver = NULL;
    SimplexTableau *tab = NULL;
    LPModel *model = NULL;
    int entering = -99;
    double theta = -1.0;
    int rc;

    if (build_flip_fixture(&solver, &tab, &model) != 0) {
        ASSERT_TRUE(0, "fixture build");
        return;
    }

    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_FLIP;
    solver->use_dual_bound_flip = 0;
    solver->telemetry.perf_dual_bound_flip_applied = 0;

    rc = dual_ratio_test(tab, 0, &entering, &theta);
    ASSERT_TRUE(rc == 0, "dual_ratio_test succeeds when flip runtime is disabled");
    ASSERT_TRUE(entering >= 0, "flip runtime disable falls back to regular entering");
    ASSERT_TRUE(entering != -2, "flip runtime disable avoids flip-only step");
    ASSERT_TRUE(solver->telemetry.perf_dual_bound_flip_applied == 0,
                "runtime-disabled flip mode does not apply flips");

    free_flip_fixture(solver, model);
}

int main(void) {
    printf("=== Dual Ratio Flip Tests ===\n");
    test_flip_mode_applies_flip_only_step();
    test_harris_mode_keeps_regular_entering();
    test_flip_mode_respects_runtime_disable();
    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
