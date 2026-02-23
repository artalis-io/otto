/*
 * Tests for LP algorithm capability/fallback API surface.
 *
 * This module is intentionally separate from telemetry/logging tests.
 */

#include <stdio.h>
#include <string.h>
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

static RalphModel* build_small_lp(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "detect_special", 0);
    ralph_test_set_int_param(model, "presolve", 0);

    /* min x, s.t. x >= 1, x >= 0 */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    {
        int idx[] = {0};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }
    return model;
}

static void test_lp_capabilities(void) {
    RalphLPCapabilities caps;

    ASSERT_INT_EQ(ralph_get_lp_capabilities(NULL), -1,
                  "capabilities: NULL output rejected");

    memset(&caps, 0, sizeof(caps));
    ASSERT_INT_EQ(ralph_get_lp_capabilities(&caps), 0,
                  "capabilities: getter succeeds");
    ASSERT_INT_EQ(caps.supports_primal_simplex, 1,
                  "capabilities: primal supported");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "capabilities: dual supported");
    ASSERT_INT_EQ(caps.supports_barrier, 0,
                  "capabilities: barrier unsupported");
    ASSERT_INT_EQ(caps.supports_crossover, 0,
                  "capabilities: crossover unsupported");
}

static void test_param_metadata_and_scope(void) {
    RalphModel *model = build_small_lp();
    RalphParamMeta meta;
    RalphParamId pid = RALPH_PARAM_COUNT;
    int value = -999;

    ASSERT_TRUE(model != NULL, "params: model created");
    if (!model) return;

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_get_param_meta(RALPH_PARAM_LP_ALGORITHM, &meta), 0,
                  "params: metadata for lp_algorithm");
    ASSERT_TRUE(strcmp(meta.name, "lp_algorithm") == 0,
                "params: lp_algorithm canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_algorithm LP scope");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_INT,
                  "params: lp_algorithm int type");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_algorithm has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_algorithm has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "params: lp_algorithm min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL,
                  "params: lp_algorithm max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_get_param_meta(RALPH_PARAM_BARRIER_CROSSOVER, &meta), 0,
                  "params: metadata for barrier_crossover");
    ASSERT_TRUE(strcmp(meta.name, "barrier_crossover") == 0,
                "params: barrier_crossover canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: barrier_crossover LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: barrier_crossover has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: barrier_crossover has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_CROSSOVER_AUTO,
                  "params: barrier_crossover min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_CROSSOVER_ON,
                  "params: barrier_crossover max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_get_param_meta(RALPH_PARAM_LP_EXTERNAL_PROVIDER, &meta), 0,
                  "params: metadata for lp_external_provider");
    ASSERT_TRUE(strcmp(meta.name, "lp_external_provider") == 0,
                "params: lp_external_provider canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_external_provider LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_external_provider has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_external_provider has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                  "params: lp_external_provider min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_EXTERNAL_PROVIDER_GLOP,
                  "params: lp_external_provider max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_get_param_meta(RALPH_PARAM_LP_EXTERNAL_STRICT, &meta), 0,
                  "params: metadata for lp_external_strict");
    ASSERT_TRUE(strcmp(meta.name, "lp_external_strict") == 0,
                "params: lp_external_strict canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_external_strict LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_external_strict has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_external_strict has max");
    ASSERT_INT_EQ((int)meta.min_value, 0,
                  "params: lp_external_strict min");
    ASSERT_INT_EQ((int)meta.max_value, 1,
                  "params: lp_external_strict max");

    ASSERT_INT_EQ(ralph_find_param_by_name("lp_algorithm", &pid), 0,
                  "params: find lp_algorithm canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_ALGORITHM,
                  "params: lp_algorithm canonical id");
    ASSERT_INT_EQ(ralph_find_param_by_name("LPAlgorithm", &pid), 0,
                  "params: find lp_algorithm alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_ALGORITHM,
                  "params: lp_algorithm alias id");
    ASSERT_INT_EQ(ralph_find_param_by_name("lp_external_provider", &pid), 0,
                  "params: find lp_external_provider canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                  "params: lp_external_provider canonical id");
    ASSERT_INT_EQ(ralph_find_param_by_name("LPExternalProvider", &pid), 0,
                  "params: find lp_external_provider alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                  "params: lp_external_provider alias id");
    ASSERT_INT_EQ(ralph_find_param_by_name("lp_external_strict", &pid), 0,
                  "params: find lp_external_strict canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_STRICT,
                  "params: lp_external_strict canonical id");
    ASSERT_INT_EQ(ralph_find_param_by_name("LPExternalStrict", &pid), 0,
                  "params: find lp_external_strict alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_STRICT,
                  "params: lp_external_strict alias id");

    ASSERT_INT_EQ(ralph_set_mip_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX),
                  -1,
                  "params: MIP strict rejects LP algorithm id");
    ASSERT_INT_EQ(ralph_set_mip_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                             (int)RALPH_LP_CROSSOVER_AUTO),
                  -1,
                  "params: MIP strict rejects barrier crossover id");
    ASSERT_INT_EQ(ralph_set_mip_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  -1,
                  "params: MIP strict rejects lp_external_provider id");
    ASSERT_INT_EQ(ralph_set_mip_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1),
                  -1,
                  "params: MIP strict rejects lp_external_strict id");

    ASSERT_INT_EQ(ralph_set_lp_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                            (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX),
                  0,
                  "params: LP strict accepts lp_algorithm");
    ASSERT_INT_EQ(ralph_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read legacy method after lp_algorithm");
    ASSERT_INT_EQ(value, 1,
                  "params: method syncs with lp_algorithm<=auto");

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_METHOD, 0), 0,
                  "params: set legacy method");
    ASSERT_INT_EQ(ralph_get_int_param_id(model, RALPH_PARAM_LP_ALGORITHM, &value), 0,
                  "params: read lp_algorithm after method");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "params: lp_algorithm syncs from method");

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_BARRIER),
                  0,
                  "params: barrier algorithm request accepted");
    ASSERT_INT_EQ(ralph_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read method after barrier request");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_AUTO,
                  "params: method remains auto when barrier requested");

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "params: external primal algorithm request accepted");
    ASSERT_INT_EQ(ralph_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read method after external primal request");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "params: method maps to primal for external primal");

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL),
                  0,
                  "params: external dual algorithm request accepted");
    ASSERT_INT_EQ(ralph_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read method after external dual request");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "params: method maps to dual for external dual");

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "params: set lp_external_provider by id");
    ASSERT_INT_EQ(ralph_get_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER, &value), 0,
                  "params: get lp_external_provider by id");
    ASSERT_INT_EQ(value, (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK,
                  "params: lp_external_provider set/get consistent");

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM, 7), -1,
                  "params: reject lp_algorithm out of range");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER, 3), -1,
                  "params: reject barrier_crossover out of range");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER, 7), -1,
                  "params: reject lp_external_provider out of range");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 2), -1,
                  "params: reject lp_external_strict out of range");

    ASSERT_INT_EQ(ralph_test_set_int_param(model, "barrier_crossover",
                                      (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "params: set barrier_crossover by string");
    ASSERT_INT_EQ(ralph_get_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER, &value), 0,
                  "params: get barrier_crossover by id");
    ASSERT_INT_EQ(value, (int)RALPH_LP_CROSSOVER_ON,
                  "params: barrier_crossover set/get consistent");

    ralph_test_free(model);
}

