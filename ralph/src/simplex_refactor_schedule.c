/*
 * simplex_refactor_schedule.c - Refactor scheduling helpers for the simplex solver.
 *
 * EWMA timing, periodic policy state, soft-LU cost tracking, periodic cost
 * tracking, periodic feedback, and reinvert controller helpers.
 *
 * Pure code motion from simplex.c (R3.7) -- zero behavior change.
 */

#include <math.h>
#include <string.h>
#include "simplex_refactor_schedule.h"
#include "simplex_internal.h"
#include "lp_reinvert_controller.h"

/* ── Constants (previously in simplex.c, only used by moved functions) ── */

#define SOFT_LU_COST_EWMA_ALPHA 0.20
#define SOFT_LU_MAX_CONSEC_DEFER_PHASE1 12
#define SOFT_LU_MAX_CONSEC_DEFER_PHASE2 16
#define PERIODIC_COST_MAX_CONSEC_DEFER_PHASE1 2
#define PERIODIC_COST_MAX_CONSEC_DEFER_PHASE2 3

/* ══════════════════════════════════════════════════════════════════════
 * Category 1: EWMA / phase hotpath
 * ══════════════════════════════════════════════════════════════════════ */

static double ewma_update_ms(double prev_ms, double sample_ms) {
    if (!isfinite(sample_ms) || sample_ms <= 0.0) return prev_ms;
    if (!isfinite(prev_ms) || prev_ms <= 0.0) return sample_ms;
    return prev_ms + SOFT_LU_COST_EWMA_ALPHA * (sample_ms - prev_ms);
}

double phase_hotpath_ms(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) {
        return owner->telemetry.perf_phase1_pricing_ms +
               owner->telemetry.perf_phase1_ratio_ms +
               owner->telemetry.perf_phase1_pivot_ms +
               owner->telemetry.perf_phase1_compute_solution_ms +
               owner->telemetry.perf_phase1_compute_rc_ms;
    }
    if (phase == 2) {
        return owner->telemetry.perf_phase2_pricing_ms +
               owner->telemetry.perf_phase2_ratio_ms +
               owner->telemetry.perf_phase2_pivot_ms +
               owner->telemetry.perf_phase2_compute_solution_ms +
               owner->telemetry.perf_phase2_compute_rc_ms;
    }
    return 0.0;
}

/* ══════════════════════════════════════════════════════════════════════
 * Category 2: Periodic policy state
 * ══════════════════════════════════════════════════════════════════════ */

static LPPeriodicPolicyPhaseState* periodic_policy_phase_state_ptr(SimplexSolver *owner,
                                                                   int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_policy_phase1;
    if (phase == 2) return &owner->policy.periodic_policy_phase2;
    return NULL;
}

static const LPPeriodicPolicyPhaseState* periodic_policy_phase_state_ptr_const(
    const SimplexSolver *owner,
    int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_policy_phase1;
    if (phase == 2) return &owner->policy.periodic_policy_phase2;
    return NULL;
}

int periodic_policy_refactor_count(const SimplexSolver *owner, int phase) {
    const LPPeriodicPolicyPhaseState *state = periodic_policy_phase_state_ptr_const(owner, phase);
    return state ? state->refactors : 0;
}

void periodic_policy_refactor_increment(SimplexSolver *owner, int phase) {
    LPPeriodicPolicyPhaseState *state = periodic_policy_phase_state_ptr(owner, phase);
    if (!state) return;
    state->refactors++;
}

void periodic_policy_refactor_reset(SimplexSolver *owner) {
    if (!owner) return;
    owner->policy.periodic_policy_phase1.refactors = 0;
    owner->policy.periodic_policy_phase2.refactors = 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * Category 3: Soft LU cost tracking
 * ══════════════════════════════════════════════════════════════════════ */

static double* soft_lu_iter_cost_ewma_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_cost_gate_phase1.iter_cost_ewma;
    if (phase == 2) return &owner->policy.soft_lu_cost_gate_phase2.iter_cost_ewma;
    return NULL;
}

