/*
 * simplex_refactor_schedule.h - Refactor scheduling helpers for the simplex solver.
 *
 * EWMA timing, periodic policy state, soft-LU cost tracking, periodic cost
 * tracking, periodic feedback, and reinvert controller helpers.
 *
 * Pure code motion from simplex.c (R3.7) -- zero behavior change.
 */

#ifndef SIMPLEX_REFACTOR_SCHEDULE_H
#define SIMPLEX_REFACTOR_SCHEDULE_H

#include "lp.h"
#include "lp_refactor_policy.h"
#include "lp_reinvert_controller.h"

/* ── Types shared between simplex.c and zone handlers ─────────────── */

typedef struct {
    int active;
    int suggested_refactor;
    LPReinvertControllerDecision decision;
} LPReinvertShadowEval;

/* ── EWMA / phase hotpath ─────────────────────────────────────────── */

double phase_hotpath_ms(const SimplexSolver *owner, int phase);

/* ── Periodic policy state ────────────────────────────────────────── */

int periodic_policy_refactor_count(const SimplexSolver *owner, int phase);
void periodic_policy_refactor_increment(SimplexSolver *owner, int phase);
void periodic_policy_refactor_reset(SimplexSolver *owner);

/* ── Soft LU cost tracking ────────────────────────────────────────── */

void soft_lu_record_iter_cost(SimplexSolver *owner, int phase, double iter_ms);
void soft_lu_record_refactor_cost(SimplexSolver *owner, int phase, double refactor_ms);
double soft_lu_iter_cost_ewma(const SimplexSolver *owner, int phase);
double soft_lu_refactor_cost_ewma(const SimplexSolver *owner, int phase);
void soft_lu_record_defer(SimplexSolver *owner, int phase);
int soft_lu_consecutive_defers(const SimplexSolver *owner, int phase);
void soft_lu_set_consecutive_defers(SimplexSolver *owner, int phase, int value);
void soft_lu_reset_defer_streak(SimplexSolver *owner, int phase);
void soft_lu_record_cap_forced(SimplexSolver *owner, int phase);

/* ── Periodic cost tracking ───────────────────────────────────────── */

void periodic_cost_record_defer(SimplexSolver *owner, int phase);
int periodic_cost_consecutive_defers(const SimplexSolver *owner, int phase);
void periodic_cost_set_consecutive_defers(SimplexSolver *owner, int phase, int value);
void periodic_cost_reset_defer_streak(SimplexSolver *owner, int phase);
void periodic_cost_record_cap_forced(SimplexSolver *owner, int phase);
void periodic_cost_record_gate_reason(SimplexSolver *owner, int phase,
                                      LPPeriodicCostDampenReason reason);
int periodic_cost_iter_samples(const SimplexSolver *owner, int phase);
int periodic_cost_refactor_samples(const SimplexSolver *owner, int phase);

/* ── Periodic feedback ────────────────────────────────────────────── */

double periodic_feedback_bias_for_phase(const SimplexSolver *owner, int phase);
void periodic_feedback_set_hint(SimplexSolver *owner, int phase,
                                int interval, double pressure);
void periodic_feedback_record_refactor(SimplexSolver *owner, int phase,
                                       int reason, int updates_before, int status);

/* ── Reinvert controller helpers ──────────────────────────────────── */

LPReinvertControllerState *reinvert_state_for_phase(SimplexSolver *solver, int phase);
void reinvert_phase1_pressure_safety_update(SimplexSolver *solver,
    int iter, int no_pivot_streak, int no_progress_streak,
    int ratio_breakdown_count, int dir_skip_no_recompute_streak,
    int hard_lu_trigger);
int reinvert_controller_controls_periodic_phase(const SimplexSolver *solver, int phase);
void reinvert_shadow_eval_reset(LPReinvertShadowEval *eval);
void reinvert_shadow_prepare_phase(SimplexSolver *solver, SimplexTableau *tab,
    int phase, int iter, const LPLUHealthRefactorDecision *lu_decision,
    int periodic_nominal, int min_update_age, int policy_cooldown,
    int control_periodic, int *periodic_candidate_io,
    LPReinvertShadowEval *eval);
void reinvert_shadow_finalize_phase(SimplexSolver *solver, int phase,
    const LPReinvertShadowEval *eval, int final_refactor);

/* ── Runtime trigger recording ────────────────────────────────────── */

void runtime_record_periodic_refactor_trigger(SimplexSolver *solver,
                                              int phase, int lu_health_triggered);

/* ── Test wrappers ────────────────────────────────────────────────── */

void simplex_reinvert_phase1_pressure_safety_step_for_test(
    int iter, int no_pivot_streak, int no_progress_streak,
    int ratio_breakdown_count, int dir_skip_no_recompute_streak,
    int hard_lu_trigger, int *last_iter_io, int *burst_io,
    int *demoted_io, int *demotions_io);

int simplex_reinvert_periodic_control_for_test(int mode, int phase,
    int hard_lu_trigger, int periodic_due, int decision);

int simplex_reinvert_periodic_control_with_phase1_demotion_for_test(
    int mode, int phase, int phase1_demoted,
    int hard_lu_trigger, int periodic_due, int decision);

int simplex_soft_lu_defer_plan_for_test(int phase, int m, int use_bland,
    int degenerate_count, int num_updates, int max_updates,
    int spike_pool_used, int spike_pool_capacity, double cond_estimate,
    double growth_factor, double refactor_cost_ewma, double iter_cost_ewma,
    int consecutive_defers, int *defer_reason_out, int *cap_blocked_out,
    int *next_consecutive_out);

int simplex_periodic_cost_defer_plan_for_test(int phase, int m, int use_bland,
    int degenerate_count, int num_updates, int max_updates,
    int spike_pool_used, int spike_pool_capacity, double cond_estimate,
    double growth_factor, double refactor_cost_ewma, double iter_cost_ewma,
    int refactor_samples, int iter_samples, int consecutive_defers,
    int *gate_reason_out, int *defer_reason_out, int *cap_blocked_out,
    int *next_consecutive_out);

#endif /* SIMPLEX_REFACTOR_SCHEDULE_H */
