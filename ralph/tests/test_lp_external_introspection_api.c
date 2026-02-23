/*
 * Tests for LP external provider introspection APIs.
 *
 * This module is intentionally separate from adapter lifecycle/failure tests.
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
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
} IntroFixture;

static int fixture_get_capabilities(RalphLPExternalCapabilities *caps, void *user_data) {
    IntroFixture *fx = (IntroFixture*)user_data;
    if (!fx || !caps) return -1;
    *caps = fx->caps;
    return fx->caps_rc;
}

static int fixture_solve(RalphLPExternalBackendKind backend,
                         void *solver_handle,
                         void *user_data) {
    IntroFixture *fx = (IntroFixture*)user_data;
    (void)backend;
    (void)solver_handle;
    if (!fx) return -1;
    return fx->solve_rc;
}

static RalphLPExternalAdapter build_adapter(IntroFixture *fx,
                                            RalphLPExternalProvider provider,
                                            const char *provider_name) {
    RalphLPExternalAdapter adapter;
    memset(&adapter, 0, sizeof(adapter));
    adapter.abi_version = RALPH_LP_EXTERNAL_ADAPTER_ABI_VERSION;
    adapter.provider = provider;
    adapter.provider_name = provider_name;
    adapter.get_capabilities = fixture_get_capabilities;
    adapter.solve = fixture_solve;
    adapter.user_data = fx;
    return adapter;
}

static int provider_in_list(const RalphLPExternalProvider *providers,
                            int count,
                            RalphLPExternalProvider provider) {
    for (int i = 0; i < count; i++) {
        if (providers[i] == provider) return 1;
    }
    return 0;
}

static void test_introspection_guards_and_empty(void) {
    RalphLPExternalCapabilities caps;
    RalphLPExternalProvider providers[8];
    int count = -1;

    ralph_unregister_all_lp_external_adapters();

    ASSERT_INT_EQ(ralph_get_lp_external_provider_capabilities(
                      RALPH_LP_EXTERNAL_PROVIDER_GLPK, NULL),
                  -1,
                  "guards: NULL caps rejected");
    ASSERT_INT_EQ(ralph_get_lp_external_provider_capabilities(
                      RALPH_LP_EXTERNAL_PROVIDER_NONE, &caps),
                  -1,
                  "guards: provider none rejected");
    ASSERT_INT_EQ(ralph_get_lp_external_provider_capabilities(
                      (RalphLPExternalProvider)99, &caps),
                  -1,
                  "guards: invalid provider rejected");
    ASSERT_INT_EQ(ralph_get_lp_external_provider_capabilities(
                      RALPH_LP_EXTERNAL_PROVIDER_GLPK, &caps),
                  -1,
                  "guards: unregistered provider unavailable");

    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(NULL, 0, NULL),
                  -1,
                  "guards: NULL count rejected");
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(NULL, -1, &count),
                  -1,
                  "guards: negative capacity rejected");
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(NULL, 2, &count),
                  -1,
                  "guards: NULL providers with nonzero capacity rejected");

    count = -1;
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(NULL, 0, &count),
                  0,
                  "guards: empty query succeeds");
    ASSERT_INT_EQ(count, 0,
                  "guards: empty query count is zero");

    memset(providers, 0, sizeof(providers));
    count = -1;
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(providers, 8, &count),
                  0,
                  "guards: empty list request succeeds");
    ASSERT_INT_EQ(count, 0,
                  "guards: empty list count zero");
}

static void test_introspection_capabilities_and_list(void) {
    IntroFixture glpk_fx;
    IntroFixture clp_fx;
    RalphLPExternalAdapter glpk_adapter;
    RalphLPExternalAdapter clp_adapter;
    RalphLPExternalCapabilities caps;
    RalphLPExternalProvider providers[8];
    int count = -1;

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

    glpk_adapter = build_adapter(&glpk_fx, RALPH_LP_EXTERNAL_PROVIDER_GLPK, "IntroGLPK");
    clp_adapter = build_adapter(&clp_fx, RALPH_LP_EXTERNAL_PROVIDER_CLP, "IntroCLP");

    ralph_unregister_all_lp_external_adapters();
    ASSERT_INT_EQ(ralph_register_lp_external_adapter(&glpk_adapter), 0,
                  "introspection: register GLPK");
    ASSERT_INT_EQ(ralph_register_lp_external_adapter(&clp_adapter), 0,
                  "introspection: register CLP");

    memset(&caps, 0, sizeof(caps));
    ASSERT_INT_EQ(ralph_get_lp_external_provider_capabilities(
                      RALPH_LP_EXTERNAL_PROVIDER_GLPK, &caps),
                  0,
                  "introspection: GLPK caps available");
    ASSERT_INT_EQ(caps.supports_simplex, 1,
                  "introspection: GLPK supports simplex");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 0,
                  "introspection: GLPK dual unsupported");
    ASSERT_INT_EQ(caps.supports_barrier, 0,
                  "introspection: GLPK barrier unsupported");

    memset(&caps, 0, sizeof(caps));
    ASSERT_INT_EQ(ralph_get_lp_external_provider_capabilities(
                      RALPH_LP_EXTERNAL_PROVIDER_CLP, &caps),
                  0,
                  "introspection: CLP caps available");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "introspection: CLP dual supported");
    ASSERT_INT_EQ(caps.supports_barrier, 1,
                  "introspection: CLP barrier supported");
    ASSERT_INT_EQ(caps.supports_crossover, 1,
                  "introspection: CLP crossover supported");

    count = -1;
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(NULL, 0, &count),
                  0,
                  "introspection: count-only query succeeds");
    ASSERT_INT_EQ(count, 2,
                  "introspection: count-only query reports two providers");

    memset(providers, 0, sizeof(providers));
    count = -1;
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(providers, 1, &count),
                  -1,
                  "introspection: insufficient capacity rejected");
    ASSERT_INT_EQ(count, 2,
                  "introspection: insufficient-capacity returns required count");

    memset(providers, 0, sizeof(providers));
    count = -1;
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(providers, 8, &count),
                  0,
                  "introspection: list query succeeds");
    ASSERT_INT_EQ(count, 2,
                  "introspection: list query returns two providers");
    ASSERT_INT_EQ(provider_in_list(providers, count, RALPH_LP_EXTERNAL_PROVIDER_GLPK), 1,
                  "introspection: GLPK present in provider list");
    ASSERT_INT_EQ(provider_in_list(providers, count, RALPH_LP_EXTERNAL_PROVIDER_CLP), 1,
                  "introspection: CLP present in provider list");

    ASSERT_INT_EQ(ralph_unregister_lp_external_adapter(RALPH_LP_EXTERNAL_PROVIDER_GLPK), 0,
                  "introspection: unregister GLPK");
    count = -1;
    ASSERT_INT_EQ(ralph_get_lp_external_registered_providers(NULL, 0, &count),
                  0,
                  "introspection: count query after unregister succeeds");
    ASSERT_INT_EQ(count, 1,
                  "introspection: one provider remains");

    ralph_unregister_all_lp_external_adapters();
}

typedef struct {
    IntroFixture glpk_fx;
    IntroFixture clp_fx;
    RalphLPExternalAdapter glpk_adapter;
    RalphLPExternalAdapter clp_adapter;
    int iterations;
    atomic_int failed;
} IntroThreadHarness;

static void* intro_writer_thread(void *arg) {
    IntroThreadHarness *h = (IntroThreadHarness*)arg;
    for (int i = 0; i < h->iterations; i++) {
        if ((i & 1) == 0) {
            if (ralph_register_lp_external_adapter(&h->glpk_adapter) != 0) {
                atomic_store(&h->failed, 1);
                break;
            }
        } else {
            if (ralph_unregister_lp_external_adapter(RALPH_LP_EXTERNAL_PROVIDER_GLPK) != 0) {
                atomic_store(&h->failed, 1);
                break;
            }
        }

        if ((i % 3) == 0) {
            if (ralph_register_lp_external_adapter(&h->clp_adapter) != 0) {
                atomic_store(&h->failed, 1);
                break;
            }
        } else if ((i % 5) == 0) {
            if (ralph_unregister_lp_external_adapter(RALPH_LP_EXTERNAL_PROVIDER_CLP) != 0) {
                atomic_store(&h->failed, 1);
                break;
            }
        }
    }

    (void)ralph_unregister_lp_external_adapter(RALPH_LP_EXTERNAL_PROVIDER_GLPK);
    (void)ralph_unregister_lp_external_adapter(RALPH_LP_EXTERNAL_PROVIDER_CLP);
    return NULL;
}

static void* intro_reader_thread(void *arg) {
    IntroThreadHarness *h = (IntroThreadHarness*)arg;
    for (int i = 0; i < h->iterations; i++) {
        RalphLPExternalProvider providers[8];
        RalphLPExternalCapabilities caps;
        int count = -1;
        int rc;

        memset(providers, 0, sizeof(providers));
        rc = ralph_get_lp_external_registered_providers(providers, 8, &count);
        if (rc != 0) {
            atomic_store(&h->failed, 1);
            break;
        }
        if (count < 0 || count > (int)RALPH_LP_EXTERNAL_PROVIDER_GLOP) {
            atomic_store(&h->failed, 1);
            break;
        }

        for (int p = 0; p < count; p++) {
            RalphLPExternalProvider provider = providers[p];
            int caps_rc;
            if (provider < RALPH_LP_EXTERNAL_PROVIDER_GLPK ||
                provider > RALPH_LP_EXTERNAL_PROVIDER_GLOP) {
                atomic_store(&h->failed, 1);
                break;
            }
            memset(&caps, 0, sizeof(caps));
            caps_rc = ralph_get_lp_external_provider_capabilities(provider, &caps);
            if (caps_rc != 0 && caps_rc != -1) {
                atomic_store(&h->failed, 1);
                break;
            }
            if (caps_rc == 0 &&
                !caps.supports_simplex &&
                !caps.supports_dual_simplex &&
                !caps.supports_barrier) {
                atomic_store(&h->failed, 1);
                break;
            }
        }
        if (atomic_load(&h->failed)) break;
    }
    return NULL;
}

static void test_introspection_thread_safety(void) {
    IntroThreadHarness harness;
    pthread_t writer;
    pthread_t reader;

    memset(&harness, 0, sizeof(harness));
    harness.iterations = 2000;
    atomic_init(&harness.failed, 0);

    harness.glpk_fx.caps.supports_simplex = 1;
    harness.glpk_fx.caps.supports_dual_simplex = 0;
    harness.glpk_fx.caps.supports_barrier = 0;
    harness.glpk_fx.caps.supports_crossover = 0;
    harness.clp_fx.caps.supports_simplex = 0;
    harness.clp_fx.caps.supports_dual_simplex = 1;
    harness.clp_fx.caps.supports_barrier = 1;
    harness.clp_fx.caps.supports_crossover = 1;
    harness.glpk_adapter =
        build_adapter(&harness.glpk_fx, RALPH_LP_EXTERNAL_PROVIDER_GLPK, "ThreadGLPK");
    harness.clp_adapter =
        build_adapter(&harness.clp_fx, RALPH_LP_EXTERNAL_PROVIDER_CLP, "ThreadCLP");

    ralph_unregister_all_lp_external_adapters();
    ASSERT_INT_EQ(pthread_create(&writer, NULL, intro_writer_thread, &harness), 0,
                  "thread-safety: create writer");
    ASSERT_INT_EQ(pthread_create(&reader, NULL, intro_reader_thread, &harness), 0,
                  "thread-safety: create reader");
    ASSERT_INT_EQ(pthread_join(writer, NULL), 0,
                  "thread-safety: join writer");
    ASSERT_INT_EQ(pthread_join(reader, NULL), 0,
                  "thread-safety: join reader");
    ASSERT_INT_EQ(atomic_load(&harness.failed), 0,
                  "thread-safety: introspection APIs stable under churn");

    ralph_unregister_all_lp_external_adapters();
}

int main(void) {
    printf("=== LP External Introspection API Tests ===\n");

    test_introspection_guards_and_empty();
    test_introspection_capabilities_and_list();
    test_introspection_thread_safety();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
