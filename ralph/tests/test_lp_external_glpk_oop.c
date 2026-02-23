/*
 * Tests for GLPK out-of-process external adapter registration + execution.
 *
 * Uses mock shell scripts (no GLPK dependency in unit test gate).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ralph.h"

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

static int write_mock_script(const char *script_body, char *path, size_t path_size) {
    int fd;
    FILE *f;

    if (!script_body || !path || path_size < 32) return -1;
    snprintf(path, path_size, "/tmp/ralph_glpk_mock_XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) return -1;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(path);
        return -1;
    }
    if (fputs(script_body, f) == EOF) {
        fclose(f);
        unlink(path);
        return -1;
    }
    if (fclose(f) != 0) {
        unlink(path);
        return -1;
    }
    if (chmod(path, 0700) != 0) {
        unlink(path);
        return -1;
    }
    return 0;
}

static RalphModel* build_small_lp(void) {
    RalphModel *model = ralph_create();
    if (!model) return NULL;

    ralph_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_set_int_param(model, "detect_special", 0);
    ralph_set_int_param(model, "presolve", 0);
    ralph_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    {
        int idx[] = {0};
        double val[] = {1.0};
        ralph_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }
    return model;
}

static void test_register_caps_and_unregister(void) {
    char script_path[256];
    const char *script =
        "#!/bin/sh\n"
        "exit 0\n";
    RalphLPExternalCapabilities caps;
    int rc_script;

    ralph_unregister_all_lp_external_adapters();
    rc_script = write_mock_script(script, script_path, sizeof(script_path));
    ASSERT_INT_EQ(rc_script, 0,
                  "register/caps: create mock script");
    if (rc_script != 0) return;

    ASSERT_INT_EQ(ralph_register_lp_external_glpk_oop(script_path), 0,
                  "register/caps: register GLPK OOP");
    ASSERT_INT_EQ(ralph_is_lp_external_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 1,
                  "register/caps: GLPK registered");

    memset(&caps, 0, sizeof(caps));
    ASSERT_INT_EQ(ralph_get_lp_external_provider_capabilities(RALPH_LP_EXTERNAL_PROVIDER_GLPK, &caps),
                  0,
                  "register/caps: query capabilities");
    ASSERT_INT_EQ(caps.supports_simplex, 1,
                  "register/caps: supports simplex");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "register/caps: supports dual simplex");
    ASSERT_INT_EQ(caps.supports_barrier, 0,
                  "register/caps: barrier disabled");

    ASSERT_INT_EQ(ralph_unregister_lp_external_glpk_oop(), 0,
                  "register/caps: unregister helper");
    ASSERT_INT_EQ(ralph_is_lp_external_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 0,
                  "register/caps: GLPK removed");
    unlink(script_path);
}

static void test_primal_simplex_oop_success_with_duals(void) {
    char script_path[256];
    const char *script =
        "#!/bin/sh\n"
        "wri=\"\"\n"
        "next_wri=0\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$next_wri\" = \"1\" ]; then wri=\"$arg\"; next_wri=0; continue; fi\n"
        "  if [ \"$arg\" = \"--write\" ]; then next_wri=1; continue; fi\n"
        "done\n"
        "if [ -z \"$wri\" ]; then exit 2; fi\n"
        "cat > \"$wri\" <<'EOF_WR'\n"
        "c Status:     OPTIMAL\n"
        "c Objective:  obj = 1 (MINimum)\n"
        "s bas 1 1 f f 1\n"
        "i 1 l 1 1\n"
        "j 1 b 1 0\n"
        "e o f\n"
        "EOF_WR\n"
        "echo \"  11 simplex iterations\"\n"
        "exit 0\n";
    RalphModel *model = NULL;
    double x = 0.0;
    double y = 0.0;
    double rc = 0.0;
    int rc_script;

    ralph_unregister_all_lp_external_adapters();
    rc_script = write_mock_script(script, script_path, sizeof(script_path));
    ASSERT_INT_EQ(rc_script, 0,
                  "primal/success: create mock script");
    if (rc_script != 0) return;
    ASSERT_INT_EQ(ralph_register_lp_external_glpk_oop(script_path), 0,
                  "primal/success: register GLPK OOP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "primal/success: model created");
    if (!model) {
        ralph_unregister_all_lp_external_adapters();
        unlink(script_path);
        return;
    }

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "primal/success: set external primal");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "primal/success: set provider GLPK");
    ASSERT_INT_EQ(ralph_optimize_lp(model), 0,
                  "primal/success: solve succeeds");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "primal/success: status optimal");
    ASSERT_DBL_CLOSE(ralph_get_objval(model), 1.0, 1e-9,
                     "primal/success: objective propagated");
    ASSERT_INT_EQ(ralph_get_iterations(model), 11,
                  "primal/success: iterations propagated");
    ASSERT_INT_EQ(ralph_get_solution(model, &x), 0,
                  "primal/success: solution available");
    ASSERT_DBL_CLOSE(x, 1.0, 1e-9,
                     "primal/success: solution propagated");
    ASSERT_INT_EQ(ralph_get_dual_solution(model, &y), 0,
                  "primal/success: dual solution available");
    ASSERT_DBL_CLOSE(y, 1.0, 1e-9,
                     "primal/success: dual propagated");
    ASSERT_INT_EQ(ralph_get_reduced_costs(model, &rc), 0,
                  "primal/success: reduced costs available");
    ASSERT_DBL_CLOSE(rc, 0.0, 1e-9,
                     "primal/success: reduced costs propagated");

    ralph_free(model);
    ralph_unregister_all_lp_external_adapters();
    unlink(script_path);
}

static void test_dual_simplex_routes_dual_flag(void) {
    char script_path[256];
    const char *script =
        "#!/bin/sh\n"
        "wri=\"\"\n"
        "mode=\"primal\"\n"
        "next_wri=0\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$next_wri\" = \"1\" ]; then wri=\"$arg\"; next_wri=0; continue; fi\n"
        "  if [ \"$arg\" = \"--write\" ]; then next_wri=1; continue; fi\n"
        "  if [ \"$arg\" = \"--dual\" ]; then mode=\"dual\"; fi\n"
        "done\n"
        "if [ -z \"$wri\" ]; then exit 2; fi\n"
        "if [ \"$mode\" != \"dual\" ]; then exit 9; fi\n"
        "cat > \"$wri\" <<'EOF_WR'\n"
        "c Status:     OPTIMAL\n"
        "c Objective:  obj = 2 (MINimum)\n"
        "s bas 1 1 f f 2\n"
        "i 1 l 2 2\n"
        "j 1 b 2 0\n"
        "e o f\n"
        "EOF_WR\n"
        "echo \"  17 simplex iterations\"\n"
        "exit 0\n";
    RalphModel *model = NULL;
    double x = 0.0;
    int rc_script;

    ralph_unregister_all_lp_external_adapters();
    rc_script = write_mock_script(script, script_path, sizeof(script_path));
    ASSERT_INT_EQ(rc_script, 0,
                  "dual/route: create mock script");
    if (rc_script != 0) return;
    ASSERT_INT_EQ(ralph_register_lp_external_glpk_oop(script_path), 0,
                  "dual/route: register GLPK OOP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "dual/route: model created");
    if (!model) {
        ralph_unregister_all_lp_external_adapters();
        unlink(script_path);
        return;
    }

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL),
                  0,
                  "dual/route: set external dual");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "dual/route: set provider GLPK");
    ASSERT_INT_EQ(ralph_optimize_lp(model), 0,
                  "dual/route: solve succeeds");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "dual/route: status optimal");
    ASSERT_INT_EQ(ralph_get_solution(model, &x), 0,
                  "dual/route: solution available");
    ASSERT_DBL_CLOSE(x, 2.0, 1e-9,
                     "dual/route: dual flag script output used");
    ASSERT_INT_EQ(ralph_get_iterations(model), 17,
                  "dual/route: iterations propagated");

    ralph_free(model);
    ralph_unregister_all_lp_external_adapters();
    unlink(script_path);
}

static void test_status_hints_for_infeasible_and_unbounded(void) {
    char inf_script[256];
    char unb_script[256];
    const char *inf_body =
        "#!/bin/sh\n"
        "wri=\"\"\n"
        "next_wri=0\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$next_wri\" = \"1\" ]; then wri=\"$arg\"; next_wri=0; continue; fi\n"
        "  if [ \"$arg\" = \"--write\" ]; then next_wri=1; continue; fi\n"
        "done\n"
        "if [ -z \"$wri\" ]; then exit 2; fi\n"
        "cat > \"$wri\" <<'EOF_WR'\n"
        "c Status:     UNDEFINED\n"
        "s bas 1 1 u u 0\n"
        "i 1 b 0 0\n"
        "j 1 l 0 0\n"
        "e o f\n"
        "EOF_WR\n"
        "echo \"PROBLEM HAS NO PRIMAL FEASIBLE SOLUTION\"\n"
        "exit 0\n";
    const char *unb_body =
        "#!/bin/sh\n"
        "wri=\"\"\n"
        "next_wri=0\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$next_wri\" = \"1\" ]; then wri=\"$arg\"; next_wri=0; continue; fi\n"
        "  if [ \"$arg\" = \"--write\" ]; then next_wri=1; continue; fi\n"
        "done\n"
        "if [ -z \"$wri\" ]; then exit 2; fi\n"
        "cat > \"$wri\" <<'EOF_WR'\n"
        "c Status:     UNDEFINED\n"
        "s bas 1 1 u u 0\n"
        "i 1 b 0 0\n"
        "j 1 l 0 0\n"
        "e o f\n"
        "EOF_WR\n"
        "echo \"PROBLEM HAS NO DUAL FEASIBLE SOLUTION\"\n"
        "exit 0\n";
    RalphModel *model = NULL;

    ralph_unregister_all_lp_external_adapters();
    ASSERT_INT_EQ(write_mock_script(inf_body, inf_script, sizeof(inf_script)), 0,
                  "status-hints: create infeasible script");
    ASSERT_INT_EQ(ralph_register_lp_external_glpk_oop(inf_script), 0,
                  "status-hints: register infeasible script");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "status-hints: infeasible model created");
    if (model) {
        ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                      0,
                      "status-hints: set ext primal (inf)");
        ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                      0,
                      "status-hints: set provider (inf)");
        ASSERT_INT_EQ(ralph_optimize_lp(model), 0,
                      "status-hints: infeasible solve returns success rc");
        ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_INFEASIBLE,
                      "status-hints: mapped infeasible");
        ralph_free(model);
    }
    ralph_unregister_all_lp_external_adapters();

    ASSERT_INT_EQ(write_mock_script(unb_body, unb_script, sizeof(unb_script)), 0,
                  "status-hints: create unbounded script");
    ASSERT_INT_EQ(ralph_register_lp_external_glpk_oop(unb_script), 0,
                  "status-hints: register unbounded script");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "status-hints: unbounded model created");
    if (model) {
        ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                      0,
                      "status-hints: set ext primal (unb)");
        ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                      0,
                      "status-hints: set provider (unb)");
        ASSERT_INT_EQ(ralph_optimize_lp(model), 0,
                      "status-hints: unbounded solve returns success rc");
        ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_UNBOUNDED,
                      "status-hints: mapped unbounded");
        ralph_free(model);
    }

    ralph_unregister_all_lp_external_adapters();
    unlink(inf_script);
    unlink(unb_script);
}

static void test_time_limit_maps_to_external_failure_report(void) {
    char script_path[256];
    const char *script =
        "#!/bin/sh\n"
        "wri=\"\"\n"
        "next_wri=0\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$next_wri\" = \"1\" ]; then wri=\"$arg\"; next_wri=0; continue; fi\n"
        "  if [ \"$arg\" = \"--write\" ]; then next_wri=1; continue; fi\n"
        "done\n"
        "if [ -z \"$wri\" ]; then exit 2; fi\n"
        "cat > \"$wri\" <<'EOF_WR'\n"
        "c Status:     TIME LIMIT EXCEEDED\n"
        "s bas 1 1 u u 0\n"
        "i 1 b 0 0\n"
        "j 1 l 0 0\n"
        "e o f\n"
        "EOF_WR\n"
        "echo \"TIME LIMIT EXCEEDED\"\n"
        "exit 0\n";
    RalphModel *model = NULL;
    RalphLPExternalFailureReport report;
    int rc_script;

    ralph_unregister_all_lp_external_adapters();
    rc_script = write_mock_script(script, script_path, sizeof(script_path));
    ASSERT_INT_EQ(rc_script, 0,
                  "time-limit: create mock script");
    if (rc_script != 0) return;
    ASSERT_INT_EQ(ralph_register_lp_external_glpk_oop(script_path), 0,
                  "time-limit: register GLPK OOP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "time-limit: model created");
    if (!model) {
        ralph_unregister_all_lp_external_adapters();
        unlink(script_path);
        return;
    }

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "time-limit: set external primal");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "time-limit: set provider GLPK");
    ASSERT_INT_EQ(ralph_optimize_lp(model), -1,
                  "time-limit: solve returns failure");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_TIME_LIMIT,
                  "time-limit: status mapped");

    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), 0,
                  "time-limit: failure report available");
    ASSERT_INT_EQ((int)report.stage, (int)RALPH_LP_EXTERNAL_FAILURE_STAGE_EXECUTION,
                  "time-limit: stage execution");
    ASSERT_INT_EQ((int)report.reason, (int)RALPH_LP_EXTERNAL_FAILURE_ADAPTER_TIME_LIMIT,
                  "time-limit: reason mapped");
    ASSERT_INT_EQ(report.adapter_return_code, (int)RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT,
                  "time-limit: adapter rc tracked");

    ralph_free(model);
    ralph_unregister_all_lp_external_adapters();
    unlink(script_path);
}

int main(void) {
    printf("=== LP External GLPK OOP Adapter Tests ===\n");

    test_register_caps_and_unregister();
    test_primal_simplex_oop_success_with_duals();
    test_dual_simplex_routes_dual_flag();
    test_status_hints_for_infeasible_and_unbounded();
    test_time_limit_maps_to_external_failure_report();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_run == tests_passed) ? 0 : 1;
}
