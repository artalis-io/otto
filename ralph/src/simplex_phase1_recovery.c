/*
 * simplex_phase1_recovery.c - Phase 1 crisis/recovery state and progress ops.
 *
 * Extracted from simplex.c as part of R3 (crisis machinery consolidation).
 * See simplex_phase1_recovery.h for category descriptions.
 */

#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "simplex_phase1_recovery.h"
#include "simplex_internal.h"
#include "lp_refactor_policy.h"
#include "lp_glpk_strict.h"
#include "simplex_perturb.h"
#include "lp_log.h"

/* Progress window constants (moved from simplex.c) */
#define PHASE1_NO_PIVOT_PROGRESS_WINDOW 6
#define PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN 1e-4
#define PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN 1e-8
#define PHASE1_NO_PIVOT_LADDER_RESCUE_COOLDOWN_ITERS 16

typedef struct {
    int m;
    int n;
    int *basis;
    VarStatus *var_status;
    double *x;
} P1TableauSnapshot;

static int p1_tableau_snapshot_take(SimplexTableau *tab,
                                     P1TableauSnapshot *snap) {
    if (!tab || !snap) return -1;
    memset(snap, 0, sizeof(*snap));
    snap->m = tab->m;
    snap->n = tab->n;
    snap->basis = (int*)malloc((size_t)tab->m * sizeof(int));
    snap->var_status = (VarStatus*)malloc((size_t)tab->n * sizeof(VarStatus));
    snap->x = (double*)malloc((size_t)tab->n * sizeof(double));
    if (!snap->basis || !snap->var_status || !snap->x) {
        free(snap->basis);
        free(snap->var_status);
        free(snap->x);
        memset(snap, 0, sizeof(*snap));
        return -1;
    }
    memcpy(snap->basis, tab->basis, (size_t)tab->m * sizeof(int));
    memcpy(snap->var_status, tab->var_status, (size_t)tab->n * sizeof(VarStatus));
    memcpy(snap->x, tab->x, (size_t)tab->n * sizeof(double));
    return 0;
}

static void p1_tableau_snapshot_free(P1TableauSnapshot *snap) {
    if (!snap) return;
    free(snap->basis);
    free(snap->var_status);
    free(snap->x);
    memset(snap, 0, sizeof(*snap));
}

static int p1_tableau_snapshot_restore(SimplexTableau *tab,
                                       P1TableauSnapshot *snap) {
    if (!tab || !snap || !snap->basis || !snap->var_status || !snap->x) {
        return -1;
    }
    if (snap->m != tab->m || snap->n != tab->n) {
        return -1;
    }

    memcpy(tab->basis, snap->basis, (size_t)tab->m * sizeof(int));
    memcpy(tab->var_status, snap->var_status, (size_t)tab->n * sizeof(VarStatus));
    memcpy(tab->x, snap->x, (size_t)tab->n * sizeof(double));
    for (int j = 0; j < tab->n; j++) {
        tab->basis_pos[j] = -1;
    }
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (j < 0 || j >= tab->n) {
            return -1;
        }
        tab->basis_pos[j] = k;
        tab->var_status[j] = RALPH_BASIC;
    }
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    tab->basis_cache_valid = 0;
    tab->basis_cache_total_nnz = 0;
    if (tableau_refactorize(tab) != 0) {
        return -1;
    }
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);
    return 0;
}

