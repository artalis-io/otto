#include <math.h>
#include "lp.h"
#include "lp_refactor_policy.h"

#define PHASE1_PERIODIC_REFACTOR_MIN_INTERVAL 24
#define PHASE1_PERIODIC_REFACTOR_MAX_INTERVAL 96
#define PHASE2_PERIODIC_REFACTOR_MIN_INTERVAL 10
#define PHASE2_PERIODIC_REFACTOR_MAX_INTERVAL 80
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_M 1200
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_COUNT 20
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MIN_INTERVAL 24
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_SPIKE_PCT 30
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_COND 1e6
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_GROWTH 1e4
#define PERIODIC_REFACTOR_MIN_UPDATE_AGE 8
#define PERIODIC_REFACTOR_PRESSURE_TRIGGER 0.40
#define PERIODIC_REFACTOR_SIZE_START_M 350
#define PERIODIC_REFACTOR_SIZE_FULL_M 500
#define PERIODIC_REFACTOR_RELAX_NUM 1
#define PERIODIC_REFACTOR_RELAX_DEN 3
#define PHASE2_POLICY_COOLDOWN_MIN_M 450
#define PHASE2_POLICY_COOLDOWN_DEGEN_TRIGGER 40
#define PHASE2_POLICY_COOLDOWN_MIN_UPDATES 16
#define PHASE2_POLICY_COOLDOWN_MAX_UPDATES 96
#define PHASE1_POLICY_COOLDOWN_MIN_M 1200
#define PHASE1_POLICY_COOLDOWN_DEGEN_TRIGGER 40
#define PHASE1_POLICY_COOLDOWN_POLICY_TRIGGER 40
#define PHASE1_POLICY_COOLDOWN_MIN_UPDATES 24
#define PHASE1_POLICY_COOLDOWN_MAX_UPDATES 192
#define PHASE1_POLICY_COOLDOWN_NUM 2
#define PHASE1_POLICY_COOLDOWN_DEN 1
#define PHASE1_DIR_STABILIZE_BASE_COOLDOWN_UPDATES 8
#define PHASE1_DIR_STABILIZE_ADAPT_MIN_M 700
#define PHASE1_DIR_STABILIZE_STREAK_STEP 3
#define PHASE1_DIR_STABILIZE_MAX_MULT 8
#define PHASE1_DIR_STABILIZE_MAX_COOLDOWN_UPDATES 64
#define PHASE1_DIR_STABILIZE_FORCE_RATIO_BASE 100.0
#define PHASE1_DIR_STABILIZE_FORCE_RATIO_COOLDOWN 1000.0
#define LU_HEALTH_HARD_COND_MIN_UPDATES 10
#define LU_HEALTH_HARD_COND_RATIO 1e10
#define LU_HEALTH_SOFT_COND_MED 1e6
#define LU_HEALTH_SOFT_COND_HIGH 1e8
#define LU_HEALTH_SOFT_SPIKE_WARN_PCT 85
#define LU_HEALTH_SOFT_SPIKE_WORK_MULT 8
#define LU_HEALTH_SOFT_MIN_UPDATE_AGE_MIN 10
#define LU_HEALTH_SOFT_MIN_UPDATE_AGE_MAX 30
#define LU_HEALTH_SOFT_MIN_UPDATE_AGE_NUM 1
#define LU_HEALTH_SOFT_MIN_UPDATE_AGE_DEN 5
#define LU_HEALTH_SOFT_BREACH_THRESHOLD_BASE 3
#define LU_HEALTH_SOFT_BREACH_THRESHOLD_HIGH 2
#define LU_SOFT_COST_GATE_PHASE1_MIN_M 1200
#define LU_SOFT_COST_GATE_PHASE2_MIN_M 700
#define LU_SOFT_COST_GATE_MIN_DEGEN 20
#define LU_SOFT_COST_GATE_MIN_REFACTOR_MS 1.0
#define LU_SOFT_COST_GATE_RATIO_TRIGGER 8.0
#define LU_SOFT_COST_GATE_UPDATE_RESERVE_NUM 1
#define LU_SOFT_COST_GATE_UPDATE_RESERVE_DEN 8
#define LU_SOFT_COST_GATE_UPDATE_RESERVE_MIN 4
#define LU_SOFT_COST_GATE_MAX_SPIKE_PCT 70
#define LU_SOFT_COST_GATE_MAX_COND 1e7
#define LU_SOFT_COST_GATE_MAX_GROWTH 1e5
#define PERIODIC_COST_DAMPEN_PHASE1_MIN_M 700
#define PERIODIC_COST_DAMPEN_PHASE2_MIN_M 700
#define PERIODIC_COST_DAMPEN_MIN_REFACTOR_MS 1.0
#define PERIODIC_COST_DAMPEN_RATIO_TRIGGER 6.0
#define PERIODIC_COST_DAMPEN_UPDATE_RESERVE_NUM 1
#define PERIODIC_COST_DAMPEN_UPDATE_RESERVE_DEN 10
#define PERIODIC_COST_DAMPEN_UPDATE_RESERVE_MIN 6
#define PERIODIC_COST_DAMPEN_MIN_ITER_SAMPLES 8
#define PERIODIC_COST_DAMPEN_MIN_REFACTOR_SAMPLES 1

