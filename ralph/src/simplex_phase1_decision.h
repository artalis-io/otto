/*
 * simplex_phase1_decision.h - Phase 1 decision functions for the simplex method.
 *
 * Extracted from simplex.c as part of R3.8 (decision machinery consolidation).
 * Contains no-pivot ladder, rescue guards, relax plans, escape gates, and
 * soft LU cooldown decision logic plus their test wrappers.
 */

#ifndef SIMPLEX_PHASE1_DECISION_H
#define SIMPLEX_PHASE1_DECISION_H

#include "lp.h"
#include "lp_refactor_policy.h"

/* ── Core decision functions ─────────────────────────────────────── */

double phase1_artificial_abs_sum(const SimplexTableau *tab);

int phase1_no_pivot_ladder_step(SimplexSolver *solver, int m,
                                int degenerate_count,
                                LPPhase1NoPivotForceReason reason,
                                int no_pivot_streak,
                                int no_progress_streak,
                                int force_pivot_mode_active,
                                int *refactor_threshold_out);

int phase1_no_pivot_ladder_apply_rescue_guard(SimplexSolver *solver,
                                              int ladder_step,
                                              int rescue_cooldown_iters,
                                              int rescue_fail_streak);

int phase1_direct_dual_rescue_guard_plan(SimplexSolver *solver,
                                         int rescue_cooldown_iters,
                                         int rescue_fail_streak);

int phase1_dir_stabilize_escape_gate_plan(int m, int degenerate_count,
    int dir_skip_event_streak, int no_progress_streak, int escape_cooldown,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int *next_escape_cooldown_out, int *triggered_out, int *hard_bypass_out);

int phase1_force_pivot_refactor_relax_plan(int m, int degenerate_count,
    int no_progress_streak, int force_pivot_mode_active,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int dual_rescue_attempts, int dual_rescue_successes,
    int dual_rescue_fail_streak);

int phase1_force_extreme_refactor_relax_plan(int m, int degenerate_count,
    int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int dual_rescue_attempts, int dual_rescue_successes,
    int dual_rescue_fail_streak);

int phase1_force_extreme_tiny_theta_relax_plan(int m, int degenerate_count,
    int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int tiny_theta_followup_streak);

int phase1_force_extreme_bound_flip_relax_plan(int m, int degenerate_count,
    int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int bound_flip_followup_streak);

int phase1_force_extreme_catastrophic_tiny_theta_relax_plan(int m,
    int degenerate_count, int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int tiny_theta_followup_streak, double pivot_ratio);

int phase1_soft_lu_policy_cooldown_updates(int m, int degenerate_count,
                                           int periodic_interval);

/* ── Test wrappers ───────────────────────────────────────────────── */

int simplex_phase1_no_pivot_force_plan_for_test(int m,
    int degenerate_count, int streak, int cooldown, int reason,
    int *next_streak_out, int *next_cooldown_out);

int simplex_phase1_no_pivot_ladder_plan_for_test(int m,
    int degenerate_count, int reason, int no_pivot_streak,
    int no_progress_streak, int force_pivot_mode_active,
    int *refactor_threshold_out);

int simplex_phase1_no_pivot_ladder_rescue_guard_plan_for_test(
    int ladder_step, int rescue_cooldown_iters, int rescue_fail_streak);

int simplex_phase1_direct_dual_rescue_guard_plan_for_test(
    int rescue_cooldown_iters, int rescue_fail_streak);

int simplex_phase1_dir_skip_rescue_cadence_plan_for_test(
    int dir_skip_event_streak);

int simplex_phase1_force_pivot_mode_plan_for_test(int m,
    int degenerate_count, int queue_force_pending,
    int dir_skip_event_streak, int active_budget,
    int *next_streak_out, int *next_budget_out, int *next_pending_out);

int simplex_phase1_dir_stabilize_escape_gate_plan_for_test(
    int m, int degenerate_count, int dir_skip_event_streak,
    int no_progress_streak, int escape_cooldown,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int *next_escape_cooldown_out, int *triggered_out, int *hard_bypass_out);

int simplex_phase1_force_pivot_relax_plan_for_test(
    int m, int degenerate_count, int no_progress_streak,
    int force_pivot_mode_active, int force_extreme_dir,
    int force_lu_health, int lu_hard_trigger,
    int dual_rescue_attempts, int dual_rescue_successes,
    int dual_rescue_fail_streak);

int simplex_phase1_force_extreme_relax_plan_for_test(
    int m, int degenerate_count, int no_progress_streak,
    double dir_inf_ratio, int force_extreme_dir,
    int force_lu_health, int lu_hard_trigger,
    int dual_rescue_attempts, int dual_rescue_successes,
    int dual_rescue_fail_streak);

int simplex_phase1_force_extreme_tiny_theta_relax_plan_for_test(
    int m, int degenerate_count, int no_progress_streak,
    double dir_inf_ratio, int force_extreme_dir,
    int force_lu_health, int lu_hard_trigger,
    int tiny_theta_followup_streak);

int simplex_phase1_force_extreme_bound_flip_relax_plan_for_test(
    int m, int degenerate_count, int no_progress_streak,
    double dir_inf_ratio, int force_extreme_dir,
    int force_lu_health, int lu_hard_trigger,
    int bound_flip_followup_streak);

int simplex_phase1_force_extreme_catastrophic_tiny_theta_relax_plan_for_test(
    int m, int degenerate_count, int no_progress_streak,
    double dir_inf_ratio, int force_extreme_dir,
    int force_lu_health, int lu_hard_trigger,
    int tiny_theta_followup_streak, double pivot_ratio);

int simplex_phase1_soft_lu_policy_cooldown_plan_for_test(
    int m, int degenerate_count, int periodic_interval,
    int lu_soft_cost_deferred, int periodic_policy_cooldown,
    int *next_cooldown_out);

#endif /* SIMPLEX_PHASE1_DECISION_H */