static double* soft_lu_refactor_cost_ewma_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_cost_gate_phase1.refactor_cost_ewma;
    if (phase == 2) return &owner->policy.soft_lu_cost_gate_phase2.refactor_cost_ewma;
    return NULL;
}

/* periodic_cost sample pointer helpers (used by soft_lu_record_* and periodic_cost_*) */

static int* periodic_cost_iter_samples_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.iter_samples;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.iter_samples;
    return NULL;
}

static int* periodic_cost_refactor_samples_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.refactor_samples;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.refactor_samples;
    return NULL;
}

int periodic_cost_iter_samples(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.periodic_cost_gate_phase1.iter_samples;
    if (phase == 2) return owner->policy.periodic_cost_gate_phase2.iter_samples;
    return 0;
}

int periodic_cost_refactor_samples(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.periodic_cost_gate_phase1.refactor_samples;
    if (phase == 2) return owner->policy.periodic_cost_gate_phase2.refactor_samples;
    return 0;
}

void soft_lu_record_iter_cost(SimplexSolver *owner, int phase, double iter_ms) {
    int *samples_ptr = periodic_cost_iter_samples_ptr(owner, phase);
    double *ewma_ptr = soft_lu_iter_cost_ewma_ptr(owner, phase);
    if (!ewma_ptr) return;
    if (!isfinite(iter_ms) || iter_ms <= 0.0) return;
    *ewma_ptr = ewma_update_ms(*ewma_ptr, iter_ms);
    if (samples_ptr) (*samples_ptr)++;
}

void soft_lu_record_refactor_cost(SimplexSolver *owner, int phase, double refactor_ms) {
    int *samples_ptr = periodic_cost_refactor_samples_ptr(owner, phase);
    double *ewma_ptr = soft_lu_refactor_cost_ewma_ptr(owner, phase);
    if (!ewma_ptr) return;
    if (!isfinite(refactor_ms) || refactor_ms <= 0.0) return;
    *ewma_ptr = ewma_update_ms(*ewma_ptr, refactor_ms);
    if (samples_ptr) (*samples_ptr)++;
}

double soft_lu_iter_cost_ewma(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) return owner->policy.soft_lu_cost_gate_phase1.iter_cost_ewma;
    if (phase == 2) return owner->policy.soft_lu_cost_gate_phase2.iter_cost_ewma;
    return 0.0;
}

double soft_lu_refactor_cost_ewma(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) return owner->policy.soft_lu_cost_gate_phase1.refactor_cost_ewma;
    if (phase == 2) return owner->policy.soft_lu_cost_gate_phase2.refactor_cost_ewma;
    return 0.0;
}

void soft_lu_record_defer(SimplexSolver *owner, int phase) {
    if (!owner) return;
    if (phase == 1) owner->policy.soft_lu_cost_gate_phase1.defers++;
    else if (phase == 2) owner->policy.soft_lu_cost_gate_phase2.defers++;
}

static int soft_lu_defer_cap_for_phase(int phase) {
    if (phase == 1) return SOFT_LU_MAX_CONSEC_DEFER_PHASE1;
    if (phase == 2) return SOFT_LU_MAX_CONSEC_DEFER_PHASE2;
    return 0;
}

static int* soft_lu_consecutive_defers_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_cost_gate_phase1.consecutive_defers;
    if (phase == 2) return &owner->policy.soft_lu_cost_gate_phase2.consecutive_defers;
    return NULL;
}

static int* soft_lu_cap_forced_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_cost_gate_phase1.defer_cap_forced;
    if (phase == 2) return &owner->policy.soft_lu_cost_gate_phase2.defer_cap_forced;
    return NULL;
}

int soft_lu_consecutive_defers(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.soft_lu_cost_gate_phase1.consecutive_defers;
    if (phase == 2) return owner->policy.soft_lu_cost_gate_phase2.consecutive_defers;
    return 0;
}

void soft_lu_set_consecutive_defers(SimplexSolver *owner, int phase, int value) {
    int *ptr = soft_lu_consecutive_defers_ptr(owner, phase);
    if (!ptr) return;
    if (value < 0) value = 0;
    *ptr = value;
}

