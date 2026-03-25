/*
 * simplex_phase1_recovery.c - Phase 1 crisis/recovery state and progress ops.
 *
 * Extracted from simplex.c as part of R3 (crisis machinery consolidation).
 * See simplex_phase1_recovery.h for category descriptions.
 */

#include <math.h>
#include <limits.h>
#include "simplex_phase1_recovery.h"
#include "simplex_internal.h"
#include "lp_refactor_policy.h"
#include "lp_glpk_strict.h"
#include "lp_log.h"

/* Progress window constants (moved from simplex.c) */
#define PHASE1_NO_PIVOT_PROGRESS_WINDOW 6
#define PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN 1e-4
#define PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN 1e-8

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
