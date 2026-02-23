#include <string.h>

#include "lp_dispatch.h"
#include "lp_backend.h"

#define LP_DISPATCH_EXTERNAL_PROVIDER_ANY (-1)

typedef struct {
    LPDispatchBackend backend;
    int available;
    int supports_crossover;
    RalphLPAlgorithm fallback_algorithm;
    RalphLPFallbackReason unavailable_reason;
} LPDispatchBackendSpec;

static LPExternalProvider lp_dispatch_to_internal_provider(int provider) {
    switch ((RalphLPExternalProvider)provider) {
        case RALPH_LP_EXTERNAL_PROVIDER_GLPK:
            return LP_EXTERNAL_PROVIDER_GLPK;
        case RALPH_LP_EXTERNAL_PROVIDER_HIGHS:
            return LP_EXTERNAL_PROVIDER_HIGHS;
        case RALPH_LP_EXTERNAL_PROVIDER_CLP:
            return LP_EXTERNAL_PROVIDER_CLP;
        case RALPH_LP_EXTERNAL_PROVIDER_CPLEX:
            return LP_EXTERNAL_PROVIDER_CPLEX;
        case RALPH_LP_EXTERNAL_PROVIDER_GUROBI:
            return LP_EXTERNAL_PROVIDER_GUROBI;
        case RALPH_LP_EXTERNAL_PROVIDER_GLOP:
            return LP_EXTERNAL_PROVIDER_GLOP;
        case RALPH_LP_EXTERNAL_PROVIDER_NONE:
        default:
            return LP_EXTERNAL_PROVIDER_NONE;
    }
}

static int lp_dispatch_backend_is_external(LPDispatchBackend backend) {
    return backend == LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL ||
           backend == LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL ||
           backend == LP_DISPATCH_BACKEND_BARRIER_EXTERNAL;
}

static LPExternalProvider lp_dispatch_provider_for_query(int requested_external_provider) {
    if (requested_external_provider == LP_DISPATCH_EXTERNAL_PROVIDER_ANY) {
        return LP_EXTERNAL_PROVIDER_NONE;
    }
    return lp_dispatch_to_internal_provider(requested_external_provider);
}

static int lp_dispatch_get_backend_spec(LPDispatchBackend backend,
                                        int requested_external_provider,
                                        LPDispatchBackendSpec *spec) {
    LPBackendCapability capability = {0, 0};
    LPExternalProvider provider_hint = lp_dispatch_provider_for_query(requested_external_provider);
    int explicit_none_external =
        (lp_dispatch_backend_is_external(backend) &&
         requested_external_provider != LP_DISPATCH_EXTERNAL_PROVIDER_ANY &&
         requested_external_provider == (int)RALPH_LP_EXTERNAL_PROVIDER_NONE);

    if (!spec) return -1;
    if (!explicit_none_external &&
        lp_backend_get_capability(backend, provider_hint, &capability) != 0) {
        return -1;
    }

    memset(spec, 0, sizeof(*spec));
    spec->backend = backend;
    spec->available = explicit_none_external ? 0 : (capability.available ? 1 : 0);
    spec->supports_crossover =
        explicit_none_external ? 0 : (capability.supports_crossover ? 1 : 0);

    switch (backend) {
        case LP_DISPATCH_BACKEND_SIMPLEX:
            spec->fallback_algorithm = RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
            spec->unavailable_reason = RALPH_LP_FALLBACK_NONE;
            return 0;
        case LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL:
            spec->fallback_algorithm = RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
            spec->unavailable_reason = RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE;
            return 0;
        case LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL:
            spec->fallback_algorithm = RALPH_LP_ALGORITHM_DUAL_SIMPLEX;
            spec->unavailable_reason = RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE;
            return 0;
        case LP_DISPATCH_BACKEND_BARRIER_NATIVE:
            spec->fallback_algorithm = RALPH_LP_ALGORITHM_AUTO;
            spec->unavailable_reason = RALPH_LP_FALLBACK_BARRIER_UNAVAILABLE;
            return 0;
        case LP_DISPATCH_BACKEND_BARRIER_EXTERNAL:
            spec->fallback_algorithm = RALPH_LP_ALGORITHM_AUTO;
            spec->unavailable_reason = RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE;
            return 0;
        default:
            return -1;
    }
}

static LPDispatchBackend lp_dispatch_backend_for_algorithm(RalphLPAlgorithm algorithm) {
    switch (algorithm) {
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX:
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX:
        case RALPH_LP_ALGORITHM_AUTO:
            return LP_DISPATCH_BACKEND_SIMPLEX;
        case RALPH_LP_ALGORITHM_BARRIER:
            return LP_DISPATCH_BACKEND_BARRIER_NATIVE;
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL:
            return LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL;
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL:
            return LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL;
        case RALPH_LP_ALGORITHM_BARRIER_EXTERNAL:
            return LP_DISPATCH_BACKEND_BARRIER_EXTERNAL;
        default:
            return LP_DISPATCH_BACKEND_SIMPLEX;
    }
}