void soft_lu_reset_defer_streak(SimplexSolver *owner, int phase) {
    soft_lu_set_consecutive_defers(owner, phase, 0);
}

void soft_lu_record_cap_forced(SimplexSolver *owner, int phase) {
    int *ptr = soft_lu_cap_forced_ptr(owner, phase);
    if (!ptr) return;
    (*ptr)++;
}

/* ══════════════════════════════════════════════════════════════════════
 * Category 4: Periodic cost tracking
 * ══════════════════════════════════════════════════════════════════════ */

void periodic_cost_record_defer(SimplexSolver *owner, int phase) {
    if (!owner) return;
    if (phase == 1) owner->policy.periodic_cost_gate_phase1.defers++;
    else if (phase == 2) owner->policy.periodic_cost_gate_phase2.defers++;
}

static int periodic_cost_defer_cap_for_phase(int phase) {
    if (phase == 1) return PERIODIC_COST_MAX_CONSEC_DEFER_PHASE1;
    if (phase == 2) return PERIODIC_COST_MAX_CONSEC_DEFER_PHASE2;
    return 0;
}

static int* periodic_cost_consecutive_defers_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.consecutive_defers;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.consecutive_defers;
    return NULL;
}

static int* periodic_cost_cap_forced_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.defer_cap_forced;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.defer_cap_forced;
    return NULL;
}

static int* periodic_cost_checks_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.checks;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.checks;
    return NULL;
}

static int* periodic_cost_block_small_m_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.block_small_m;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.block_small_m;
    return NULL;
}

static int* periodic_cost_block_invalid_inputs_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.block_invalid_inputs;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.block_invalid_inputs;
    return NULL;
}

static int* periodic_cost_block_warmup_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.block_warmup;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.block_warmup;
    return NULL;
}

static int* periodic_cost_block_invalid_cost_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.block_invalid_cost;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.block_invalid_cost;
    return NULL;
}

static int* periodic_cost_block_ratio_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.block_ratio;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.block_ratio;
    return NULL;
}

static int* periodic_cost_block_update_reserve_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.block_update_reserve;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.block_update_reserve;
    return NULL;
}

static int* periodic_cost_last_reason_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_phase1.last_reason;
    if (phase == 2) return &owner->policy.periodic_cost_gate_phase2.last_reason;
    return NULL;
}

int periodic_cost_consecutive_defers(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.periodic_cost_gate_phase1.consecutive_defers;
    if (phase == 2) return owner->policy.periodic_cost_gate_phase2.consecutive_defers;
    return 0;
}

void periodic_cost_set_consecutive_defers(SimplexSolver *owner, int phase, int value) {
    int *ptr = periodic_cost_consecutive_defers_ptr(owner, phase);
    if (!ptr) return;
    if (value < 0) value = 0;
    *ptr = value;
}

void periodic_cost_reset_defer_streak(SimplexSolver *owner, int phase) {
    periodic_cost_set_consecutive_defers(owner, phase, 0);
}

void periodic_cost_record_cap_forced(SimplexSolver *owner, int phase) {
    int *ptr = periodic_cost_cap_forced_ptr(owner, phase);
    if (!ptr) return;
    (*ptr)++;
}

void periodic_cost_record_gate_reason(SimplexSolver *owner,
                                             int phase,
                                             LPPeriodicCostDampenReason reason) {
    int *checks_ptr = periodic_cost_checks_ptr(owner, phase);
    int *last_reason_ptr = periodic_cost_last_reason_ptr(owner, phase);
    int *block_ptr = NULL;

    if (!owner) return;
    if (checks_ptr) (*checks_ptr)++;
    if (last_reason_ptr) *last_reason_ptr = (int)reason;

    switch (reason) {
        case LP_PERIODIC_COST_DAMPEN_BLOCK_SMALL_M:
            block_ptr = periodic_cost_block_small_m_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_INPUTS:
            block_ptr = periodic_cost_block_invalid_inputs_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_WARMUP:
            block_ptr = periodic_cost_block_warmup_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_COST:
            block_ptr = periodic_cost_block_invalid_cost_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO:
            block_ptr = periodic_cost_block_ratio_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_UPDATE_RESERVE:
            block_ptr = periodic_cost_block_update_reserve_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_DEFER:
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE:
        default:
            break;
    }

    if (block_ptr) (*block_ptr)++;
}

