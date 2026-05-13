/*
 * simplex_phase1_engine.h - Phase 1 feasibility-engine primitives.
 *
 * This module keeps Phase 1 feasibility scoring separate from the primal
 * objective/recovery machinery.  The helpers are intentionally pure so the
 * solver can grow a dedicated feasibility pivot policy without entangling LU
 * repair or Phase 2 behavior.
 */

#ifndef SIMPLEX_PHASE1_ENGINE_H
#define SIMPLEX_PHASE1_ENGINE_H

#include "lp.h"

typedef struct {
    double current_art_sum;
    double predicted_art_sum;
    double art_delta;
    double pivot_abs;
    double theta;
    int entering_is_artificial;
    int leaving_is_artificial;
    int leaving_positive_artificial;
    int artificial_basic_before;
    int artificial_basic_after;
} P1FeasCandidate;

typedef struct {
    double decrease;
    double pivot_abs;
    double theta;
    int removes_positive_artificial;
    int artificial_basic_after;
    int valid;
} P1FeasScore;

double p1_engine_artificial_sum(const SimplexTableau *tab);

int p1_engine_predict_artificial_sum(const SimplexTableau *tab,
                                     int entering,
                                     int leaving,
                                     double theta,
                                     double *current_sum,
                                     double *predicted_sum);

int p1_engine_direction_preserves_artificial_progress(const SimplexTableau *tab,
                                                      int entering,
                                                      int leaving,
                                                      double theta);

P1FeasScore p1_engine_score_candidate(P1FeasCandidate candidate);

int p1_engine_score_better(P1FeasScore a, P1FeasScore b);

#endif /* SIMPLEX_PHASE1_ENGINE_H */
