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

static int lp_external_provider_valid(LPExternalProvider provider) {
    return provider >= LP_EXTERNAL_PROVIDER_GLPK &&
           provider <= LP_EXTERNAL_PROVIDER_GLOP;
}

static LPExternalAdapterEntry* lp_external_registry_entry(LPExternalProvider provider) {
    if (!lp_external_provider_valid(provider)) return NULL;
    return &g_lp_external_registry.entries[(int)provider];
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

    if (!adapter) return -1;
    if (adapter->abi_version != LP_EXTERNAL_ADAPTER_ABI_VERSION) return -1;
    if (!lp_external_provider_valid(adapter->provider)) return -1;
    if (!adapter->get_capabilities || !adapter->solve) return -1;

    memset(&caps, 0, sizeof(caps));
    if (adapter->get_capabilities(&caps, adapter->user_data) != 0) return -1;
    if (!lp_external_capabilities_valid(&caps)) return -1;

    entry = lp_external_registry_entry(adapter->provider);
    if (!entry) return -1;
    entry->adapter = *adapter;
    entry->registered = 1;
    return 0;
}

int lp_external_adapter_unregister(LPExternalProvider provider) {
    LPExternalAdapterEntry *entry = lp_external_registry_entry(provider);
    if (!entry) return -1;
    memset(entry, 0, sizeof(*entry));
    return 0;
}

void lp_external_adapter_unregister_all(void) {
    memset(&g_lp_external_registry, 0, sizeof(g_lp_external_registry));
}

int lp_external_adapter_is_registered(LPExternalProvider provider) {
    if (provider == LP_EXTERNAL_PROVIDER_NONE) {
        for (int p = (int)LP_EXTERNAL_PROVIDER_GLPK;
             p <= (int)LP_EXTERNAL_PROVIDER_GLOP;
             p++) {
            if (g_lp_external_registry.entries[p].registered) return 1;
        }
        return 0;
    }

    LPExternalAdapterEntry *entry = lp_external_registry_entry(provider);
    if (!entry) return 0;
    return entry->registered ? 1 : 0;
}

const char* lp_external_adapter_registered_name(LPExternalProvider provider) {
    LPExternalAdapterEntry *entry = lp_external_registry_entry(provider);
    if (!entry || !entry->registered) return lp_external_provider_name(LP_EXTERNAL_PROVIDER_NONE);
    if (entry->adapter.provider_name) return entry->adapter.provider_name;
    return lp_external_provider_name(provider);
}

int lp_external_adapter_get_capabilities(LPExternalProvider provider,
                                         LPExternalCapabilities *caps) {
    LPExternalAdapterEntry *entry;

    if (!caps) return -1;
    if (!lp_external_provider_valid(provider)) return -1;

    entry = lp_external_registry_entry(provider);
    if (!entry || !entry->registered) return -1;

    memset(caps, 0, sizeof(*caps));
    if (entry->adapter.get_capabilities(caps, entry->adapter.user_data) != 0) {
        memset(caps, 0, sizeof(*caps));
        return -1;
    }
    if (!lp_external_capabilities_valid(caps)) {
        memset(caps, 0, sizeof(*caps));
        return -1;
    }
    return 0;
}

int lp_external_adapter_solve(LPExternalProvider provider,
                              LPExternalBackendKind backend,
                              SimplexSolver *solver) {
    LPExternalAdapterEntry *entry;

    if (!lp_external_provider_valid(provider)) return -1;
    entry = lp_external_registry_entry(provider);
    if (!entry || !entry->registered) return -1;
    if (!solver) return -1;
    if (backend < LP_EXTERNAL_BACKEND_SIMPLEX || backend > LP_EXTERNAL_BACKEND_BARRIER) return -1;

    return entry->adapter.solve(backend, solver, entry->adapter.user_data);
}