/* ══════════════════════════════════════════════════════════════════════
 * Category 5: Periodic feedback
 * ══════════════════════════════════════════════════════════════════════ */

double periodic_feedback_bias_for_phase(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) return owner->policy.periodic_feedback_phase1.bias;
    if (phase == 2) return owner->policy.periodic_feedback_phase2.bias;
    return 0.0;
}

static int periodic_feedback_state_load(const SimplexSolver *owner,
                                        int phase,
                                        LPPeriodicFeedbackState *state) {
    const LPPeriodicFeedbackPhaseState *phase_state = NULL;
    if (!owner || !state) return 0;
    if (phase == 1) phase_state = &owner->policy.periodic_feedback_phase1;
    if (phase == 2) phase_state = &owner->policy.periodic_feedback_phase2;
    if (!phase_state) return 0;
    state->bias = phase_state->bias;
    state->last_reason = phase_state->last_reason;
    state->last_interval = phase_state->last_interval;
    state->hint_interval = phase_state->hint_interval;
    state->hint_pressure = phase_state->hint_pressure;
    return 1;
}

static void periodic_feedback_state_store(SimplexSolver *owner,
                                          int phase,
                                          const LPPeriodicFeedbackState *state) {
    LPPeriodicFeedbackPhaseState *phase_state = NULL;
    if (!owner || !state) return;
    if (phase == 1) phase_state = &owner->policy.periodic_feedback_phase1;
    if (phase == 2) phase_state = &owner->policy.periodic_feedback_phase2;
    if (!phase_state) return;
    phase_state->bias = state->bias;
    phase_state->last_reason = state->last_reason;
    phase_state->last_interval = state->last_interval;
    phase_state->hint_interval = state->hint_interval;
    phase_state->hint_pressure = state->hint_pressure;
}

void periodic_feedback_set_hint(SimplexSolver *owner,
                                       int phase,
                                       int interval,
                                       double run_pressure) {
    LPPeriodicFeedbackState state = {0.0, 0, 0, 0, 0.0};
    if (!periodic_feedback_state_load(owner, phase, &state)) return;
    lp_refactor_policy_periodic_feedback_set_hint(&state, interval, run_pressure);
    periodic_feedback_state_store(owner, phase, &state);
}

void periodic_feedback_record_refactor(SimplexSolver *owner,
                                              int phase,
                                              int reason,
                                              int updates_before,
                                              int status) {
    LPPeriodicFeedbackState state = {0.0, 0, 0, 0, 0.0};
    if (!periodic_feedback_state_load(owner, phase, &state)) return;
    lp_refactor_policy_periodic_feedback_record_refactor(
        &state,
        reason,
        updates_before,
        status);
    periodic_feedback_state_store(owner, phase, &state);
}

/* ══════════════════════════════════════════════════════════════════════
 * Category 6: Reinvert controller helpers
 * ══════════════════════════════════════════════════════════════════════ */

LPReinvertControllerState *reinvert_state_for_phase(SimplexSolver *solver,
                                                           int phase) {
    if (!solver) return NULL;
    if (phase == 1) return &solver->policy.reinvert_state_phase1;
    if (phase == 2) return &solver->policy.reinvert_state_phase2;
    return NULL;
}

static double average_solve_density(long long sol_nnz_total, int samples, int m) {
    double density;
    double denom;
    if (samples <= 0 || m <= 0) return 0.0;
    denom = (double)samples * (double)m;
    if (!(denom > 0.0)) return 0.0;
    density = (double)sol_nnz_total / denom;
    if (!(density > 0.0)) return 0.0;
    if (density > 1.0) return 1.0;
    return density;
}

static int reinvert_controller_mode_get(const SimplexSolver *solver) {
    int mode;
    if (!solver) return LP_REINVERT_MODE_SHADOW;
    mode = solver->policy.reinvert_controller_mode;
    if (!lp_reinvert_controller_mode_is_valid(mode)) {
        return LP_REINVERT_MODE_SHADOW;
    }
    return mode;
}

