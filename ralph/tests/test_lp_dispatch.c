/*
 * Tests for internal LP backend dispatch planner.
 *
 * This module is intentionally separate from public API tests.
 */

#include <stdio.h>
#include <string.h>

#include "../src/lp_dispatch.h"
#include "../src/lp_external_adapter.h"

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
    LPExternalCapabilities caps;
    int solve_return_code;
    int solve_calls;
    LPExternalBackendKind last_backend;
} FakeAdapterState;

static int fake_adapter_get_capabilities(LPExternalCapabilities *caps, void *user_data) {
    FakeAdapterState *state = (FakeAdapterState*)user_data;
    if (!caps || !state) return -1;
    *caps = state->caps;
    return 0;
}

static int fake_adapter_solve(LPExternalBackendKind backend,
                              SimplexSolver *solver,
                              void *user_data) {
    FakeAdapterState *state = (FakeAdapterState*)user_data;
    if (!state || !solver) return -1;
    state->solve_calls++;
    state->last_backend = backend;
    return state->solve_return_code;
}

static int register_fake_adapter(FakeAdapterState *state,
                                 LPExternalProvider provider,
                                 const LPExternalCapabilities *caps,
                                 int solve_return_code) {
    LPExternalAdapter adapter;
    if (!state || !caps) return -1;

    memset(state, 0, sizeof(*state));
    state->caps = *caps;
    state->solve_return_code = solve_return_code;
    state->last_backend = LP_EXTERNAL_BACKEND_SIMPLEX;

    memset(&adapter, 0, sizeof(adapter));
    adapter.abi_version = LP_EXTERNAL_ADAPTER_ABI_VERSION;
    adapter.provider = provider;
    adapter.provider_name = NULL;
    adapter.get_capabilities = fake_adapter_get_capabilities;
    adapter.solve = fake_adapter_solve;
    adapter.user_data = state;
    return lp_external_adapter_register(&adapter);
}

static void test_dispatch_validation(void) {
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid(-1), 0,
                  "validation: algorithm -1 invalid");
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid((int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX), 1,
                  "validation: primal valid");
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid((int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL), 1,
                  "validation: barrier external valid");
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid(7), 0,
                  "validation: algorithm 7 invalid");

    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid(-1), 0,
                  "validation: crossover -1 invalid");
    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid((int)RALPH_LP_CROSSOVER_AUTO), 1,
                  "validation: crossover auto valid");
    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid((int)RALPH_LP_CROSSOVER_ON), 1,
                  "validation: crossover on valid");
    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid(3), 0,
                  "validation: crossover 3 invalid");

    ASSERT_INT_EQ(lp_dispatch_external_provider_value_valid(-1), 0,
                  "validation: external provider -1 invalid");
    ASSERT_INT_EQ(lp_dispatch_external_provider_value_valid((int)RALPH_LP_EXTERNAL_PROVIDER_NONE), 1,
                  "validation: external provider none valid");
    ASSERT_INT_EQ(lp_dispatch_external_provider_value_valid((int)RALPH_LP_EXTERNAL_PROVIDER_GLOP), 1,
                  "validation: external provider glop valid");
    ASSERT_INT_EQ(lp_dispatch_external_provider_value_valid(7), 0,
                  "validation: external provider 7 invalid");
}