void p1_recovery_init(P1RecoveryState *rs,
                      const SimplexSolver *solver,
                      const SimplexTableau *tab) {
    /* ── Cycling ─────────────────────────────────────────────────── */
    rs->cycling.degenerate_count = 0;
    rs->cycling.degen_threshold =
        lp_refactor_policy_phase1_degen_threshold(tab->m);
    rs->cycling.use_bland = 0;
    rs->cycling.last_obj = tab->obj_value;
    rs->cycling.stall_count = 0;
    rs->cycling.stall_threshold =
        lp_refactor_policy_phase1_stall_threshold(tab->m);
    rs->cycling.perturb_attempts = 0;
    rs->cycling.max_perturb_attempts = 15;
    rs->cycling.pricing_strategy =
        (solver->phase1_pricing >= 0) ? solver->phase1_pricing
                                      : solver->pricing_strategy;
    rs->cycling.auto_dantzig_enabled = 0;
    rs->cycling.auto_partial_enabled =
        (solver->phase1_pricing < 0 && rs->cycling.pricing_strategy == 3) ? 1 : 0;
    rs->cycling.auto_partial_abandoned = 0;

    /* ── Numerical ───────────────────────────────────────────────── */
    rs->numerical.dir_stabilize_cooldown = 0;
    rs->numerical.dir_stabilize_repeat_count = 0;
    rs->numerical.dir_stabilize_moderate_defer_pending = 0;
    rs->numerical.rc_only_streak = 0;
    rs->numerical.dir_skip_event_streak = 0;
    rs->numerical.dir_skip_no_recompute_streak = 0;
    rs->numerical.dir_force_refactor_streak = 0;
    rs->numerical.last_dir_skip_entering = -1;
    rs->numerical.dir_skip_same_entering_streak = 0;
    rs->numerical.shadow_guard_followup_pending = 0;
    rs->numerical.shadow_guard_followup_direction_pending = 0;
    rs->numerical.force_extreme_followup_pending = 0;
    rs->numerical.force_extreme_followup_direction_pending = 0;
    rs->numerical.force_extreme_followup_bound_flip_streak = 0;
    rs->numerical.force_extreme_followup_tiny_theta_streak = 0;
    rs->numerical.force_extreme_tiny_theta_relax_branch_pending = 0;
    rs->numerical.force_extreme_tiny_theta_relax_next_pending = 0;

    /* ── Basis repair ────────────────────────────────────────────── */
    rs->basis.fail_entering = -1;
    rs->basis.fail_leaving_pos = -1;
    rs->basis.fail_reason = PHASE1_PIVOT_FAIL_NONE;
    rs->basis.fail_repeat_count = 0;
    rs->basis.ratio_breakdown_count = 0;
    rs->basis.ratio_breakdown_last_entering = -1;
    rs->basis.ratio_breakdown_same_entering_streak = 0;
    rs->basis.excluded_entering_a = -1;
    rs->basis.excluded_entering_ttl_a = 0;
    rs->basis.excluded_entering_b = -1;
    rs->basis.excluded_entering_ttl_b = 0;
    for (int k = 0; k < PHASE1_ENTERING_EXCLUSION_POOL_SIZE; k++) {
        rs->basis.excluded_entering_pool[k] = -1;
        rs->basis.excluded_entering_pool_ttl[k] = 0;
    }
    rs->basis.last_failed_stabilize_entering = -1;
    rs->basis.failed_stabilize_same_entering_streak = 0;
    rs->basis.last_failed_stabilize_retry_alt = -1;
    rs->basis.failed_stabilize_retry_alt_streak = 0;
    rs->basis.failed_stabilize_retry_alt_ratio_fail_streak = 0;
    rs->basis.failed_stabilize_retry_pool_sample_counter = 0;

    /* ── Progress ────────────────────────────────────────────────── */
    rs->progress.no_pivot_streak = 0;
    rs->progress.no_pivot_no_progress_streak = 0;
    rs->progress.no_pivot_prev_art_sum = RALPH_INFINITY;
    rs->progress.no_pivot_anchor_art_sum = RALPH_INFINITY;
    rs->progress.no_pivot_progress_window_steps = 0;
    rs->progress.no_pivot_force_pending = 0;
    rs->progress.no_pivot_force_reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
    rs->progress.no_pivot_force_cooldown = 0;
    rs->progress.no_pivot_ladder_rescue_cooldown = 0;
    rs->progress.no_pivot_ladder_rescue_fail_streak = 0;
    rs->progress.window_pressure_events = 0;
    rs->progress.window_pressure_failed_stabilize = 0;
    rs->progress.window_pressure_dir_skip = 0;
    rs->progress.window_pressure_local_memory_fail = 0;
    rs->progress.window_pressure_alternations = 0;
    rs->progress.window_pressure_last_event_kind = PHASE1_WINDOW_PRESSURE_EVENT_NONE;
    rs->progress.window_pressure_force_pivot_armed = 0;
    rs->progress.dir_escape_cooldown = 0;
    rs->progress.force_pivot_attempt_budget = 0;
    rs->progress.no_entering_cleanup_streak = 0;

    /* ── Shared ──────────────────────────────────────────────────── */
    rs->shared.recompute_interval =
        lp_refactor_policy_phase1_recompute_interval();
    rs->shared.periodic_policy_cooldown = 0;
    rs->shared.periodic_policy_pressure_decay = 0.0;
    rs->shared.lu_soft_health_streak = 0;

    /* ── Profile ──────────────────────────────────────────────────── */
    rs->profile = P1_RECOVERY_PROFILE_STANDARD;
}

