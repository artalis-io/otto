/*
 * Tests for LP external failure mapping/report API surface.
 *
 * This module is intentionally separate from dispatch and adapter lifecycle tests.
 */

#include <stdio.h>
#include <string.h>

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

typedef struct {
    RalphLPExternalCapabilities caps;
    int caps_rc;
    int solve_rc;
    int solve_calls;
    RalphLPExternalBackendKind last_backend;
} FailureFixture;

static int failure_fixture_get_capabilities(RalphLPExternalCapabilities *caps, void *user_data) {
    FailureFixture *fx = (FailureFixture*)user_data;
    if (!fx || !caps) return -1;
    *caps = fx->caps;
    return fx->caps_rc;
}

static int failure_fixture_solve(RalphLPExternalBackendKind backend,
                                 void *solver_handle,
                                 void *user_data) {
    FailureFixture *fx = (FailureFixture*)user_data;
    if (!fx || !solver_handle) return -1;
    fx->solve_calls++;
    fx->last_backend = backend;
    return fx->solve_rc;
}

static RalphLPExternalAdapter build_failure_adapter(FailureFixture *fx,
                                                    RalphLPExternalProvider provider,
                                                    const char *provider_name) {
    RalphLPExternalAdapter adapter;
    memset(&adapter, 0, sizeof(adapter));
    adapter.abi_version = RALPH_LP_EXTERNAL_ADAPTER_ABI_VERSION;
    adapter.provider = provider;
    adapter.provider_name = provider_name;
    adapter.get_capabilities = failure_fixture_get_capabilities;
    adapter.solve = failure_fixture_solve;
    adapter.user_data = fx;
    return adapter;
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

static void test_guards_and_no_external_failure(void) {
    RalphModel *model = build_small_lp();
    RalphModel *mip = ralph_create();
    RalphLPExternalFailureReport report;

    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(NULL, &report), -1,
                  "guards: NULL model rejected");
    ASSERT_TRUE(model != NULL, "guards: LP model created");
    if (!model) {
        if (mip) ralph_free(mip);
        return;
    }
    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, NULL), -1,
                  "guards: NULL output rejected");
    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), -1,
                  "guards: report unavailable before solve");

    ASSERT_INT_EQ(ralph_optimize_lp(model), 0,
                  "guards: default LP solve succeeds");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "guards: default LP status optimal");
    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), -1,
                  "guards: no external failure report on internal solve");

    ASSERT_INT_EQ(ralph_set_obj_coef(model, 0, 2.0), 0,
                  "guards: model edit succeeds");
    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), -1,
                  "guards: report unavailable after invalidation");

    ASSERT_TRUE(mip != NULL, "guards: MIP model created");
    if (mip) {
        ralph_set_obj_sense(mip, RALPH_MAXIMIZE);
        ralph_add_var(mip, 0.0, 1.0, 1.0, RALPH_BINARY);
        ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(mip, &report), -1,
                      "guards: report rejected for MIP model");
        ralph_free(mip);
    }

    ralph_free(model);
}

