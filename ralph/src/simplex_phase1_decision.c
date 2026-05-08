/*
 * simplex_phase1_decision.c - Phase 1 decision functions.
 *
 * Contains no-pivot ladder, rescue guards, relax plans, escape gates,
 * and soft LU cooldown decision logic plus their test wrappers.
 *
 * Zero behavior change — pure code motion from simplex.c (R3.8).
 */

#include "simplex_phase1_decision.h"
#include "simplex_internal.h"
#include "simplex_phase1_recovery.h"
#include "lp_refactor_policy.h"
#include "lp_glpk_strict.h"
#include "lp_log.h"

#include <math.h>

static int phase1_dual_rescue_guard_step(SimplexSolver *solver,
                                         int rescue_cooldown_iters,
                                         int rescue_fail_streak,
                                         int ladder_context) {
    if (solver &&
        !lp_glpk_strict_allow_phase1_dual_rescue(solver->glpk_strict_mode)) {
        return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
    }
    if (rescue_fail_streak >= PHASE1_NO_PIVOT_LADDER_RESCUE_FAIL_CAP) {
        if (ladder_context) {
            lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(solver, 1);
        } else {
            lp_telemetry_record_phase1_direct_dual_rescue_guard(solver, 1);
        }
        return PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR;
    }
    if (rescue_cooldown_iters > 0) {
        if (ladder_context) {
            lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(solver, 0);
        } else {
            lp_telemetry_record_phase1_direct_dual_rescue_guard(solver, 0);
        }
        return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
    }
    return PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE;
}

double phase1_artificial_abs_sum(const SimplexTableau *tab) {
    double art_sum = 0.0;
    if (!tab || tab->num_artificial <= 0 || !tab->artificial_vars || !tab->x) {
        return 0.0;
    }
    for (int k = 0; k < tab->num_artificial; k++) {
        int j = tab->artificial_vars[k];
        if (j >= 0 && j < tab->n) {
            art_sum += fabs(tab->x[j]);
        }
    }
    return art_sum;
}

int phase1_no_pivot_ladder_step(
    SimplexSolver *solver,
    int m,
    int degenerate_count,
    LPPhase1NoPivotForceReason reason,
    int no_pivot_streak,
    int no_progress_streak,
    int force_pivot_mode_active,
    int *refactor_threshold_out) {
    if (solver &&
        !lp_glpk_strict_allow_phase1_no_pivot_ladder(
            solver->glpk_strict_mode)) {
        if (refactor_threshold_out) *refactor_threshold_out = 0;
        return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
    }
    return lp_refactor_policy_phase1_no_pivot_ladder_step(
        m,
        degenerate_count,
        (int)reason,
        no_pivot_streak,
        no_progress_streak,
        force_pivot_mode_active,
        refactor_threshold_out);
}

int phase1_no_pivot_ladder_apply_rescue_guard(
    SimplexSolver *solver,
    int ladder_step,
    int rescue_cooldown_iters,
    int rescue_fail_streak) {
    if (ladder_step != PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
        return ladder_step;
    }
    return phase1_dual_rescue_guard_step(
        solver, rescue_cooldown_iters, rescue_fail_streak, 1);
}

int phase1_direct_dual_rescue_guard_plan(SimplexSolver *solver,
                                         int rescue_cooldown_iters,
                                         int rescue_fail_streak) {
    return phase1_dual_rescue_guard_step(
        solver, rescue_cooldown_iters, rescue_fail_streak, 0);
}

int phase1_dir_stabilize_escape_gate_plan(int m,
                                                 int degenerate_count,
                                                 int dir_skip_event_streak,
                                                 int no_progress_streak,
                                                 int escape_cooldown,
                                                 int force_extreme_dir,
                                                 int force_lu_health,
                                                 int lu_hard_trigger,
                                                 int *next_escape_cooldown_out,
                                                 int *triggered_out,
                                                 int *hard_bypass_out) {
    return lp_refactor_policy_phase1_dir_stabilize_escape_gate_plan(
        m,
        degenerate_count,
        dir_skip_event_streak,
        no_progress_streak,
        escape_cooldown,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        next_escape_cooldown_out,
        triggered_out,
        hard_bypass_out);
}

int phase1_force_pivot_refactor_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    int force_pivot_mode_active,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    return lp_refactor_policy_phase1_force_pivot_refactor_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        force_pivot_mode_active,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        dual_rescue_attempts,
        dual_rescue_successes,
        dual_rescue_fail_streak);
}

int phase1_force_extreme_refactor_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    return lp_refactor_policy_phase1_force_extreme_refactor_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        dual_rescue_attempts,
        dual_rescue_successes,
        dual_rescue_fail_streak);
}

int phase1_force_extreme_tiny_theta_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak) {
    return lp_refactor_policy_phase1_force_extreme_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak);
}

int phase1_force_extreme_bound_flip_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int bound_flip_followup_streak) {
    return lp_refactor_policy_phase1_force_extreme_bound_flip_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        bound_flip_followup_streak);
}

int phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak,
    double pivot_ratio) {
    return lp_refactor_policy_phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak,
        pivot_ratio);
}

int phase1_soft_lu_policy_cooldown_updates(int m,
                                                  int degenerate_count,
                                                  int periodic_interval) {
    return lp_refactor_policy_phase1_soft_lu_policy_cooldown_updates(
        m,
        degenerate_count,
        periodic_interval);
}

