#include <pthread.h>
#include <string.h>

#include "lp_external_adapter.h"

typedef struct {
    int registered;
    LPExternalAdapter adapter;
} LPExternalAdapterEntry;

typedef struct {
    LPExternalAdapterEntry entries[LP_EXTERNAL_PROVIDER_GLOP + 1];
} LPExternalAdapterRegistry;

static LPExternalAdapterRegistry g_lp_external_registry = {0};
static pthread_mutex_t g_lp_external_registry_mutex;
static pthread_once_t g_lp_external_registry_mutex_once = PTHREAD_ONCE_INIT;
static int g_lp_external_registry_mutex_ready = 0;

static void lp_external_registry_mutex_init_once(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return;
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
        pthread_mutexattr_destroy(&attr);
        return;
    }
    if (pthread_mutex_init(&g_lp_external_registry_mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return;
    }
    pthread_mutexattr_destroy(&attr);
    g_lp_external_registry_mutex_ready = 1;
}

static int lp_external_registry_lock(void) {
    if (pthread_once(&g_lp_external_registry_mutex_once,
                     lp_external_registry_mutex_init_once) != 0) {
        return -1;
    }
    if (!g_lp_external_registry_mutex_ready) return -1;
    if (pthread_mutex_lock(&g_lp_external_registry_mutex) != 0) return -1;
    return 0;
}

static void lp_external_registry_unlock(void) {
    if (!g_lp_external_registry_mutex_ready) return;
    (void)pthread_mutex_unlock(&g_lp_external_registry_mutex);
}

static int lp_external_provider_valid(LPExternalProvider provider) {
    return provider >= LP_EXTERNAL_PROVIDER_GLPK &&
           provider <= LP_EXTERNAL_PROVIDER_GLOP;
}

static LPExternalAdapterEntry* lp_external_registry_entry(LPExternalProvider provider) {
    if (!lp_external_provider_valid(provider)) return NULL;
    return &g_lp_external_registry.entries[(int)provider];
}

static void lp_external_registry_entry_reset(LPExternalAdapterEntry *entry) {
    if (!entry) return;
    if (entry->registered && entry->adapter.destroy_user_data && entry->adapter.user_data) {
        entry->adapter.destroy_user_data(entry->adapter.user_data);
    }
    memset(entry, 0, sizeof(*entry));
}

const char* lp_external_provider_name(LPExternalProvider provider) {
    switch (provider) {
        case LP_EXTERNAL_PROVIDER_GLPK:
            return "GLPK";
        case LP_EXTERNAL_PROVIDER_HIGHS:
            return "HiGHS";
        case LP_EXTERNAL_PROVIDER_CLP:
            return "CLP";
        case LP_EXTERNAL_PROVIDER_CPLEX:
            return "CPLEX";
        case LP_EXTERNAL_PROVIDER_GUROBI:
            return "Gurobi";
        case LP_EXTERNAL_PROVIDER_GLOP:
            return "GLOP";
        case LP_EXTERNAL_PROVIDER_NONE:
        default:
            return "none";
    }
}

static int lp_external_capabilities_valid(const LPExternalCapabilities *caps) {
    if (!caps) return 0;
    if (caps->supports_simplex < 0 || caps->supports_simplex > 1) return 0;
    if (caps->supports_dual_simplex < 0 || caps->supports_dual_simplex > 1) return 0;
    if (caps->supports_barrier < 0 || caps->supports_barrier > 1) return 0;
    if (caps->supports_crossover < 0 || caps->supports_crossover > 1) return 0;
    if (!caps->supports_simplex && !caps->supports_dual_simplex && !caps->supports_barrier) {
        return 0;
    }
    return 1;
}

int lp_external_adapter_register(const LPExternalAdapter *adapter) {
    LPExternalCapabilities caps;
    LPExternalAdapterEntry *entry;
    int rc = -1;

    if (!adapter) return -1;
    if (adapter->abi_version != LP_EXTERNAL_ADAPTER_ABI_VERSION) return -1;
    if (!lp_external_provider_valid(adapter->provider)) return -1;
    if (!adapter->get_capabilities || !adapter->solve) return -1;

    memset(&caps, 0, sizeof(caps));
    if (adapter->get_capabilities(&caps, adapter->user_data) != 0) return -1;
    if (!lp_external_capabilities_valid(&caps)) return -1;

    entry = lp_external_registry_entry(adapter->provider);
    if (!entry) return -1;
    if (lp_external_registry_lock() != 0) return -1;
    lp_external_registry_entry_reset(entry);
    entry->adapter = *adapter;
    entry->registered = 1;
    rc = 0;
    lp_external_registry_unlock();
    return rc;
}

