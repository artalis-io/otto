/*
 * Tests for GLPK out-of-process external adapter registration + execution.
 *
 * No GLPK dependency in the unit test gate: this binary impersonates glpsol
 * when armed through the environment. See "Mock glpsol" below.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _MSC_VER
  /* MSVC has no <unistd.h>; <io.h> declares the same POSIX I/O names. */
  #include <io.h>
#else
  #include <unistd.h>
#endif
#include <sys/stat.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include "../src/lp_external_oop.h"
#include "test_tmp.h"

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

#define ASSERT_DBL_CLOSE(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) <= (tol)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12g != %.12g)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * Mock glpsol
 *
 * These tests need a program that behaves like glpsol without depending on
 * one. That used to be a "#!/bin/sh" script written to a temp file, which
 * cannot work on Windows: CreateProcess has no shebang handling, so the whole
 * suite below was skipped there and the adapter went unexercised on the one
 * platform whose process runner was newest.
 *
 * So this binary impersonates glpsol instead, re-executed by the adapter under
 * test. The scenario cannot travel in argv -- the adapter builds glpsol's
 * argument list itself and has no slot for a test flag -- so it travels in the
 * environment, which the child inherits either way.
 * ------------------------------------------------------------------------ */

#define MOCK_ENV "RALPH_MOCK_GLPSOL"

/* Path this binary can be re-executed by; see self_exe_path(). */
static char self_exe[1024];

static int self_exe_path(char *buf, size_t size, const char *argv0)
{
#ifdef _WIN32
    (void)argv0;
    if (GetModuleFileNameA(NULL, buf, (DWORD)size) == 0) return -1;
    return 0;
#else
    if (!argv0 || argv0[0] == '\0') return -1;
    if (snprintf(buf, size, "%s", argv0) >= (int)size) return -1;
    return 0;
#endif
}

/* Returns 0 on success, matching what write_mock_script() used to return, so
 * the call sites keep their shape. */
static int arm_mock(const char *scenario) {
    return sh_pal_setenv(MOCK_ENV, scenario);
}

static void disarm_mock(void) {
    (void)sh_pal_setenv(MOCK_ENV, "");
}

/* The GLPK --write payload each scenario produces. */
static const char *mock_write_body(const char *scenario) {
    if (strcmp(scenario, "dual") == 0) {
        return "c Status:     OPTIMAL\n"
               "c Objective:  obj = 2 (MINimum)\n"
               "s bas 1 1 f f 2\n"
               "i 1 l 2 2\n"
               "j 1 b 2 0\n"
               "e o f\n";
    }
    if (strcmp(scenario, "timelimit") == 0) {
        return "c Status:     TIME LIMIT EXCEEDED\n"
               "s bas 1 1 u u 0\n"
               "i 1 b 0 0\n"
               "j 1 l 0 0\n"
               "e o f\n";
    }
    if (strcmp(scenario, "infeasible") == 0 || strcmp(scenario, "unbounded") == 0) {
        /* Both are UNDEFINED in the file; they differ only in the stdout line,
         * which is where the adapter reads the hint from. */
        return "c Status:     UNDEFINED\n"
               "s bas 1 1 u u 0\n"
               "i 1 b 0 0\n"
               "j 1 l 0 0\n"
               "e o f\n";
    }
    /* primal and offset */
    return "c Status:     OPTIMAL\n"
           "c Objective:  obj = 1 (MINimum)\n"
           "s bas 1 1 f f 1\n"
           "i 1 l 1 1\n"
           "j 1 b 1 0\n"
           "e o f\n";
}

/* The one line each scenario prints. The adapter parses iteration counts and
 * status hints out of glpsol's stdout, so these are load-bearing. */
static const char *mock_stdout_line(const char *scenario) {
    if (strcmp(scenario, "primal") == 0)     return "  11 simplex iterations";
    if (strcmp(scenario, "dual") == 0)       return "  17 simplex iterations";
    if (strcmp(scenario, "offset") == 0)     return "  9 simplex iterations";
    if (strcmp(scenario, "infeasible") == 0) return "PROBLEM HAS NO PRIMAL FEASIBLE SOLUTION";
    if (strcmp(scenario, "unbounded") == 0)  return "PROBLEM HAS NO DUAL FEASIBLE SOLUTION";
    if (strcmp(scenario, "timelimit") == 0)  return "TIME LIMIT EXCEEDED";
    return NULL;
}