static void test_algorithm_report_guards_and_invalidation(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "report: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), -1,
                  "report: unavailable before solve");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "report: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "report: LP status optimal");

    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), 0,
                  "report: available after LP solve");
    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "report: default requested algorithm");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "report: default effective algorithm");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "report: no fallback for default algorithm");

    ASSERT_INT_EQ(ralph_test_set_obj_coef(model, 0, 2.0), 0,
                  "report: mutate model invalidates state");
    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), -1,
                  "report: unavailable after invalidation");

    ralph_test_free(model);
}

static void test_barrier_fallback_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "barrier: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_BARRIER),
                  0,
                  "barrier: request barrier algorithm");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                         (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "barrier: request crossover ON");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "barrier: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "barrier: LP status optimal");
    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), 0,
                  "barrier: report available");

    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_BARRIER,
                  "barrier: requested algorithm captured");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "barrier: effective algorithm fallback to auto");
    ASSERT_INT_EQ((int)report.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "barrier: requested crossover captured");
    ASSERT_INT_EQ((int)report.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "barrier: effective crossover fallback to auto");
    ASSERT_INT_EQ(report.fallback_applied, 1,
                  "barrier: fallback marked as applied");
    ASSERT_INT_EQ((int)report.fallback_reason,
                  (int)RALPH_LP_FALLBACK_BARRIER_UNAVAILABLE,
                  "barrier: fallback reason is barrier unavailable");

    ralph_test_free(model);
}