static int lp_dispatch_simplex_method_for_algorithm(RalphLPAlgorithm algorithm) {
    switch (algorithm) {
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX:
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL:
            return 1;
        case RALPH_LP_ALGORITHM_AUTO:
            return 2;
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX:
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL:
        default:
            return 0;
    }
}

int lp_dispatch_algorithm_value_valid(int value) {
    return value >= (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX &&
           value <= (int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL;
}

int lp_dispatch_algorithm_is_external(int value) {
    if (!lp_dispatch_algorithm_value_valid(value)) return 0;
    return value == (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL ||
           value == (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL ||
           value == (int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL;
}

int lp_dispatch_crossover_value_valid(int value) {
    return value >= (int)RALPH_LP_CROSSOVER_AUTO &&
           value <= (int)RALPH_LP_CROSSOVER_ON;
}

int lp_dispatch_external_provider_value_valid(int value) {
    return value >= (int)RALPH_LP_EXTERNAL_PROVIDER_NONE &&
           value <= (int)RALPH_LP_EXTERNAL_PROVIDER_GLOP;
}

int lp_dispatch_set_requested_algorithm(int requested_algorithm,
                                        int *normalized_algorithm,
                                        int *legacy_method) {
    if (!normalized_algorithm || !legacy_method) return -1;
    if (!lp_dispatch_algorithm_value_valid(requested_algorithm)) return -1;

    *normalized_algorithm = requested_algorithm;
    switch ((RalphLPAlgorithm)requested_algorithm) {
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX:
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL:
            *legacy_method = (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
            break;
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX:
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL:
            *legacy_method = (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX;
            break;
        case RALPH_LP_ALGORITHM_AUTO:
        case RALPH_LP_ALGORITHM_BARRIER:
        case RALPH_LP_ALGORITHM_BARRIER_EXTERNAL:
            *legacy_method = (int)RALPH_LP_ALGORITHM_AUTO;
            break;
        default:
            return -1;
    }
    return 0;
}

int lp_dispatch_set_requested_crossover(int requested_crossover,
                                        int *normalized_crossover) {
    if (!normalized_crossover) return -1;
    if (!lp_dispatch_crossover_value_valid(requested_crossover)) return -1;

    *normalized_crossover = requested_crossover;
    return 0;
}

int lp_dispatch_set_requested_external_provider(int requested_external_provider,
                                                int *normalized_external_provider) {
    if (!normalized_external_provider) return -1;
    if (!lp_dispatch_external_provider_value_valid(requested_external_provider)) return -1;

    *normalized_external_provider = requested_external_provider;
    return 0;
}

void lp_dispatch_get_capabilities(RalphLPCapabilities *caps) {
    LPDispatchBackendSpec simplex;
    LPDispatchBackendSpec simplex_external;
    LPDispatchBackendSpec dual_simplex_external;
    LPDispatchBackendSpec barrier_native;
    LPDispatchBackendSpec barrier_external;
    int has_simplex = 0;
    int has_simplex_external = 0;
    int has_dual_simplex_external = 0;
    int has_native = 0;
    int has_external = 0;

    if (!caps) return;

    has_simplex = (lp_dispatch_get_backend_spec(LP_DISPATCH_BACKEND_SIMPLEX,
                                                LP_DISPATCH_EXTERNAL_PROVIDER_ANY,
                                                &simplex) == 0);
    has_simplex_external =
        (lp_dispatch_get_backend_spec(LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL,
                                      LP_DISPATCH_EXTERNAL_PROVIDER_ANY,
                                      &simplex_external) == 0);
    has_dual_simplex_external =
        (lp_dispatch_get_backend_spec(LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL,
                                      LP_DISPATCH_EXTERNAL_PROVIDER_ANY,
                                      &dual_simplex_external) == 0);
    has_native = (lp_dispatch_get_backend_spec(LP_DISPATCH_BACKEND_BARRIER_NATIVE,
                                               LP_DISPATCH_EXTERNAL_PROVIDER_ANY,
                                               &barrier_native) == 0);
    has_external = (lp_dispatch_get_backend_spec(LP_DISPATCH_BACKEND_BARRIER_EXTERNAL,
                                                 LP_DISPATCH_EXTERNAL_PROVIDER_ANY,
                                                 &barrier_external) == 0);

    caps->supports_primal_simplex =
        ((has_simplex && simplex.available) ||
         (has_simplex_external && simplex_external.available)) ? 1 : 0;
    caps->supports_dual_simplex =
        ((has_simplex && simplex.available) ||
         (has_dual_simplex_external && dual_simplex_external.available)) ? 1 : 0;
    caps->supports_barrier =
        ((has_native && barrier_native.available) ||
         (has_external && barrier_external.available)) ? 1 : 0;
    caps->supports_crossover =
        ((has_simplex && simplex.available && simplex.supports_crossover) ||
         (has_simplex_external && simplex_external.available && simplex_external.supports_crossover) ||
         (has_dual_simplex_external && dual_simplex_external.available &&
          dual_simplex_external.supports_crossover) ||
         (has_native && barrier_native.available && barrier_native.supports_crossover) ||
         (has_external && barrier_external.available && barrier_external.supports_crossover)) ? 1 : 0;
}

int lp_dispatch_build_plan(int requested_algorithm,
                           int requested_external_provider,
                           int requested_crossover,
                           LPDispatchPlan *plan) {
    LPDispatchBackendSpec requested_spec;
    LPDispatchBackendSpec effective_spec;
    LPDispatchBackendSpec simplex_spec;
    int effective_provider_for_query = LP_DISPATCH_EXTERNAL_PROVIDER_ANY;

    if (!plan) return -1;
    if (!lp_dispatch_algorithm_value_valid(requested_algorithm)) return -1;
    if (!lp_dispatch_external_provider_value_valid(requested_external_provider)) return -1;
    if (!lp_dispatch_crossover_value_valid(requested_crossover)) return -1;

    memset(plan, 0, sizeof(*plan));
    plan->requested_algorithm = (RalphLPAlgorithm)requested_algorithm;
    plan->effective_algorithm = plan->requested_algorithm;
    plan->requested_external_provider = (RalphLPExternalProvider)requested_external_provider;
    plan->effective_external_provider = RALPH_LP_EXTERNAL_PROVIDER_NONE;
    plan->requested_crossover = (RalphLPCrossoverMode)requested_crossover;
    plan->effective_crossover = plan->requested_crossover;
    plan->requested_backend = lp_dispatch_backend_for_algorithm(plan->requested_algorithm);
    plan->effective_backend = plan->requested_backend;
    plan->fallback_applied = 0;
    plan->fallback_reason = RALPH_LP_FALLBACK_NONE;

    if (lp_dispatch_get_backend_spec(plan->requested_backend,
                                     requested_external_provider,
                                     &requested_spec) != 0) {
        return -1;
    }
    if (lp_dispatch_get_backend_spec(LP_DISPATCH_BACKEND_SIMPLEX,
                                     LP_DISPATCH_EXTERNAL_PROVIDER_ANY,
                                     &simplex_spec) != 0) {
        return -1;
    }
    if (!simplex_spec.available) return -1;

    if (!requested_spec.available) {
        plan->effective_backend = LP_DISPATCH_BACKEND_SIMPLEX;
        plan->effective_algorithm = requested_spec.fallback_algorithm;
        plan->effective_external_provider = RALPH_LP_EXTERNAL_PROVIDER_NONE;
        plan->fallback_applied = 1;
        plan->fallback_reason = requested_spec.unavailable_reason;
    } else if (lp_dispatch_backend_is_external(plan->effective_backend)) {
        plan->effective_external_provider = plan->requested_external_provider;
    }

    if (lp_dispatch_backend_is_external(plan->effective_backend)) {
        effective_provider_for_query = (int)plan->effective_external_provider;
    }
    if (lp_dispatch_get_backend_spec(plan->effective_backend,
                                     effective_provider_for_query,
                                     &effective_spec) != 0) {
        return -1;
    }
    if (!effective_spec.available) return -1;

    if (!effective_spec.supports_crossover &&
        plan->requested_crossover != RALPH_LP_CROSSOVER_AUTO) {
        plan->effective_crossover = RALPH_LP_CROSSOVER_AUTO;
        if (!plan->fallback_applied) {
            plan->fallback_applied = 1;
            plan->fallback_reason = RALPH_LP_FALLBACK_CROSSOVER_UNAVAILABLE;
        }
    }

    if (plan->effective_backend == LP_DISPATCH_BACKEND_SIMPLEX) {
        plan->simplex_method = lp_dispatch_simplex_method_for_algorithm(plan->effective_algorithm);
    } else {
        plan->simplex_method = (int)RALPH_LP_ALGORITHM_AUTO;
    }

    return 0;
}

void lp_dispatch_plan_to_report(const LPDispatchPlan *plan,
                                RalphLPSolveAlgorithmReport *report) {
    if (!plan || !report) return;

    report->requested_algorithm = plan->requested_algorithm;
    report->effective_algorithm = plan->effective_algorithm;
    report->requested_crossover = plan->requested_crossover;
    report->effective_crossover = plan->effective_crossover;
    report->fallback_applied = plan->fallback_applied;
    report->fallback_reason = plan->fallback_reason;
}