static int run_mock_glpsol(int argc, char **argv, const char *scenario) {
    const char *write_path = NULL;
    const char *line;
    FILE *f;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* The capabilities test registers an adapter and never solves, so the
     * child is only ever spawned by accident. Exiting 0 keeps that harmless. */
    if (strcmp(scenario, "caps") == 0) return 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--write") == 0 && i + 1 < argc) {
            write_path = argv[i + 1];
            i++;
        }
    }
    if (!write_path) return 2;

    if (strcmp(scenario, "dual") == 0) {
        int saw_dual = 0;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--dual") == 0) saw_dual = 1;
        }
        /* The point of that test: --dual must actually reach glpsol. */
        if (!saw_dual) return 9;
    }

    f = fopen(write_path, "w");
    if (!f) return 3;
    fputs(mock_write_body(scenario), f);
    if (fclose(f) != 0) return 3;

    line = mock_stdout_line(scenario);
    if (line) printf("%s\n", line);
    return 0;
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

static void test_register_caps_and_unregister(void) {
    RalphLPExternalCapabilities caps;
    int rc_script;

    ralph_lp_external_unregister_all_adapters();
    rc_script = arm_mock("caps");
    ASSERT_INT_EQ(rc_script, 0,
                  "register/caps: create mock script");
    if (rc_script != 0) return;

    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(self_exe), 0,
                  "register/caps: register GLPK OOP");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 1,
                  "register/caps: GLPK registered");

    memset(&caps, 0, sizeof(caps));
    ASSERT_INT_EQ(ralph_lp_external_provider_capabilities(RALPH_LP_EXTERNAL_PROVIDER_GLPK, &caps),
                  0,
                  "register/caps: query capabilities");
    ASSERT_INT_EQ(caps.supports_simplex, 1,
                  "register/caps: supports simplex");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "register/caps: supports dual simplex");
    ASSERT_INT_EQ(caps.supports_barrier, 0,
                  "register/caps: barrier disabled");

    ASSERT_INT_EQ(ralph_lp_external_unregister_glpk_oop(), 0,
                  "register/caps: unregister helper");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 0,
                  "register/caps: GLPK removed");
    disarm_mock();
}

static void test_primal_simplex_oop_success_with_duals(void) {
    RalphModel *model = NULL;
    double x = 0.0;
    double y = 0.0;
    double rc = 0.0;
    int rc_script;

    ralph_lp_external_unregister_all_adapters();
    rc_script = arm_mock("primal");
    ASSERT_INT_EQ(rc_script, 0,
                  "primal/success: create mock script");
    if (rc_script != 0) return;
    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(self_exe), 0,
                  "primal/success: register GLPK OOP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "primal/success: model created");
    if (!model) {
        ralph_lp_external_unregister_all_adapters();
        disarm_mock();
        return;
    }

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "primal/success: set external primal");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "primal/success: set provider GLPK");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "primal/success: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "primal/success: status optimal");
    ASSERT_DBL_CLOSE(ralph_test_get_objval(model), 1.0, 1e-9,
                     "primal/success: objective propagated");
    ASSERT_INT_EQ(ralph_test_get_iterations(model), 11,
                  "primal/success: iterations propagated");
    ASSERT_INT_EQ(ralph_test_get_solution(model, &x), 0,
                  "primal/success: solution available");
    ASSERT_DBL_CLOSE(x, 1.0, 1e-9,
                     "primal/success: solution propagated");
    ASSERT_INT_EQ(ralph_test_get_dual_solution(model, &y), 0,
                  "primal/success: dual solution available");
    ASSERT_DBL_CLOSE(y, 1.0, 1e-9,
                     "primal/success: dual propagated");
    ASSERT_INT_EQ(ralph_test_get_reduced_costs(model, &rc), 0,
                  "primal/success: reduced costs available");
    ASSERT_DBL_CLOSE(rc, 0.0, 1e-9,
                     "primal/success: reduced costs propagated");

    ralph_test_free(model);
    ralph_lp_external_unregister_all_adapters();
    disarm_mock();
}

static void test_dual_simplex_routes_dual_flag(void) {
    RalphModel *model = NULL;
    double x = 0.0;
    int rc_script;

    ralph_lp_external_unregister_all_adapters();
    rc_script = arm_mock("dual");
    ASSERT_INT_EQ(rc_script, 0,
                  "dual/route: create mock script");
    if (rc_script != 0) return;
    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(self_exe), 0,
                  "dual/route: register GLPK OOP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "dual/route: model created");
    if (!model) {
        ralph_lp_external_unregister_all_adapters();
        disarm_mock();
        return;
    }

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL),
                  0,
                  "dual/route: set external dual");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "dual/route: set provider GLPK");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "dual/route: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "dual/route: status optimal");
    ASSERT_INT_EQ(ralph_test_get_solution(model, &x), 0,
                  "dual/route: solution available");
    ASSERT_DBL_CLOSE(x, 2.0, 1e-9,
                     "dual/route: dual flag script output used");
    ASSERT_INT_EQ(ralph_test_get_iterations(model), 17,
                  "dual/route: iterations propagated");

    ralph_test_free(model);
    ralph_lp_external_unregister_all_adapters();
    disarm_mock();
}

