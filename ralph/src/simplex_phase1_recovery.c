/*
 * simplex_phase1_recovery.c - Phase 1 crisis/recovery state initialization.
 *
 * Extracted from simplex.c as part of R3 (crisis machinery consolidation).
 * See simplex_phase1_recovery.h for category descriptions.
 */

#include <math.h>
#include "simplex_phase1_recovery.h"
#include "lp_refactor_policy.h"

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