/* ========================================================================
 * Progress operations (extracted from simplex.c)
 * ======================================================================== */

void p1_progress_reset(P1ProgressState *ps) {
    ps->no_pivot_no_progress_streak = 0;
    ps->no_pivot_prev_art_sum = RALPH_INFINITY;
    ps->no_pivot_anchor_art_sum = RALPH_INFINITY;
    ps->no_pivot_progress_window_steps = 0;
}

void p1_progress_update(SimplexSolver *solver,
                        const SimplexTableau *tab,
                        P1ProgressState *ps) {
    double art_sum;
    double prev_art_sum;
    double anchor_art_sum;
    double abs_improve_anchor;
    double rel_improve_anchor;
    double abs_improve_prev;
    double rel_improve_prev;
    double abs_target_anchor;
    double abs_target_prev;
    double window_rel_target;
    int window_steps;
    int improved = 0;

    if (!tab || !ps) {
        return;
    }

    art_sum = phase1_artificial_abs_sum(tab);
    prev_art_sum = ps->no_pivot_prev_art_sum;
    anchor_art_sum = ps->no_pivot_anchor_art_sum;
    window_steps = ps->no_pivot_progress_window_steps;

    if (!isfinite(prev_art_sum) || !isfinite(anchor_art_sum) || window_steps < 0) {
        ps->no_pivot_prev_art_sum = art_sum;
        ps->no_pivot_anchor_art_sum = art_sum;
        ps->no_pivot_progress_window_steps = 0;
        return;
    }

    if (window_steps < INT_MAX) {
        window_steps++;
    }

    abs_improve_anchor = anchor_art_sum - art_sum;
    rel_improve_anchor = abs_improve_anchor / (1.0 + fabs(anchor_art_sum));
    abs_improve_prev = prev_art_sum - art_sum;
    rel_improve_prev = abs_improve_prev / (1.0 + fabs(prev_art_sum));
    abs_target_anchor = fmax(
        PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN,
        PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN * (1.0 + fabs(anchor_art_sum)));
    abs_target_prev = fmax(
        PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN,
        0.5 * PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN * (1.0 + fabs(prev_art_sum)));
    window_rel_target = 0.5 * PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN;

    if (abs_improve_anchor >= abs_target_anchor) {
        improved = 1;
    } else if (rel_improve_prev >= PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN &&
               abs_improve_prev >= abs_target_prev) {
        improved = 1;
    } else if (window_steps >= PHASE1_NO_PIVOT_PROGRESS_WINDOW &&
               rel_improve_anchor >= window_rel_target &&
               abs_improve_anchor >= PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN) {
        improved = 1;
    }

    if (improved) {
        ps->no_pivot_no_progress_streak = 0;
        ps->no_pivot_anchor_art_sum = art_sum;
        window_steps = 0;
    } else {
        if (ps->no_pivot_no_progress_streak < INT_MAX) {
            (ps->no_pivot_no_progress_streak)++;
        }
        lp_telemetry_record_phase1_no_pivot_no_progress(solver);
        if (window_steps >= PHASE1_NO_PIVOT_PROGRESS_WINDOW) {
            ps->no_pivot_anchor_art_sum = art_sum;
            window_steps = 0;
        }
    }
    ps->no_pivot_prev_art_sum = art_sum;
    ps->no_pivot_progress_window_steps = window_steps;
}

