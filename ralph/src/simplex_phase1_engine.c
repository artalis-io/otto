/*
 * simplex_phase1_engine.c - Phase 1 feasibility-engine primitives.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "simplex_phase1_engine.h"
#include "simplex_internal.h"

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

typedef struct {
    int m;
    int n;
    int artificial_basic_count;
    int devex_refcount;
    int work2_sparse_valid;
    int work2_sparse_nnz;
    int work2_sparse_entering;
    int *basis;
    int *work2_sparse_idx;
    VarStatus *var_status;
    double *x;
    double *work2;
} P1CleanupSnapshot;

static int p1_cleanup_snapshot_take(SimplexTableau *tab,
                                    P1CleanupSnapshot *snap) {
    if (!tab || !snap || tab->m <= 0 || tab->n <= 0 ||
        !tab->basis || !tab->var_status || !tab->x) {
        return -1;
    }
    memset(snap, 0, sizeof(*snap));
    snap->m = tab->m;
    snap->n = tab->n;
    snap->artificial_basic_count = tab->artificial_basic_count;
    snap->devex_refcount = tab->devex_refcount;
    snap->work2_sparse_valid = tab->work2_sparse_valid;
    snap->work2_sparse_nnz = tab->work2_sparse_nnz;
    snap->work2_sparse_entering = tab->work2_sparse_entering;
    snap->basis = (int*)malloc((size_t)tab->m * sizeof(int));
    snap->work2_sparse_idx = (tab->work2_sparse_idx && tab->m > 0)
        ? (int*)malloc((size_t)tab->m * sizeof(int)) : NULL;
    snap->var_status = (VarStatus*)malloc((size_t)tab->n * sizeof(VarStatus));
    snap->x = (double*)malloc((size_t)tab->n * sizeof(double));
    snap->work2 = (tab->work2 && tab->m > 0)
        ? (double*)malloc((size_t)tab->m * sizeof(double)) : NULL;
    if (!snap->basis || !snap->var_status || !snap->x ||
        (tab->work2_sparse_idx && !snap->work2_sparse_idx) ||
        (tab->work2 && !snap->work2)) {
        free(snap->basis);
        free(snap->work2_sparse_idx);
        free(snap->var_status);
        free(snap->x);
        free(snap->work2);
        memset(snap, 0, sizeof(*snap));
        return -1;
    }
    memcpy(snap->basis, tab->basis, (size_t)tab->m * sizeof(int));
    if (snap->work2_sparse_idx) {
        memcpy(snap->work2_sparse_idx, tab->work2_sparse_idx,
               (size_t)tab->m * sizeof(int));
    }
    memcpy(snap->var_status, tab->var_status, (size_t)tab->n * sizeof(VarStatus));
    memcpy(snap->x, tab->x, (size_t)tab->n * sizeof(double));
    if (snap->work2) {
        memcpy(snap->work2, tab->work2, (size_t)tab->m * sizeof(double));
    }
    return 0;
}

static void p1_cleanup_snapshot_free(P1CleanupSnapshot *snap) {
    if (!snap) return;
    free(snap->basis);
    free(snap->work2_sparse_idx);
    free(snap->var_status);
    free(snap->x);
    free(snap->work2);
    memset(snap, 0, sizeof(*snap));
}

static int p1_cleanup_snapshot_restore(SimplexTableau *tab,
                                       const P1CleanupSnapshot *snap) {
    if (!tab || !snap || !snap->basis || !snap->var_status || !snap->x ||
        snap->m != tab->m || snap->n != tab->n ||
        !tab->basis || !tab->basis_pos || !tab->var_status || !tab->x) {
        return -1;
    }

    memcpy(tab->basis, snap->basis, (size_t)tab->m * sizeof(int));
    if (snap->work2_sparse_idx && tab->work2_sparse_idx) {
        memcpy(tab->work2_sparse_idx, snap->work2_sparse_idx,
               (size_t)tab->m * sizeof(int));
    }
    memcpy(tab->var_status, snap->var_status, (size_t)tab->n * sizeof(VarStatus));
    memcpy(tab->x, snap->x, (size_t)tab->n * sizeof(double));
    if (snap->work2 && tab->work2) {
        memcpy(tab->work2, snap->work2, (size_t)tab->m * sizeof(double));
    }
    tab->artificial_basic_count = snap->artificial_basic_count;
    tab->devex_refcount = snap->devex_refcount;
    tab->work2_sparse_valid = snap->work2_sparse_valid;
    tab->work2_sparse_nnz = snap->work2_sparse_nnz;
    tab->work2_sparse_entering = snap->work2_sparse_entering;
    for (int j = 0; j < tab->n; j++) {
        tab->basis_pos[j] = -1;
    }
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (j < 0 || j >= tab->n) return -1;
        tab->basis_pos[j] = k;
        tab->var_status[j] = RALPH_BASIC;
    }
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    tab->basis_cache_valid = 0;
    tab->basis_cache_total_nnz = 0;
    if (tableau_refactorize(tab) != 0) return -1;
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);
    return 0;
}

void p1_progress_window_init(P1ProgressWindow *window) {
    if (!window) return;
    window->anchor_art_sum = RALPH_INFINITY;
    window->previous_art_sum = RALPH_INFINITY;
    window->window_iters = 0;
    window->stale_windows = 0;
    window->cleanup_due = 0;
    window->refactor_due = 0;
    window->perturb_due = 0;
    window->perturb_cooldown = 0;
}

int p1_progress_window_update(P1ProgressWindow *window,
                              double art_sum,
                              int window_size,
                              double rel_drop_target,
                              double abs_drop_target,
                              int cleanup_after_stale_windows,
                              int perturb_after_stale_windows,
                              int perturb_cooldown_iters) {
    double anchor;
    double drop;
    double target;
    int stale = 0;

    if (!window || !isfinite(art_sum) || art_sum < 0.0) return 0;
    if (window_size <= 0) window_size = 1;
    if (rel_drop_target < 0.0) rel_drop_target = 0.0;
    if (abs_drop_target < 0.0) abs_drop_target = 0.0;
    if (cleanup_after_stale_windows <= 0) cleanup_after_stale_windows = 1;
    if (perturb_after_stale_windows <= 0) perturb_after_stale_windows = 2;
    if (perturb_cooldown_iters < 0) perturb_cooldown_iters = 0;

    window->cleanup_due = 0;
    window->refactor_due = 0;
    window->perturb_due = 0;
    if (window->perturb_cooldown > 0) window->perturb_cooldown--;

    if (!isfinite(window->anchor_art_sum) ||
        !isfinite(window->previous_art_sum) ||
        window->window_iters < 0) {
        window->anchor_art_sum = art_sum;
        window->previous_art_sum = art_sum;
        window->window_iters = 0;
        window->stale_windows = 0;
        return 0;
    }

    anchor = window->anchor_art_sum;
    drop = anchor - art_sum;
    target = fmax(abs_drop_target, rel_drop_target * fmax(1.0, fabs(anchor)));

    if (drop >= target) {
        window->anchor_art_sum = art_sum;
        window->previous_art_sum = art_sum;
        window->window_iters = 0;
        window->stale_windows = 0;
        return 0;
    }

    if (window->window_iters < 2147483647) window->window_iters++;
    if (window->window_iters >= window_size) {
        stale = 1;
        window->window_iters = 0;
        window->anchor_art_sum = art_sum;
        if (window->stale_windows < 2147483647) window->stale_windows++;
    }

    if (stale && window->stale_windows >= cleanup_after_stale_windows) {
        window->refactor_due = 1;
        window->cleanup_due = 1;
    }
    if (stale &&
        window->stale_windows >= perturb_after_stale_windows &&
        window->perturb_cooldown <= 0) {
        window->perturb_due = 1;
        window->perturb_cooldown = perturb_cooldown_iters;
        window->stale_windows = 0;
    }

    window->previous_art_sum = art_sum;
    return stale;
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

int p1_candidate_basis_refactorable(SimplexTableau *tab,
                                    int entering,
                                    int leaving,
                                    double theta,
                                    double max_artificial_increase) {
    double before_art_sum;
    double after_art_sum;
    P1CleanupSnapshot snap;
    int leaving_var;
    VarStatus entering_old_status;
    double dir;
    double pivot;

    if (!tab || entering < 0 || entering >= tab->n || !isfinite(theta) ||
        theta < 0.0 || max_artificial_increase < 0.0) {
        return 0;
    }
    if (leaving < 0) {
        return p1_engine_direction_preserves_artificial_progress(tab,
                                                                 entering,
                                                                 leaving,
                                                                 theta);
    }
    if (leaving >= tab->m || !tab->A_ext || !tab->lu || !tab->work1 ||
        !tab->work2 || !tab->basis || !tab->basis_pos || !tab->var_status ||
        !tab->x) {
        return 0;
    }

    before_art_sum = p1_engine_artificial_sum(tab);
    if (!isfinite(before_art_sum)) return 0;
    if (p1_cleanup_snapshot_take(tab, &snap) != 0) return 0;

    pivot = tab->work2[leaving];
    if (!isfinite(pivot) || fabs(pivot) < RALPH_PIVOT_TOL) {
        p1_cleanup_snapshot_free(&snap);
        return 0;
    }

    leaving_var = tab->basis[leaving];
    entering_old_status = tab->var_status[entering];
    dir = (entering_old_status == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;

    tab->basis[leaving] = entering;
    tab->basis_pos[entering] = leaving;
    tab->basis_pos[leaving_var] = -1;
    tab->var_status[entering] = RALPH_BASIC;
    if (pivot * dir > 0.0) {
        tab->var_status[leaving_var] = RALPH_NONBASIC_LOWER;
        tab->x[leaving_var] = tab->lb_ext[leaving_var];
    } else {
        tab->var_status[leaving_var] = RALPH_NONBASIC_UPPER;
        tab->x[leaving_var] = tab->ub_ext[leaving_var];
    }
    tab->artificial_basic_count = p1_artificial_basic_count(tab);

    if (tableau_refactorize_strict_probe(tab) != 0) {
        (void)p1_cleanup_snapshot_restore(tab, &snap);
        p1_cleanup_snapshot_free(&snap);
        return 0;
    }
    tableau_compute_solution(tab);
    after_art_sum = p1_engine_artificial_sum(tab);
    (void)p1_cleanup_snapshot_restore(tab, &snap);
    p1_cleanup_snapshot_free(&snap);

    if (!isfinite(after_art_sum)) return 0;
    return after_art_sum <= before_art_sum + max_artificial_increase;
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
    if (evals > 0 && best_score.valid) {
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

int p1_cleanup_zero_artificials(SimplexTableau *tab, int max_pivots) {
    int pivots = 0;

    if (!tab || max_pivots == 0 || !tab->lu || !tab->basis || !tab->basis_pos ||
        !tab->var_status || !tab->x || !tab->work1 || !tab->work2 ||
        !tab->work4 || !tab->A_ext) {
        return 0;
    }

    for (int a = 0; a < tab->num_artificial; a++) {
        int art_j = tab->artificial_vars[a];
        int pos;
        int best_j = -1;
        double best_abs_coef = 0.0;

        if (max_pivots > 0 && pivots >= max_pivots) break;
        if (art_j < 0 || art_j >= tab->n) continue;
        if (tab->var_status[art_j] != RALPH_BASIC) continue;
        if (fabs(tab->x[art_j]) > RALPH_FEAS_TOL) continue;

        pos = tab->basis_pos[art_j];
        if (pos < 0 || pos >= tab->m) continue;

        vec_set_zero(tab->work1, tab->m);
        tab->work1[pos] = 1.0;
        lu_solve_transpose(tab->lu, tab->work1, tab->work4);

        for (int pass = 0; pass < 2; pass++) {
            int j_begin = (pass == 0) ? 0 : tab->num_structural_ext;
            int j_end = (pass == 0) ? tab->num_structural_ext : tab->n;

            if (j_begin < 0) j_begin = 0;
            if (j_end > tab->n) j_end = tab->n;

            for (int j = j_begin; j < j_end; j++) {
                double coef;
                double abs_coef;

                if (p1_var_is_artificial(tab, j)) continue;
                if (!p1_entering_movable(tab, j)) continue;

                coef = sparse_dot_column(tab->A_ext, j, tab->work4);
                abs_coef = fabs(coef);
                if (abs_coef > best_abs_coef) {
                    best_abs_coef = abs_coef;
                    best_j = j;
                }
            }

            if (best_abs_coef >= 1e-4) break;
        }

        if (best_j < 0 || best_abs_coef <= RALPH_PIVOT_TOL) continue;

        double before_art_sum = p1_engine_artificial_sum(tab);
        P1CleanupSnapshot snap;
        if (p1_cleanup_snapshot_take(tab, &snap) != 0) continue;

        sparse_get_column(tab->A_ext, best_j, tab->work1);
        lu_solve(tab->lu, tab->work1, tab->work2);
        tab->work2_sparse_valid = 0;
        tab->work2_sparse_nnz = 0;
        tab->work2_sparse_entering = -1;
        if (fabs(tab->work2[pos]) < fmax(1e-4, RALPH_PIVOT_TOL)) {
            p1_cleanup_snapshot_free(&snap);
            continue;
        }
        if (!p1_candidate_basis_refactorable(tab, best_j, pos, 0.0, 0.0)) {
            p1_cleanup_snapshot_free(&snap);
            continue;
        }

        if (simplex_pivot(tab, best_j, pos, 0.0, 0) == 0) {
            double after_art_sum;
            if (tableau_refactorize(tab) != 0) {
                (void)p1_cleanup_snapshot_restore(tab, &snap);
                p1_cleanup_snapshot_free(&snap);
                continue;
            }
            tableau_compute_solution(tab);
            after_art_sum = p1_engine_artificial_sum(tab);
            if (!isfinite(after_art_sum) ||
                after_art_sum > before_art_sum +
                    fmax(1e-8, 1e-9 * fmax(1.0, before_art_sum))) {
                (void)p1_cleanup_snapshot_restore(tab, &snap);
                p1_cleanup_snapshot_free(&snap);
                continue;
            }
            tableau_compute_reduced_costs(tab);
            pivots++;
        } else {
            (void)p1_cleanup_snapshot_restore(tab, &snap);
        }
        p1_cleanup_snapshot_free(&snap);
    }

    return pivots;
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
        candidate.theta <= RALPH_FEAS_TOL ||
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

    if (a.theta > b.theta + RALPH_FEAS_TOL) return 1;
    if (b.theta > a.theta + RALPH_FEAS_TOL) return 0;

    if (a.pivot_abs > b.pivot_abs * 1.1) return 1;
    if (b.pivot_abs > a.pivot_abs * 1.1) return 0;

    return 0;
}
