#ifndef LP_BACKEND_H
#define LP_BACKEND_H

#include "lp.h"
#include "lp_dispatch.h"

typedef struct {
    int available;
    int supports_crossover;
} LPBackendCapability;

/* Query runtime-executable capability for one backend. */
int lp_backend_get_capability(LPDispatchBackend backend, LPBackendCapability *capability);

/* Execute the LP solve using the selected backend implementation. */
int lp_backend_run(LPDispatchBackend backend, SimplexSolver *solver);

#endif