static void test_dispatch_setters(void) {
    int alg = -1;
    int method = -1;
    int crossover = -1;
    int provider = -1;

    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm((int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL,
                                                      &alg,
                                                      &method),
                  0,
                  "setters: dual external algorithm accepted");
    ASSERT_INT_EQ(alg, (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL,
                  "setters: normalized dual external algorithm");
    ASSERT_INT_EQ(method, (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "setters: dual external maps method to dual");

    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm((int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL,
                                                      &alg,
                                                      &method),
                  0,
                  "setters: barrier external algorithm accepted");
    ASSERT_INT_EQ(alg, (int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL,
                  "setters: normalized barrier external algorithm");
    ASSERT_INT_EQ(method, (int)RALPH_LP_ALGORITHM_AUTO,
                  "setters: barrier external maps method to auto");

    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm(7, &alg, &method), -1,
                  "setters: reject invalid algorithm");
    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm((int)RALPH_LP_ALGORITHM_AUTO,
                                                      NULL,
                                                      &method),
                  -1,
                  "setters: reject NULL normalized algorithm output");

    ASSERT_INT_EQ(lp_dispatch_set_requested_crossover((int)RALPH_LP_CROSSOVER_ON,
                                                      &crossover),
                  0,
                  "setters: crossover on accepted");
    ASSERT_INT_EQ(crossover, (int)RALPH_LP_CROSSOVER_ON,
                  "setters: normalized crossover on");
    ASSERT_INT_EQ(lp_dispatch_set_requested_crossover(3, &crossover), -1,
                  "setters: reject invalid crossover");
    ASSERT_INT_EQ(lp_dispatch_set_requested_crossover((int)RALPH_LP_CROSSOVER_AUTO,
                                                      NULL),
                  -1,
                  "setters: reject NULL normalized crossover output");

    ASSERT_INT_EQ(lp_dispatch_set_requested_external_provider(
                      (int)RALPH_LP_EXTERNAL_PROVIDER_HIGHS, &provider),
                  0,
                  "setters: external provider accepted");
    ASSERT_INT_EQ(provider, (int)RALPH_LP_EXTERNAL_PROVIDER_HIGHS,
                  "setters: normalized external provider");
    ASSERT_INT_EQ(lp_dispatch_set_requested_external_provider(7, &provider), -1,
                  "setters: reject invalid external provider");
    ASSERT_INT_EQ(lp_dispatch_set_requested_external_provider(
                      (int)RALPH_LP_EXTERNAL_PROVIDER_NONE, NULL),
                  -1,
                  "setters: reject NULL normalized external provider output");
}

static void test_dispatch_capabilities_default(void) {
    RalphLPCapabilities caps;

    lp_external_adapter_unregister();
    memset(&caps, 0, sizeof(caps));
    lp_dispatch_get_capabilities(&caps);

    ASSERT_INT_EQ(caps.supports_primal_simplex, 1,
                  "capabilities/default: primal supported");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "capabilities/default: dual supported");
    ASSERT_INT_EQ(caps.supports_barrier, 0,
                  "capabilities/default: barrier unsupported");
    ASSERT_INT_EQ(caps.supports_crossover, 0,
                  "capabilities/default: crossover unsupported");
}

static void test_dispatch_capabilities_external(void) {
    RalphLPCapabilities caps;
    FakeAdapterState state;
    LPExternalCapabilities fake_caps;

    memset(&fake_caps, 0, sizeof(fake_caps));
    fake_caps.supports_simplex = 1;
    fake_caps.supports_dual_simplex = 1;
    fake_caps.supports_barrier = 1;
    fake_caps.supports_crossover = 1;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(register_fake_adapter(&state,
                                        LP_EXTERNAL_PROVIDER_HIGHS,
                                        &fake_caps,
                                        0),
                  0,
                  "capabilities/external: register fake adapter");

    memset(&caps, 0, sizeof(caps));
    lp_dispatch_get_capabilities(&caps);
    ASSERT_INT_EQ(caps.supports_primal_simplex, 1,
                  "capabilities/external: primal supported");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "capabilities/external: dual supported");
    ASSERT_INT_EQ(caps.supports_barrier, 1,
                  "capabilities/external: barrier supported");
    ASSERT_INT_EQ(caps.supports_crossover, 1,
                  "capabilities/external: crossover supported");

    lp_external_adapter_unregister();
}

static void test_dispatch_plan_simplex_path(void) {
    LPDispatchPlan plan;
    RalphLPSolveAlgorithmReport report;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                                         (int)RALPH_LP_CROSSOVER_AUTO,
                                         &plan),
                  0,
                  "plan/simplex: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/simplex: requested backend simplex");
    ASSERT_INT_EQ((int)plan.effective_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/simplex: effective backend simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "plan/simplex: effective algorithm dual");
    ASSERT_INT_EQ(plan.simplex_method, 1,
                  "plan/simplex: simplex method dual");
    ASSERT_INT_EQ(plan.fallback_applied, 0,
                  "plan/simplex: no fallback applied");

    memset(&report, 0, sizeof(report));
    lp_dispatch_plan_to_report(&plan, &report);
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "plan/simplex: report effective algorithm dual");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "plan/simplex: report no fallback");
}

