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

static double clamp_unit_interval(double x) {
    if (!(x > 0.0)) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
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
