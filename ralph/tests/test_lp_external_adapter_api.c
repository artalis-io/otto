/*
 * Tests for public LP external adapter API surface.
 *
 * This module is intentionally separate from internal adapter/dispatch tests.
 */

#include <stdio.h>
#include <string.h>
#include "sh_pal.h"
#include <stdatomic.h>

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

typedef struct {
    RalphLPExternalCapabilities caps;
    int caps_rc;
    int solve_rc;
    int solve_calls;
    RalphLPExternalBackendKind last_backend;
} PublicAdapterFixture;

static int public_get_capabilities(RalphLPExternalCapabilities *caps, void *user_data) {
    PublicAdapterFixture *fx = (PublicAdapterFixture*)user_data;
    if (!fx || !caps) return -1;
    *caps = fx->caps;
    return fx->caps_rc;
}

static int public_solve(RalphLPExternalBackendKind backend,
                        void *solver_handle,
                        void *user_data) {
    PublicAdapterFixture *fx = (PublicAdapterFixture*)user_data;
    if (!fx || !solver_handle) return -1;
    fx->solve_calls++;
    fx->last_backend = backend;
    return fx->solve_rc;
}

static RalphLPExternalAdapter build_public_adapter(PublicAdapterFixture *fx,
                                                   RalphLPExternalProvider provider,
                                                   const char *provider_name) {
    RalphLPExternalAdapter adapter;
    memset(&adapter, 0, sizeof(adapter));
    adapter.abi_version = RALPH_LP_EXTERNAL_ADAPTER_ABI_VERSION;
    adapter.provider = provider;
    adapter.provider_name = provider_name;
    adapter.get_capabilities = public_get_capabilities;
    adapter.solve = public_solve;
    adapter.user_data = fx;
    return adapter;
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

typedef struct {
    RalphLPExternalAdapter adapter;
    int iterations;
    atomic_int failed;
} PublicThreadHarness;

static int public_thread_get_capabilities(RalphLPExternalCapabilities *caps, void *user_data) {
    (void)user_data;
    if (!caps) return -1;
    memset(caps, 0, sizeof(*caps));
    caps->supports_simplex = 1;
    return 0;
}

static int public_thread_solve(RalphLPExternalBackendKind backend,
                               void *solver_handle,
                               void *user_data) {
    (void)backend;
    (void)solver_handle;
    (void)user_data;
    return -1;
}

static void* public_thread_writer(void *arg) {
    PublicThreadHarness *harness = (PublicThreadHarness*)arg;
    for (int i = 0; i < harness->iterations; i++) {
        if (ralph_lp_external_register_adapter(&harness->adapter) != 0) {
            atomic_store(&harness->failed, 1);
            break;
        }
        if ((i & 1) == 0) {
            if (ralph_lp_external_unregister_adapter(RALPH_LP_EXTERNAL_PROVIDER_GLPK) != 0) {
                atomic_store(&harness->failed, 1);
                break;
            }
        }
    }
    (void)ralph_lp_external_unregister_adapter(RALPH_LP_EXTERNAL_PROVIDER_GLPK);
    return NULL;
}

static void* public_thread_reader(void *arg) {
    PublicThreadHarness *harness = (PublicThreadHarness*)arg;
    for (int i = 0; i < harness->iterations; i++) {
        RalphLPCapabilities caps;
        int registered;
        const char *provider_name;

        memset(&caps, 0, sizeof(caps));
        if (ralph_lp_get_capabilities(&caps) != 0) {
            atomic_store(&harness->failed, 1);
            break;
        }
        registered = ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_GLPK);
        if (registered != 0 && registered != 1) {
            atomic_store(&harness->failed, 1);
            break;
        }
        provider_name = ralph_lp_external_provider_name(RALPH_LP_EXTERNAL_PROVIDER_GLPK);
        if (!provider_name) {
            atomic_store(&harness->failed, 1);
            break;
        }
    }
    return NULL;
}

