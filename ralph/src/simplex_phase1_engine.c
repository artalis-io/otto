/*
 * simplex_phase1_engine.c - Phase 1 feasibility-engine primitives.
 */

#include <math.h>
#include "simplex_phase1_engine.h"

static double p1_entering_bound_flip_distance(const SimplexTableau *tab,
                                              int entering) {
    double dist;

    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        if (tab->lb_ext[entering] <= -RALPH_INFINITY / 2.0) {
            return RALPH_INFINITY;
        }
        dist = tab->x[entering] - tab->lb_ext[entering];
    } else {
        if (tab->ub_ext[entering] >= RALPH_INFINITY / 2.0) {
            return RALPH_INFINITY;
        }
        dist = tab->ub_ext[entering] - tab->x[entering];
    }
    return dist > 0.0 ? dist : 0.0;
}

static int p1_var_is_artificial(const SimplexTableau *tab, int j) {
    if (!tab || j < 0 || j >= tab->n) return 0;
    if (tab->is_artificial_var) return tab->is_artificial_var[j] != 0;
    for (int k = 0; k < tab->num_artificial; k++) {
        if (tab->artificial_vars[k] == j) return 1;
    }
    return 0;
}

static int p1_artificial_basic_count(const SimplexTableau *tab) {
    int count = 0;

    if (!tab || !tab->basis) return 0;
    for (int k = 0; k < tab->m; k++) {
        if (p1_var_is_artificial(tab, tab->basis[k])) count++;
    }
    return count;
}

static void p1_zero_redundant_artificial_directions(SimplexTableau *tab) {
    if (!tab || tab->num_redundant <= 0) return;
    for (int a = 0; a < tab->num_artificial; a++) {
        int art_j = tab->artificial_vars[a];
        if (art_j >= 0 && art_j < tab->n &&
            tab->var_status[art_j] == RALPH_BASIC) {
            int pos = tab->basis_pos[art_j];
            if (pos >= 0 && pos < tab->m) {
                tab->work2[pos] = 0.0;
            }
        }
    }
}

static P1FeasScore p1_score_leaving_candidate(const SimplexTableau *tab,
                                              int entering,
                                              int leaving,
                                              double theta,
                                              double pivot_abs,
                                              int artificial_basic_before) {
    P1FeasCandidate candidate;
    P1FeasScore invalid;
    int entering_is_artificial;
    int leaving_is_artificial = 0;
    int leaving_positive_artificial = 0;
    int artificial_basic_after;

    invalid.decrease = 0.0;
    invalid.pivot_abs = 0.0;
    invalid.theta = 0.0;
    invalid.removes_positive_artificial = 0;
    invalid.artificial_basic_after = artificial_basic_before;
    invalid.valid = 0;

    if (!tab || entering < 0 || entering >= tab->n) return invalid;
    entering_is_artificial = p1_var_is_artificial(tab, entering);
    artificial_basic_after = artificial_basic_before;

    if (leaving >= 0 && leaving < tab->m) {
        int leaving_var = tab->basis[leaving];
        leaving_is_artificial = p1_var_is_artificial(tab, leaving_var);
        leaving_positive_artificial =
            leaving_is_artificial && fabs(tab->x[leaving_var]) > RALPH_FEAS_TOL;
        if (leaving_is_artificial && !entering_is_artificial) {
            artificial_basic_after--;
        } else if (!leaving_is_artificial && entering_is_artificial) {
            artificial_basic_after++;
        }
    }
    if (artificial_basic_after < 0) artificial_basic_after = 0;

    if (!p1_engine_predict_artificial_sum(tab, entering, leaving, theta,
                                          &candidate.current_art_sum,
                                          &candidate.predicted_art_sum)) {
        return invalid;
    }
    candidate.art_delta = candidate.predicted_art_sum - candidate.current_art_sum;
    candidate.pivot_abs = pivot_abs;
    candidate.theta = theta;
    candidate.entering_is_artificial = entering_is_artificial;
    candidate.leaving_is_artificial = leaving_is_artificial;
    candidate.leaving_positive_artificial = leaving_positive_artificial;
    candidate.artificial_basic_before = artificial_basic_before;
    candidate.artificial_basic_after = artificial_basic_after;
    return p1_engine_score_candidate(candidate);
}