static void test_external_objective_includes_model_offset(void) {
    RalphModel *model = NULL;
    int rc_script;

    ralph_lp_external_unregister_all_adapters();
    rc_script = arm_mock("offset");
    ASSERT_INT_EQ(rc_script, 0,
                  "objective-offset: create mock script");
    if (rc_script != 0) return;
    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(self_exe), 0,
                  "objective-offset: register GLPK OOP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "objective-offset: model created");
    if (!model) {
        ralph_lp_external_unregister_all_adapters();
        disarm_mock();
        return;
    }

    ASSERT_INT_EQ(ralph_test_set_obj_offset(model, 7.25), 0,
                  "objective-offset: set model objective offset");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "objective-offset: set external primal");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "objective-offset: set provider GLPK");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "objective-offset: solve succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "objective-offset: status optimal");
    ASSERT_DBL_CLOSE(ralph_test_get_objval(model), 8.25, 1e-9,
                     "objective-offset: external objective includes model offset");

    ralph_test_free(model);
    ralph_lp_external_unregister_all_adapters();
    disarm_mock();
}

static void test_status_hints_for_infeasible_and_unbounded(void) {
    RalphModel *model = NULL;

    ralph_lp_external_unregister_all_adapters();
    ASSERT_INT_EQ(arm_mock("infeasible"), 0,
                  "status-hints: create infeasible script");
    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(self_exe), 0,
                  "status-hints: register infeasible script");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "status-hints: infeasible model created");
    if (model) {
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                      0,
                      "status-hints: set ext primal (inf)");
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                      0,
                      "status-hints: set provider (inf)");
        ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                      "status-hints: infeasible solve returns success rc");
        ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_INFEASIBLE,
                      "status-hints: mapped infeasible");
        ralph_test_free(model);
    }
    ralph_lp_external_unregister_all_adapters();

    ASSERT_INT_EQ(arm_mock("unbounded"), 0,
                  "status-hints: create unbounded script");
    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(self_exe), 0,
                  "status-hints: register unbounded script");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "status-hints: unbounded model created");
    if (model) {
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                      0,
                      "status-hints: set ext primal (unb)");
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                      0,
                      "status-hints: set provider (unb)");
        ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                      "status-hints: unbounded solve returns success rc");
        ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_UNBOUNDED,
                      "status-hints: mapped unbounded");
        ralph_test_free(model);
    }

    ralph_lp_external_unregister_all_adapters();
    disarm_mock();
    disarm_mock();
}

static void test_time_limit_maps_to_external_failure_report(void) {
    RalphModel *model = NULL;
    RalphLPExternalFailureReport report;
    int rc_script;

    ralph_lp_external_unregister_all_adapters();
    rc_script = arm_mock("timelimit");
    ASSERT_INT_EQ(rc_script, 0,
                  "time-limit: create mock script");
    if (rc_script != 0) return;
    ASSERT_INT_EQ(ralph_lp_external_register_glpk_oop(self_exe), 0,
                  "time-limit: register GLPK OOP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "time-limit: model created");
    if (!model) {
        ralph_lp_external_unregister_all_adapters();
        disarm_mock();
        return;
    }

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "time-limit: set external primal");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "time-limit: set provider GLPK");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                  "time-limit: solve returns failure");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_TIME_LIMIT,
                  "time-limit: status mapped");

    ASSERT_INT_EQ(ralph_lp_get_last_external_failure_report(model, &report), 0,
                  "time-limit: failure report available");
    ASSERT_INT_EQ((int)report.stage, (int)RALPH_LP_EXTERNAL_FAILURE_STAGE_EXECUTION,
                  "time-limit: stage execution");
    ASSERT_INT_EQ((int)report.reason, (int)RALPH_LP_EXTERNAL_FAILURE_ADAPTER_TIME_LIMIT,
                  "time-limit: reason mapped");
    ASSERT_INT_EQ(report.adapter_return_code, (int)RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT,
                  "time-limit: adapter rc tracked");

    ralph_test_free(model);
    ralph_lp_external_unregister_all_adapters();
    disarm_mock();
}

int main(int argc, char **argv) {
    const char *scenario = getenv(MOCK_ENV);

    /* Armed means this process is the child: behave like glpsol and exit. The
     * parent is started by the Makefile with the variable unset, so it always
     * falls through to the tests. */
    if (scenario && scenario[0]) return run_mock_glpsol(argc, argv, scenario);

    printf("=== LP External GLPK OOP Adapter Tests ===\n");

    if (self_exe_path(self_exe, sizeof(self_exe), argc > 0 ? argv[0] : NULL) != 0) {
        printf("  FAIL: could not determine own executable path\n");
        return 1;
    }

    test_register_caps_and_unregister();
    test_primal_simplex_oop_success_with_duals();
    test_dual_simplex_routes_dual_flag();
    test_external_objective_includes_model_offset();
    test_status_hints_for_infeasible_and_unbounded();
    test_time_limit_maps_to_external_failure_report();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_run == tests_passed) ? 0 : 1;
}