static void test_public_adapter_validation_and_lifecycle(void) {
    PublicAdapterFixture glpk_fx;
    PublicAdapterFixture clp_fx;
    RalphLPExternalAdapter glpk_adapter;
    RalphLPExternalAdapter clp_adapter;

    memset(&glpk_fx, 0, sizeof(glpk_fx));
    glpk_fx.caps.supports_simplex = 1;
    glpk_fx.caps.supports_dual_simplex = 0;
    glpk_fx.caps.supports_barrier = 0;
    glpk_fx.caps.supports_crossover = 0;

    memset(&clp_fx, 0, sizeof(clp_fx));
    clp_fx.caps.supports_simplex = 0;
    clp_fx.caps.supports_dual_simplex = 1;
    clp_fx.caps.supports_barrier = 1;
    clp_fx.caps.supports_crossover = 1;

    ralph_lp_external_unregister_all_adapters();
    ASSERT_INT_EQ(ralph_lp_external_register_adapter(NULL), -1,
                  "public adapter: reject NULL adapter");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_NONE), 0,
                  "public adapter: starts unregistered");

    glpk_adapter = build_public_adapter(&glpk_fx, RALPH_LP_EXTERNAL_PROVIDER_GLPK, "PublicGLPK");
    clp_adapter = build_public_adapter(&clp_fx, RALPH_LP_EXTERNAL_PROVIDER_CLP, "PublicCLP");
    ASSERT_INT_EQ(ralph_lp_external_register_adapter(&glpk_adapter), 0,
                  "public adapter: register GLPK");
    ASSERT_INT_EQ(ralph_lp_external_register_adapter(&clp_adapter), 0,
                  "public adapter: register CLP");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_NONE), 1,
                  "public adapter: any registered");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 1,
                  "public adapter: GLPK registered");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_CLP), 1,
                  "public adapter: CLP registered");
    ASSERT_TRUE(strcmp(ralph_lp_external_provider_name(RALPH_LP_EXTERNAL_PROVIDER_GLPK), "GLPK") == 0,
                "public adapter: provider name GLPK");
    ASSERT_TRUE(strcmp(ralph_lp_external_provider_name(RALPH_LP_EXTERNAL_PROVIDER_CLP), "CLP") == 0,
                "public adapter: provider name CLP");

    ASSERT_INT_EQ(ralph_lp_external_unregister_adapter(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 0,
                  "public adapter: unregister GLPK");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 0,
                  "public adapter: GLPK removed");
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_CLP), 1,
                  "public adapter: CLP remains");

    ralph_lp_external_unregister_all_adapters();
    ASSERT_INT_EQ(ralph_lp_external_is_adapter_registered(RALPH_LP_EXTERNAL_PROVIDER_NONE), 0,
                  "public adapter: unregister all clears registry");
}

