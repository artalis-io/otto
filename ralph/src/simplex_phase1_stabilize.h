#ifndef SIMPLEX_PHASE1_STABILIZE_H
#define SIMPLEX_PHASE1_STABILIZE_H

#include "lp.h"
#include "lp_refactor_policy.h"

/* ── Recompute helpers ──────────────────────────────────────────────── */

void phase1_recompute_full_with_reason(SimplexSolver *solver,
                                       SimplexTableau *tab,
                                       int *rc_only_streak,
                                       LPPhase1RecomputeReason reason);

int phase1_recompute_rc_only_guarded(SimplexSolver *solver,
                                     SimplexTableau *tab,
                                     int *rc_only_streak);

void phase1_recompute_dir_skip_safe(SimplexSolver *solver,
                                    SimplexTableau *tab,
                                    int degenerate_count,
                                    int no_pivot_streak,
                                    int *rc_only_streak,
                                    int *dir_skip_no_recompute_streak);

/* ── Direction shape ────────────────────────────────────────────────── */

/* Extract direction vector shape metrics. Also used by Phase 2 pivot
 * geometry telemetry in simplex.c. */
void phase1_direction_shape_from_vector(
    const double *dirvec,
    int m,
    int leaving,
    double *dir_inf_out,
    int *dir_nnz_out,
    double *pivot_abs_out);

void phase1_failed_stabilize_retry_direction_shape(
    const SimplexTableau *tab,
    int leaving,
    double *dir_inf_out,
    int *dir_nnz_out,
    double *pivot_abs_out);

/* ── Failed-stabilize retry helpers ─────────────────────────────────── */

int phase1_failed_stabilize_retry_penalty_plan(int entering,
    int last_failed_entering, int same_entering_streak);

int phase1_failed_stabilize_retry_local_memory_plan(int original_entering,
    int last_retry_alt, int last_retry_alt_streak, int retry_penalize_last_failed);

int phase1_failed_stabilize_retry_guarded_selector_plan(
    int retry_ratio_fail_streak,
    int last_retry_alt_streak,
    int eligible_count,
    int bland_entering,
    int best_entering,
    double bland_score,
    double best_score);

int phase1_failed_stabilize_retry_direction_guard_plan(double dir_inf,
    int dir_nnz, double pivot_abs, int retry_alt_streak);

int phase1_failed_stabilize_retry_shadow_guard_plan(double actual_dir_inf,
    int actual_dir_nnz, double actual_pivot_abs, int shadow_ratio_success,
    double shadow_dir_inf, int shadow_dir_nnz, double shadow_pivot_abs,
    int retry_alt_streak);

void phase1_failed_stabilize_retry_sample_pool(SimplexSolver *solver,
    SimplexTableau *tab, int excluded_a, int excluded_b, int *sample_counter);

int phase1_failed_stabilize_retry_select_local_memory(SimplexSolver *solver,
    SimplexTableau *tab, int original_entering, int last_retry_alt,
    int retry_ratio_fail_streak, int last_retry_alt_streak,
    int *entering, int *bland_entering_out, int *best_entering_out,
    int *used_guarded_out);

int phase1_failed_stabilize_retry_eval_candidates(SimplexSolver *solver,
    SimplexTableau *tab, int excluded_a, int excluded_b,
    int *bland_entering_out, double *bland_score_out,
    int *best_entering_out, double *best_score_out, int *eligible_count_out);

void phase1_failed_stabilize_retry_shadow_direction_proxy(SimplexSolver *solver,
    SimplexTableau *tab, int entering, int *ratio_success_out,
    int *dir_stable_out, double *dir_inf_out, int *dir_nnz_out,
    double *pivot_abs_out);

#endif /* SIMPLEX_PHASE1_STABILIZE_H */