static void test_dispatch_plan_internal_preferred_even_with_external(void) {
    LPDispatchPlan plan;
    FakeAdapterState state;
    LPExternalCapabilities fake_caps;

    memset(&fake_caps, 0, sizeof(fake_caps));
    fake_caps.supports_simplex = 1;
    fake_caps.supports_dual_simplex = 1;
    fake_caps.supports_barrier = 1;
    fake_caps.supports_crossover = 1;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(register_fake_adapter(&state,
                                        LP_EXTERNAL_PROVIDER_GLPK,
                                        &fake_caps,
                                        0),
                  0,
                  "plan/internal-pref: register fake adapter");

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK,
                                         (int)RALPH_LP_CROSSOVER_AUTO,
                                         &plan),
                  0,
                  "plan/internal-pref: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend,
                  (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/internal-pref: requested backend stays internal simplex");
    ASSERT_INT_EQ((int)plan.effective_backend,
                  (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/internal-pref: effective backend stays internal simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "plan/internal-pref: effective algorithm primal");
    ASSERT_INT_EQ(plan.fallback_applied, 0,
                  "plan/internal-pref: no fallback applied");

    lp_external_adapter_unregister();
}

static void test_dispatch_plan_external_simplex(void) {
    LPDispatchPlan plan;
    FakeAdapterState state;
    LPExternalCapabilities fake_caps;

    memset(&fake_caps, 0, sizeof(fake_caps));
    fake_caps.supports_simplex = 1;
    fake_caps.supports_dual_simplex = 0;
    fake_caps.supports_barrier = 0;
    fake_caps.supports_crossover = 0;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(register_fake_adapter(&state,
                                        LP_EXTERNAL_PROVIDER_GLPK,
                                        &fake_caps,
                                        0),
                  0,
                  "plan/external-simplex: register fake adapter");

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK,
                                         (int)RALPH_LP_CROSSOVER_AUTO,
                                         &plan),
                  0,
                  "plan/external-simplex: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend,
                  (int)LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL,
                  "plan/external-simplex: requested backend external simplex");
    ASSERT_INT_EQ((int)plan.effective_backend,
                  (int)LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL,
                  "plan/external-simplex: effective backend external simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL,
                  "plan/external-simplex: effective algorithm external primal");
    ASSERT_INT_EQ(plan.fallback_applied, 0,
                  "plan/external-simplex: no fallback applied");

    lp_external_adapter_unregister();
}

static void test_dispatch_plan_external_dual_simplex(void) {
    LPDispatchPlan plan;
    FakeAdapterState state;
    LPExternalCapabilities fake_caps;

    memset(&fake_caps, 0, sizeof(fake_caps));
    fake_caps.supports_simplex = 0;
    fake_caps.supports_dual_simplex = 1;
    fake_caps.supports_barrier = 0;
    fake_caps.supports_crossover = 0;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(register_fake_adapter(&state,
                                        LP_EXTERNAL_PROVIDER_CLP,
                                        &fake_caps,
                                        0),
                  0,
                  "plan/external-dual: register fake adapter");

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_CLP,
                                         (int)RALPH_LP_CROSSOVER_AUTO,
                                         &plan),
                  0,
                  "plan/external-dual: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend,
                  (int)LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL,
                  "plan/external-dual: requested backend external dual simplex");
    ASSERT_INT_EQ((int)plan.effective_backend,
                  (int)LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL,
                  "plan/external-dual: effective backend external dual simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL,
                  "plan/external-dual: effective algorithm external dual");
    ASSERT_INT_EQ(plan.fallback_applied, 0,
                  "plan/external-dual: no fallback applied");

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_AUTO,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_CLP,
                                         (int)RALPH_LP_CROSSOVER_AUTO,
                                         &plan),
                  0,
                  "plan/external-dual: auto build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend,
                  (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/external-dual: auto uses internal simplex backend");

    lp_external_adapter_unregister();
}