int p1_progress_note_no_pivot(SimplexSolver *solver,
                              int m,
                              int degenerate_count,
                              LPPhase1NoPivotForceReason reason,
                              P1ProgressState *ps) {
    int next_streak = 0;
    int next_cooldown = 0;
    int should_force = 0;

    if (!ps) return 0;

    if (ps->no_pivot_streak < INT_MAX) (ps->no_pivot_streak)++;
    lp_telemetry_record_phase1_no_pivot_event(solver, reason);
    if (solver &&
        !lp_glpk_strict_allow_phase1_no_pivot_force(
            solver->glpk_strict_mode)) {
        return 0;
    }

    should_force = lp_refactor_policy_phase1_no_pivot_force_transition(
        m,
        degenerate_count,
        ps->no_pivot_streak,
        ps->no_pivot_force_cooldown,
        &next_streak,
        &next_cooldown);
    ps->no_pivot_streak = next_streak;
    ps->no_pivot_force_cooldown = next_cooldown;

    if (should_force) {
        lp_telemetry_record_phase1_no_pivot_force(solver, reason);
    }
    return should_force;
}

void p1_window_pressure_note(SimplexSolver *solver,
                             int event_kind,
                             int local_memory_fail,
                             P1ProgressState *ps) {
    int alternated = 0;

    if (!ps) {
        return;
    }
    if (event_kind != PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE &&
        event_kind != PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP) {
        return;
    }

    if (ps->window_pressure_events < INT_MAX) (ps->window_pressure_events)++;
    if (event_kind == PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE) {
        if (ps->window_pressure_failed_stabilize < INT_MAX) (ps->window_pressure_failed_stabilize)++;
    } else if (ps->window_pressure_dir_skip < INT_MAX) {
        (ps->window_pressure_dir_skip)++;
    }
    if (local_memory_fail && ps->window_pressure_local_memory_fail < INT_MAX) {
        (ps->window_pressure_local_memory_fail)++;
    }
    if (ps->window_pressure_last_event_kind != PHASE1_WINDOW_PRESSURE_EVENT_NONE &&
        ps->window_pressure_last_event_kind != event_kind) {
        alternated = 1;
        if (ps->window_pressure_alternations < INT_MAX) (ps->window_pressure_alternations)++;
    }
    ps->window_pressure_last_event_kind = event_kind;

    lp_telemetry_record_phase1_window_pressure_event(
        solver,
        event_kind == PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE,
        event_kind == PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP,
        local_memory_fail,
        alternated,
        ps->window_pressure_events,
        ps->window_pressure_failed_stabilize,
        ps->window_pressure_dir_skip,
        ps->window_pressure_local_memory_fail,
        ps->window_pressure_alternations);
}

void p1_window_pressure_reset(SimplexSolver *solver,
                              P1ProgressState *ps) {
    if (!ps) {
        return;
    }
    if (ps->window_pressure_events > 0) {
        lp_telemetry_record_phase1_window_pressure_progress_reset(solver);
    }
    ps->window_pressure_events = 0;
    ps->window_pressure_failed_stabilize = 0;
    ps->window_pressure_dir_skip = 0;
    ps->window_pressure_local_memory_fail = 0;
    ps->window_pressure_alternations = 0;
    ps->window_pressure_last_event_kind = PHASE1_WINDOW_PRESSURE_EVENT_NONE;
}