static int reinvert_controller_collect_shadow(const SimplexSolver *solver) {
    return reinvert_controller_mode_get(solver) != LP_REINVERT_MODE_OFF;
}

void reinvert_phase1_pressure_safety_update(SimplexSolver *solver,
                                                   int iter,
                                                   int no_pivot_streak,
                                                   int no_progress_streak,
                                                   int ratio_breakdown_count,
                                                   int dir_skip_no_recompute_streak,
                                                   int hard_lu_trigger) {
    if (!solver) return;
    if (reinvert_controller_mode_get(solver) != LP_REINVERT_MODE_CONTROL_ALL) return;
    lp_refactor_policy_phase1_reinvert_pressure_safety_step(
        iter,
        no_pivot_streak,
        no_progress_streak,
        ratio_breakdown_count,
        dir_skip_no_recompute_streak,
        hard_lu_trigger,
        &solver->policy.reinvert_phase1.pressure_last_iter,
        &solver->policy.reinvert_phase1.pressure_burst,
        &solver->policy.reinvert_phase1.control_demoted,
        &solver->policy.reinvert_phase1.control_demotions);
}

static int reinvert_controller_controls_periodic_phase_effective(int mode,
                                                                 int phase,
                                                                 int phase1_demoted) {
    if (mode == LP_REINVERT_MODE_CONTROL_ALL) {
        if (phase == 1 && phase1_demoted) return 0;
        return phase == 1 || phase == 2;
    }
    return (mode == LP_REINVERT_MODE_CONTROL_PHASE1 && phase == 1);
}

int reinvert_controller_controls_periodic_phase(const SimplexSolver *solver,
                                                       int phase) {
    int mode = reinvert_controller_mode_get(solver);
    int phase1_demoted = 0;
    if (solver) {
        phase1_demoted = solver->policy.reinvert_phase1.control_demoted ? 1 : 0;
    }
    return reinvert_controller_controls_periodic_phase_effective(mode, phase, phase1_demoted);
}

static int reinvert_periodic_apply_decision(int phase,
                                            int hard_lu_trigger,
                                            int periodic_due,
                                            int decision,
                                            int control_enabled) {
    (void)phase;
    if (hard_lu_trigger) return periodic_due ? 1 : 0;
    if (!control_enabled) return periodic_due ? 1 : 0;
    switch ((LPReinvertDecision)decision) {
        case LP_REINVERT_DECISION_FORCE:
            return 1;
        case LP_REINVERT_DECISION_DEFER:
            return 0;
        case LP_REINVERT_DECISION_ALLOW:
        default:
            return periodic_due ? 1 : 0;
    }
}

void reinvert_shadow_eval_reset(LPReinvertShadowEval *eval) {
    if (!eval) return;
    memset(eval, 0, sizeof(*eval));
}

