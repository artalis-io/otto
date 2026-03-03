#include <limits.h>
#include <math.h>
#include <string.h>
#include "lp_reinvert_controller.h"

#define REINVERT_EWMA_ALPHA 0.20
#define REINVERT_WARMUP_MIN_ITER_SAMPLES 8
#define REINVERT_WARMUP_MIN_REFACTOR_SAMPLES 1
#define REINVERT_COST_RATIO_TRIGGER 6.0
#define REINVERT_DENSITY_FORCE_TRIGGER 0.75
#define REINVERT_SOFT_BREACH_FORCE_STREAK 2
#define REINVERT_UPDATE_RESERVE_NUM 1
#define REINVERT_UPDATE_RESERVE_DEN 10
#define REINVERT_UPDATE_RESERVE_MIN 6

static double clamp_unit(double x) {
    if (!(x > 0.0)) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

static int clamp_non_negative(int x) {
    return (x < 0) ? 0 : x;
}

static int signal_phase_is_valid(int phase) {
    return phase == 1 || phase == 2;
}

int lp_reinvert_controller_mode_is_valid(int mode) {
    return mode >= LP_REINVERT_MODE_OFF && mode <= LP_REINVERT_MODE_CONTROL_ALL;
}

static double ewma_update(double current, double sample) {
    if (!isfinite(sample) || sample <= 0.0) return current;
    if (!isfinite(current) || current <= 0.0) return sample;
    return (1.0 - REINVERT_EWMA_ALPHA) * current + REINVERT_EWMA_ALPHA * sample;
}

static double solve_density_from_signals(const LPReinvertControllerSignals *signals) {
    int have_ftran;
    int have_btran;
    if (!signals) return 0.0;
    have_ftran = isfinite(signals->ftran_density) && signals->ftran_density > 0.0;
    have_btran = isfinite(signals->btran_density) && signals->btran_density > 0.0;
    if (have_ftran && have_btran) {
        return clamp_unit(0.5 * (signals->ftran_density + signals->btran_density));
    }
    if (have_ftran) return clamp_unit(signals->ftran_density);
    if (have_btran) return clamp_unit(signals->btran_density);
    return 0.0;
}

static int should_hold_update_reserve(const LPReinvertControllerSignals *signals) {
    int reserve;
    if (!signals || signals->max_updates <= 0 || signals->num_updates < 0) return 0;
    reserve = (signals->max_updates * REINVERT_UPDATE_RESERVE_NUM) /
              REINVERT_UPDATE_RESERVE_DEN;
    if (reserve < REINVERT_UPDATE_RESERVE_MIN) reserve = REINVERT_UPDATE_RESERVE_MIN;
    return (signals->max_updates - signals->num_updates) <= reserve;
}

static LPReinvertControllerDecision decision_default(void) {
    LPReinvertControllerDecision out;
    out.decision = LP_REINVERT_DECISION_DEFER;
    out.reason = LP_REINVERT_REASON_INVALID_INPUTS;
    out.refactor_to_iter_cost_ratio = 0.0;
    out.solve_density = 0.0;
    out.cooldown_updates_next = 0;
    out.soft_lu_breach_streak_next = 0;
    return out;
}

void lp_reinvert_controller_state_reset(LPReinvertControllerState *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

void lp_reinvert_controller_state_record_iter_cost(LPReinvertControllerState *state,
                                                   double iter_ms) {
    if (!state) return;
    if (!isfinite(iter_ms) || iter_ms <= 0.0) return;
    state->iter_cost_ewma_ms = ewma_update(state->iter_cost_ewma_ms, iter_ms);
    if (state->iter_cost_samples < INT_MAX) state->iter_cost_samples++;
}

void lp_reinvert_controller_state_record_refactor_cost(
    LPReinvertControllerState *state,
    double refactor_ms) {
    if (!state) return;
    if (!isfinite(refactor_ms) || refactor_ms <= 0.0) return;
    state->refactor_cost_ewma_ms = ewma_update(state->refactor_cost_ewma_ms, refactor_ms);
    if (state->refactor_cost_samples < INT_MAX) state->refactor_cost_samples++;
}

void lp_reinvert_controller_state_record_solve_density(LPReinvertControllerState *state,
                                                       double ftran_density,
                                                       double btran_density) {
    double sample = 0.0;
    int have_ftran = isfinite(ftran_density) && ftran_density > 0.0;
    int have_btran = isfinite(btran_density) && btran_density > 0.0;
    if (!state) return;

    if (have_ftran && have_btran) sample = 0.5 * (ftran_density + btran_density);
    else if (have_ftran) sample = ftran_density;
    else if (have_btran) sample = btran_density;
    else return;

    sample = clamp_unit(sample);
    state->solve_density_ewma = ewma_update(state->solve_density_ewma, sample);
    if (state->density_samples < INT_MAX) state->density_samples++;
}

void lp_reinvert_controller_state_record_update_age_ratio(
    LPReinvertControllerState *state,
    int num_updates,
    int max_updates) {
    double ratio;
    if (!state || num_updates < 0 || max_updates <= 0) return;
    ratio = clamp_unit((double)num_updates / (double)max_updates);
    state->update_age_ratio_ewma = ewma_update(state->update_age_ratio_ewma, ratio);
    if (state->update_age_samples < INT_MAX) state->update_age_samples++;
}

void lp_reinvert_controller_state_set_cooldown(LPReinvertControllerState *state,
                                               int cooldown_updates) {
    if (!state) return;
    state->cooldown_updates_remaining = clamp_non_negative(cooldown_updates);
}

void lp_reinvert_controller_state_apply_decision(
    LPReinvertControllerState *state,
    const LPReinvertControllerDecision *decision) {
    if (!state || !decision) return;
    state->cooldown_updates_remaining = clamp_non_negative(decision->cooldown_updates_next);
    state->soft_lu_breach_streak = clamp_non_negative(decision->soft_lu_breach_streak_next);
}

LPReinvertControllerDecision lp_reinvert_controller_decide(
    const LPReinvertControllerState *state,
    const LPReinvertControllerSignals *signals) {
    LPReinvertControllerDecision out = decision_default();
    int cooldown;
    int periodic_due;
    int warmup_ready;
    int reserve_hold;

    if (!state || !signals) return out;
    if (!signal_phase_is_valid(signals->phase) || signals->m <= 0 ||
        signals->iter < 0 || signals->num_updates < 0 || signals->max_updates < 0) {
        return out;
    }

    cooldown = clamp_non_negative(signals->cooldown_updates);
    if (state->cooldown_updates_remaining > cooldown) {
        cooldown = state->cooldown_updates_remaining;
    }
    periodic_due = signals->periodic_due ? 1 : 0;
    out.solve_density = state->solve_density_ewma;
    {
        double signal_density = solve_density_from_signals(signals);
        if (signal_density > out.solve_density) out.solve_density = signal_density;
    }

    if (state->iter_cost_ewma_ms > 0.0 && state->refactor_cost_ewma_ms > 0.0) {
        out.refactor_to_iter_cost_ratio = state->refactor_cost_ewma_ms /
                                          state->iter_cost_ewma_ms;
    } else {
        out.refactor_to_iter_cost_ratio = 0.0;
    }

    if (signals->hard_lu_trigger) {
        out.decision = LP_REINVERT_DECISION_FORCE;
        out.reason = LP_REINVERT_REASON_HARD_LU_HEALTH;
        out.cooldown_updates_next = 0;
        out.soft_lu_breach_streak_next = 0;
        return out;
    }

    out.soft_lu_breach_streak_next = state->soft_lu_breach_streak;
    if (signals->soft_lu_trigger) {
        if (out.soft_lu_breach_streak_next < INT_MAX) {
            out.soft_lu_breach_streak_next++;
        }
    } else {
        out.soft_lu_breach_streak_next = 0;
    }

    if (signals->soft_lu_trigger) {
        if (out.soft_lu_breach_streak_next >= REINVERT_SOFT_BREACH_FORCE_STREAK &&
            signals->num_updates >= clamp_non_negative(signals->min_update_age)) {
            out.decision = LP_REINVERT_DECISION_FORCE;
        } else {
            out.decision = LP_REINVERT_DECISION_DEFER;
        }
        out.reason = LP_REINVERT_REASON_SOFT_LU_HEALTH;
        out.cooldown_updates_next = cooldown;
        return out;
    }

    if (!periodic_due) {
        out.decision = LP_REINVERT_DECISION_ALLOW;
        out.reason = LP_REINVERT_REASON_NONE;
        out.cooldown_updates_next = cooldown;
        return out;
    }

    if (signals->num_updates < clamp_non_negative(signals->min_update_age)) {
        out.decision = LP_REINVERT_DECISION_DEFER;
        out.reason = LP_REINVERT_REASON_UPDATE_AGE;
        out.cooldown_updates_next = cooldown;
        return out;
    }

    if (cooldown > 0) {
        out.decision = LP_REINVERT_DECISION_DEFER;
        out.reason = LP_REINVERT_REASON_COOLDOWN;
        out.cooldown_updates_next = cooldown - 1;
        return out;
    }

    warmup_ready = state->iter_cost_samples >= REINVERT_WARMUP_MIN_ITER_SAMPLES &&
                   state->refactor_cost_samples >= REINVERT_WARMUP_MIN_REFACTOR_SAMPLES;
    if (!warmup_ready) {
        out.decision = LP_REINVERT_DECISION_DEFER;
        out.reason = LP_REINVERT_REASON_WARMUP;
        out.cooldown_updates_next = cooldown;
        return out;
    }

    if (out.solve_density >= REINVERT_DENSITY_FORCE_TRIGGER) {
        out.decision = LP_REINVERT_DECISION_FORCE;
        out.reason = LP_REINVERT_REASON_DENSITY_PRESSURE;
        out.cooldown_updates_next = 0;
        return out;
    }

    reserve_hold = should_hold_update_reserve(signals);
    if (out.refactor_to_iter_cost_ratio >= REINVERT_COST_RATIO_TRIGGER &&
        !reserve_hold) {
        out.decision = LP_REINVERT_DECISION_DEFER;
        out.reason = LP_REINVERT_REASON_COST_DAMPEN;
        out.cooldown_updates_next = cooldown;
        return out;
    }

    out.decision = LP_REINVERT_DECISION_ALLOW;
    out.reason = LP_REINVERT_REASON_PERIODIC_CADENCE;
    out.cooldown_updates_next = cooldown;
    return out;
}

const char *lp_reinvert_controller_reason_string(LPReinvertReason reason) {
    switch (reason) {
        case LP_REINVERT_REASON_NONE:
            return "none";
        case LP_REINVERT_REASON_INVALID_INPUTS:
            return "invalid_inputs";
        case LP_REINVERT_REASON_WARMUP:
            return "warmup";
        case LP_REINVERT_REASON_COOLDOWN:
            return "cooldown";
        case LP_REINVERT_REASON_HARD_LU_HEALTH:
            return "hard_lu_health";
        case LP_REINVERT_REASON_SOFT_LU_HEALTH:
            return "soft_lu_health";
        case LP_REINVERT_REASON_UPDATE_AGE:
            return "update_age";
        case LP_REINVERT_REASON_COST_DAMPEN:
            return "cost_dampen";
        case LP_REINVERT_REASON_DENSITY_PRESSURE:
            return "density_pressure";
        case LP_REINVERT_REASON_PERIODIC_CADENCE:
            return "periodic_cadence";
        default:
            return "unknown";
    }
}