static void test_public_adapter_provider_dispatch(void) {
    PublicAdapterFixture glpk_fx;
    PublicAdapterFixture clp_fx;
    RalphLPExternalAdapter glpk_adapter;
    RalphLPExternalAdapter clp_adapter;
    RalphModel *model = NULL;

    memset(&glpk_fx, 0, sizeof(glpk_fx));
    glpk_fx.caps.supports_simplex = 1;
    glpk_fx.caps.supports_dual_simplex = 0;
    glpk_fx.caps.supports_barrier = 0;
    glpk_fx.caps.supports_crossover = 0;
    glpk_fx.solve_rc = -1;

    memset(&clp_fx, 0, sizeof(clp_fx));
    clp_fx.caps.supports_simplex = 0;
    clp_fx.caps.supports_dual_simplex = 1;
    clp_fx.caps.supports_barrier = 0;
    clp_fx.caps.supports_crossover = 0;
    clp_fx.solve_rc = -1;

    glpk_adapter = build_public_adapter(&glpk_fx, RALPH_LP_EXTERNAL_PROVIDER_GLPK, "PublicGLPK");
    clp_adapter = build_public_adapter(&clp_fx, RALPH_LP_EXTERNAL_PROVIDER_CLP, "PublicCLP");
    ralph_lp_external_unregister_all_adapters();
    ASSERT_INT_EQ(ralph_lp_external_register_adapter(&glpk_adapter), 0,
                  "public dispatch: register GLPK");
    ASSERT_INT_EQ(ralph_lp_external_register_adapter(&clp_adapter), 0,
                  "public dispatch: register CLP");

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "public dispatch: model created for GLPK");
    if (model) {
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                      0,
                      "public dispatch: set external primal");
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                      0,
                      "public dispatch: set provider GLPK");
        ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                      "public dispatch: GLPK adapter solve rc propagates");
        ASSERT_INT_EQ(glpk_fx.solve_calls, 1,
                      "public dispatch: GLPK solve called");
        ASSERT_INT_EQ(clp_fx.solve_calls, 0,
                      "public dispatch: CLP not called for GLPK request");
        ASSERT_INT_EQ((int)glpk_fx.last_backend, (int)RALPH_LP_EXTERNAL_BACKEND_SIMPLEX,
                      "public dispatch: GLPK simplex backend selected");
        ralph_test_free(model);
    }

    model = build_small_lp();
    ASSERT_TRUE(model != NULL, "public dispatch: model created for CLP");
    if (model) {
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL),
                      0,
                      "public dispatch: set external dual");
        ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_CLP),
                      0,
                      "public dispatch: set provider CLP");
        ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                      "public dispatch: CLP adapter solve rc propagates");
        ASSERT_INT_EQ(clp_fx.solve_calls, 1,
                      "public dispatch: CLP solve called");
        ASSERT_INT_EQ((int)clp_fx.last_backend, (int)RALPH_LP_EXTERNAL_BACKEND_DUAL_SIMPLEX,
                      "public dispatch: CLP dual backend selected");
        ralph_test_free(model);
    }

    ralph_lp_external_unregister_all_adapters();
}

static void test_public_adapter_thread_safety(void) {
    PublicThreadHarness harness;
    ShThread writer_thread;
    ShThread reader_thread;

    memset(&harness, 0, sizeof(harness));
    harness.iterations = 2000;
    atomic_init(&harness.failed, 0);

    memset(&harness.adapter, 0, sizeof(harness.adapter));
    harness.adapter.abi_version = RALPH_LP_EXTERNAL_ADAPTER_ABI_VERSION;
    harness.adapter.provider = RALPH_LP_EXTERNAL_PROVIDER_GLPK;
    harness.adapter.provider_name = "PublicThreadSafeGLPK";
    harness.adapter.get_capabilities = public_thread_get_capabilities;
    harness.adapter.solve = public_thread_solve;

    ralph_lp_external_unregister_all_adapters();
    ASSERT_INT_EQ(sh_thread_create(&writer_thread, public_thread_writer, &harness),
                  0,
                  "public thread-safety: create writer thread");
    ASSERT_INT_EQ(sh_thread_create(&reader_thread, public_thread_reader, &harness),
                  0,
                  "public thread-safety: create reader thread");

    ASSERT_INT_EQ(sh_thread_join(&writer_thread, NULL), 0,
                  "public thread-safety: join writer thread");
    ASSERT_INT_EQ(sh_thread_join(&reader_thread, NULL), 0,
                  "public thread-safety: join reader thread");
    ASSERT_INT_EQ(atomic_load(&harness.failed), 0,
                  "public thread-safety: registry/capability queries stable");

    ralph_lp_external_unregister_all_adapters();
}

int main(void) {
    printf("=== LP External Adapter Public API Tests ===\n");

    test_public_adapter_validation_and_lifecycle();
    test_public_adapter_provider_dispatch();
    test_public_adapter_thread_safety();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
