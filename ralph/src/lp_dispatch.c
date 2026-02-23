#include <string.h>

#include "lp_dispatch.h"

typedef struct {
    LPDispatchBackend backend;
    int available;
    int supports_crossover;
    RalphLPAlgorithm fallback_algorithm;
    RalphLPFallbackReason unavailable_reason;
} LPDispatchBackendSpec;

static const LPDispatchBackendSpec LP_DISPATCH_BACKENDS[] = {
    {
        .backend = LP_DISPATCH_BACKEND_SIMPLEX,
        .available = 1,
        .supports_crossover = 0,
        .fallback_algorithm = RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
        .unavailable_reason = RALPH_LP_FALLBACK_NONE
    },
    {
        .backend = LP_DISPATCH_BACKEND_BARRIER,
        .available = 0,
        .supports_crossover = 0,
        .fallback_algorithm = RALPH_LP_ALGORITHM_AUTO,
        .unavailable_reason = RALPH_LP_FALLBACK_BARRIER_UNAVAILABLE
    }
};

static const LPDispatchBackendSpec* lp_dispatch_find_backend(LPDispatchBackend backend) {
    size_t n = sizeof(LP_DISPATCH_BACKENDS) / sizeof(LP_DISPATCH_BACKENDS[0]);
    for (size_t i = 0; i < n; i++) {
        if (LP_DISPATCH_BACKENDS[i].backend == backend) return &LP_DISPATCH_BACKENDS[i];
    }
    return NULL;
}

static LPDispatchBackend lp_dispatch_backend_for_algorithm(RalphLPAlgorithm algorithm) {
    if (algorithm == RALPH_LP_ALGORITHM_BARRIER) {
        return LP_DISPATCH_BACKEND_BARRIER;
    }
    return LP_DISPATCH_BACKEND_SIMPLEX;
}

static int lp_dispatch_simplex_method_for_algorithm(RalphLPAlgorithm algorithm) {
    switch (algorithm) {
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX:
            return 1;
        case RALPH_LP_ALGORITHM_AUTO:
            return 2;
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX:
        default:
            return 0;
    }
}

int lp_dispatch_algorithm_value_valid(int value) {
    return value >= (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX &&
           value <= (int)RALPH_LP_ALGORITHM_BARRIER;
}

int lp_dispatch_crossover_value_valid(int value) {
    return value >= (int)RALPH_LP_CROSSOVER_AUTO &&
           value <= (int)RALPH_LP_CROSSOVER_ON;
}

int lp_dispatch_set_requested_algorithm(int requested_algorithm,
                                        int *normalized_algorithm,
                                        int *legacy_method) {
    if (!normalized_algorithm || !legacy_method) return -1;
    if (!lp_dispatch_algorithm_value_valid(requested_algorithm)) return -1;

    *normalized_algorithm = requested_algorithm;
    if (requested_algorithm <= (int)RALPH_LP_ALGORITHM_AUTO) {
        *legacy_method = requested_algorithm;
    } else {
        /* Barrier maps to AUTO for backward-compatible legacy method semantics. */
        *legacy_method = (int)RALPH_LP_ALGORITHM_AUTO;
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

void lp_dispatch_get_capabilities(RalphLPCapabilities *caps) {
    const LPDispatchBackendSpec *simplex = lp_dispatch_find_backend(LP_DISPATCH_BACKEND_SIMPLEX);
    const LPDispatchBackendSpec *barrier = lp_dispatch_find_backend(LP_DISPATCH_BACKEND_BARRIER);

    if (!caps) return;

    caps->supports_primal_simplex = (simplex && simplex->available) ? 1 : 0;
    caps->supports_dual_simplex = (simplex && simplex->available) ? 1 : 0;
    caps->supports_barrier = (barrier && barrier->available) ? 1 : 0;
    caps->supports_crossover = 0;

    size_t n = sizeof(LP_DISPATCH_BACKENDS) / sizeof(LP_DISPATCH_BACKENDS[0]);
    for (size_t i = 0; i < n; i++) {
        if (LP_DISPATCH_BACKENDS[i].available && LP_DISPATCH_BACKENDS[i].supports_crossover) {
            caps->supports_crossover = 1;
            break;
        }
    }
}

int lp_dispatch_build_plan(int requested_algorithm,
                           int requested_crossover,
                           LPDispatchPlan *plan) {
    const LPDispatchBackendSpec *requested_spec = NULL;
    const LPDispatchBackendSpec *effective_spec = NULL;
    const LPDispatchBackendSpec *simplex_spec = NULL;

    if (!plan) return -1;
    if (!lp_dispatch_algorithm_value_valid(requested_algorithm)) return -1;
    if (!lp_dispatch_crossover_value_valid(requested_crossover)) return -1;

    memset(plan, 0, sizeof(*plan));
    plan->requested_algorithm = (RalphLPAlgorithm)requested_algorithm;
    plan->effective_algorithm = plan->requested_algorithm;
    plan->requested_crossover = (RalphLPCrossoverMode)requested_crossover;
    plan->effective_crossover = plan->requested_crossover;
    plan->requested_backend = lp_dispatch_backend_for_algorithm(plan->requested_algorithm);
    plan->effective_backend = plan->requested_backend;
    plan->fallback_applied = 0;
    plan->fallback_reason = RALPH_LP_FALLBACK_NONE;

    requested_spec = lp_dispatch_find_backend(plan->requested_backend);
    simplex_spec = lp_dispatch_find_backend(LP_DISPATCH_BACKEND_SIMPLEX);

    if (!requested_spec) return -1;
    if (!simplex_spec || !simplex_spec->available) return -1;

    if (!requested_spec->available) {
        plan->effective_backend = LP_DISPATCH_BACKEND_SIMPLEX;
        plan->effective_algorithm = requested_spec->fallback_algorithm;
        plan->fallback_applied = 1;
        plan->fallback_reason = requested_spec->unavailable_reason;
    }

    effective_spec = lp_dispatch_find_backend(plan->effective_backend);
    if (!effective_spec) return -1;

    if (!effective_spec->supports_crossover &&
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