static void test_dispatch_plan_external_requires_matching_provider(void) {
    LPDispatchPlan plan;
    FakeAdapterState state;
    LPExternalCapabilities fake_caps;

    memset(&fake_caps, 0, sizeof(fake_caps));
    fake_caps.supports_simplex = 0;
    fake_caps.supports_dual_simplex = 1;
    fake_caps.supports_barrier = 0;
    fake_caps.supports_crossover = 0;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(register_fake_adapter(&state,
                                        LP_EXTERNAL_PROVIDER_CLP,
                                        &fake_caps,
                                        0),
                  0,
                  "plan/external-provider-gate: register fake adapter");

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                                         (int)RALPH_LP_CROSSOVER_ON,
                                         &plan),
                  0,
                  "plan/external-provider-gate: build succeeds with none provider");
    ASSERT_INT_EQ((int)plan.effective_backend,
                  (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/external-provider-gate: none provider falls back to simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "plan/external-provider-gate: fallback algorithm dual");
    ASSERT_INT_EQ(plan.fallback_applied, 1,
                  "plan/external-provider-gate: fallback applied");
    ASSERT_INT_EQ((int)plan.fallback_reason,
                  (int)RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE,
                  "plan/external-provider-gate: reason external unavailable");
    ASSERT_INT_EQ((int)plan.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "plan/external-provider-gate: crossover auto after fallback");

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK,
                                         (int)RALPH_LP_CROSSOVER_AUTO,
                                         &plan),
                  0,
                  "plan/external-provider-gate: build succeeds with mismatched provider");
    ASSERT_INT_EQ((int)plan.effective_backend,
                  (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/external-provider-gate: mismatched provider falls back to simplex");
    ASSERT_INT_EQ((int)plan.fallback_reason,
                  (int)RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE,
                  "plan/external-provider-gate: mismatched reason external unavailable");

    lp_external_adapter_unregister();
}

static void test_dispatch_plan_barrier_stub_fallback(void) {
    LPDispatchPlan plan;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_BARRIER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                                         (int)RALPH_LP_CROSSOVER_ON,
                                         &plan),
                  0,
                  "plan/barrier: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend, (int)LP_DISPATCH_BACKEND_BARRIER_NATIVE,
                  "plan/barrier: requested backend barrier-native");
    ASSERT_INT_EQ((int)plan.effective_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/barrier: effective backend simplex fallback");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "plan/barrier: effective algorithm auto fallback");
    ASSERT_INT_EQ((int)plan.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "plan/barrier: requested crossover captured");
    ASSERT_INT_EQ((int)plan.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "plan/barrier: effective crossover fallback to auto");
    ASSERT_INT_EQ(plan.fallback_applied, 1,
                  "plan/barrier: fallback applied");
    ASSERT_INT_EQ((int)plan.fallback_reason,
                  (int)RALPH_LP_FALLBACK_BARRIER_UNAVAILABLE,
                  "plan/barrier: fallback reason barrier unavailable");
    ASSERT_INT_EQ(plan.simplex_method, 2,
                  "plan/barrier: simplex method auto after fallback");
}

static void test_dispatch_plan_barrier_external(void) {
    LPDispatchPlan plan;
    FakeAdapterState state;
    LPExternalCapabilities fake_caps;

    memset(&fake_caps, 0, sizeof(fake_caps));
    fake_caps.supports_simplex = 0;
    fake_caps.supports_dual_simplex = 0;
    fake_caps.supports_barrier = 1;
    fake_caps.supports_crossover = 1;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(register_fake_adapter(&state,
                                        LP_EXTERNAL_PROVIDER_CPLEX,
                                        &fake_caps,
                                        0),
                  0,
                  "plan/barrier-external: register fake adapter");

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_CPLEX,
                                         (int)RALPH_LP_CROSSOVER_ON,
                                         &plan),
                  0,
                  "plan/barrier-external: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend,
                  (int)LP_DISPATCH_BACKEND_BARRIER_EXTERNAL,
                  "plan/barrier-external: requested backend external barrier");
    ASSERT_INT_EQ((int)plan.effective_backend,
                  (int)LP_DISPATCH_BACKEND_BARRIER_EXTERNAL,
                  "plan/barrier-external: effective backend external barrier");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL,
                  "plan/barrier-external: effective algorithm external barrier");
    ASSERT_INT_EQ((int)plan.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "plan/barrier-external: crossover retained");
    ASSERT_INT_EQ(plan.fallback_applied, 0,
                  "plan/barrier-external: no fallback applied");

    lp_external_adapter_unregister();
}

static void test_dispatch_plan_crossover_only_fallback(void) {
    LPDispatchPlan plan;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                                         (int)RALPH_LP_CROSSOVER_ON,
                                         &plan),
                  0,
                  "plan/crossover: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/crossover: requested backend simplex");
    ASSERT_INT_EQ((int)plan.effective_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/crossover: effective backend simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "plan/crossover: effective algorithm primal");
    ASSERT_INT_EQ((int)plan.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "plan/crossover: effective crossover auto");
    ASSERT_INT_EQ(plan.fallback_applied, 1,
                  "plan/crossover: fallback applied");
    ASSERT_INT_EQ((int)plan.fallback_reason,
                  (int)RALPH_LP_FALLBACK_CROSSOVER_UNAVAILABLE,
                  "plan/crossover: fallback reason crossover unavailable");
}

int main(void) {
    printf("=== LP Dispatch Module Tests ===\n");

    lp_external_adapter_unregister();
    test_dispatch_validation();
    test_dispatch_setters();
    test_dispatch_capabilities_default();
    test_dispatch_capabilities_external();
    test_dispatch_plan_simplex_path();
    test_dispatch_plan_internal_preferred_even_with_external();
    test_dispatch_plan_external_simplex();
    test_dispatch_plan_external_dual_simplex();
    test_dispatch_plan_external_requires_matching_provider();
    test_dispatch_plan_barrier_stub_fallback();
    test_dispatch_plan_barrier_external();
    test_dispatch_plan_crossover_only_fallback();
    lp_external_adapter_unregister();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