static int p1_entering_excluded(int j, const int *excluded_vars, int excluded_count) {
    if (!excluded_vars || excluded_count <= 0) return 0;
    for (int k = 0; k < excluded_count; k++) {
        if (excluded_vars[k] == j) return 1;
    }
    return 0;
}

static int p1_entering_eligible(const SimplexTableau *tab, int j) {
    double rc;
    VarStatus st;

    if (!tab || j < 0 || j >= tab->n || !tab->rc || !tab->var_status) return 0;
    st = tab->var_status[j];
    if (st == RALPH_BASIC) return 0;

    rc = tab->rc[j];
    if (st == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) return 1;
    if (st == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) return 1;
    if (st == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) return 1;
    return 0;
}

static int p1_entering_movable(const SimplexTableau *tab, int j) {
    VarStatus st;

    if (!tab || j < 0 || j >= tab->n || !tab->var_status ||
        !tab->x || !tab->lb_ext || !tab->ub_ext) {
        return 0;
    }
    st = tab->var_status[j];
    if (st == RALPH_BASIC || st == RALPH_FIXED) return 0;
    if (st == RALPH_NONBASIC_LOWER) {
        return tab->ub_ext[j] > tab->x[j] + RALPH_FEAS_TOL;
    }
    if (st == RALPH_NONBASIC_UPPER) {
        return tab->x[j] > tab->lb_ext[j] + RALPH_FEAS_TOL;
    }
    return st == RALPH_NONBASIC_FREE;
}

static int p1_consider_entering_candidate(SimplexTableau *tab,
                                          int j,
                                          int *best_entering,
                                          int *best_leaving,
                                          double *best_theta,
                                          P1FeasScore *best_score) {
    int cand_leaving = -1;
    double cand_theta = RALPH_INFINITY;
    P1FeasScore cand_score;

    if (p1_select_leaving_feasibility(tab, j, &cand_leaving,
                                      &cand_theta, &cand_score) != 0) {
        return 0;
    }
    if (p1_engine_score_better(cand_score, *best_score)) {
        *best_score = cand_score;
        *best_entering = j;
        *best_leaving = cand_leaving;
        *best_theta = cand_theta;
        return 1;
    }
    return 0;
}

