/*
 * simplex_phase1_stabilize.c - Direction stabilization and retry helpers.
 *
 * Contains recompute helpers, direction shape extraction, and all
 * failed-stabilize retry logic used by the Phase 1 zone handlers.
 *
 * Zero behavior change — pure code motion from simplex.c (R3.6).
 */

#include "simplex_phase1_stabilize.h"
#include "simplex_internal.h"
#include "simplex_phase1_engine.h"
#include "simplex_pricing.h"
#include "lp_refactor_policy.h"
#include "lp_log.h"

#include <math.h>
#include <limits.h>

/* ── Static helpers ─────────────────────────────────────────────────── */

static void phase1_recompute_full_no_reason(SimplexSolver *solver,
                                            SimplexTableau *tab,
                                            int *rc_only_streak) {
    tab->phase1_compute_solution_context =
        LP_PHASE1_COMPUTE_CTX_RECOMPUTE_GUARD_FORCED_FULL;
    tab->phase1_compute_rc_context =
        LP_PHASE1_COMPUTE_CTX_RECOMPUTE_GUARD_FORCED_FULL;
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);
    if (rc_only_streak) *rc_only_streak = 0;
    (void)solver;
}

static void phase1_zero_redundant_direction_entries(const SimplexTableau *tab,
                                                    double *dirvec) {
    if (!tab || !dirvec || tab->num_redundant <= 0) return;
    for (int a = 0; a < tab->num_artificial; a++) {
        int art_j = tab->artificial_vars[a];
        if (tab->var_status[art_j] == RALPH_BASIC) {
            int pos = tab->basis_pos[art_j];
            if (pos >= 0 && pos < tab->m) {
                dirvec[pos] = 0.0;
            }
        }
    }
}

static double phase1_entering_bound_flip_distance(const SimplexTableau *tab,
                                                  int entering) {
    double dist;
    if (!tab || entering < 0 || entering >= tab->n) return RALPH_INFINITY;
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

static int phase1_ratio_test_harris_on_direction(const SimplexTableau *tab,
                                                 int entering,
                                                 const double *dirvec,
                                                 int *leaving,
                                                 double *theta) {
    double dir = 1.0;
    double max_abs_dk = 0.0;
    double theta_max = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_is_degen = 1;

    if (!tab || !dirvec || !leaving || !theta) return -1;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    for (int k = 0; k < tab->m; k++) {
        double dk = (dir > 0.0) ? dirvec[k] : -dirvec[k];
        double abs_dk = fabs(dk);
        if (abs_dk > max_abs_dk) max_abs_dk = abs_dk;
    }

    *leaving = -1;
    *theta = RALPH_INFINITY;
    {
        double enter_range = phase1_entering_bound_flip_distance(tab, entering);
        if (enter_range < RALPH_INFINITY / 2) {
            theta_max = enter_range;
        }
    }

    {
        (void)max_abs_dk;
        double pivot_tol = RALPH_PIVOT_TOL;
        for (int k = 0; k < tab->m; k++) {
            double dk = (dir > 0.0) ? dirvec[k] : -dirvec[k];
            double ratio_harris;
            double ratio_exact;
            int j;
            double xj;
            int is_degen;
            double pivot_size;
            int select = 0;

            if (fabs(dk) < pivot_tol) continue;
            j = tab->basis[k];
            xj = tab->x[j];
            if (dk > 0.0) {
                double slack = xj - tab->lb_ext[j];
                ratio_harris = (slack + RALPH_FEAS_TOL) / dk;
                ratio_exact = slack / dk;
            } else {
                double slack = tab->ub_ext[j] - xj;
                ratio_harris = (slack + RALPH_FEAS_TOL) / (-dk);
                ratio_exact = slack / (-dk);
            }
            if (ratio_harris < theta_max) theta_max = ratio_harris;
            if (ratio_exact > theta_max + RALPH_FEAS_TOL) continue;

            is_degen = (ratio_exact < 1e-8);
            pivot_size = fabs(dk);
            if (*leaving < 0) {
                select = 1;
            } else if (!is_degen && best_is_degen) {
                select = 1;
            } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                select = 1;
            }
            if (!select) continue;
            best_pivot = pivot_size;
            best_is_degen = is_degen;
            *leaving = k;
            *theta = ratio_exact > 0.0 ? ratio_exact : 0.0;
        }

        if (*leaving >= 0 && *theta > theta_max + RALPH_FEAS_TOL) {
            best_pivot = 0.0;
            best_is_degen = 1;
            *leaving = -1;
            *theta = RALPH_INFINITY;
            for (int k = 0; k < tab->m; k++) {
                double dk = (dir > 0.0) ? dirvec[k] : -dirvec[k];
                double ratio_exact;
                int j;
                double xj;
                int is_degen;
                double pivot_size;
                int select = 0;

                if (fabs(dk) < pivot_tol) continue;
                j = tab->basis[k];
                xj = tab->x[j];
                if (dk > 0.0) {
                    ratio_exact = (xj - tab->lb_ext[j]) / dk;
                } else {
                    ratio_exact = (tab->ub_ext[j] - xj) / (-dk);
                }
                if (ratio_exact > theta_max + RALPH_FEAS_TOL) continue;
                is_degen = (ratio_exact < 1e-8);
                pivot_size = fabs(dk);
                if (*leaving < 0) {
                    select = 1;
                } else if (!is_degen && best_is_degen) {
                    select = 1;
                } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                    select = 1;
                }
                if (!select) continue;
                best_pivot = pivot_size;
                best_is_degen = is_degen;
                *leaving = k;
                *theta = ratio_exact > 0.0 ? ratio_exact : 0.0;
            }
        }
    }

    if (theta_max >= RALPH_INFINITY / 2) {
        *theta = RALPH_INFINITY;
        return -1;
    }
    {
        double enter_range = phase1_entering_bound_flip_distance(tab, entering);
        if (enter_range <= theta_max &&
            enter_range < RALPH_INFINITY / 2 &&
            (*leaving < 0 || enter_range < *theta)) {
            *theta = enter_range;
            *leaving = -2;
        }
    }
    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