static double clamp_unit_interval(double x) {
    if (!(x > 0.0)) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

static int clamp_int_range(int x, int lo, int hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static int lu_soft_min_update_age(int max_updates) {
    int min_age = (max_updates * LU_HEALTH_SOFT_MIN_UPDATE_AGE_NUM) /
                  LU_HEALTH_SOFT_MIN_UPDATE_AGE_DEN;
    return clamp_int_range(min_age,
                           LU_HEALTH_SOFT_MIN_UPDATE_AGE_MIN,
                           LU_HEALTH_SOFT_MIN_UPDATE_AGE_MAX);
}

static int periodic_interval_bounds(int phase, int *min_interval, int *max_interval) {
    if (!min_interval || !max_interval) return 0;
    if (phase == 1) {
        *min_interval = PHASE1_PERIODIC_REFACTOR_MIN_INTERVAL;
        *max_interval = PHASE1_PERIODIC_REFACTOR_MAX_INTERVAL;
        return 1;
    }
    if (phase == 2) {
        *min_interval = PHASE2_PERIODIC_REFACTOR_MIN_INTERVAL;
        *max_interval = PHASE2_PERIODIC_REFACTOR_MAX_INTERVAL;
        return 1;
    }
    return 0;
}

static int phase2_large_degenerate_relax_ok(int phase,
                                            int m,
                                            int use_bland,
                                            int degenerate_count,
                                            int spike_pool_used,
                                            int spike_pool_capacity,
                                            double cond_estimate,
                                            double growth_factor) {
    if (phase != 2) return 0;
    if (m < PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_M) return 0;
    if (use_bland) return 0;
    if (degenerate_count < PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_COUNT) return 0;
    if (spike_pool_capacity > 0 &&
        spike_pool_used * 100 >
            spike_pool_capacity * PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_SPIKE_PCT) {
        return 0;
    }
    if (isfinite(cond_estimate) && cond_estimate > PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_COND) {
        return 0;
    }
    if (isfinite(growth_factor) && growth_factor > PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_GROWTH) {
        return 0;
    }
    return 1;
}

static double compute_large_basis_pressure(int m) {
    if (m <= PERIODIC_REFACTOR_SIZE_START_M) return 0.0;
    if (m >= PERIODIC_REFACTOR_SIZE_FULL_M) return 1.0;
    return (double)(m - PERIODIC_REFACTOR_SIZE_START_M) /
           (double)(PERIODIC_REFACTOR_SIZE_FULL_M - PERIODIC_REFACTOR_SIZE_START_M);
}

static double compute_lu_health_pressure(int spike_pool_used,
                                         int spike_pool_capacity,
                                         double cond_estimate,
                                         double growth_factor,
                                         int use_bland,
                                         int degenerate_count) {
    double deg_pressure = use_bland ? 1.0 : clamp_unit_interval((double)degenerate_count / 20.0);
    double spike_pressure = 0.0;
    double cond_pressure = 0.0;
    double growth_pressure = 0.0;
    double health_pressure;

    if (spike_pool_capacity > 0 && spike_pool_used > 0) {
        spike_pressure = clamp_unit_interval((double)spike_pool_used / (double)spike_pool_capacity);
    }
    if (isfinite(cond_estimate) && cond_estimate > 1.0) {
        cond_pressure = clamp_unit_interval(log10(cond_estimate) / 8.0);
    }
    if (isfinite(growth_factor) && growth_factor > 1.0) {
        growth_pressure = clamp_unit_interval(growth_factor / RALPH_LU_GROWTH_REFACTOR_THRESHOLD);
    }

    health_pressure = 0.45 * deg_pressure +
                      0.25 * spike_pressure +
                      0.15 * cond_pressure +
                      0.15 * growth_pressure;

    if (deg_pressure > health_pressure) health_pressure = deg_pressure;
    if (growth_pressure > health_pressure) health_pressure = growth_pressure;
    return clamp_unit_interval(health_pressure);
}

LPPeriodicRefactorPolicy lp_refactor_policy_build_from_metrics(int phase,
                                                               int m,
                                                               int max_updates,
                                                               int num_updates,
                                                               int spike_pool_used,
                                                               int spike_pool_capacity,
                                                               double cond_estimate,
                                                               double growth_factor,
                                                               int use_bland,
                                                               int degenerate_count,
                                                               double feedback_bias) {
    LPPeriodicRefactorPolicy policy = {0, 0, 0.0, 0.0};
    int min_interval = 0;
    int max_interval = 0;
    int base_interval;
    int size_interval;
    int relax_span;
    double size_pressure;
    double health_pressure;
    double update_pressure = 0.0;

    if (!periodic_interval_bounds(phase, &min_interval, &max_interval)) return policy;
    if (phase2_large_degenerate_relax_ok(phase,
                                         m,
                                         use_bland,
                                         degenerate_count,
                                         spike_pool_used,
                                         spike_pool_capacity,
                                         cond_estimate,
                                         growth_factor) &&
        min_interval < PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MIN_INTERVAL) {
        min_interval = PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MIN_INTERVAL;
    }

    base_interval = (phase == 1)
        ? ((max_updates > 0) ? (max_updates / 3) : min_interval)
        : ((max_updates > 0) ? (max_updates / 4) : min_interval);
    if (base_interval < min_interval) base_interval = min_interval;
    if (base_interval > max_interval) base_interval = max_interval;

    size_pressure = compute_large_basis_pressure(m);
    size_interval = base_interval -
                    (int)(size_pressure * (double)(base_interval - min_interval) + 0.5);
    if (size_interval < min_interval) size_interval = min_interval;
    if (size_interval > max_interval) size_interval = max_interval;

    health_pressure = compute_lu_health_pressure(spike_pool_used,
                                                 spike_pool_capacity,
                                                 cond_estimate,
                                                 growth_factor,
                                                 use_bland,
                                                 degenerate_count);
    policy.interval_pressure = (health_pressure > size_pressure) ? health_pressure : size_pressure;
    policy.interval_pressure = clamp_unit_interval(policy.interval_pressure + feedback_bias);

    relax_span = max_interval - size_interval;
    relax_span = (relax_span * PERIODIC_REFACTOR_RELAX_NUM) / PERIODIC_REFACTOR_RELAX_DEN;
    if (relax_span < 0) relax_span = 0;

    policy.interval = size_interval +
                      (int)(((1.0 - policy.interval_pressure) * (double)relax_span) + 0.5);
    if (policy.interval < min_interval) policy.interval = min_interval;
    if (policy.interval > max_interval) policy.interval = max_interval;
    if (max_updates >= min_interval && policy.interval > max_updates) {
        policy.interval = max_updates;
    }

    if (max_updates > 0 && num_updates > 0) {
        update_pressure = clamp_unit_interval((double)num_updates / (double)max_updates);
    }
    policy.run_pressure = policy.interval_pressure;
    if (update_pressure > policy.run_pressure) policy.run_pressure = update_pressure;
    if (use_bland || degenerate_count >= 20) policy.run_pressure = 1.0;

    policy.min_update_age = policy.interval / 2;
    if (policy.run_pressure >= 0.85) {
        int early_age = policy.interval / 3;
        if (early_age > 0 && early_age < policy.min_update_age) {
            policy.min_update_age = early_age;
        }
    }
    if (policy.min_update_age < PERIODIC_REFACTOR_MIN_UPDATE_AGE) {
        policy.min_update_age = PERIODIC_REFACTOR_MIN_UPDATE_AGE;
    }
    if (policy.min_update_age > policy.interval) {
        policy.min_update_age = policy.interval;
    }

    return policy;
}

int lp_refactor_policy_should_run_metrics(int iter,
                                          int num_updates,
                                          const LPPeriodicRefactorPolicy *policy,
                                          int use_bland,
                                          int degenerate_count) {
    if (!policy || policy->interval <= 0 || iter <= 0 || num_updates <= 0) return 0;
    if (num_updates < policy->min_update_age) return 0;
    if ((num_updates % policy->interval) != 0) return 0;

    if (use_bland || degenerate_count >= 20) return 1;
    return policy->run_pressure >= PERIODIC_REFACTOR_PRESSURE_TRIGGER;
}

int lp_refactor_policy_phase2_cooldown_eligible(int m,
                                                int degenerate_count,
                                                int spike_pool_used,
                                                int spike_pool_capacity,
                                                double cond_estimate,
                                                double growth_factor) {
    if (m < PHASE2_POLICY_COOLDOWN_MIN_M) return 0;
    if (degenerate_count < PHASE2_POLICY_COOLDOWN_DEGEN_TRIGGER) return 0;
    if (spike_pool_capacity > 0 &&
        spike_pool_used * 100 >
            spike_pool_capacity * PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_SPIKE_PCT) {
        return 0;
    }
    if (isfinite(cond_estimate) && cond_estimate > PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_COND) {
        return 0;
    }
    if (isfinite(growth_factor) && growth_factor > PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_GROWTH) {
        return 0;
    }
    return 1;
}

int lp_refactor_policy_phase2_cooldown_window_updates(int interval) {
    int cooldown = interval + interval / 2;
    if (cooldown < PHASE2_POLICY_COOLDOWN_MIN_UPDATES) {
        cooldown = PHASE2_POLICY_COOLDOWN_MIN_UPDATES;
    }
    if (cooldown > PHASE2_POLICY_COOLDOWN_MAX_UPDATES) {
        cooldown = PHASE2_POLICY_COOLDOWN_MAX_UPDATES;
    }
    return cooldown;
}

int lp_refactor_policy_phase1_cooldown_eligible(int m,
                                                int degenerate_count,
                                                int periodic_policy_refactor_count,
                                                int spike_pool_used,
                                                int spike_pool_capacity,
                                                double cond_estimate,
                                                double growth_factor) {
    (void)spike_pool_used;
    (void)spike_pool_capacity;
    (void)cond_estimate;
    (void)growth_factor;
    if (m < PHASE1_POLICY_COOLDOWN_MIN_M) return 0;
    if (degenerate_count >= PHASE1_POLICY_COOLDOWN_DEGEN_TRIGGER) return 1;
    if (periodic_policy_refactor_count >= PHASE1_POLICY_COOLDOWN_POLICY_TRIGGER) return 1;
    return 0;
}

int lp_refactor_policy_phase1_cooldown_window_updates(int interval) {
    int cooldown = (interval * PHASE1_POLICY_COOLDOWN_NUM) / PHASE1_POLICY_COOLDOWN_DEN;
    if (cooldown < PHASE1_POLICY_COOLDOWN_MIN_UPDATES) {
        cooldown = PHASE1_POLICY_COOLDOWN_MIN_UPDATES;
    }
    if (cooldown > PHASE1_POLICY_COOLDOWN_MAX_UPDATES) {
        cooldown = PHASE1_POLICY_COOLDOWN_MAX_UPDATES;
    }
    return cooldown;
}

int lp_refactor_policy_phase1_dir_stabilize_cooldown_updates(int m,
                                                             int degenerate_count,
                                                             int repeat_streak) {
    int cooldown = PHASE1_DIR_STABILIZE_BASE_COOLDOWN_UPDATES;
    int mult = 1;

    (void)degenerate_count;

    if (repeat_streak < 2) return cooldown;
    if (m < PHASE1_DIR_STABILIZE_ADAPT_MIN_M) return cooldown;

    mult += (repeat_streak - 1) / PHASE1_DIR_STABILIZE_STREAK_STEP;
    if (mult > PHASE1_DIR_STABILIZE_MAX_MULT) {
        mult = PHASE1_DIR_STABILIZE_MAX_MULT;
    }
    cooldown *= mult;
    if (cooldown > PHASE1_DIR_STABILIZE_MAX_COOLDOWN_UPDATES) {
        cooldown = PHASE1_DIR_STABILIZE_MAX_COOLDOWN_UPDATES;
    }
    return cooldown;
}

int lp_refactor_policy_phase1_dir_stabilize_force_extreme_ratio(
    double dir_inf_ratio,
    int cooldown_active) {
    double threshold = PHASE1_DIR_STABILIZE_FORCE_RATIO_BASE;

    if (cooldown_active) {
        threshold = PHASE1_DIR_STABILIZE_FORCE_RATIO_COOLDOWN;
    }
    return (dir_inf_ratio > threshold) ? 1 : 0;
}

LPLUHealthRefactorDecision lp_refactor_policy_lu_health_refactor_decision(
    int m,
    int use_ft_updates,
    int num_updates,
    int max_updates,
    int spike_pool_used,
    int spike_pool_capacity,
    double cond_estimate,
    double growth_factor,
    int soft_breach_streak) {
    LPLUHealthRefactorDecision decision = {0, 0, 0, 0, 0, 0};
    int adaptive_limit;
    double cond_ratio = 0.0;

    (void)m;

    if (max_updates <= 0) max_updates = 1;
    if (num_updates < 0) num_updates = 0;
    if (spike_pool_used < 0) spike_pool_used = 0;
    if (soft_breach_streak < 0) soft_breach_streak = 0;

    decision.soft_breach_threshold = LU_HEALTH_SOFT_BREACH_THRESHOLD_BASE;
    decision.soft_min_update_age = lu_soft_min_update_age(max_updates);

    if (num_updates >= max_updates) {
        decision.hard_trigger = 1;
        decision.refactor_now = 1;
        return decision;
    }

    if (isfinite(growth_factor) && growth_factor > RALPH_LU_GROWTH_REFACTOR_THRESHOLD) {
        decision.hard_trigger = 1;
        decision.refactor_now = 1;
        return decision;
    }

    if (num_updates >= LU_HEALTH_HARD_COND_MIN_UPDATES &&
        isfinite(growth_factor) &&
        isfinite(cond_estimate) &&
        growth_factor > 0.0 &&
        cond_estimate > 0.0) {
        cond_ratio = growth_factor * cond_estimate;
        if (cond_ratio > LU_HEALTH_HARD_COND_RATIO) {
            decision.hard_trigger = 1;
            decision.refactor_now = 1;
            return decision;
        }
    }

    adaptive_limit = max_updates;
    if (isfinite(cond_estimate)) {
        if (cond_estimate > LU_HEALTH_SOFT_COND_HIGH) {
            adaptive_limit = max_updates / 4;
            decision.soft_breach_threshold = LU_HEALTH_SOFT_BREACH_THRESHOLD_HIGH;
        } else if (cond_estimate > LU_HEALTH_SOFT_COND_MED) {
            adaptive_limit = max_updates / 2;
        }
    }
    if (adaptive_limit < 1) adaptive_limit = 1;
    if (adaptive_limit < max_updates && num_updates >= adaptive_limit) {
        decision.soft_trigger = 1;
    }

    if (use_ft_updates && spike_pool_capacity > 0) {
        if (spike_pool_used >
            (spike_pool_capacity * LU_HEALTH_SOFT_SPIKE_WARN_PCT) / 100) {
            decision.soft_trigger = 1;
            decision.soft_breach_threshold = LU_HEALTH_SOFT_BREACH_THRESHOLD_HIGH;
        }
    }

    if (use_ft_updates && m >= 500 &&
        spike_pool_used > m * LU_HEALTH_SOFT_SPIKE_WORK_MULT) {
        decision.soft_trigger = 1;
    }

    if (decision.soft_trigger) {
        decision.soft_breach_streak_next = soft_breach_streak + 1;
        if (decision.soft_breach_streak_next < 0) {
            decision.soft_breach_streak_next = 0;
        }
    } else {
        decision.soft_breach_streak_next = 0;
    }

    if (decision.soft_trigger &&
        decision.soft_breach_streak_next >= decision.soft_breach_threshold &&
        num_updates >= decision.soft_min_update_age) {
        decision.refactor_now = 1;
    }

    return decision;
}

int lp_refactor_policy_soft_lu_cost_gate_should_defer(int phase,
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
                                                      double iter_cost_ewma_ms) {
    int min_m;
    int update_reserve;
    double ratio;

    if (phase == 1) {
        min_m = LU_SOFT_COST_GATE_PHASE1_MIN_M;
    } else if (phase == 2) {
        min_m = LU_SOFT_COST_GATE_PHASE2_MIN_M;
    } else {
        return 0;
    }

    if (m < min_m) return 0;
    if (use_bland) return 0;
    if (degenerate_count < LU_SOFT_COST_GATE_MIN_DEGEN) return 0;
    if (max_updates <= 0 || num_updates < 0) return 0;
    if (!isfinite(refactor_cost_ewma_ms) || !isfinite(iter_cost_ewma_ms)) return 0;
    if (refactor_cost_ewma_ms < LU_SOFT_COST_GATE_MIN_REFACTOR_MS) return 0;
    if (!(iter_cost_ewma_ms > 0.0)) return 0;

    ratio = refactor_cost_ewma_ms / iter_cost_ewma_ms;
    if (ratio < LU_SOFT_COST_GATE_RATIO_TRIGGER) return 0;

    update_reserve = (max_updates * LU_SOFT_COST_GATE_UPDATE_RESERVE_NUM) /
                     LU_SOFT_COST_GATE_UPDATE_RESERVE_DEN;
    if (update_reserve < LU_SOFT_COST_GATE_UPDATE_RESERVE_MIN) {
        update_reserve = LU_SOFT_COST_GATE_UPDATE_RESERVE_MIN;
    }
    if (num_updates >= max_updates - update_reserve) return 0;

    if (spike_pool_capacity > 0 &&
        spike_pool_used * 100 > spike_pool_capacity * LU_SOFT_COST_GATE_MAX_SPIKE_PCT) {
        return 0;
    }
    if (isfinite(cond_estimate) && cond_estimate > LU_SOFT_COST_GATE_MAX_COND) return 0;
    if (isfinite(growth_factor) && growth_factor > LU_SOFT_COST_GATE_MAX_GROWTH) return 0;

    return 1;
}

LPPeriodicCostDampenReason lp_refactor_policy_periodic_cost_dampen_decision(
    int phase,
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
    int iter_cost_samples) {
    int min_m;
    int update_reserve;
    double ratio;

    (void)use_bland;
    (void)degenerate_count;
    (void)spike_pool_used;
    (void)spike_pool_capacity;
    (void)cond_estimate;
    (void)growth_factor;

    if (phase == 1) {
        min_m = PERIODIC_COST_DAMPEN_PHASE1_MIN_M;
    } else if (phase == 2) {
        min_m = PERIODIC_COST_DAMPEN_PHASE2_MIN_M;
    } else {
        return LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE;
    }

    if (m < min_m) return LP_PERIODIC_COST_DAMPEN_BLOCK_SMALL_M;
    if (max_updates <= 0 || num_updates <= 0) return LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_INPUTS;
    if (iter_cost_samples < PERIODIC_COST_DAMPEN_MIN_ITER_SAMPLES ||
        refactor_cost_samples < PERIODIC_COST_DAMPEN_MIN_REFACTOR_SAMPLES) {
        return LP_PERIODIC_COST_DAMPEN_BLOCK_WARMUP;
    }
    if (!isfinite(refactor_cost_ewma_ms) || !isfinite(iter_cost_ewma_ms)) {
        return LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_COST;
    }
    if (refactor_cost_ewma_ms < PERIODIC_COST_DAMPEN_MIN_REFACTOR_MS) {
        return LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_COST;
    }
    if (!(iter_cost_ewma_ms > 0.0)) return LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_COST;

    ratio = refactor_cost_ewma_ms / iter_cost_ewma_ms;
    if (ratio < PERIODIC_COST_DAMPEN_RATIO_TRIGGER) return LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO;

    update_reserve = (max_updates * PERIODIC_COST_DAMPEN_UPDATE_RESERVE_NUM) /
                     PERIODIC_COST_DAMPEN_UPDATE_RESERVE_DEN;
    if (update_reserve < PERIODIC_COST_DAMPEN_UPDATE_RESERVE_MIN) {
        update_reserve = PERIODIC_COST_DAMPEN_UPDATE_RESERVE_MIN;
    }
    if (num_updates >= max_updates - update_reserve) {
        return LP_PERIODIC_COST_DAMPEN_BLOCK_UPDATE_RESERVE;
    }

    return LP_PERIODIC_COST_DAMPEN_DEFER;
}

int lp_refactor_policy_periodic_cost_dampen_should_defer(int phase,
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
                                                         int iter_cost_samples) {
    LPPeriodicCostDampenReason decision =
        lp_refactor_policy_periodic_cost_dampen_decision(phase,
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
    return decision == LP_PERIODIC_COST_DAMPEN_DEFER;
}

const char* lp_refactor_policy_periodic_cost_dampen_reason_string(
    LPPeriodicCostDampenReason reason) {
    switch (reason) {
        case LP_PERIODIC_COST_DAMPEN_DEFER: return "defer";
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE: return "invalid_phase";
        case LP_PERIODIC_COST_DAMPEN_BLOCK_SMALL_M: return "small_matrix";
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_INPUTS: return "invalid_inputs";
        case LP_PERIODIC_COST_DAMPEN_BLOCK_WARMUP: return "warmup";
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_COST: return "invalid_cost";
        case LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO: return "ratio_below_threshold";
        case LP_PERIODIC_COST_DAMPEN_BLOCK_UPDATE_RESERVE: return "update_reserve";
        default: return "unknown";
    }
}
