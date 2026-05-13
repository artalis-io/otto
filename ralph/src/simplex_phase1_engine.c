/*
 * simplex_phase1_engine.c - Phase 1 feasibility-engine primitives.
 */

#include <math.h>
#include "simplex_phase1_engine.h"

double p1_engine_artificial_sum(const SimplexTableau *tab) {
    double sum = 0.0;

    if (!tab || !tab->x || !tab->artificial_vars) return RALPH_INFINITY;
    for (int k = 0; k < tab->num_artificial; k++) {
        int j = tab->artificial_vars[k];
        if (j < 0 || j >= tab->n) return RALPH_INFINITY;
        sum += fabs(tab->x[j]);
    }
    return sum;
}

int p1_engine_predict_artificial_sum(const SimplexTableau *tab,
                                     int entering,
                                     int leaving,
                                     double theta,
                                     double *current_sum,
                                     double *predicted_sum) {
    double current = 0.0;
    double predicted = 0.0;
    double dir;
    double step;

    if (!tab || entering < 0 || entering >= tab->n ||
        !tab->x || !tab->work2 || !tab->basis || !tab->basis_pos ||
        !tab->artificial_vars || !isfinite(theta) || theta < 0.0) {
        return 0;
    }

    dir = (tab->var_status[entering] == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;
    step = theta * dir;

    for (int a = 0; a < tab->num_artificial; a++) {
        int j = tab->artificial_vars[a];
        double x_new;
        int pos;

        if (j < 0 || j >= tab->n) return 0;
        current += fabs(tab->x[j]);
        x_new = tab->x[j];

        if (j == entering) {
            if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
                x_new += theta;
            } else {
                x_new -= theta;
            }
        }

        pos = tab->basis_pos[j];
        if (pos >= 0 && pos < tab->m) {
            x_new += -step * tab->work2[pos];
        }

        if (leaving == -2 && j == entering) {
            if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
                x_new = tab->ub_ext[entering];
            } else {
                x_new = tab->lb_ext[entering];
            }
        } else if (leaving >= 0 && leaving < tab->m &&
                   j == tab->basis[leaving]) {
            if (tab->work2[leaving] * dir > 0.0) {
                x_new = tab->lb_ext[j];
            } else {
                x_new = tab->ub_ext[j];
            }
        }

        if (!isfinite(x_new)) return 0;
        predicted += fabs(x_new);
    }

    if (current_sum) *current_sum = current;
    if (predicted_sum) *predicted_sum = predicted;
    return 1;
}

int p1_engine_direction_preserves_artificial_progress(const SimplexTableau *tab,
                                                      int entering,
                                                      int leaving,
                                                      double theta) {
    double current = 0.0;
    double predicted = 0.0;
    double scale;
    double tol;

    if (!p1_engine_predict_artificial_sum(tab, entering, leaving, theta,
                                          &current, &predicted)) {
        return 0;
    }

    scale = fmax(1.0, current);
    tol = fmax(1000.0 * RALPH_FEAS_TOL, 1e-9 * scale);
    return predicted <= current + tol;
}

P1FeasScore p1_engine_score_candidate(P1FeasCandidate candidate) {
    P1FeasScore score;
    score.decrease = 0.0;
    score.pivot_abs = 0.0;
    score.theta = 0.0;
    score.removes_positive_artificial = 0;
    score.artificial_basic_after = candidate.artificial_basic_after;
    score.valid = 0;

    if (!isfinite(candidate.current_art_sum) ||
        !isfinite(candidate.predicted_art_sum) ||
        !isfinite(candidate.pivot_abs) ||
        !isfinite(candidate.theta) ||
        candidate.pivot_abs <= 0.0 ||
        candidate.theta < 0.0 ||
        candidate.predicted_art_sum > candidate.current_art_sum +
            fmax(1000.0 * RALPH_FEAS_TOL,
                 1e-9 * fmax(1.0, candidate.current_art_sum))) {
        return score;
    }

    score.decrease = candidate.current_art_sum - candidate.predicted_art_sum;
    score.pivot_abs = candidate.pivot_abs;
    score.theta = candidate.theta;
    score.removes_positive_artificial =
        (candidate.leaving_is_artificial &&
         candidate.leaving_positive_artificial &&
         !candidate.entering_is_artificial) ? 1 : 0;
    score.valid = 1;
    return score;
}

int p1_engine_score_better(P1FeasScore a, P1FeasScore b) {
    double scale;
    double decrease_tol;

    if (a.valid && !b.valid) return 1;
    if (!a.valid) return 0;

    scale = fmax(1.0, fmax(fabs(a.decrease), fabs(b.decrease)));
    decrease_tol = fmax(1000.0 * RALPH_FEAS_TOL, 1e-9 * scale);

    if (a.decrease > b.decrease + decrease_tol) return 1;
    if (b.decrease > a.decrease + decrease_tol) return 0;

    if (a.removes_positive_artificial != b.removes_positive_artificial) {
        return a.removes_positive_artificial > b.removes_positive_artificial;
    }

    if (a.artificial_basic_after != b.artificial_basic_after) {
        return a.artificial_basic_after < b.artificial_basic_after;
    }

    if (a.pivot_abs > b.pivot_abs * 1.1) return 1;
    if (b.pivot_abs > a.pivot_abs * 1.1) return 0;

    return a.theta > b.theta + RALPH_FEAS_TOL;
}