/* Find eligible entering candidates excluding two columns. */
static int phase1_failed_stabilize_retry_find_candidates(
    SimplexTableau *tab,
    int excluded_a,
    int excluded_b,
    int *bland_entering,
    double *bland_score_out,
    int *best_entering,
    double *best_score_out,
    int *eligible_count_out) {
    int first_eligible = -1;
    double first_score = 0.0;
    int best_eligible = -1;
    double best_score = 0.0;
    int eligible_count = 0;

    if (!tab) return 1;
    if (bland_entering) *bland_entering = -1;
    if (bland_score_out) *bland_score_out = 0.0;
    if (best_entering) *best_entering = -1;
    if (best_score_out) *best_score_out = 0.0;
    if (eligible_count_out) *eligible_count_out = 0;
    if (!tab->duals_valid) {
        tableau_compute_duals(tab);
    }

    for (int j = 0; j < tab->n; j++) {
        double score = 0.0;

        if (j == excluded_a || j == excluded_b) continue;
        if (!devex_entering_eligible(tab, j, &score)) continue;

        if (first_eligible < 0) {
            first_eligible = j;
            first_score = score;
        }
        eligible_count++;
        if (best_eligible < 0 || score > best_score) {
            best_eligible = j;
            best_score = score;
        }
    }

    if (bland_entering) *bland_entering = first_eligible;
    if (bland_score_out) *bland_score_out = first_score;
    if (best_entering) *best_entering = best_eligible;
    if (best_score_out) *best_score_out = best_score;
    if (eligible_count_out) *eligible_count_out = eligible_count;
    return (first_eligible >= 0 && best_eligible >= 0) ? 0 : 1;
}

/* ── Recompute helpers ──────────────────────────────────────────────── */

void phase1_recompute_full_with_reason(SimplexSolver *solver,
                                       SimplexTableau *tab,
                                       int *rc_only_streak,
                                       LPPhase1RecomputeReason reason) {
    /* Full recompute is required after basis/LU/perturbation state changes. */
    tab->phase1_compute_solution_context = LP_PHASE1_COMPUTE_CTX_RECOMPUTE_FULL;
    tab->phase1_compute_rc_context = LP_PHASE1_COMPUTE_CTX_RECOMPUTE_FULL;
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);
    if (tab->phase == 1 &&
        reason != LP_PHASE1_RECOMPUTE_REASON_PERTURB &&
        tab->artificial_basic_count > 0) {
        double art_sum = p1_engine_artificial_sum(tab);
        if (isfinite(art_sum) &&
            (art_sum <= 1e-4 || tab->artificial_basic_count <= 32)) {
            (void)p1_cleanup_zero_artificials(tab, 1);
        }
    }
    if (rc_only_streak) *rc_only_streak = 0;
    lp_telemetry_record_phase1_recompute(solver, reason);
}