static int p1_select_entering_for_positive_artificial_rows(
    SimplexTableau *tab,
    const int *excluded_vars,
    int excluded_count,
    int max_evals,
    int *evals,
    int *best_entering,
    int *best_leaving,
    double *best_theta,
    P1FeasScore *best_score) {
    if (!tab || !evals || !best_entering || !best_leaving ||
        !best_theta || !best_score || !tab->work1 || !tab->work4) {
        return 0;
    }

    int picked_art[6] = {-1, -1, -1, -1, -1, -1};

    for (int row_pick = 0; row_pick < 6; row_pick++) {
        int best_art_j = -1;
        double best_art_x = 0.0;
        int pos;

        if (max_evals > 0 && *evals >= max_evals) break;

        for (int a = 0; a < tab->num_artificial; a++) {
            int art_j = tab->artificial_vars[a];
            double art_x;
            int already_picked = 0;

            if (art_j < 0 || art_j >= tab->n) continue;
            for (int p = 0; p < row_pick; p++) {
                if (picked_art[p] == art_j) {
                    already_picked = 1;
                    break;
                }
            }
            if (already_picked) continue;
            if (tab->var_status[art_j] != RALPH_BASIC) continue;
            art_x = fabs(tab->x[art_j]);
            if (art_x <= best_art_x || art_x <= RALPH_FEAS_TOL) continue;
            if (best_score->removes_positive_artificial &&
                best_score->decrease >= art_x - RALPH_FEAS_TOL) {
                continue;
            }
            best_art_x = art_x;
            best_art_j = art_j;
        }

        if (best_art_j < 0) break;
        picked_art[row_pick] = best_art_j;

        pos = tab->basis_pos[best_art_j];
        if (pos < 0 || pos >= tab->m) continue;

        vec_set_zero(tab->work1, tab->m);
        tab->work1[pos] = 1.0;
        lu_solve_transpose(tab->lu, tab->work1, tab->work4);

        for (int pass = 0; pass < 2; pass++) {
            int j_begin = (pass == 0) ? 0 : tab->num_structural_ext;
            int j_end = (pass == 0) ? tab->num_structural_ext : tab->n;
            int row_best_j = -1;
            double row_best_dk = 0.0;

            if (max_evals > 0 && *evals >= max_evals) break;
            if (j_begin < 0) j_begin = 0;
            if (j_end > tab->n) j_end = tab->n;

            for (int j = j_begin; j < j_end; j++) {
                double coef;
                double dir;
                double dk;

                if (p1_entering_excluded(j, excluded_vars, excluded_count)) continue;
                if (p1_var_is_artificial(tab, j)) continue;
                if (!p1_entering_movable(tab, j)) continue;

                coef = sparse_dot_column(tab->A_ext, j, tab->work4);
                dir = (tab->var_status[j] == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;
                dk = coef * dir;

                /* A positive basic artificial at its lower bound leaves when
                 * the entering direction decreases it, i.e. dk > 0 in the
                 * primal ratio-test convention. */
                if (dk <= RALPH_PIVOT_TOL) continue;

                if (dk > row_best_dk) {
                    row_best_dk = dk;
                    row_best_j = j;
                }
            }

            if (row_best_j >= 0) {
                (*evals)++;
                p1_consider_entering_candidate(tab, row_best_j, best_entering,
                                               best_leaving, best_theta,
                                               best_score);
                if (best_score->removes_positive_artificial &&
                    best_score->decrease > RALPH_FEAS_TOL) {
                    return 1;
                }
            }
        }
    }

    return best_score->valid;
}

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

int p1_select_leaving_feasibility(SimplexTableau *tab,
                                  int entering,
                                  int *leaving,
                                  double *theta,
                                  P1FeasScore *score_out) {
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    double dir = 1.0;
    double theta_max = RALPH_INFINITY;
    double enter_range;
    int artificial_basic_before;
    int best_leaving = -1;
    double best_theta = RALPH_INFINITY;
    P1FeasScore best_score;

    if (!tab || !leaving || !theta || entering < 0 || entering >= tab->n ||
        !tab->A_ext || !tab->lu || !tab->work2 || !tab->basis ||
        !tab->x || !tab->lb_ext || !tab->ub_ext || !tab->var_status) {
        return -1;
    }

    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);
    {
        double t_ftran_ms = lp_telemetry_timer_start();
        tab->work2_sparse_valid = 0;
        tab->work2_sparse_nnz = 0;
        tab->work2_sparse_entering = -1;
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2,
                              tab->work2_sparse_idx, &tab->work2_sparse_nnz);
        tab->work2_sparse_valid = 1;
        tab->work2_sparse_entering = entering;
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }
    p1_zero_redundant_artificial_directions(tab);

    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    enter_range = p1_entering_bound_flip_distance(tab, entering);
    if (enter_range < RALPH_INFINITY / 2.0) {
        theta_max = enter_range;
    }

    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        double abs_dk = fabs(dk);
        int j;
        double ratio_harris;

        if (abs_dk < RALPH_PIVOT_TOL) continue;
        j = tab->basis[k];
        if (dk > 0.0) {
            ratio_harris = (tab->x[j] - tab->lb_ext[j] + RALPH_FEAS_TOL) / dk;
        } else {
            ratio_harris = (tab->ub_ext[j] - tab->x[j] + RALPH_FEAS_TOL) / (-dk);
        }
        if (ratio_harris < theta_max) {
            theta_max = ratio_harris;
        }
    }

    if (theta_max >= RALPH_INFINITY / 2.0) {
        return -1;
    }

    best_score.valid = 0;
    best_score.decrease = 0.0;
    best_score.pivot_abs = 0.0;
    best_score.theta = 0.0;
    best_score.removes_positive_artificial = 0;
    best_score.artificial_basic_after = p1_artificial_basic_count(tab);
    artificial_basic_before = best_score.artificial_basic_after;

    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        double abs_dk = fabs(dk);
        int j;
        double ratio_exact;
        double candidate_theta;
        P1FeasScore score;

        if (abs_dk < RALPH_PIVOT_TOL) continue;
        j = tab->basis[k];
        if (dk > 0.0) {
            ratio_exact = (tab->x[j] - tab->lb_ext[j]) / dk;
        } else {
            ratio_exact = (tab->ub_ext[j] - tab->x[j]) / (-dk);
        }
        candidate_theta = ratio_exact > 0.0 ? ratio_exact : 0.0;
        if (candidate_theta > theta_max + RALPH_FEAS_TOL) continue;

        score = p1_score_leaving_candidate(tab, entering, k, candidate_theta,
                                           abs_dk, artificial_basic_before);
        if (p1_engine_score_better(score, best_score)) {
            best_score = score;
            best_leaving = k;
            best_theta = candidate_theta;
        }
    }

    if (enter_range <= theta_max && enter_range < RALPH_INFINITY / 2.0) {
        P1FeasScore score =
            p1_score_leaving_candidate(tab, entering, -2, enter_range,
                                       RALPH_INFINITY, artificial_basic_before);
        if (p1_engine_score_better(score, best_score)) {
            best_score = score;
            best_leaving = -2;
            best_theta = enter_range;
        }
    }

    if (!best_score.valid) {
        return -1;
    }

    *leaving = best_leaving;
    *theta = best_theta;
    if (score_out) *score_out = best_score;
    return 0;
}