int simplex_phase1_no_pivot_force_plan_for_test(int m,
                                                 int degenerate_count,
                                                 int streak,
                                                 int cooldown,
                                                 int reason,
                                                 int *next_streak_out,
                                                 int *next_cooldown_out) {
    P1ProgressState tmp_ps;
    int should_force;
    memset(&tmp_ps, 0, sizeof(tmp_ps));
    tmp_ps.no_pivot_streak = streak;
    tmp_ps.no_pivot_force_cooldown = cooldown;
    should_force = p1_progress_note_no_pivot(NULL,
                                             m,
                                             degenerate_count,
                                             (LPPhase1NoPivotForceReason)reason,
                                             &tmp_ps);
    if (next_streak_out) *next_streak_out = tmp_ps.no_pivot_streak;
    if (next_cooldown_out) *next_cooldown_out = tmp_ps.no_pivot_force_cooldown;
    return should_force;
}

int simplex_phase1_no_pivot_ladder_plan_for_test(int m,
                                                  int degenerate_count,
                                                  int reason,
                                                  int no_pivot_streak,
                                                  int no_progress_streak,
                                                  int force_pivot_mode_active,
                                                  int *refactor_threshold_out) {
    return phase1_no_pivot_ladder_step(
        NULL,
        m,
        degenerate_count,
        (LPPhase1NoPivotForceReason)reason,
        no_pivot_streak,
        no_progress_streak,
        force_pivot_mode_active,
        refactor_threshold_out);
}

int simplex_phase1_no_pivot_ladder_rescue_guard_plan_for_test(
    int ladder_step,
    int rescue_cooldown_iters,
    int rescue_fail_streak) {
    return phase1_no_pivot_ladder_apply_rescue_guard(NULL,
                                                     ladder_step,
                                                     rescue_cooldown_iters,
                                                     rescue_fail_streak);
}

int simplex_phase1_direct_dual_rescue_guard_plan_for_test(
    int rescue_cooldown_iters,
    int rescue_fail_streak) {
    return phase1_direct_dual_rescue_guard_plan(
        NULL, rescue_cooldown_iters, rescue_fail_streak);
}

int simplex_phase1_dir_skip_rescue_cadence_plan_for_test(
    int dir_skip_event_streak) {
    return lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(
        dir_skip_event_streak);
}

int simplex_phase1_force_pivot_mode_plan_for_test(int m,
                                                   int degenerate_count,
                                                   int queue_force_pending,
                                                   int dir_skip_event_streak,
                                                   int active_budget,
                                                   int *next_streak_out,
                                                   int *next_budget_out,
                                                   int *next_pending_out) {
    int streak = dir_skip_event_streak;
    int budget = active_budget;
    int pending = 0;
    LPPhase1NoPivotForceReason force_reason =
        LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
    P1ProgressState tmp_ps;
    memset(&tmp_ps, 0, sizeof(tmp_ps));
    tmp_ps.force_pivot_attempt_budget = budget;
    int activated = p1_progress_activate_force_pivot(
        NULL,
        m,
        degenerate_count,
        queue_force_pending,
        &streak,
        &tmp_ps,
        &pending,
        &force_reason);
    if (next_streak_out) *next_streak_out = streak;
    if (next_budget_out) *next_budget_out = tmp_ps.force_pivot_attempt_budget;
    if (next_pending_out) *next_pending_out = pending;
    return activated;
}

int simplex_phase1_dir_stabilize_escape_gate_plan_for_test(
    int m,
    int degenerate_count,
    int dir_skip_event_streak,
    int no_progress_streak,
    int escape_cooldown,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int *next_escape_cooldown_out,
    int *triggered_out,
    int *hard_bypass_out) {
    return phase1_dir_stabilize_escape_gate_plan(
        m,
        degenerate_count,
        dir_skip_event_streak,
        no_progress_streak,
        escape_cooldown,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        next_escape_cooldown_out,
        triggered_out,
        hard_bypass_out);
}

int simplex_phase1_force_pivot_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    int force_pivot_mode_active,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    return phase1_force_pivot_refactor_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        force_pivot_mode_active,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        dual_rescue_attempts,
        dual_rescue_successes,
        dual_rescue_fail_streak);
}

int simplex_phase1_force_extreme_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    return phase1_force_extreme_refactor_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        dual_rescue_attempts,
        dual_rescue_successes,
        dual_rescue_fail_streak);
}

int simplex_phase1_force_extreme_tiny_theta_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak) {
    return phase1_force_extreme_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak);
}

int simplex_phase1_force_extreme_bound_flip_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int bound_flip_followup_streak) {
    return phase1_force_extreme_bound_flip_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        bound_flip_followup_streak);
}

int simplex_phase1_force_extreme_catastrophic_tiny_theta_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak,
    double pivot_ratio) {
    return phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak,
        pivot_ratio);
}

int simplex_phase1_soft_lu_policy_cooldown_plan_for_test(
    int m,
    int degenerate_count,
    int periodic_interval,
    int lu_soft_cost_deferred,
    int periodic_policy_cooldown,
    int *next_cooldown_out) {
    int next_cooldown = periodic_policy_cooldown;
    if (lu_soft_cost_deferred) {
        int soft_cooldown = phase1_soft_lu_policy_cooldown_updates(m,
                                                                    degenerate_count,
                                                                    periodic_interval);
        if (soft_cooldown > next_cooldown) {
            next_cooldown = soft_cooldown;
        }
    }
    if (next_cooldown_out) *next_cooldown_out = next_cooldown;
    return next_cooldown > periodic_policy_cooldown;
}