int phase1_recompute_rc_only_guarded(SimplexSolver *solver,
                                            SimplexTableau *tab,
                                            int *rc_only_streak) {
    /* RC-only refresh is safe only while basis/LU and primal x are unchanged.
     * Guard long RC-only streaks with a forced full recompute to bound drift. */
    if (rc_only_streak && *rc_only_streak >= PHASE1_RC_ONLY_STREAK_GUARD) {
        lp_telemetry_record_phase1_recompute_guard_forced_full(solver);
        phase1_recompute_full_no_reason(solver, tab, rc_only_streak);
        return 1;
    }
    tab->phase1_compute_rc_context = LP_PHASE1_COMPUTE_CTX_RECOMPUTE_RC_ONLY;
    tableau_compute_reduced_costs(tab);
    lp_telemetry_record_phase1_recompute_rc_only(solver);
    if (rc_only_streak) (*rc_only_streak)++;
    return 0;
}

void phase1_recompute_dir_skip_safe(SimplexSolver *solver,
                                           SimplexTableau *tab,
                                           int degenerate_count,
                                           int no_pivot_streak,
                                           int *rc_only_streak,
                                           int *dir_skip_no_recompute_streak) {
    int no_recompute_streak = 0;
    if (dir_skip_no_recompute_streak) {
        no_recompute_streak = *dir_skip_no_recompute_streak;
    }
    if (lp_refactor_policy_phase1_dir_skip_should_skip_recompute(
            tab->m,
            tab->n,
            degenerate_count,
            no_pivot_streak,
            no_recompute_streak)) {
        if (dir_skip_no_recompute_streak) {
            (*dir_skip_no_recompute_streak)++;
        }
        lp_telemetry_record_phase1_dir_stabilize_skip_no_recompute(solver);
        return;
    }
    if (dir_skip_no_recompute_streak && *dir_skip_no_recompute_streak > 0) {
        lp_telemetry_record_phase1_dir_stabilize_skip_guard_refresh(solver);
        *dir_skip_no_recompute_streak = 0;
    }
    int allow_rc_only = lp_refactor_policy_phase1_dir_skip_allow_rc_only(
        tab->m, tab->n, degenerate_count, no_pivot_streak);
    if (!allow_rc_only) {
        phase1_recompute_full_with_reason(solver,
                                          tab,
                                          rc_only_streak,
                                          LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
        lp_telemetry_record_phase1_dir_stabilize_skip(solver, 1);
        return;
    }
    int used_full = phase1_recompute_rc_only_guarded(solver, tab, rc_only_streak);
    lp_telemetry_record_phase1_dir_stabilize_skip(solver, used_full);
    if (used_full) {
        lp_telemetry_record_phase1_recompute(solver, LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
    }
}

/* ── Direction shape ────────────────────────────────────────────────── */

void phase1_direction_shape_from_vector(
    const double *dirvec,
    int m,
    int leaving,
    double *dir_inf_out,
    int *dir_nnz_out,
    double *pivot_abs_out) {
    double dir_inf = 0.0;
    int dir_nnz = 0;
    double pivot_abs = 0.0;

    if (!dirvec || m <= 0) {
        if (dir_inf_out) *dir_inf_out = 0.0;
        if (dir_nnz_out) *dir_nnz_out = 0;
        if (pivot_abs_out) *pivot_abs_out = 0.0;
        return;
    }

    for (int k = 0; k < m; k++) {
        double absval = fabs(dirvec[k]);
        if (absval > RALPH_ZERO_TOL) dir_nnz++;
        if (absval > dir_inf) dir_inf = absval;
    }
    if (leaving >= 0 && leaving < m) {
        pivot_abs = fabs(dirvec[leaving]);
    }

    if (dir_inf_out) *dir_inf_out = dir_inf;
    if (dir_nnz_out) *dir_nnz_out = dir_nnz;
    if (pivot_abs_out) *pivot_abs_out = pivot_abs;
}

void phase1_failed_stabilize_retry_direction_shape(
    const SimplexTableau *tab,
    int leaving,
    double *dir_inf_out,
    int *dir_nnz_out,
    double *pivot_abs_out) {
    if (!tab) {
        if (dir_inf_out) *dir_inf_out = 0.0;
        if (dir_nnz_out) *dir_nnz_out = 0;
        if (pivot_abs_out) *pivot_abs_out = 0.0;
        return;
    }
    phase1_direction_shape_from_vector(
        tab->work2,
        tab->m,
        leaving,
        dir_inf_out,
        dir_nnz_out,
        pivot_abs_out);
}

/* ── Failed-stabilize retry helpers ─────────────────────────────────── */

int phase1_failed_stabilize_retry_penalty_plan(int original_entering,
                                                      int last_failed_entering,
                                                      int same_entering_streak) {
    if (original_entering < 0 || last_failed_entering < 0) return 0;
    if (same_entering_streak < PHASE1_FAILED_STABILIZE_RETRY_PENALTY_TRIGGER) {
        return 0;
    }
    return (last_failed_entering != original_entering);
}

int phase1_failed_stabilize_retry_local_memory_plan(int original_entering,
                                                           int last_retry_alt,
                                                           int last_retry_alt_streak,
                                                           int retry_penalize_last_failed) {
    if (retry_penalize_last_failed) return 0;
    if (original_entering < 0 || last_retry_alt < 0) return 0;
    if (last_retry_alt_streak < PHASE1_FAILED_STABILIZE_RETRY_LOCAL_MEMORY_TRIGGER) {
        return 0;
    }
    return (last_retry_alt != original_entering);
}

int phase1_failed_stabilize_retry_guarded_selector_plan(
    int retry_ratio_fail_streak,
    int last_retry_alt_streak,
    int eligible_count,
    int bland_entering,
    int best_entering,
    double bland_score,
    double best_score) {
    if (retry_ratio_fail_streak < PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_RATIO_FAIL_TRIGGER) {
        return 0;
    }
    if (last_retry_alt_streak < PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_TRIGGER) {
        return 0;
    }
    if (eligible_count < PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_MIN_ELIGIBLE) {
        return 0;
    }
    if (bland_entering < 0 || best_entering < 0 || best_entering == bland_entering) {
        return 0;
    }
    if (!isfinite(bland_score) || !isfinite(best_score) ||
        bland_score <= 0.0 || best_score <= 0.0) {
        return 0;
    }
    return best_score >=
           PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_SCORE_RATIO * bland_score;
}

int phase1_failed_stabilize_retry_direction_guard_plan(
    double dir_inf,
    int dir_nnz,
    double pivot_abs,
    int retry_alt_streak) {
    double pivot_dir_ratio = 0.0;

    if (retry_alt_streak < PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_STREAK_TRIGGER) {
        return 0;
    }
    if (!isfinite(dir_inf) || dir_inf <
            PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_MIN_DIR_INF_RATIO *
                RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
        return 0;
    }
    if (dir_nnz < PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_MIN_NNZ) {
        return 0;
    }
    if (!isfinite(pivot_abs) || pivot_abs < 0.0) {
        return 0;
    }
    if (dir_inf > 0.0) {
        pivot_dir_ratio = pivot_abs / dir_inf;
    }
    return pivot_dir_ratio <=
           PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_MAX_PIVOT_DIR_RATIO;
}

int phase1_failed_stabilize_retry_shadow_guard_plan(
    double actual_dir_inf,
    int actual_dir_nnz,
    double actual_pivot_abs,
    int shadow_ratio_success,
    double shadow_dir_inf,
    int shadow_dir_nnz,
    double shadow_pivot_abs,
    int retry_alt_streak) {
    double actual_pivot_dir_ratio = 0.0;
    double shadow_pivot_dir_ratio = 0.0;

    if (retry_alt_streak < PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_STREAK_TRIGGER) {
        return 0;
    }
    if (!isfinite(actual_dir_inf) || actual_dir_inf <
            PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_DIR_INF_RATIO *
                RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
        return 0;
    }
    if (actual_dir_nnz < PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_NNZ) {
        return 0;
    }
    if (!isfinite(actual_pivot_abs) || actual_pivot_abs < 0.0) {
        return 0;
    }
    if (actual_dir_inf > 0.0) {
        actual_pivot_dir_ratio = actual_pivot_abs / actual_dir_inf;
    }
    if (actual_pivot_dir_ratio <=
            PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_ACTUAL_PIVOT_DIR_RATIO ||
        actual_pivot_dir_ratio >
            PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MAX_ACTUAL_PIVOT_DIR_RATIO) {
        return 0;
    }
    if (!shadow_ratio_success) {
        return 0;
    }
    if (!isfinite(shadow_dir_inf) ||
        shadow_dir_inf <
            PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_SHADOW_DIR_RATIO *
                RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
        return 0;
    }
    if (shadow_dir_inf <
        PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_SHADOW_DIR_MULT *
            actual_dir_inf) {
        return 0;
    }
    if (shadow_dir_nnz < actual_dir_nnz ||
        shadow_dir_nnz < PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_NNZ) {
        return 0;
    }
    if (!isfinite(shadow_pivot_abs) || shadow_pivot_abs < 0.0) {
        return 0;
    }
    if (shadow_dir_inf > 0.0) {
        shadow_pivot_dir_ratio = shadow_pivot_abs / shadow_dir_inf;
    }
    return shadow_pivot_dir_ratio <=
           PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MAX_SHADOW_PIVOT_DIR_RATIO;
}

void phase1_failed_stabilize_retry_sample_pool(SimplexSolver *solver,
                                                      SimplexTableau *tab,
                                                      int excluded_a,
                                                      int excluded_b,
                                                      int *sample_counter) {
    int eligible_count = 0;
    int first_eligible = -1;
    int best_eligible = -1;
    double best_score = 0.0;

    if (!solver || !tab || !sample_counter) return;
    if (((*sample_counter)++ & 15) != 0) return;
    (void)phase1_failed_stabilize_retry_find_candidates(
        tab,
        excluded_a,
        excluded_b,
        &first_eligible,
        NULL,
        &best_eligible,
        &best_score,
        &eligible_count);

    lp_telemetry_record_phase1_failed_stabilize_retry_pool_sample(
        solver,
        eligible_count,
        (best_eligible >= 0 && first_eligible >= 0 && best_eligible != first_eligible));
}

int phase1_failed_stabilize_retry_select_local_memory(
    SimplexSolver *solver,
    SimplexTableau *tab,
    int original_entering,
    int last_retry_alt,
    int retry_ratio_fail_streak,
    int last_retry_alt_streak,
    int *entering,
    int *bland_entering_out,
    int *best_entering_out,
    int *used_guarded_out) {
    int bland_entering = -1;
    int best_entering = -1;
    int eligible_count = 0;
    double bland_score = 0.0;
    double best_score = 0.0;
    int use_guarded = 0;

    if (bland_entering_out) *bland_entering_out = -1;
    if (best_entering_out) *best_entering_out = -1;
    if (used_guarded_out) *used_guarded_out = 0;
    if (!entering) return 1;
    if (phase1_failed_stabilize_retry_find_candidates(
            tab,
            original_entering,
            last_retry_alt,
            &bland_entering,
            &bland_score,
            &best_entering,
            &best_score,
            &eligible_count) != 0) {
        return 1;
    }

    lp_telemetry_record_phase1_failed_stabilize_retry_selector_eval(
        solver,
        best_entering >= 0 && bland_entering >= 0 && best_entering != bland_entering,
        bland_score,
        best_score);

    use_guarded = phase1_failed_stabilize_retry_guarded_selector_plan(
        retry_ratio_fail_streak,
        last_retry_alt_streak,
        eligible_count,
        bland_entering,
        best_entering,
        bland_score,
        best_score);
    *entering = use_guarded ? best_entering : bland_entering;
    if (bland_entering_out) *bland_entering_out = bland_entering;
    if (best_entering_out) *best_entering_out = best_entering;
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(
        solver, use_guarded, eligible_count);
    if (used_guarded_out) *used_guarded_out = use_guarded;
    return 0;
}

int phase1_failed_stabilize_retry_eval_candidates(
    SimplexSolver *solver,
    SimplexTableau *tab,
    int excluded_a,
    int excluded_b,
    int *bland_entering_out,
    double *bland_score_out,
    int *best_entering_out,
    double *best_score_out,
    int *eligible_count_out) {
    int bland_entering = -1;
    int best_entering = -1;
    int eligible_count = 0;
    double bland_score = 0.0;
    double best_score = 0.0;

    if (bland_entering_out) *bland_entering_out = -1;
    if (bland_score_out) *bland_score_out = 0.0;
    if (best_entering_out) *best_entering_out = -1;
    if (best_score_out) *best_score_out = 0.0;
    if (eligible_count_out) *eligible_count_out = 0;
    if (!solver || !tab) return 1;
    if (phase1_failed_stabilize_retry_find_candidates(
            tab,
            excluded_a,
            excluded_b,
            &bland_entering,
            &bland_score,
            &best_entering,
            &best_score,
            &eligible_count) != 0) {
        return 1;
    }
    if (bland_entering_out) *bland_entering_out = bland_entering;
    if (bland_score_out) *bland_score_out = bland_score;
    if (best_entering_out) *best_entering_out = best_entering;
    if (best_score_out) *best_score_out = best_score;
    if (eligible_count_out) *eligible_count_out = eligible_count;
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_eval(
        solver,
        best_entering >= 0 && bland_entering >= 0 && best_entering != bland_entering,
        bland_score,
        best_score);
    return 0;
}

void phase1_failed_stabilize_retry_shadow_direction_proxy(
    SimplexSolver *solver,
    SimplexTableau *tab,
    int entering,
    int *ratio_success_out,
    int *dir_stable_out,
    double *dir_inf_out,
    int *dir_nnz_out,
    double *pivot_abs_out) {
    int leaving = -1;
    int ratio_status = -1;
    int dir_nnz = 0;
    double theta = RALPH_INFINITY;
    double dir_inf = 0.0;
    double pivot_abs = 0.0;
    int dir_stable = 0;
    int col_nnz = 0;
    const int *col_idx = NULL;
    const double *col_val = NULL;

    if (ratio_success_out) *ratio_success_out = 0;
    if (dir_stable_out) *dir_stable_out = 0;
    if (dir_inf_out) *dir_inf_out = 0.0;
    if (dir_nnz_out) *dir_nnz_out = 0;
    if (pivot_abs_out) *pivot_abs_out = 0.0;
    if (!solver || !tab || entering < 0 || !tab->work4) return;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);
    lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work4, NULL, NULL);
    phase1_zero_redundant_direction_entries(tab, tab->work4);
    ratio_status = phase1_ratio_test_harris_on_direction(
        tab, entering, tab->work4, &leaving, &theta);
    phase1_direction_shape_from_vector(
        tab->work4, tab->m, leaving, &dir_inf, &dir_nnz, &pivot_abs);
    dir_stable = (ratio_status == 0 && dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow(
        solver,
        ratio_status == 0,
        dir_stable,
        dir_inf,
        dir_nnz,
        pivot_abs);
    if (ratio_success_out) *ratio_success_out = (ratio_status == 0);
    if (dir_stable_out) *dir_stable_out = dir_stable;
    if (dir_inf_out) *dir_inf_out = dir_inf;
    if (dir_nnz_out) *dir_nnz_out = dir_nnz;
    if (pivot_abs_out) *pivot_abs_out = pivot_abs;
    (void)theta;
}

/* ── Test wrappers ──────────────────────────────────────────────────── */

int simplex_phase1_failed_stabilize_retry_penalty_plan_for_test(
    int entering,
    int last_failed_entering,
    int same_entering_streak) {
    return phase1_failed_stabilize_retry_penalty_plan(
        entering, last_failed_entering, same_entering_streak);
}

int simplex_phase1_failed_stabilize_retry_local_memory_plan_for_test(
    int original_entering,
    int last_retry_alt,
    int last_retry_alt_streak,
    int retry_penalize_last_failed) {
    return phase1_failed_stabilize_retry_local_memory_plan(
        original_entering,
        last_retry_alt,
        last_retry_alt_streak,
        retry_penalize_last_failed);
}

int simplex_phase1_failed_stabilize_retry_guarded_selector_plan_for_test(
    int retry_ratio_fail_streak,
    int last_retry_alt_streak,
    int eligible_count,
    int bland_entering,
    int best_entering,
    double bland_score,
    double best_score) {
    return phase1_failed_stabilize_retry_guarded_selector_plan(
        retry_ratio_fail_streak,
        last_retry_alt_streak,
        eligible_count,
        bland_entering,
        best_entering,
        bland_score,
        best_score);
}

int simplex_phase1_failed_stabilize_retry_direction_guard_plan_for_test(
    double dir_inf,
    int dir_nnz,
    double pivot_abs,
    int retry_alt_streak) {
    return phase1_failed_stabilize_retry_direction_guard_plan(
        dir_inf,
        dir_nnz,
        pivot_abs,
        retry_alt_streak);
}

int simplex_phase1_failed_stabilize_retry_shadow_guard_plan_for_test(
    double actual_dir_inf,
    int actual_dir_nnz,
    double actual_pivot_abs,
    int shadow_ratio_success,
    double shadow_dir_inf,
    int shadow_dir_nnz,
    double shadow_pivot_abs,
    int retry_alt_streak) {
    return phase1_failed_stabilize_retry_shadow_guard_plan(
        actual_dir_inf,
        actual_dir_nnz,
        actual_pivot_abs,
        shadow_ratio_success,
        shadow_dir_inf,
        shadow_dir_nnz,
        shadow_pivot_abs,
        retry_alt_streak);
}