void reinvert_shadow_prepare_phase(SimplexSolver *solver,
                                          SimplexTableau *tab,
                                          int phase,
                                          int iter,
                                          const LPLUHealthRefactorDecision *lu_health_decision,
                                          int periodic_due,
                                          int min_update_age,
                                          int cooldown_updates,
                                          int allow_control,
                                          int *periodic_due_io,
                                          LPReinvertShadowEval *eval) {
    LPReinvertControllerState *state;
    LPReinvertControllerSignals signals;
    int control_enabled;
    double ftran_density;
    double btran_density;

    if (!eval) return;
    reinvert_shadow_eval_reset(eval);
    if (!solver || !tab || !tab->lu || !lu_health_decision) return;
    if (!reinvert_controller_collect_shadow(solver)) return;

    state = reinvert_state_for_phase(solver, phase);
    if (!state) return;

    lp_reinvert_controller_state_record_update_age_ratio(state,
                                                         lu_get_num_updates(tab->lu),
                                                         lu_get_max_updates(tab->lu));
    ftran_density = average_solve_density(solver->telemetry.perf_ftran_sol_nnz_total,
                                          solver->telemetry.perf_ftran_nnz_samples,
                                          tab->m);
    btran_density = average_solve_density(solver->telemetry.perf_btran_sol_nnz_total,
                                          solver->telemetry.perf_btran_nnz_samples,
                                          tab->m);
    lp_reinvert_controller_state_record_solve_density(state,
                                                      ftran_density,
                                                      btran_density);
    lp_reinvert_controller_state_set_cooldown(state, cooldown_updates);

    signals.phase = phase;
    signals.iter = iter;
    signals.m = tab->m;
    signals.num_updates = lu_get_num_updates(tab->lu);
    signals.max_updates = lu_get_max_updates(tab->lu);
    signals.periodic_due = periodic_due ? 1 : 0;
    signals.min_update_age = (min_update_age > 0) ? min_update_age : 0;
    signals.cooldown_updates = (cooldown_updates > 0) ? cooldown_updates : 0;
    signals.hard_lu_trigger = lu_health_decision->hard_trigger ? 1 : 0;
    signals.soft_lu_trigger = lu_health_decision->soft_trigger ? 1 : 0;
    signals.ftran_density = ftran_density;
    signals.btran_density = btran_density;

    eval->decision = lp_reinvert_controller_decide(state, &signals);
    eval->active = 1;
    eval->suggested_refactor =
        (eval->decision.decision == LP_REINVERT_DECISION_FORCE) ||
        (eval->decision.decision == LP_REINVERT_DECISION_ALLOW && signals.periodic_due);

    control_enabled = allow_control &&
                      reinvert_controller_controls_periodic_phase(solver, phase);
    if (periodic_due_io) {
        *periodic_due_io = reinvert_periodic_apply_decision(phase,
                                                            lu_health_decision->hard_trigger,
                                                            periodic_due,
                                                            eval->decision.decision,
                                                            control_enabled);
    }
}

void reinvert_shadow_finalize_phase(SimplexSolver *solver,
                                           int phase,
                                           const LPReinvertShadowEval *eval,
                                           int actual_refactor) {
    LPReinvertControllerState *state;
    if (!solver || !eval || !eval->active) return;
    lp_telemetry_record_reinvert_shadow(solver,
                                        phase,
                                        eval->decision.decision,
                                        eval->decision.reason,
                                        eval->suggested_refactor,
                                        actual_refactor);
    state = reinvert_state_for_phase(solver, phase);
    lp_reinvert_controller_state_apply_decision(state, &eval->decision);
}

/* ══════════════════════════════════════════════════════════════════════
 * Category 7: Test wrappers
 * ══════════════════════════════════════════════════════════════════════ */

void simplex_reinvert_phase1_pressure_safety_step_for_test(
    int iter,
    int no_pivot_streak,
    int no_progress_streak,
    int ratio_breakdown_count,
    int dir_skip_no_recompute_streak,
    int hard_lu_trigger,
    int *last_iter_io,
    int *burst_io,
    int *demoted_io,
    int *demotions_io) {
    lp_refactor_policy_phase1_reinvert_pressure_safety_step(
        iter,
        no_pivot_streak,
        no_progress_streak,
        ratio_breakdown_count,
        dir_skip_no_recompute_streak,
        hard_lu_trigger,
        last_iter_io,
        burst_io,
        demoted_io,
        demotions_io);
}

int simplex_reinvert_periodic_control_for_test(int mode,
                                               int phase,
                                               int hard_lu_trigger,
                                               int periodic_due,
                                               int decision) {
    int control_enabled =
        reinvert_controller_controls_periodic_phase_effective(mode, phase, 0);
    return reinvert_periodic_apply_decision(phase,
                                            hard_lu_trigger,
                                            periodic_due,
                                            decision,
                                            control_enabled);
}

int simplex_reinvert_periodic_control_with_phase1_demotion_for_test(
    int mode,
    int phase,
    int phase1_demoted,
    int hard_lu_trigger,
    int periodic_due,
    int decision) {
    int control_enabled = reinvert_controller_controls_periodic_phase_effective(
        mode,
        phase,
        phase1_demoted);
    return reinvert_periodic_apply_decision(phase,
                                            hard_lu_trigger,
                                            periodic_due,
                                            decision,
                                            control_enabled);
}