int lp_external_adapter_unregister(LPExternalProvider provider) {
    int rc = -1;
    LPExternalAdapterEntry *entry = lp_external_registry_entry(provider);
    if (!entry) return -1;
    if (lp_external_registry_lock() != 0) return -1;
    lp_external_registry_entry_reset(entry);
    rc = 0;
    lp_external_registry_unlock();
    return rc;
}

void lp_external_adapter_unregister_all(void) {
    if (lp_external_registry_lock() != 0) return;
    for (int p = (int)LP_EXTERNAL_PROVIDER_GLPK;
         p <= (int)LP_EXTERNAL_PROVIDER_GLOP;
         p++) {
        lp_external_registry_entry_reset(&g_lp_external_registry.entries[p]);
    }
    lp_external_registry_unlock();
}

int lp_external_adapter_is_registered(LPExternalProvider provider) {
    int is_registered = 0;
    if (lp_external_registry_lock() != 0) return 0;

    if (provider == LP_EXTERNAL_PROVIDER_NONE) {
        for (int p = (int)LP_EXTERNAL_PROVIDER_GLPK;
             p <= (int)LP_EXTERNAL_PROVIDER_GLOP;
             p++) {
            if (g_lp_external_registry.entries[p].registered) {
                is_registered = 1;
                break;
            }
        }
        lp_external_registry_unlock();
        return is_registered;
    }

    {
        LPExternalAdapterEntry *entry = lp_external_registry_entry(provider);
        if (entry) is_registered = entry->registered ? 1 : 0;
    }
    lp_external_registry_unlock();
    return is_registered;
}

int lp_external_adapter_list_registered(LPExternalProvider *providers,
                                        int capacity,
                                        int *count) {
    int needed = 0;
    int out_idx = 0;
    int query_only = 0;

    if (!count) return -1;
    if (capacity < 0) return -1;
    if (!providers && capacity > 0) return -1;
    query_only = (!providers && capacity == 0) ? 1 : 0;
    if (lp_external_registry_lock() != 0) return -1;

    for (int p = (int)LP_EXTERNAL_PROVIDER_GLPK;
         p <= (int)LP_EXTERNAL_PROVIDER_GLOP;
         p++) {
        if (!g_lp_external_registry.entries[p].registered) continue;
        needed++;
        if (providers && out_idx < capacity) {
            providers[out_idx++] = (LPExternalProvider)p;
        }
    }

    *count = needed;
    lp_external_registry_unlock();
    if (query_only) return 0;
    if (needed > capacity) return -1;
    return 0;
}

const char* lp_external_adapter_registered_name(LPExternalProvider provider) {
    const char *name = lp_external_provider_name(LP_EXTERNAL_PROVIDER_NONE);
    if (lp_external_registry_lock() != 0) return name;

    LPExternalAdapterEntry *entry = lp_external_registry_entry(provider);
    if (!entry || !entry->registered) {
        lp_external_registry_unlock();
        return name;
    }
    if (entry->adapter.provider_name) {
        name = entry->adapter.provider_name;
    } else {
        name = lp_external_provider_name(provider);
    }
    lp_external_registry_unlock();
    return name;
}

int lp_external_adapter_get_capabilities(LPExternalProvider provider,
                                         LPExternalCapabilities *caps) {
    LPExternalAdapterEntry *entry;
    int rc = -1;

    if (!caps) return -1;
    if (!lp_external_provider_valid(provider)) return -1;
    if (lp_external_registry_lock() != 0) return -1;

    entry = lp_external_registry_entry(provider);
    if (!entry || !entry->registered) {
        lp_external_registry_unlock();
        return -1;
    }

    memset(caps, 0, sizeof(*caps));
    if (entry->adapter.get_capabilities(caps, entry->adapter.user_data) != 0) {
        memset(caps, 0, sizeof(*caps));
        lp_external_registry_unlock();
        return -1;
    }
    if (!lp_external_capabilities_valid(caps)) {
        memset(caps, 0, sizeof(*caps));
        lp_external_registry_unlock();
        return -1;
    }
    rc = 0;
    lp_external_registry_unlock();
    return rc;
}

int lp_external_adapter_solve(LPExternalProvider provider,
                              LPExternalBackendKind backend,
                              SimplexSolver *solver) {
    LPExternalAdapterEntry *entry;
    int rc = -1;

    if (!lp_external_provider_valid(provider)) return -1;
    if (!solver) return -1;
    if (backend < LP_EXTERNAL_BACKEND_SIMPLEX || backend > LP_EXTERNAL_BACKEND_BARRIER) return -1;
    if (lp_external_registry_lock() != 0) return -1;

    entry = lp_external_registry_entry(provider);
    if (!entry || !entry->registered) {
        lp_external_registry_unlock();
        return -1;
    }

    rc = entry->adapter.solve(backend, solver, entry->adapter.user_data);
    lp_external_registry_unlock();
    return rc;
}
