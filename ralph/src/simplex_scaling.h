#ifndef SIMPLEX_SCALING_H
#define SIMPLEX_SCALING_H

#include "lp.h"

/* Geometric mean scaling */
int apply_scaling(SimplexSolver *solver);

/* Post-solve verification (may downgrade OPTIMAL -> IMPRECISE) */
void verify_solution(SimplexSolver *solver);

/* Unscale solution after solving */
void unscale_solution(SimplexSolver *solver);

/* Restore model to original (unscaled) state */
void restore_model(SimplexSolver *solver);

#endif /* SIMPLEX_SCALING_H */
