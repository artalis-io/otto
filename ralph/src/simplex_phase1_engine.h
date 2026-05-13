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

typedef struct {
    double anchor_art_sum;
    double previous_art_sum;
    int window_iters;
    int stale_windows;
    int cleanup_due;
    int refactor_due;
    int perturb_due;
    int perturb_cooldown;
} P1ProgressWindow;

double p1_engine_artificial_sum(const SimplexTableau *tab);

void p1_progress_window_init(P1ProgressWindow *window);

int p1_progress_window_update(P1ProgressWindow *window,
                              double art_sum,
                              int window_size,
                              double rel_drop_target,
                              double abs_drop_target,
                              int cleanup_after_stale_windows,
                              int perturb_after_stale_windows,
                              int perturb_cooldown_iters);

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

int p1_select_leaving_feasibility(SimplexTableau *tab,
                                  int entering,
                                  int *leaving,
                                  double *theta,
                                  P1FeasScore *score_out);

int p1_select_entering_feasibility(SimplexTableau *tab,
                                   const int *excluded_vars,
                                   int excluded_count,
                                   int max_evals,
                                   int *entering,
                                   int *leaving,
                                   double *theta,
                                   P1FeasScore *score_out);

int p1_cleanup_zero_artificials(SimplexTableau *tab, int max_pivots);

P1FeasScore p1_engine_score_candidate(P1FeasCandidate candidate);

int p1_engine_score_better(P1FeasScore a, P1FeasScore b);

#endif /* SIMPLEX_PHASE1_ENGINE_H */
