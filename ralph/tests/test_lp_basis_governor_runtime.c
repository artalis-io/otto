#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ralph_test_mod_api.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((int)(a) == (int)(b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

#define ASSERT_DBL_NEAR(a, b, eps, msg) do { \
    tests_run++; \
    if (fabs((double)(a) - (double)(b)) <= (eps)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12f vs %.12f)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

static RalphModel* build_phase12_case(void) {
    RalphModel *model = ralph_test_create();
    if (!model) {
        printf("  debug: create model failed\n");
        return NULL;
    }

    if (ralph_test_set_obj_sense(model, RALPH_MINIMIZE) != 0) {
        printf("  debug: set obj sense failed\n");
        ralph_test_free(model);
        return NULL;
    }

    /* Two variables with one equality row to exercise two-phase behavior. */
    if (ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS) < 0 ||
        ralph_test_add_var(model, 0.0, RALPH_INFINITY, 0.5, RALPH_CONTINUOUS) < 0) {
        printf("  debug: add vars failed\n");
        ralph_test_free(model);
        return NULL;
    }

    {
        int idx[] = {0, 1};
        double val[] = {1.0, -1.0};
        if (ralph_test_add_constraint(model, 2, idx, val, RALPH_EQUAL, 1.0) != 0) {
            printf("  debug: add equality failed\n");
            ralph_test_free(model);
            return NULL;
        }
    }

    return model;
}

static int shadow_total(const RalphLPSolverTelemetry *tel) {
    if (!tel) return 0;
    return tel->shadow_refactor_yes_phase1 +
           tel->shadow_refactor_yes_phase2 +
           tel->shadow_refactor_yes_dual +
           tel->shadow_refactor_no_phase1 +
           tel->shadow_refactor_no_phase2 +
           tel->shadow_refactor_no_dual +
           tel->shadow_backend_pick_markowitz +
           tel->shadow_backend_pick_supernode +
           tel->shadow_backend_pick_dense +
           tel->shadow_disagree_primal_refactor +
           tel->shadow_disagree_dual_refactor +
           tel->shadow_disagree_lu_backend;
}

static int solve_case(int mode,
                      int method,
                      int force_two_phase,
                      RalphStatus *status_out,
                      double *obj_out,
                      int *iters_out,
                      RalphLPSolverTelemetry *tel_out) {
    RalphModel *model = build_phase12_case();
    int rc = -1;
    if (!model || !status_out || !obj_out || !iters_out || !tel_out) {
        if (model) ralph_test_free(model);
        return -1;
    }

    memset(tel_out, 0, sizeof(*tel_out));

    if (ralph_test_set_int_param(model, "telemetry", 1) != 0) {
        printf("  debug: set telemetry failed\n");
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "verbose", 0) != 0) {
        printf("  debug: set verbose failed\n");
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "presolve", 0) != 0) {
        printf("  debug: set presolve failed\n");
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "deterministic", 1) != 0) {
        printf("  debug: set deterministic failed\n");
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "random_seed", 7) != 0) {
        printf("  debug: set random_seed failed\n");
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "lp_threads", 1) != 0) {
        printf("  debug: set lp_threads failed\n");
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "method", method) != 0) {
        printf("  debug: set method failed (%d)\n", method);
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "force_two_phase", force_two_phase) != 0) {
        printf("  debug: set force_two_phase failed (%d)\n", force_two_phase);
        ralph_test_free(model);
        return -1;
    }
    if (ralph_test_set_int_param(model, "lp_basis_governor_mode", mode) != 0) {
        printf("  debug: set lp_basis_governor_mode failed (%d)\n", mode);
        ralph_test_free(model);
        return -1;
    }

    rc = ralph_test_optimize_lp(model);
    if (rc != 0) {
        printf("  debug: optimize rc=%d (mode=%d method=%d two_phase=%d)\n",
               rc, mode, method, force_two_phase);
    }
    *status_out = ralph_test_get_status(model);
    *obj_out = ralph_test_get_objval(model);
    *iters_out = ralph_test_get_iterations(model);
    if (ralph_core_get_last_lp_telemetry(model, tel_out) != 0) {
        ralph_test_free(model);
        return -1;
    }

    ralph_test_free(model);
    return rc;
}