/* ========================================================================
 * Cross-category helpers (extracted from simplex.c)
 * ======================================================================== */

int p1_progress_attempt_ladder_rescue(
    SimplexSolver *solver,
    SimplexTableau *tab,
    int iter,
    LPPhase1NoPivotForceReason reason,
    LPPhase1RecomputeReason recomp_reason,
    P1ProgressState *ps,
    int *rc_only_streak_io) {
    if (solver &&
        !lp_glpk_strict_allow_phase1_dual_rescue(solver->glpk_strict_mode)) {
        return 0;
    }
    P1TableauSnapshot snapshot;
    int have_snapshot = (p1_tableau_snapshot_take(tab, &snapshot) == 0);
    int rescue_status = dual_simplex_phase1_rescue(
        solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
    ps->no_pivot_ladder_rescue_cooldown =
        PHASE1_NO_PIVOT_LADDER_RESCUE_COOLDOWN_ITERS;
    if (rescue_status == 0) {
        if (have_snapshot) p1_tableau_snapshot_free(&snapshot);
        ps->no_pivot_ladder_rescue_fail_streak = 0;
        lp_telemetry_record_phase1_no_pivot_ladder_dual_rescue(
            solver, reason, 1);
        phase1_recompute_full_with_reason(
            solver,
            tab,
            rc_only_streak_io,
            recomp_reason);
        p1_progress_reset(ps);
        return 1;
    }
    if (solver->status == RALPH_STATUS_TIME_LIMIT) {
        if (have_snapshot) p1_tableau_snapshot_free(&snapshot);
        primal_remove_perturbation(tab);
        solver->iterations = iter;
        phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
        return -1;
    }
    if (have_snapshot) {
        if (p1_tableau_snapshot_restore(tab, &snapshot) != 0) {
            p1_tableau_snapshot_free(&snapshot);
            return -1;
        }
        p1_tableau_snapshot_free(&snapshot);
    }
    if (ps->no_pivot_ladder_rescue_fail_streak < INT_MAX) {
        (ps->no_pivot_ladder_rescue_fail_streak)++;
    }
    lp_telemetry_record_phase1_no_pivot_ladder_dual_rescue(
        solver, reason, 0);
    return 0;
}

int p1_progress_activate_force_pivot(
    SimplexSolver *solver,
    int m,
    int degenerate_count,
    int queue_force_pending,
    int *streak_io,
    P1ProgressState *ps,
    int *force_pending_out,
    LPPhase1NoPivotForceReason *force_reason_out) {
    int streak = 0;
    int budget = 0;
    int pending = 0;
    int reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
    int activated;

    if (solver &&
        !lp_glpk_strict_allow_phase1_force_pivot_mode(
            solver->glpk_strict_mode)) {
        return 0;
    }

    if (streak_io) {
        streak = *streak_io;
    }
    budget = ps->force_pivot_attempt_budget;
    activated = lp_refactor_policy_phase1_activate_force_pivot_mode(
        m,
        degenerate_count,
        streak,
        budget,
        queue_force_pending,
        &streak,
        &budget,
        &pending,
        &reason);
    ps->force_pivot_attempt_budget = budget;
    if (streak_io) {
        *streak_io = streak;
    }
    if (activated && force_pending_out && force_reason_out) {
        *force_pending_out = pending;
        *force_reason_out = (LPPhase1NoPivotForceReason)reason;
    }
    return activated;
}

/* ========================================================================
 * Numerical followup consumption (R3.3)
 * ======================================================================== */

void p1_numerical_consume_followup(SimplexSolver *solver,
                                   P1FollowupEvent event,
                                   P1NumericalState *ns) {
    if (!ns) return;

    /* Shadow guard followup */
    if (ns->shadow_guard_followup_pending) {
        ns->shadow_guard_followup_pending = 0;
        switch (event) {
        case P1_FOLLOWUP_EVENT_FAILED_STABILIZE:
            lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_failed_stabilize(solver);
            break;
        case P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN:
            lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown(solver);
            break;
        case P1_FOLLOWUP_EVENT_PIVOT_FAIL:
            lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_fail(solver);
            break;
        case P1_FOLLOWUP_EVENT_PIVOT_SUCCESS:
            lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_success(solver);
            break;
        }
    }

    /* Force extreme tiny theta relax followup */
    if (ns->force_extreme_tiny_theta_relax_next_pending) {
        ns->force_extreme_tiny_theta_relax_next_pending = 0;
        switch (event) {
        case P1_FOLLOWUP_EVENT_FAILED_STABILIZE:
            lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_failed_stabilize(solver);
            break;
        case P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN:
            lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown(solver);
            break;
        case P1_FOLLOWUP_EVENT_PIVOT_FAIL:
            lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_pivot_fail(solver);
            break;
        case P1_FOLLOWUP_EVENT_PIVOT_SUCCESS:
            lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_pivot_success(solver);
            break;
        }
    }

    /* Force extreme followup */
    if (ns->force_extreme_followup_pending) {
        ns->force_extreme_followup_pending = 0;
        switch (event) {
        case P1_FOLLOWUP_EVENT_FAILED_STABILIZE:
            lp_telemetry_record_phase1_force_extreme_followup_next_failed_stabilize(solver);
            break;
        case P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN:
            lp_telemetry_record_phase1_force_extreme_followup_next_ratio_breakdown(solver);
            break;
        case P1_FOLLOWUP_EVENT_PIVOT_FAIL:
            lp_telemetry_record_phase1_force_extreme_followup_next_pivot_fail(solver);
            break;
        case P1_FOLLOWUP_EVENT_PIVOT_SUCCESS:
            lp_telemetry_record_phase1_force_extreme_followup_next_pivot_success(solver);
            break;
        }
    }
}

/* ========================================================================
 * Streak / entering trackers (R3.3)
 * ======================================================================== */

void p1_numerical_note_dir_skip_entering(SimplexSolver *solver,
                                         int entering,
                                         P1NumericalState *ns) {
    int streak = 0;
    int same_entering = 0;

    if (entering < 0 || !ns) return;
    if (ns->last_dir_skip_entering == entering) {
        same_entering = 1;
        streak = ns->dir_skip_same_entering_streak;
        if (streak < INT_MAX) streak++;
    } else {
        ns->last_dir_skip_entering = entering;
        streak = 1;
    }
    ns->dir_skip_same_entering_streak = streak;
    lp_telemetry_record_phase1_dir_skip_entering(solver, same_entering, streak);
}

void p1_basis_note_failed_stabilize_entering(SimplexSolver *solver,
                                             int entering,
                                             P1BasisRepairState *bs) {
    int streak = 0;
    int same_entering = 0;

    if (entering < 0 || !bs) return;
    if (bs->last_failed_stabilize_entering == entering) {
        same_entering = 1;
        streak = bs->failed_stabilize_same_entering_streak;
        if (streak < INT_MAX) streak++;
    } else {
        bs->last_failed_stabilize_entering = entering;
        streak = 1;
    }
    bs->failed_stabilize_same_entering_streak = streak;
    lp_telemetry_record_phase1_failed_stabilize_entering(
        solver, same_entering, streak);
}

void p1_basis_note_failed_stabilize_retry_alt(SimplexSolver *solver,
                                              int entering,
                                              P1BasisRepairState *bs) {
    int streak = 0;
    int same_entering = 0;

    if (entering < 0 || !bs) return;
    if (bs->last_failed_stabilize_retry_alt == entering) {
        same_entering = 1;
        streak = bs->failed_stabilize_retry_alt_streak;
        if (streak < INT_MAX) streak++;
    } else {
        bs->last_failed_stabilize_retry_alt = entering;
        streak = 1;
    }
    bs->failed_stabilize_retry_alt_streak = streak;
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_alternate(
        solver, same_entering, streak);
}

/* ========================================================================
 * Direction recording (R3.3)
 * ======================================================================== */

void p1_numerical_record_shadow_direction(SimplexSolver *solver,
                                          const SimplexTableau *tab,
                                          int leaving, double theta) {
    double dir_inf = 0.0;
    int dir_nnz = 0;
    double pivot_abs = 0.0;

    if (!solver || !tab) return;
    phase1_failed_stabilize_retry_direction_shape(
        tab, leaving, &dir_inf, &dir_nnz, &pivot_abs);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_followup_direction(
        solver, leaving, theta, dir_inf, dir_nnz, pivot_abs);
}

void p1_numerical_record_extreme_direction(SimplexSolver *solver,
                                           const SimplexTableau *tab,
                                           int leaving, double theta) {
    double dir_inf = 0.0;
    int dir_nnz = 0;
    double pivot_abs = 0.0;

    if (!solver || !tab) return;
    phase1_failed_stabilize_retry_direction_shape(
        tab, leaving, &dir_inf, &dir_nnz, &pivot_abs);
    lp_telemetry_record_phase1_force_extreme_followup_direction(
        solver, leaving, theta, dir_inf, dir_nnz, pivot_abs);
}

/* ========================================================================
 * Basis repair: entering exclusion (R3.4)
 * ======================================================================== */

static void p1_basis_sync_exclusion_compat_slots(P1BasisRepairState *bs) {
    int out = 0;

    if (!bs) return;
    bs->excluded_entering_a = -1;
    bs->excluded_entering_ttl_a = 0;
    bs->excluded_entering_b = -1;
    bs->excluded_entering_ttl_b = 0;
    for (int k = 0; k < PHASE1_ENTERING_EXCLUSION_POOL_SIZE && out < 2; k++) {
        if (bs->excluded_entering_pool_ttl[k] <= 0 ||
            bs->excluded_entering_pool[k] < 0) {
            continue;
        }
        if (out == 0) {
            bs->excluded_entering_a = bs->excluded_entering_pool[k];
            bs->excluded_entering_ttl_a = bs->excluded_entering_pool_ttl[k];
        } else {
            bs->excluded_entering_b = bs->excluded_entering_pool[k];
            bs->excluded_entering_ttl_b = bs->excluded_entering_pool_ttl[k];
        }
        out++;
    }
}

int p1_basis_is_entering_excluded(const P1BasisRepairState *bs, int var) {
    if (!bs || var < 0) return 0;
    for (int k = 0; k < PHASE1_ENTERING_EXCLUSION_POOL_SIZE; k++) {
        if (bs->excluded_entering_pool_ttl[k] > 0 &&
            bs->excluded_entering_pool[k] == var) {
            return 1;
        }
    }
    return 0;
}

void p1_basis_tick_entering_exclusions(P1BasisRepairState *bs) {
    if (!bs) return;
    for (int k = 0; k < PHASE1_ENTERING_EXCLUSION_POOL_SIZE; k++) {
        if (bs->excluded_entering_pool_ttl[k] > 0) {
            bs->excluded_entering_pool_ttl[k]--;
            if (bs->excluded_entering_pool_ttl[k] == 0) {
                bs->excluded_entering_pool[k] = -1;
            }
        }
    }
    p1_basis_sync_exclusion_compat_slots(bs);
}

void p1_basis_clear_entering_exclusions(P1BasisRepairState *bs) {
    if (!bs) return;
    for (int k = 0; k < PHASE1_ENTERING_EXCLUSION_POOL_SIZE; k++) {
        bs->excluded_entering_pool[k] = -1;
        bs->excluded_entering_pool_ttl[k] = 0;
    }
    p1_basis_sync_exclusion_compat_slots(bs);
}

static void p1_basis_exclude_entering_impl(int var, int ttl,
                                           P1BasisRepairState *bs) {
    int replace = -1;

    if (!bs || var < 0 || ttl <= 0) return;
    for (int k = 0; k < PHASE1_ENTERING_EXCLUSION_POOL_SIZE; k++) {
        if (bs->excluded_entering_pool_ttl[k] > 0 &&
            bs->excluded_entering_pool[k] == var) {
            bs->excluded_entering_pool_ttl[k] = ttl;
            p1_basis_sync_exclusion_compat_slots(bs);
            return;
        }
        if (bs->excluded_entering_pool_ttl[k] <= 0) {
            replace = k;
            break;
        }
        if (replace < 0 ||
            bs->excluded_entering_pool_ttl[k] <
                bs->excluded_entering_pool_ttl[replace]) {
            replace = k;
        }
    }
    bs->excluded_entering_pool[replace] = var;
    bs->excluded_entering_pool_ttl[replace] = ttl;
    p1_basis_sync_exclusion_compat_slots(bs);
}

void p1_basis_exclude_entering(SimplexSolver *solver,
                               int var, int ttl,
                               P1BasisRepairState *bs) {
    int repeated_slot = 0;

    if (var < 0 || ttl <= 0 || !bs) return;
    if (p1_basis_is_entering_excluded(bs, var)) {
        repeated_slot = 1;
    }
    p1_basis_exclude_entering_impl(var, ttl, bs);
    lp_telemetry_record_phase1_entering_exclusion(solver, repeated_slot);
}

void p1_basis_pivot_fail_maybe_exclude(SimplexSolver *solver,
                                       int fail_repeat_count,
                                       int entering,
                                       P1BasisRepairState *bs) {
    if (fail_repeat_count < PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER) return;
    p1_basis_exclude_entering(solver, entering,
                              PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_ITERS, bs);
    lp_telemetry_record_phase1_pivot_fail_recovery_exclusion(solver);
}

/* ========================================================================
 * Direct dual rescue (R3.4)
 * ======================================================================== */

int p1_progress_attempt_direct_rescue(SimplexSolver *solver,
                                      SimplexTableau *tab,
                                      int iter,
                                      P1ProgressState *ps) {
    int rescue_status;
    int guard_step = phase1_direct_dual_rescue_guard_plan(
        solver,
        ps ? ps->no_pivot_ladder_rescue_cooldown : 0,
        ps ? ps->no_pivot_ladder_rescue_fail_streak : 0);
    if (guard_step != PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
        return 0;
    }

    P1TableauSnapshot snapshot;
    int have_snapshot = (p1_tableau_snapshot_take(tab, &snapshot) == 0);
    rescue_status = dual_simplex_phase1_rescue(
        solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
    if (ps) {
        ps->no_pivot_ladder_rescue_cooldown =
            PHASE1_NO_PIVOT_LADDER_RESCUE_COOLDOWN_ITERS;
    }
    if (rescue_status == 0) {
        if (have_snapshot) p1_tableau_snapshot_free(&snapshot);
        if (ps) ps->no_pivot_ladder_rescue_fail_streak = 0;
        lp_telemetry_record_phase1_direct_dual_rescue(solver, 1);
        return 1;
    }
    if (solver->status == RALPH_STATUS_TIME_LIMIT) {
        if (have_snapshot) p1_tableau_snapshot_free(&snapshot);
        primal_remove_perturbation(tab);
        solver->iterations = iter;
        phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
        return -1;
    }
    if (have_snapshot) {
        if (p1_tableau_snapshot_restore(tab, &snapshot) != 0) {
            p1_tableau_snapshot_free(&snapshot);
            return -1;
        }
        p1_tableau_snapshot_free(&snapshot);
    }
    if (ps && ps->no_pivot_ladder_rescue_fail_streak < INT_MAX) {
        (ps->no_pivot_ladder_rescue_fail_streak)++;
    }
    lp_telemetry_record_phase1_direct_dual_rescue(solver, 0);
    return 0;
}