int simplex_soft_lu_defer_plan_for_test(int phase,
                                        int m,
                                        int use_bland,
                                        int degenerate_count,
                                        int num_updates,
                                        int max_updates,
                                        int spike_pool_used,
                                        int spike_pool_capacity,
                                        double cond_estimate,
                                        double growth_factor,
                                        double refactor_cost_ewma_ms,
                                        double iter_cost_ewma_ms,
                                        int consecutive_defers,
                                        int *cap_out,
                                        int *cap_blocked_out,
                                        int *next_consecutive_defers_out) {
    int cap = soft_lu_defer_cap_for_phase(phase);
    int should_defer = 0;
    int cap_blocked = 0;
    int next_consecutive = 0;

    if (consecutive_defers < 0) consecutive_defers = 0;
    if (lp_refactor_policy_soft_lu_cost_gate_should_defer(phase,
                                                           m,
                                                           use_bland,
                                                           degenerate_count,
                                                           num_updates,
                                                           max_updates,
                                                           spike_pool_used,
                                                           spike_pool_capacity,
                                                           cond_estimate,
                                                           growth_factor,
                                                           refactor_cost_ewma_ms,
                                                           iter_cost_ewma_ms)) {
        if (cap > 0 && consecutive_defers >= cap) {
            cap_blocked = 1;
        } else {
            should_defer = 1;
            next_consecutive = consecutive_defers + 1;
        }
    }

    if (cap_out) *cap_out = cap;
    if (cap_blocked_out) *cap_blocked_out = cap_blocked;
    if (next_consecutive_defers_out) *next_consecutive_defers_out = next_consecutive;
    return should_defer;
}

int simplex_periodic_cost_defer_plan_for_test(int phase,
                                              int m,
                                              int use_bland,
                                              int degenerate_count,
                                              int num_updates,
                                              int max_updates,
                                              int spike_pool_used,
                                              int spike_pool_capacity,
                                              double cond_estimate,
                                              double growth_factor,
                                              double refactor_cost_ewma_ms,
                                              double iter_cost_ewma_ms,
                                              int refactor_cost_samples,
                                              int iter_cost_samples,
                                              int consecutive_defers,
                                              int *reason_out,
                                              int *cap_out,
                                              int *cap_blocked_out,
                                              int *next_consecutive_defers_out) {
    LPPeriodicCostDampenReason decision;
    int cap = periodic_cost_defer_cap_for_phase(phase);
    int should_defer = 0;
    int cap_blocked = 0;
    int next_consecutive = 0;

    if (consecutive_defers < 0) consecutive_defers = 0;
    decision = lp_refactor_policy_periodic_cost_dampen_decision(
        phase,
        m,
        use_bland,
        degenerate_count,
        num_updates,
        max_updates,
        spike_pool_used,
        spike_pool_capacity,
        cond_estimate,
        growth_factor,
        refactor_cost_ewma_ms,
        iter_cost_ewma_ms,
        refactor_cost_samples,
        iter_cost_samples);
    if (decision == LP_PERIODIC_COST_DAMPEN_DEFER) {
        if (cap > 0 && consecutive_defers >= cap) {
            cap_blocked = 1;
        } else {
            should_defer = 1;
            next_consecutive = consecutive_defers + 1;
        }
    }

    if (reason_out) *reason_out = (int)decision;
    if (cap_out) *cap_out = cap;
    if (cap_blocked_out) *cap_blocked_out = cap_blocked;
    if (next_consecutive_defers_out) *next_consecutive_defers_out = next_consecutive;
    return should_defer;
}

/* ══════════════════════════════════════════════════════════════════════
 * Category 8: Runtime trigger recording
 * ══════════════════════════════════════════════════════════════════════ */

/* Behavioral counter for periodic policy triggers; kept separate from telemetry. */
void runtime_record_periodic_refactor_trigger(SimplexSolver *solver,
                                                     int phase,
                                                     int lu_health_triggered) {
    if (!solver) return;
    if (!lu_health_triggered) {
        periodic_policy_refactor_increment(solver, phase);
    }
    lp_telemetry_record_periodic_refactor_trigger(solver, phase, lu_health_triggered);
}
