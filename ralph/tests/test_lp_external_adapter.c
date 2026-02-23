/*
 * Tests for LP external adapter registry/contract module.
 *
 * This module is intentionally orthogonal to dispatch tests.
 */

#include <stdio.h>
#include <string.h>

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
    int caps_rc;
    int solve_rc;
    int solve_calls;
    LPExternalBackendKind last_backend;
} AdapterFixture;

static int fixture_get_capabilities(LPExternalCapabilities *caps, void *user_data) {
    AdapterFixture *fx = (AdapterFixture*)user_data;
    if (!fx || !caps) return -1;
    *caps = fx->caps;
    return fx->caps_rc;
}

static int fixture_solve(LPExternalBackendKind backend,
                         SimplexSolver *solver,
                         void *user_data) {
    AdapterFixture *fx = (AdapterFixture*)user_data;
    if (!fx || !solver) return -1;
    fx->solve_calls++;
    fx->last_backend = backend;
    return fx->solve_rc;
}

static LPExternalAdapter build_fixture_adapter(AdapterFixture *fx,
                                               LPExternalProvider provider,
                                               const char *provider_name) {
    LPExternalAdapter adapter;
    memset(&adapter, 0, sizeof(adapter));
    adapter.abi_version = LP_EXTERNAL_ADAPTER_ABI_VERSION;
    adapter.provider = provider;
    adapter.provider_name = provider_name;
    adapter.get_capabilities = fixture_get_capabilities;
    adapter.solve = fixture_solve;
    adapter.user_data = fx;
    return adapter;
}

static void test_provider_names(void) {
    ASSERT_TRUE(strcmp(lp_external_provider_name(LP_EXTERNAL_PROVIDER_NONE), "none") == 0,
                "provider-name: none");
    ASSERT_TRUE(strcmp(lp_external_provider_name(LP_EXTERNAL_PROVIDER_GLPK), "GLPK") == 0,
                "provider-name: glpk");
    ASSERT_TRUE(strcmp(lp_external_provider_name(LP_EXTERNAL_PROVIDER_HIGHS), "HiGHS") == 0,
                "provider-name: highs");
    ASSERT_TRUE(strcmp(lp_external_provider_name(LP_EXTERNAL_PROVIDER_CLP), "CLP") == 0,
                "provider-name: clp");
    ASSERT_TRUE(strcmp(lp_external_provider_name(LP_EXTERNAL_PROVIDER_CPLEX), "CPLEX") == 0,
                "provider-name: cplex");
    ASSERT_TRUE(strcmp(lp_external_provider_name(LP_EXTERNAL_PROVIDER_GUROBI), "Gurobi") == 0,
                "provider-name: gurobi");
    ASSERT_TRUE(strcmp(lp_external_provider_name(LP_EXTERNAL_PROVIDER_GLOP), "GLOP") == 0,
                "provider-name: glop");
}

static void test_register_validation(void) {
    AdapterFixture fx;
    LPExternalAdapter adapter;

    memset(&fx, 0, sizeof(fx));
    fx.caps.supports_simplex = 1;
    fx.caps.supports_dual_simplex = 0;
    fx.caps.supports_barrier = 0;
    fx.caps.supports_crossover = 0;

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(lp_external_adapter_register(NULL), -1,
                  "register: NULL adapter rejected");

    adapter = build_fixture_adapter(&fx, LP_EXTERNAL_PROVIDER_HIGHS, NULL);
    adapter.abi_version = 0;
    ASSERT_INT_EQ(lp_external_adapter_register(&adapter), -1,
                  "register: bad ABI rejected");

    adapter = build_fixture_adapter(&fx, LP_EXTERNAL_PROVIDER_NONE, NULL);
    ASSERT_INT_EQ(lp_external_adapter_register(&adapter), -1,
                  "register: provider none rejected");

    adapter = build_fixture_adapter(&fx, LP_EXTERNAL_PROVIDER_HIGHS, NULL);
    adapter.get_capabilities = NULL;
    ASSERT_INT_EQ(lp_external_adapter_register(&adapter), -1,
                  "register: missing capabilities callback rejected");

    adapter = build_fixture_adapter(&fx, LP_EXTERNAL_PROVIDER_HIGHS, NULL);
    adapter.solve = NULL;
    ASSERT_INT_EQ(lp_external_adapter_register(&adapter), -1,
                  "register: missing solve callback rejected");

    adapter = build_fixture_adapter(&fx, LP_EXTERNAL_PROVIDER_HIGHS, NULL);
    fx.caps_rc = -1;
    ASSERT_INT_EQ(lp_external_adapter_register(&adapter), -1,
                  "register: failing capabilities callback rejected");

    fx.caps_rc = 0;
    fx.caps.supports_simplex = 0;
    fx.caps.supports_dual_simplex = 0;
    fx.caps.supports_barrier = 0;
    ASSERT_INT_EQ(lp_external_adapter_register(&adapter), -1,
                  "register: adapter with zero backends rejected");

    lp_external_adapter_unregister();
}