static void test_mode_off_vs_shadow_runtime(void) {
    RalphStatus st_off = RALPH_STATUS_UNKNOWN, st_shadow = RALPH_STATUS_UNKNOWN;
    double obj_off = 0.0, obj_shadow = 0.0;
    int it_off = -1, it_shadow = -1;
    RalphLPSolverTelemetry tel_off, tel_shadow;

    printf("  basis-governor runtime: off vs shadow...\n");
    ASSERT_INT_EQ(solve_case(0, 0, 1, &st_off, &obj_off, &it_off, &tel_off), 0,
                  "solve off mode succeeds");
    ASSERT_INT_EQ(solve_case(1, 0, 1, &st_shadow, &obj_shadow, &it_shadow, &tel_shadow), 0,
                  "solve shadow mode succeeds");
    ASSERT_INT_EQ(st_off, RALPH_STATUS_OPTIMAL, "off mode status optimal");
    ASSERT_INT_EQ(st_shadow, RALPH_STATUS_OPTIMAL, "shadow mode status optimal");
    ASSERT_DBL_NEAR(obj_off, obj_shadow, 1e-9, "off/shadow objective parity");
    ASSERT_INT_EQ(tel_off.basis_governor_mode, 0, "off telemetry mode");
    ASSERT_INT_EQ(tel_shadow.basis_governor_mode, 1, "shadow telemetry mode");
    ASSERT_INT_EQ(shadow_total(&tel_off), 0, "off mode has no shadow activity");
    ASSERT(shadow_total(&tel_shadow) > 0,
           "shadow mode records shadow activity");
}

static void test_mode_control_phase2_dual_unchanged(void) {
    RalphStatus st_shadow = RALPH_STATUS_UNKNOWN, st_ctrl = RALPH_STATUS_UNKNOWN;
    double obj_shadow = 0.0, obj_ctrl = 0.0;
    int it_shadow = -1, it_ctrl = -1;
    RalphLPSolverTelemetry tel_shadow, tel_ctrl;

    printf("  basis-governor runtime: dual unaffected by control_phase2...\n");
    ASSERT_INT_EQ(solve_case(1, 1, 0, &st_shadow, &obj_shadow, &it_shadow, &tel_shadow), 0,
                  "dual shadow solve succeeds");
    ASSERT_INT_EQ(solve_case(2, 1, 0, &st_ctrl, &obj_ctrl, &it_ctrl, &tel_ctrl), 0,
                  "dual control_phase2 solve succeeds");
    ASSERT_INT_EQ(st_shadow, st_ctrl, "dual status parity shadow vs control_phase2");
    ASSERT_DBL_NEAR(obj_shadow, obj_ctrl, 1e-9, "dual objective parity shadow vs control_phase2");
    ASSERT_INT_EQ(it_shadow, it_ctrl, "dual iterations unchanged in control_phase2");
    ASSERT_INT_EQ(tel_shadow.shadow_refactor_yes_dual, tel_ctrl.shadow_refactor_yes_dual,
                  "dual yes-refactor telemetry unchanged");
    ASSERT_INT_EQ(tel_shadow.shadow_refactor_no_dual, tel_ctrl.shadow_refactor_no_dual,
                  "dual no-refactor telemetry unchanged");
    ASSERT_INT_EQ(tel_shadow.shadow_disagree_dual_refactor, tel_ctrl.shadow_disagree_dual_refactor,
                  "dual disagree telemetry unchanged");
}

static void test_mode_control_phase2_preserves_phase1(void) {
    RalphStatus st_shadow = RALPH_STATUS_UNKNOWN, st_ctrl = RALPH_STATUS_UNKNOWN;
    double obj_shadow = 0.0, obj_ctrl = 0.0;
    int it_shadow = -1, it_ctrl = -1;
    RalphLPSolverTelemetry tel_shadow, tel_ctrl;

    printf("  basis-governor runtime: phase1 unchanged by control_phase2...\n");
    ASSERT_INT_EQ(solve_case(1, 0, 1, &st_shadow, &obj_shadow, &it_shadow, &tel_shadow), 0,
                  "phase1 shadow solve succeeds");
    ASSERT_INT_EQ(solve_case(2, 0, 1, &st_ctrl, &obj_ctrl, &it_ctrl, &tel_ctrl), 0,
                  "phase1 control_phase2 solve succeeds");
    ASSERT_INT_EQ(st_shadow, st_ctrl, "phase1 status parity shadow vs control_phase2");
    ASSERT_DBL_NEAR(obj_shadow, obj_ctrl, 1e-9, "phase1 objective parity shadow vs control_phase2");
    ASSERT_INT_EQ(tel_shadow.perf_phase1_pricing_calls, tel_ctrl.perf_phase1_pricing_calls,
                  "phase1 pricing activity unchanged");
    ASSERT_INT_EQ(tel_shadow.shadow_refactor_yes_phase1, tel_ctrl.shadow_refactor_yes_phase1,
                  "phase1 yes-refactor telemetry unchanged");
    ASSERT_INT_EQ(tel_shadow.shadow_refactor_no_phase1, tel_ctrl.shadow_refactor_no_phase1,
                  "phase1 no-refactor telemetry unchanged");
    ASSERT_INT_EQ(tel_shadow.perf_phase1_refactor_calls, tel_ctrl.perf_phase1_refactor_calls,
                  "phase1 refactor calls unchanged");
}

int main(void) {
    printf("=== LP Basis Governor Runtime Tests ===\n");
    test_mode_off_vs_shadow_runtime();
    test_mode_control_phase2_dual_unchanged();
    test_mode_control_phase2_preserves_phase1();
    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