int p1_select_entering_feasibility(SimplexTableau *tab,
                                   const int *excluded_vars,
                                   int excluded_count,
                                   int max_evals,
                                   int *entering,
                                   int *leaving,
                                   double *theta,
                                   P1FeasScore *score_out) {
    int best_entering = -1;
    int best_leaving = -1;
    double best_theta = RALPH_INFINITY;
    P1FeasScore best_score;
    int evals = 0;

    if (!tab || !entering || !leaving || !theta || max_evals == 0) {
        return -1;
    }

    best_score.decrease = 0.0;
    best_score.pivot_abs = 0.0;
    best_score.theta = 0.0;
    best_score.removes_positive_artificial = 0;
    best_score.artificial_basic_after = p1_artificial_basic_count(tab);
    best_score.valid = 0;

    if (p1_select_entering_for_positive_artificial_rows(tab,
                                                        excluded_vars,
                                                        excluded_count,
                                                        max_evals,
                                                        &evals,
                                                        &best_entering,
                                                        &best_leaving,
                                                        &best_theta,
                                                        &best_score)) {
        goto restore_selected;
    }
    if (evals > 0) {
        goto restore_selected;
    }

    for (int j = 0; j < tab->n; j++) {
        if (p1_entering_excluded(j, excluded_vars, excluded_count)) continue;
        if (!p1_entering_eligible(tab, j)) continue;

        if (max_evals > 0 && evals >= max_evals) break;
        evals++;

        p1_consider_entering_candidate(tab, j, &best_entering,
                                       &best_leaving, &best_theta,
                                       &best_score);
    }

restore_selected:
    if (!best_score.valid || best_entering < 0) {
        return -1;
    }

    /* Restore work2 to the selected entering column for simplex_pivot(). */
    if (p1_select_leaving_feasibility(tab, best_entering, &best_leaving,
                                      &best_theta, &best_score) != 0) {
        return -1;
    }

    *entering = best_entering;
    *leaving = best_leaving;
    *theta = best_theta;
    if (score_out) *score_out = best_score;
    return 0;
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