static void test_register_lifecycle_and_solve(void) {
    AdapterFixture fx;
    LPExternalAdapter adapter;
    LPExternalCapabilities caps;
    SimplexSolver dummy_solver;

    memset(&fx, 0, sizeof(fx));
    fx.caps.supports_simplex = 1;
    fx.caps.supports_dual_simplex = 1;
    fx.caps.supports_barrier = 1;
    fx.caps.supports_crossover = 1;
    fx.caps_rc = 0;
    fx.solve_rc = 0;
    fx.last_backend = LP_EXTERNAL_BACKEND_SIMPLEX;

    memset(&dummy_solver, 0, sizeof(dummy_solver));

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(lp_external_adapter_is_registered(), 0,
                  "lifecycle: starts unregistered");
    ASSERT_INT_EQ((int)lp_external_adapter_provider(), (int)LP_EXTERNAL_PROVIDER_NONE,
                  "lifecycle: provider none when unregistered");
    ASSERT_TRUE(strcmp(lp_external_adapter_provider_name(), "none") == 0,
                "lifecycle: provider-name none when unregistered");
    ASSERT_INT_EQ(lp_external_adapter_get_capabilities(&caps), -1,
                  "lifecycle: capabilities unavailable when unregistered");
    ASSERT_INT_EQ(lp_external_adapter_solve(LP_EXTERNAL_BACKEND_SIMPLEX, &dummy_solver), -1,
                  "lifecycle: solve unavailable when unregistered");

    adapter = build_fixture_adapter(&fx, LP_EXTERNAL_PROVIDER_GLOP, "FakeGLOPAdapter");
    ASSERT_INT_EQ(lp_external_adapter_register(&adapter), 0,
                  "lifecycle: register succeeds");
    ASSERT_INT_EQ(lp_external_adapter_is_registered(), 1,
                  "lifecycle: registered state true");
    ASSERT_INT_EQ((int)lp_external_adapter_provider(), (int)LP_EXTERNAL_PROVIDER_GLOP,
                  "lifecycle: provider stored");
    ASSERT_TRUE(strcmp(lp_external_adapter_provider_name(), "FakeGLOPAdapter") == 0,
                "lifecycle: provider-name override stored");

    memset(&caps, 0, sizeof(caps));
    ASSERT_INT_EQ(lp_external_adapter_get_capabilities(&caps), 0,
                  "lifecycle: capabilities query succeeds");
    ASSERT_INT_EQ(caps.supports_simplex, 1,
                  "lifecycle: capabilities simplex");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "lifecycle: capabilities dual simplex");
    ASSERT_INT_EQ(caps.supports_barrier, 1,
                  "lifecycle: capabilities barrier");
    ASSERT_INT_EQ(caps.supports_crossover, 1,
                  "lifecycle: capabilities crossover");

    ASSERT_INT_EQ(lp_external_adapter_solve(LP_EXTERNAL_BACKEND_DUAL_SIMPLEX, &dummy_solver), 0,
                  "lifecycle: solve callback success");
    ASSERT_INT_EQ(fx.solve_calls, 1,
                  "lifecycle: solve callback counted");
    ASSERT_INT_EQ((int)fx.last_backend, (int)LP_EXTERNAL_BACKEND_DUAL_SIMPLEX,
                  "lifecycle: solve backend kind forwarded");

    fx.solve_rc = -7;
    ASSERT_INT_EQ(lp_external_adapter_solve(LP_EXTERNAL_BACKEND_BARRIER, &dummy_solver), -7,
                  "lifecycle: solve callback error forwarded");

    lp_external_adapter_unregister();
    ASSERT_INT_EQ(lp_external_adapter_is_registered(), 0,
                  "lifecycle: unregister resets state");
    ASSERT_INT_EQ(lp_external_adapter_get_capabilities(&caps), -1,
                  "lifecycle: capabilities unavailable after unregister");
}

int main(void) {
    printf("=== LP External Adapter Module Tests ===\n");

    lp_external_adapter_unregister();
    test_provider_names();
    test_register_validation();
    test_register_lifecycle_and_solve();
    lp_external_adapter_unregister();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
