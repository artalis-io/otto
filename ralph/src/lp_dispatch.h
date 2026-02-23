#ifndef LP_DISPATCH_H
#define LP_DISPATCH_H

#include "ralph.h"

typedef enum {
    LP_DISPATCH_BACKEND_SIMPLEX = 0,
    LP_DISPATCH_BACKEND_BARRIER = 1
} LPDispatchBackend;

typedef struct {
    LPDispatchBackend requested_backend;
    LPDispatchBackend effective_backend;
    RalphLPAlgorithm requested_algorithm;
    RalphLPAlgorithm effective_algorithm;
    RalphLPCrossoverMode requested_crossover;
    RalphLPCrossoverMode effective_crossover;
    int simplex_method;
    int fallback_applied;
    RalphLPFallbackReason fallback_reason;
} LPDispatchPlan;

int lp_dispatch_algorithm_value_valid(int value);
int lp_dispatch_crossover_value_valid(int value);

int lp_dispatch_set_requested_algorithm(int requested_algorithm,
                                        int *normalized_algorithm,
                                        int *legacy_method);

int lp_dispatch_set_requested_crossover(int requested_crossover,
                                        int *normalized_crossover);

void lp_dispatch_get_capabilities(RalphLPCapabilities *caps);

int lp_dispatch_build_plan(int requested_algorithm,
                           int requested_crossover,
                           LPDispatchPlan *plan);

void lp_dispatch_plan_to_report(const LPDispatchPlan *plan,
                                RalphLPSolveAlgorithmReport *report);

#endif