static void test_dispatch_failure_provider_required_nonfatal(void) {
    RalphModel *model = build_small_lp();
    RalphLPExternalFailureReport report;

    ralph_unregister_all_lp_external_adapters();
    ASSERT_TRUE(model != NULL, "dispatch/provider-required: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "dispatch/provider-required: request external primal");
    ASSERT_INT_EQ(ralph_optimize_lp(model), 0,
                  "dispatch/provider-required: falls back and solves");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "dispatch/provider-required: status optimal");

    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), 0,
                  "dispatch/provider-required: report available");
    ASSERT_INT_EQ((int)report.stage, (int)RALPH_LP_EXTERNAL_FAILURE_STAGE_DISPATCH,
                  "dispatch/provider-required: stage dispatch");
    ASSERT_INT_EQ((int)report.reason, (int)RALPH_LP_EXTERNAL_FAILURE_PROVIDER_REQUIRED,
                  "dispatch/provider-required: reason provider required");
    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL,
                  "dispatch/provider-required: requested algorithm tracked");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "dispatch/provider-required: effective fallback algorithm tracked");
    ASSERT_INT_EQ((int)report.requested_provider, (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                  "dispatch/provider-required: requested provider tracked");
    ASSERT_INT_EQ((int)report.effective_provider, (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                  "dispatch/provider-required: effective provider none");
    ASSERT_INT_EQ((int)report.backend, (int)RALPH_LP_EXTERNAL_BACKEND_SIMPLEX,
                  "dispatch/provider-required: requested backend tracked");
    ASSERT_INT_EQ((int)report.fallback_reason,
                  (int)RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE,
                  "dispatch/provider-required: fallback reason tracked");
    ASSERT_INT_EQ(report.adapter_return_code, 0,
                  "dispatch/provider-required: adapter rc is zero");
    ASSERT_INT_EQ((int)report.mapped_status, (int)RALPH_STATUS_UNKNOWN,
                  "dispatch/provider-required: mapped status unknown for non-fatal fallback");
    ASSERT_INT_EQ(report.fatal, 0,
                  "dispatch/provider-required: fallback is non-fatal");

    ralph_free(model);
}

static void test_dispatch_failure_provider_unregistered_strict(void) {
    RalphModel *model = build_small_lp();
    RalphLPExternalFailureReport report;

    ralph_unregister_all_lp_external_adapters();
    ASSERT_TRUE(model != NULL, "dispatch/provider-unregistered: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "dispatch/provider-unregistered: request external primal");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "dispatch/provider-unregistered: set provider GLPK");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1),
                  0,
                  "dispatch/provider-unregistered: enable strict mode");
    ASSERT_INT_EQ(ralph_optimize_lp(model), -1,
                  "dispatch/provider-unregistered: strict mode fails solve");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_ERROR,
                  "dispatch/provider-unregistered: status error");

    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), 0,
                  "dispatch/provider-unregistered: report available");
    ASSERT_INT_EQ((int)report.stage, (int)RALPH_LP_EXTERNAL_FAILURE_STAGE_DISPATCH,
                  "dispatch/provider-unregistered: stage dispatch");
    ASSERT_INT_EQ((int)report.reason,
                  (int)RALPH_LP_EXTERNAL_FAILURE_PROVIDER_UNREGISTERED,
                  "dispatch/provider-unregistered: reason provider unregistered");
    ASSERT_INT_EQ(report.fatal, 1,
                  "dispatch/provider-unregistered: strict failure is fatal");
    ASSERT_INT_EQ((int)report.mapped_status, (int)RALPH_STATUS_ERROR,
                  "dispatch/provider-unregistered: mapped status error");

    ralph_free(model);
}

static void test_dispatch_failure_backend_unsupported(void) {
    RalphModel *model = build_small_lp();
    RalphLPExternalFailureReport report;
    FailureFixture fx;
    RalphLPExternalAdapter adapter;

    memset(&fx, 0, sizeof(fx));
    fx.caps.supports_simplex = 1;
    fx.caps.supports_dual_simplex = 0;
    fx.caps.supports_barrier = 0;
    fx.caps.supports_crossover = 0;
    fx.caps_rc = 0;
    fx.solve_rc = RALPH_LP_EXTERNAL_ADAPTER_RC_OK;
    adapter = build_failure_adapter(&fx, RALPH_LP_EXTERNAL_PROVIDER_GLPK, "FailureGLPK");

    ralph_unregister_all_lp_external_adapters();
    ASSERT_INT_EQ(ralph_register_lp_external_adapter(&adapter), 0,
                  "dispatch/backend-unsupported: register GLPK fixture");
    ASSERT_TRUE(model != NULL, "dispatch/backend-unsupported: model created");
    if (!model) {
        ralph_unregister_all_lp_external_adapters();
        return;
    }

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL),
                  0,
                  "dispatch/backend-unsupported: request external dual");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "dispatch/backend-unsupported: set provider GLPK");
    ASSERT_INT_EQ(ralph_optimize_lp(model), 0,
                  "dispatch/backend-unsupported: fallback solve succeeds");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "dispatch/backend-unsupported: status optimal");

    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), 0,
                  "dispatch/backend-unsupported: report available");
    ASSERT_INT_EQ((int)report.stage, (int)RALPH_LP_EXTERNAL_FAILURE_STAGE_DISPATCH,
                  "dispatch/backend-unsupported: stage dispatch");
    ASSERT_INT_EQ((int)report.reason,
                  (int)RALPH_LP_EXTERNAL_FAILURE_BACKEND_UNSUPPORTED,
                  "dispatch/backend-unsupported: reason backend unsupported");
    ASSERT_INT_EQ((int)report.backend, (int)RALPH_LP_EXTERNAL_BACKEND_DUAL_SIMPLEX,
                  "dispatch/backend-unsupported: backend dual tracked");
    ASSERT_INT_EQ(report.fatal, 0,
                  "dispatch/backend-unsupported: fallback non-fatal");

    ralph_free(model);
    ralph_unregister_all_lp_external_adapters();
}

