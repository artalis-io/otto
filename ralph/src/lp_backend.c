#include "lp_backend.h"
#include "lp_external_adapter.h"

static int lp_backend_capability_from_external(int supports_backend,
                                               int supports_crossover,
                                               LPBackendCapability *capability) {
    if (!capability) return -1;
    capability->available = supports_backend ? 1 : 0;
    capability->supports_crossover =
        (supports_backend && supports_crossover) ? 1 : 0;
    return 0;
}

int lp_backend_get_capability(LPDispatchBackend backend, LPBackendCapability *capability) {
    LPExternalCapabilities external_caps;

    if (!capability) return -1;

    capability->available = 0;
    capability->supports_crossover = 0;

    switch (backend) {
        case LP_DISPATCH_BACKEND_SIMPLEX:
            capability->available = 1;
            capability->supports_crossover = 0;
            return 0;
        case LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL:
            if (lp_external_adapter_get_capabilities(&external_caps) != 0) return 0;
            return lp_backend_capability_from_external(external_caps.supports_simplex,
                                                       external_caps.supports_crossover,
                                                       capability);
        case LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL:
            if (lp_external_adapter_get_capabilities(&external_caps) != 0) return 0;
            return lp_backend_capability_from_external(external_caps.supports_dual_simplex,
                                                       external_caps.supports_crossover,
                                                       capability);
        case LP_DISPATCH_BACKEND_BARRIER_NATIVE:
            /* Native barrier backend is not implemented yet. */
            capability->available = 0;
            capability->supports_crossover = 0;
            return 0;
        case LP_DISPATCH_BACKEND_BARRIER_EXTERNAL:
            if (lp_external_adapter_get_capabilities(&external_caps) != 0) return 0;
            return lp_backend_capability_from_external(external_caps.supports_barrier,
                                                       external_caps.supports_crossover,
                                                       capability);
        default:
            return -1;
    }
}

int lp_backend_run(LPDispatchBackend backend, SimplexSolver *solver) {
    if (!solver) return -1;

    switch (backend) {
        case LP_DISPATCH_BACKEND_SIMPLEX:
            simplex_solve(solver);
            return 0;
        case LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL:
            return lp_external_adapter_solve(LP_EXTERNAL_BACKEND_SIMPLEX, solver);
        case LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL:
            return lp_external_adapter_solve(LP_EXTERNAL_BACKEND_DUAL_SIMPLEX, solver);
        case LP_DISPATCH_BACKEND_BARRIER_NATIVE:
            return -1;
        case LP_DISPATCH_BACKEND_BARRIER_EXTERNAL:
            return lp_external_adapter_solve(LP_EXTERNAL_BACKEND_BARRIER, solver);
        default:
            return -1;
    }
}
