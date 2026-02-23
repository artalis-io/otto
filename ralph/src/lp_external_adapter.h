#ifndef LP_EXTERNAL_ADAPTER_H
#define LP_EXTERNAL_ADAPTER_H

#include "lp.h"

#define LP_EXTERNAL_ADAPTER_ABI_VERSION 1

typedef enum {
    LP_EXTERNAL_PROVIDER_NONE = 0,
    LP_EXTERNAL_PROVIDER_GLPK = 1,
    LP_EXTERNAL_PROVIDER_HIGHS = 2,
    LP_EXTERNAL_PROVIDER_CLP = 3,
    LP_EXTERNAL_PROVIDER_CPLEX = 4,
    LP_EXTERNAL_PROVIDER_GUROBI = 5,
    LP_EXTERNAL_PROVIDER_GLOP = 6
} LPExternalProvider;

typedef enum {
    LP_EXTERNAL_BACKEND_SIMPLEX = 0,
    LP_EXTERNAL_BACKEND_DUAL_SIMPLEX = 1,
    LP_EXTERNAL_BACKEND_BARRIER = 2
} LPExternalBackendKind;

typedef struct {
    int supports_simplex;
    int supports_dual_simplex;
    int supports_barrier;
    int supports_crossover;
} LPExternalCapabilities;

typedef struct {
    int abi_version;
    LPExternalProvider provider;
    const char *provider_name;  /* Optional override; may be NULL. */
    int (*get_capabilities)(LPExternalCapabilities *caps, void *user_data);
    int (*solve)(LPExternalBackendKind backend, SimplexSolver *solver, void *user_data);
    void *user_data;
} LPExternalAdapter;

const char* lp_external_provider_name(LPExternalProvider provider);

int lp_external_adapter_register(const LPExternalAdapter *adapter);
void lp_external_adapter_unregister(void);
int lp_external_adapter_is_registered(void);
LPExternalProvider lp_external_adapter_provider(void);
const char* lp_external_adapter_provider_name(void);

int lp_external_adapter_get_capabilities(LPExternalCapabilities *caps);
int lp_external_adapter_solve(LPExternalBackendKind backend, SimplexSolver *solver);

#endif