static void test_execution_failure_time_limit_mapping(void) {
    RalphModel *model = build_small_lp();
    RalphLPExternalFailureReport report;
    FailureFixture fx;
    RalphLPExternalAdapter adapter;

    memset(&fx, 0, sizeof(fx));
    fx.caps.supports_simplex = 1;
    fx.caps.supports_dual_simplex = 0;
    fx.caps.supports_barrier = 0;
    fx.caps.supports_crossover = 0;
    fx.caps_rc = 0;
    fx.solve_rc = RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT;
    adapter = build_failure_adapter(&fx, RALPH_LP_EXTERNAL_PROVIDER_GLPK, "FailureGLPK");

    ralph_unregister_all_lp_external_adapters();
    ASSERT_INT_EQ(ralph_register_lp_external_adapter(&adapter), 0,
                  "execution/time-limit: register GLPK fixture");
    ASSERT_TRUE(model != NULL, "execution/time-limit: model created");
    if (!model) {
        ralph_unregister_all_lp_external_adapters();
        return;
    }

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "execution/time-limit: request external primal");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "execution/time-limit: set provider GLPK");
    ASSERT_INT_EQ(ralph_optimize_lp(model), -1,
                  "execution/time-limit: external solve failure propagated");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_TIME_LIMIT,
                  "execution/time-limit: status mapped to time limit");

    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), 0,
                  "execution/time-limit: report available");
    ASSERT_INT_EQ((int)report.stage, (int)RALPH_LP_EXTERNAL_FAILURE_STAGE_EXECUTION,
                  "execution/time-limit: stage execution");
    ASSERT_INT_EQ((int)report.reason,
                  (int)RALPH_LP_EXTERNAL_FAILURE_ADAPTER_TIME_LIMIT,
                  "execution/time-limit: reason adapter time limit");
    ASSERT_INT_EQ(report.adapter_return_code, (int)RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT,
                  "execution/time-limit: adapter rc preserved");
    ASSERT_INT_EQ((int)report.mapped_status, (int)RALPH_STATUS_TIME_LIMIT,
                  "execution/time-limit: mapped status tracked");
    ASSERT_INT_EQ(report.fatal, 1,
                  "execution/time-limit: execution failure fatal");
    ASSERT_INT_EQ((int)report.effective_provider, (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK,
                  "execution/time-limit: effective provider tracked");
    ASSERT_INT_EQ((int)report.backend, (int)RALPH_LP_EXTERNAL_BACKEND_SIMPLEX,
                  "execution/time-limit: backend tracked");

    ralph_free(model);
    ralph_unregister_all_lp_external_adapters();
}

static void test_execution_failure_generic_mapping(void) {
    RalphModel *model = build_small_lp();
    RalphLPExternalFailureReport report;
    FailureFixture fx;
    RalphLPExternalAdapter adapter;

    memset(&fx, 0, sizeof(fx));
    fx.caps.supports_simplex = 1;
    fx.caps.supports_dual_simplex = 0;
    fx.caps.supports_barrier = 0;
    fx.caps.supports_crossover = 0;
    fx.caps_rc = 0;
    fx.solve_rc = -77;
    adapter = build_failure_adapter(&fx, RALPH_LP_EXTERNAL_PROVIDER_GLPK, "FailureGLPK");

    ralph_unregister_all_lp_external_adapters();
    ASSERT_INT_EQ(ralph_register_lp_external_adapter(&adapter), 0,
                  "execution/generic: register GLPK fixture");
    ASSERT_TRUE(model != NULL, "execution/generic: model created");
    if (!model) {
        ralph_unregister_all_lp_external_adapters();
        return;
    }

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "execution/generic: request external primal");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "execution/generic: set provider GLPK");
    ASSERT_INT_EQ(ralph_optimize_lp(model), -1,
                  "execution/generic: external solve failure propagated");
    ASSERT_INT_EQ((int)ralph_get_status(model), (int)RALPH_STATUS_ERROR,
                  "execution/generic: status mapped to error");

    ASSERT_INT_EQ(ralph_get_last_lp_external_failure_report(model, &report), 0,
                  "execution/generic: report available");
    ASSERT_INT_EQ((int)report.stage, (int)RALPH_LP_EXTERNAL_FAILURE_STAGE_EXECUTION,
                  "execution/generic: stage execution");
    ASSERT_INT_EQ((int)report.reason,
                  (int)RALPH_LP_EXTERNAL_FAILURE_ADAPTER_FAILED,
                  "execution/generic: reason generic adapter failure");
    ASSERT_INT_EQ(report.adapter_return_code, -77,
                  "execution/generic: adapter rc preserved");
    ASSERT_INT_EQ((int)report.mapped_status, (int)RALPH_STATUS_ERROR,
                  "execution/generic: mapped status tracked");
    ASSERT_INT_EQ(report.fatal, 1,
                  "execution/generic: execution failure fatal");

    ralph_free(model);
    ralph_unregister_all_lp_external_adapters();
}

int main(void) {
    printf("=== LP External Failure API Tests ===\n");

    test_guards_and_no_external_failure();
    test_dispatch_failure_provider_required_nonfatal();
    test_dispatch_failure_provider_unregistered_strict();
    test_dispatch_failure_backend_unsupported();
    test_execution_failure_time_limit_mapping();
    test_execution_failure_generic_mapping();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