static void test_crossover_only_fallback_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "crossover: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX),
                  0,
                  "crossover: primal algorithm requested");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                         (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "crossover: request crossover ON");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "crossover: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "crossover: LP status optimal");
    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), 0,
                  "crossover: report available");

    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "crossover: requested algorithm captured");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "crossover: effective algorithm unchanged");
    ASSERT_INT_EQ((int)report.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "crossover: requested crossover captured");
    ASSERT_INT_EQ((int)report.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "crossover: effective crossover fallback to auto");
    ASSERT_INT_EQ(report.fallback_applied, 1,
                  "crossover: fallback marked as applied");
    ASSERT_INT_EQ((int)report.fallback_reason,
                  (int)RALPH_LP_FALLBACK_CROSSOVER_UNAVAILABLE,
                  "crossover: fallback reason is crossover unavailable");

    ralph_test_free(model);
}

static void test_external_fallback_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "external-fallback: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "external-fallback: request external primal algorithm");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "external-fallback: request external provider GLPK");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                         (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "external-fallback: request crossover ON");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "external-fallback: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "external-fallback: LP status optimal");
    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), 0,
                  "external-fallback: report available");

    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL,
                  "external-fallback: requested algorithm captured");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "external-fallback: effective algorithm fallback to internal primal");
    ASSERT_INT_EQ((int)report.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "external-fallback: requested crossover captured");
    ASSERT_INT_EQ((int)report.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "external-fallback: effective crossover fallback to auto");
    ASSERT_INT_EQ(report.fallback_applied, 1,
                  "external-fallback: fallback applied");
    ASSERT_INT_EQ((int)report.fallback_reason,
                  (int)RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE,
                  "external-fallback: fallback reason external unavailable");

    ralph_test_free(model);
}

static void test_external_strict_mode_error(void) {
    RalphModel *model = build_small_lp();

    ASSERT_TRUE(model != NULL, "external-strict: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "external-strict: request external primal algorithm");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "external-strict: request external provider GLPK");
    ASSERT_INT_EQ(ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1),
                  0,
                  "external-strict: enable strict mode");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                  "external-strict: optimize fails when external backend unavailable");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_ERROR,
                  "external-strict: status is ERROR");

    ralph_test_free(model);
}

static void test_legacy_method_dispatch_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "legacy-dispatch: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_set_int_param(model, "method", 1), 0,
                  "legacy-dispatch: set method=dual");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "legacy-dispatch: solve with dual method succeeds");
    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), 0,
                  "legacy-dispatch: report available for dual method");
    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "legacy-dispatch: requested algorithm reflects dual method");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "legacy-dispatch: effective algorithm remains dual");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "legacy-dispatch: no fallback for dual method");

    ASSERT_INT_EQ(ralph_test_set_int_param(model, "method", 2), 0,
                  "legacy-dispatch: set method=auto");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "legacy-dispatch: solve with auto method succeeds");
    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(model, &report), 0,
                  "legacy-dispatch: report available for auto method");
    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "legacy-dispatch: requested algorithm reflects auto method");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "legacy-dispatch: effective algorithm remains auto");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "legacy-dispatch: no fallback for auto method");

    ralph_test_free(model);
}

static void test_lp_report_rejects_mip_models(void) {
    RalphModel *mip = ralph_test_create();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(mip != NULL, "mip-guard: model created");
    if (!mip) return;

    ralph_test_set_obj_sense(mip, RALPH_MAXIMIZE);
    ralph_test_add_var(mip, 0.0, 1.0, 1.0, RALPH_BINARY);

    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(mip, &report), -1,
                  "mip-guard: report rejected for MIP model before solve");
    ASSERT_INT_EQ(ralph_test_optimize_mip(mip), 0,
                  "mip-guard: optimize_mip succeeds");
    ASSERT_INT_EQ(ralph_get_last_lp_algorithm_report(mip, &report), -1,
                  "mip-guard: report rejected for MIP model after solve");

    ralph_test_free(mip);
}

int main(void) {
    printf("=== LP Algorithm API Tests ===\n");

    test_lp_capabilities();
    test_param_metadata_and_scope();
    test_algorithm_report_guards_and_invalidation();
    test_barrier_fallback_report();
    test_crossover_only_fallback_report();
    test_external_fallback_report();
    test_external_strict_mode_error();
    test_legacy_method_dispatch_report();
    test_lp_report_rejects_mip_models();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
