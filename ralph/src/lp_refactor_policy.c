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
#define PERIODIC_FEEDBACK_BIAS_LIMIT 0.25
#define PERIODIC_FEEDBACK_DECAY 0.85
#define PERIODIC_FEEDBACK_RELAX_STEP 0.06
#define PERIODIC_FEEDBACK_TIGHTEN_STEP 0.08
#define PERIODIC_FEEDBACK_LOW_PRESSURE 0.55
#define PERIODIC_FEEDBACK_HIGH_PRESSURE 0.85
#define PERIODIC_FEEDBACK_EARLY_RECOVERY_NUM 2
#define PERIODIC_FEEDBACK_EARLY_RECOVERY_DEN 3
#define PHASE1_POLICY_PRESSURE_DECAY_STEP 0.06
#define PHASE1_POLICY_PRESSURE_DECAY_MAX 0.24
#define PHASE1_POLICY_PRESSURE_RECOVERY_STEP 0.01
#define PHASE2_POLICY_PRESSURE_DECAY_STEP 0.06
#define PHASE2_POLICY_PRESSURE_DECAY_MAX 0.24
#define PHASE2_POLICY_PRESSURE_RECOVERY_STEP 0.01
#define PHASE2_POLICY_COOLDOWN_MIN_M 450
#define PHASE2_POLICY_COOLDOWN_DEGEN_TRIGGER 40
#define PHASE2_POLICY_COOLDOWN_MIN_UPDATES 16
#define PHASE2_POLICY_COOLDOWN_MAX_UPDATES 96
#define PHASE2_PERIODIC_RECOMPUTE_BASE_INTERVAL 200
#define PHASE2_PERIODIC_RECOMPUTE_MAX_INTERVAL 600
#define PHASE2_PERIODIC_RECOMPUTE_MIN_M 1200
#define PHASE2_PERIODIC_RECOMPUTE_FULL_M 2600
#define PHASE2_PERIODIC_RECOMPUTE_MIN_DEGEN 80
#define PHASE2_PERIODIC_RECOMPUTE_FULL_DEGEN 320
#define PHASE2_PERIODIC_RECOMPUTE_MAX_SPIKE_PCT 30
#define PHASE2_PERIODIC_RECOMPUTE_MAX_COND 1e6
#define PHASE2_PERIODIC_RECOMPUTE_MAX_GROWTH 1e4
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
#define PHASE1_DIR_STABILIZE_MODERATE_RATIO_MAX 30.0
#define PHASE1_DIR_STABILIZE_SAFE_ACCEPT_RATIO_MAX PHASE1_DIR_STABILIZE_FORCE_RATIO_BASE
#define PHASE1_DIR_STABILIZE_SAFE_PIVOT_RATIO_MIN 1e-4
#define PHASE1_DIR_SKIP_RC_ONLY_MIN_M 500
#define PHASE1_DIR_SKIP_RC_ONLY_MIN_N 2000
#define PHASE1_DIR_SKIP_RC_ONLY_MIN_DEGEN 20
#define PHASE1_DIR_SKIP_RC_ONLY_MIN_NO_PIVOT_STREAK 8
#define PHASE1_DIR_SKIP_NO_RECOMPUTE_GUARD 8
#define PHASE1_FORCE_SMALL_PIVOT_MIN_UPDATE_AGE 6
#define PHASE1_NO_PIVOT_FORCE_MIN_M 700
#define PHASE1_NO_PIVOT_FORCE_BASE_TRIGGER 48
#define PHASE1_NO_PIVOT_FORCE_MIN_TRIGGER 24
#define PHASE1_NO_PIVOT_FORCE_COOLDOWN_UPDATES 24
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_RATIO 8
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_DIR_SKIP 8
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_PIVOT_FAIL 4
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN 3
#define PHASE1_NO_PIVOT_LADDER_RESCUE_START 3
#define PHASE1_NO_PIVOT_LADDER_RESCUE_PERIOD 4
#define PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_START 8
#define PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_PERIOD 4
#define PHASE1_DIR_SKIP_LADDER_RESCUE_START 16
#define PHASE1_DIR_SKIP_LADDER_RESCUE_PERIOD 8
#define PHASE1_DEGEN_THRESHOLD_LARGE_M 700
#define PHASE1_DEGEN_THRESHOLD_DEFAULT 50
#define PHASE1_DEGEN_THRESHOLD_LARGE 20
#define PHASE1_STALL_THRESHOLD_DEFAULT 50
#define PHASE1_STALL_THRESHOLD_LARGE 30
#define PHASE1_RECOMPUTE_INTERVAL 25
#define PHASE1_STALL_OBJ_REL_TOL 1e-4
#define PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_THRESHOLD 3
#define PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_DIVISOR 3
#define PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_TRIGGER 64
#define PHASE1_DIR_SKIP_FORCE_PIVOT_MIN_TRIGGER 24
#define PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_BUDGET 12
#define PHASE1_DIR_SKIP_FORCE_PIVOT_MAX_BUDGET 32
#define PHASE1_WINDOW_FORCE_PIVOT_BASE_TRIGGER 512
#define PHASE1_WINDOW_FORCE_PIVOT_MIN_TRIGGER 384
#define PHASE1_WINDOW_FORCE_PIVOT_LOCAL_FAIL_TRIGGER 64
#define PHASE1_DIR_ESCAPE_MIN_M 200
#define PHASE1_DIR_ESCAPE_BASE_TRIGGER 48
#define PHASE1_DIR_ESCAPE_MIN_TRIGGER 20
#define PHASE1_DIR_ESCAPE_BASE_NO_PROGRESS_TRIGGER 10
#define PHASE1_DIR_ESCAPE_MIN_NO_PROGRESS_TRIGGER 4
#define PHASE1_DIR_ESCAPE_BASE_COOLDOWN_UPDATES 48
#define PHASE1_DIR_ESCAPE_MAX_COOLDOWN_UPDATES 160
#define PHASE1_FORCE_PIVOT_RELAX_MIN_M 700
#define PHASE1_FORCE_PIVOT_RELAX_DEGEN_TRIGGER 80
#define PHASE1_FORCE_PIVOT_RELAX_MAX_NO_PROGRESS 10
#define PHASE1_FORCE_PIVOT_RELAX_RESCUE_MIN_ATTEMPTS 8
#define PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_NUM 4
#define PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_DEN 5
#define PHASE1_FORCE_EXTREME_RELAX_MIN_M 700
#define PHASE1_FORCE_EXTREME_RELAX_DEGEN_TRIGGER 80
#define PHASE1_FORCE_EXTREME_RELAX_MAX_NO_PROGRESS 10
#define PHASE1_FORCE_EXTREME_RELAX_MAX_RATIO 300.0
#define PHASE1_FORCE_EXTREME_BOUND_FLIP_RELAX_MIN_M 700
#define PHASE1_FORCE_EXTREME_BOUND_FLIP_RELAX_MIN_STREAK 16
#define PHASE1_FORCE_EXTREME_CATA_TINY_THETA_RELAX_MIN_M 700
#define PHASE1_FORCE_EXTREME_CATA_TINY_THETA_RELAX_MIN_STREAK 16
#define PHASE1_FORCE_EXTREME_CATA_TINY_THETA_RELAX_MAX_PIVOT_RATIO 1e-5
#define PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_MIN_M 200
#define PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_MIN_STREAK 32
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_M 700
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_DEGEN_TRIGGER 20
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_UPDATES 12
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_MAX_UPDATES 48
#define PHASE1_REINVERT_PRESSURE_WINDOW_ITERS 96
#define PHASE1_REINVERT_PRESSURE_DEMOTE_COUNT 4
#define PHASE1_REINVERT_DEMOTE_COOLDOWN_ITERS 32
#define PHASE1_REINVERT_PRESSURE_NO_PIVOT_THRESHOLD 8
#define PHASE1_REINVERT_PRESSURE_NO_PROGRESS_THRESHOLD 24
#define PHASE1_REINVERT_PRESSURE_RATIO_BREAKDOWN_THRESHOLD 4
#define PHASE1_REINVERT_PRESSURE_DIR_SKIP_THRESHOLD 24
#define PHASE1_STAGNATION_WINDOW_ITERS 96
#define PHASE1_STAGNATION_OBJ_REL_TOL 1e-5
#define PHASE1_STAGNATION_OBJ_ABS_TOL 1e-8
#define PHASE1_STAGNATION_MIN_NO_PIVOT_EVENTS 10
#define PHASE1_STAGNATION_MIN_RETRY_DEFERS 12
#define PHASE1_STAGNATION_RETRY_RATIO_NUM 3
#define PHASE1_STAGNATION_RETRY_RATIO_DEN 5
#define PHASE1_STAGNATION_MIN_REFACTORS 6
#define PHASE1_STAGNATION_MIN_UPDATE_RECOVERY 3
#define PHASE1_STAGNATION_UPDATE_RATIO_NUM 1
#define PHASE1_STAGNATION_UPDATE_RATIO_DEN 3
#define PHASE1_STAGNATION_MIN_RECOMPUTES 12
#define PHASE1_STAGNATION_RECOMPUTE_HOT_NUM 3
#define PHASE1_STAGNATION_RECOMPUTE_HOT_DEN 4
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
#define LU_SOFT_COST_GATE_PHASE2_LARGE_MIN_DEGEN 0
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
#define DENSE_SPIKE_WARMUP_PHASE2_MEDIUM_MIN_M 300
#define DENSE_SPIKE_WARMUP_PHASE2_MEDIUM_MAX_M 500
#define DENSE_SPIKE_WARMUP_PHASE2_MEDIUM_MIN_UPDATES 16

void lp_refactor_policy_config_defaults(LPRefactorPolicyConfig *cfg) {
    if (!cfg) return;
    cfg->phase1_refactor_min_interval     = PHASE1_PERIODIC_REFACTOR_MIN_INTERVAL;   /* 24 */
    cfg->phase1_refactor_max_interval     = PHASE1_PERIODIC_REFACTOR_MAX_INTERVAL;   /* 96 */
    cfg->phase2_refactor_min_interval     = PHASE2_PERIODIC_REFACTOR_MIN_INTERVAL;   /* 10 */
    cfg->phase2_refactor_max_interval     = PHASE2_PERIODIC_REFACTOR_MAX_INTERVAL;   /* 80 */
    cfg->refactor_pressure_trigger        = PERIODIC_REFACTOR_PRESSURE_TRIGGER;      /* 0.40 */
    cfg->periodic_min_update_age          = PERIODIC_REFACTOR_MIN_UPDATE_AGE;        /* 8  */
    cfg->degen_escape_min_m               = 1200;  /* PHASE2_DEGEN_ESCAPE_MIN_M (simplex.c) */
    cfg->degen_escape_trigger             = 120;   /* PHASE2_DEGEN_ESCAPE_DEGEN_TRIGGER (simplex.c) */
    cfg->phase1_auto_dantzig_min_m        = 700;   /* PHASE1_AUTO_DANTZIG_MIN_M (simplex.c) */
    cfg->lu_cost_ewma_alpha               = 0.20;  /* SOFT_LU_COST_EWMA_ALPHA (simplex.c) */
    cfg->lu_max_consec_defer_phase1       = 6;     /* SOFT_LU_MAX_CONSEC_DEFER_PHASE1 (simplex.c) */
    cfg->lu_max_consec_defer_phase2       = 4;     /* SOFT_LU_MAX_CONSEC_DEFER_PHASE2 (simplex.c) */
    cfg->lu_cost_gate_ratio               = LU_SOFT_COST_GATE_RATIO_TRIGGER;        /* 8.0 */
    cfg->lu_spike_warn_pct                = LU_HEALTH_SOFT_SPIKE_WARN_PCT;           /* 85 */
    cfg->no_pivot_progress_window         = 6;     /* PHASE1_NO_PIVOT_PROGRESS_WINDOW (simplex.c) */
    cfg->phase1_stall_threshold_default   = PHASE1_STALL_THRESHOLD_DEFAULT;          /* 50 */
    cfg->phase2_degen_escape_policy_trigger = 200;  /* PHASE2_DEGEN_ESCAPE_POLICY_TRIGGER (simplex.c) */
    cfg->feedback_decay                   = PERIODIC_FEEDBACK_DECAY;                 /* 0.85 */
    cfg->feedback_relax_step              = PERIODIC_FEEDBACK_RELAX_STEP;            /* 0.06 */
    cfg->feedback_tighten_step            = PERIODIC_FEEDBACK_TIGHTEN_STEP;          /* 0.08 */
}

static double clamp_unit_interval(double x) {
    if (!(x > 0.0)) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

static double clamp_feedback_bias(double x) {
    if (!isfinite(x)) return 0.0;
    if (x > PERIODIC_FEEDBACK_BIAS_LIMIT) return PERIODIC_FEEDBACK_BIAS_LIMIT;
    if (x < -PERIODIC_FEEDBACK_BIAS_LIMIT) return -PERIODIC_FEEDBACK_BIAS_LIMIT;
    return x;
}

static int clamp_int_range(int x, int lo, int hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static double clamp_nonnegative(double x) {
    if (!isfinite(x) || x <= 0.0) return 0.0;
    return x;
}

static int periodic_pressure_params(int phase,
                                    double *decay_step,
                                    double *decay_max,
                                    double *recovery_step) {
    if (!decay_step || !decay_max || !recovery_step) return 0;
    if (phase == 1) {
        *decay_step = PHASE1_POLICY_PRESSURE_DECAY_STEP;
        *decay_max = PHASE1_POLICY_PRESSURE_DECAY_MAX;
        *recovery_step = PHASE1_POLICY_PRESSURE_RECOVERY_STEP;
        return 1;
    }
    if (phase == 2) {
        *decay_step = PHASE2_POLICY_PRESSURE_DECAY_STEP;
        *decay_max = PHASE2_POLICY_PRESSURE_DECAY_MAX;
        *recovery_step = PHASE2_POLICY_PRESSURE_RECOVERY_STEP;
        return 1;
    }
    return 0;
}

static int lu_soft_min_update_age(int max_updates) {
    int min_age = (max_updates * LU_HEALTH_SOFT_MIN_UPDATE_AGE_NUM) /
                  LU_HEALTH_SOFT_MIN_UPDATE_AGE_DEN;
    return clamp_int_range(min_age,
                           LU_HEALTH_SOFT_MIN_UPDATE_AGE_MIN,
                           LU_HEALTH_SOFT_MIN_UPDATE_AGE_MAX);
}

static int lu_soft_cost_gate_min_degen(int phase, int m) {
    if (phase == 2 && m >= LU_SOFT_COST_GATE_PHASE2_MIN_M) {
        return LU_SOFT_COST_GATE_PHASE2_LARGE_MIN_DEGEN;
    }
    return LU_SOFT_COST_GATE_MIN_DEGEN;
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

LPPeriodicRefactorPlan lp_refactor_policy_periodic_plan(
    int phase,
    int iter,
    int m,
    int max_updates,
    int num_updates,
    int spike_pool_used,
    int spike_pool_capacity,
    double cond_estimate,
    double growth_factor,
    int use_bland,
    int degenerate_count,
    double feedback_bias,
    int phase1_periodic_policy_refactor_count,
    int periodic_policy_cooldown,
    double periodic_policy_pressure_decay) {
    LPPeriodicRefactorPlan plan = {{0, 0, 0.0, 0.0}, 0, 0.0, 0};
    LPPeriodicRefactorPolicy effective_policy;

    plan.policy = lp_refactor_policy_build_from_metrics(phase,
                                                        m,
                                                        max_updates,
                                                        num_updates,
                                                        spike_pool_used,
                                                        spike_pool_capacity,
                                                        cond_estimate,
                                                        growth_factor,
                                                        use_bland,
                                                        degenerate_count,
                                                        feedback_bias);

    if (phase == 1) {
        plan.cooldown_eligible = lp_refactor_policy_phase1_cooldown_eligible(
            m,
            degenerate_count,
            phase1_periodic_policy_refactor_count,
            spike_pool_used,
            spike_pool_capacity,
            cond_estimate,
            growth_factor);
    } else if (phase == 2) {
        plan.cooldown_eligible = lp_refactor_policy_phase2_cooldown_eligible(
            m,
            degenerate_count,
            spike_pool_used,
            spike_pool_capacity,
            cond_estimate,
            growth_factor);
    } else {
        return plan;
    }

    effective_policy = plan.policy;
    effective_policy.run_pressure = lp_refactor_policy_periodic_pressure_effective(
        plan.cooldown_eligible,
        effective_policy.run_pressure,
        periodic_policy_pressure_decay);
    plan.effective_run_pressure = effective_policy.run_pressure;

    plan.should_run = lp_refactor_policy_should_run_metrics(iter,
                                                            num_updates,
                                                            &effective_policy,
                                                            use_bland,
                                                            degenerate_count);
    if (plan.should_run &&
        plan.cooldown_eligible &&
        periodic_policy_cooldown > 0) {
        plan.should_run = 0;
    }

    return plan;
}

double lp_refactor_policy_periodic_pressure_effective(
    int cooldown_eligible,
    double run_pressure,
    double pressure_decay) {
    double effective = clamp_unit_interval(run_pressure);
    if (!cooldown_eligible) return effective;
    return clamp_unit_interval(effective - clamp_unit_interval(pressure_decay));
}

double lp_refactor_policy_periodic_pressure_decay_recover(int phase,
                                                          double pressure_decay) {
    double decay_step = 0.0;
    double decay_max = 0.0;
    double recovery_step = 0.0;
    double next = clamp_nonnegative(pressure_decay);

    if (!periodic_pressure_params(phase, &decay_step, &decay_max, &recovery_step)) {
        return next;
    }
    if (next <= recovery_step) return 0.0;
    next -= recovery_step;
    if (next > decay_max) next = decay_max;
    return next;
}

double lp_refactor_policy_periodic_pressure_decay_penalty(int phase,
                                                          double pressure_decay) {
    double decay_step = 0.0;
    double decay_max = 0.0;
    double recovery_step = 0.0;
    double next = clamp_nonnegative(pressure_decay);

    if (!periodic_pressure_params(phase, &decay_step, &decay_max, &recovery_step)) {
        return next;
    }
    (void)recovery_step;
    next += decay_step;
    if (next > decay_max) next = decay_max;
    return next;
}

int lp_refactor_policy_periodic_cooldown_tick(int cooldown_updates) {
    if (cooldown_updates <= 0) return 0;
    return cooldown_updates - 1;
}

int lp_refactor_policy_periodic_cooldown_extend(int cooldown_updates,
                                                int candidate_updates) {
    if (cooldown_updates < 0) cooldown_updates = 0;
    if (candidate_updates < 0) candidate_updates = 0;
    return (candidate_updates > cooldown_updates)
        ? candidate_updates
        : cooldown_updates;
}

void lp_refactor_policy_periodic_post_refactor_update(int phase,
                                                      int cooldown_eligible,
                                                      int periodic_interval,
                                                      int refactor_status,
                                                      int *cooldown_updates_io,
                                                      double *pressure_decay_io) {
    int cooldown;
    if (!cooldown_updates_io || !pressure_decay_io) return;

    if (refactor_status != 0) {
        *cooldown_updates_io = 0;
        *pressure_decay_io = 0.0;
        return;
    }

    if (!cooldown_eligible) return;

    if (phase == 1) {
        cooldown = lp_refactor_policy_phase1_cooldown_window_updates(periodic_interval);
    } else if (phase == 2) {
        cooldown = lp_refactor_policy_phase2_cooldown_window_updates(periodic_interval);
    } else {
        return;
    }

    *cooldown_updates_io =
        lp_refactor_policy_periodic_cooldown_extend(*cooldown_updates_io, cooldown);
    *pressure_decay_io =
        lp_refactor_policy_periodic_pressure_decay_penalty(
            phase, *pressure_decay_io);
}

void lp_refactor_policy_periodic_feedback_set_hint(LPPeriodicFeedbackState *state,
                                                   int interval,
                                                   double run_pressure) {
    if (!state) return;
    if (interval < 0) interval = 0;
    state->hint_interval = interval;
    state->hint_pressure = clamp_unit_interval(run_pressure);
}

void lp_refactor_policy_periodic_feedback_record_refactor(
    LPPeriodicFeedbackState *state,
    int reason,
    int updates_before,
    int status) {
    double bias;
    int hint_interval;
    double hint_pressure;

    if (!state) return;

    bias = state->bias * PERIODIC_FEEDBACK_DECAY;
    hint_interval = state->hint_interval;
    hint_pressure = state->hint_pressure;

    if (status != 0) {
        if (reason == RALPH_REFACTOR_REASON_PERIODIC) {
            bias += PERIODIC_FEEDBACK_TIGHTEN_STEP;
        }
        state->bias = clamp_feedback_bias(bias);
        state->last_reason = reason;
        if (reason == RALPH_REFACTOR_REASON_PERIODIC && hint_interval > 0) {
            state->last_interval = hint_interval;
        }
        state->hint_interval = 0;
        state->hint_pressure = 0.0;
        return;
    }

    if (reason == RALPH_REFACTOR_REASON_PERIODIC) {
        double update_ratio = 1.0;
        if (hint_interval > 0 && updates_before > 0) {
            update_ratio = (double)updates_before / (double)hint_interval;
        }

        if (hint_pressure < PERIODIC_FEEDBACK_LOW_PRESSURE &&
            update_ratio <= 1.05) {
            bias -= PERIODIC_FEEDBACK_RELAX_STEP;
        } else if (hint_pressure >= PERIODIC_FEEDBACK_HIGH_PRESSURE) {
            bias += PERIODIC_FEEDBACK_TIGHTEN_STEP;
        }

        if (hint_interval > 0) {
            state->last_interval = hint_interval;
        } else if (updates_before > 0) {
            state->last_interval = updates_before;
        }
    } else if (state->last_reason == RALPH_REFACTOR_REASON_PERIODIC &&
               state->last_interval > 0 &&
               updates_before > 0) {
        int early_threshold =
            (state->last_interval * PERIODIC_FEEDBACK_EARLY_RECOVERY_NUM) /
            PERIODIC_FEEDBACK_EARLY_RECOVERY_DEN;
        if (early_threshold < PERIODIC_REFACTOR_MIN_UPDATE_AGE) {
            early_threshold = PERIODIC_REFACTOR_MIN_UPDATE_AGE;
        }

        if ((reason == RALPH_REFACTOR_REASON_UPDATE_RECOVERY ||
             reason == RALPH_REFACTOR_REASON_RATIO_RECOVERY ||
             reason == RALPH_REFACTOR_REASON_PIVOT_RECOVERY ||
             reason == RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) &&
            updates_before <= early_threshold) {
            bias += PERIODIC_FEEDBACK_TIGHTEN_STEP;
        }
    }

    state->bias = clamp_feedback_bias(bias);
    state->last_reason = reason;
    state->hint_interval = 0;
    state->hint_pressure = 0.0;
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

int lp_refactor_policy_phase2_periodic_recompute_interval(
    int m,
    int use_bland,
    int degenerate_count,
    int spike_pool_used,
    int spike_pool_capacity,
    double cond_estimate,
    double growth_factor) {
    int base_interval = PHASE2_PERIODIC_RECOMPUTE_BASE_INTERVAL;
    int max_interval = PHASE2_PERIODIC_RECOMPUTE_MAX_INTERVAL;
    int interval = base_interval;
    double m_pressure;
    double degen_pressure;
    double extend_pressure;

    if (m < PHASE2_PERIODIC_RECOMPUTE_MIN_M) return base_interval;
    if (use_bland) return base_interval;
    if (degenerate_count < PHASE2_PERIODIC_RECOMPUTE_MIN_DEGEN) return base_interval;
    if (spike_pool_capacity > 0 &&
        spike_pool_used * 100 >
            spike_pool_capacity * PHASE2_PERIODIC_RECOMPUTE_MAX_SPIKE_PCT) {
        return base_interval;
    }
    if (isfinite(cond_estimate) && cond_estimate > PHASE2_PERIODIC_RECOMPUTE_MAX_COND) {
        return base_interval;
    }
    if (isfinite(growth_factor) && growth_factor > PHASE2_PERIODIC_RECOMPUTE_MAX_GROWTH) {
        return base_interval;
    }

    m_pressure =
        clamp_unit_interval((double)(m - PHASE2_PERIODIC_RECOMPUTE_MIN_M) /
                            (double)(PHASE2_PERIODIC_RECOMPUTE_FULL_M -
                                     PHASE2_PERIODIC_RECOMPUTE_MIN_M));
    degen_pressure =
        clamp_unit_interval((double)(degenerate_count - PHASE2_PERIODIC_RECOMPUTE_MIN_DEGEN) /
                            (double)(PHASE2_PERIODIC_RECOMPUTE_FULL_DEGEN -
                                     PHASE2_PERIODIC_RECOMPUTE_MIN_DEGEN));
    extend_pressure = (m_pressure < degen_pressure) ? m_pressure : degen_pressure;

    interval = base_interval + (int)(extend_pressure * (double)(max_interval - base_interval) + 0.5);
    if (interval < base_interval) interval = base_interval;
    if (interval > max_interval) interval = max_interval;
    return interval;
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

int lp_refactor_policy_phase1_dir_skip_allow_rc_only(int m,
                                                     int n,
                                                     int degenerate_count,
                                                     int no_pivot_streak) {
    if (m < PHASE1_DIR_SKIP_RC_ONLY_MIN_M &&
        n < PHASE1_DIR_SKIP_RC_ONLY_MIN_N) {
        return 0;
    }
    if (degenerate_count >= PHASE1_DIR_SKIP_RC_ONLY_MIN_DEGEN) return 1;
    if (no_pivot_streak >= PHASE1_DIR_SKIP_RC_ONLY_MIN_NO_PIVOT_STREAK) return 1;
    return 0;
}

int lp_refactor_policy_phase1_dir_skip_should_skip_recompute(
    int m,
    int n,
    int degenerate_count,
    int no_pivot_streak,
    int no_recompute_streak) {
    if (!lp_refactor_policy_phase1_dir_skip_allow_rc_only(
            m, n, degenerate_count, no_pivot_streak)) {
        return 0;
    }
    if (no_recompute_streak < 0) no_recompute_streak = 0;
    if (no_recompute_streak >= PHASE1_DIR_SKIP_NO_RECOMPUTE_GUARD) return 0;
    return 1;
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

int lp_refactor_policy_phase1_dir_stabilize_should_defer_moderate(
    double dir_inf_ratio,
    int cooldown_active,
    int lu_health_triggered,
    int pending_repeat) {
    if (!(dir_inf_ratio > 0.0)) return 0;
    if (cooldown_active) return 0;
    if (lu_health_triggered) return 0;
    if (dir_inf_ratio > PHASE1_DIR_STABILIZE_MODERATE_RATIO_MAX) return 0;
    if (pending_repeat) return 0;
    return 1;
}

int lp_refactor_policy_phase1_dir_stabilize_accept_scaled_pivot(
    double dir_inf_ratio,
    double pivot_ratio,
    int lu_health_triggered,
    int lu_hard_triggered,
    int force_extreme_triggered) {
    if (!(dir_inf_ratio > 0.0)) return 0;
    if (!(pivot_ratio > 0.0)) return 0;
    if (lu_health_triggered || lu_hard_triggered || force_extreme_triggered) return 0;
    if (dir_inf_ratio > PHASE1_DIR_STABILIZE_SAFE_ACCEPT_RATIO_MAX) return 0;
    return pivot_ratio > PHASE1_DIR_STABILIZE_SAFE_PIVOT_RATIO_MIN;
}

int lp_refactor_policy_phase1_small_pivot_refactor_allowed(int force_refactor,
                                                           int repeat_pattern,
                                                           int lu_num_updates) {
    if (!force_refactor) return 0;
    if (lu_num_updates >= 0 &&
        lu_num_updates < PHASE1_FORCE_SMALL_PIVOT_MIN_UPDATE_AGE &&
        repeat_pattern < RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER) {
        return 0;
    }
    return 1;
}

LPBasisAction lp_refactor_policy_choose_basis_action(double pivot,
                                                     int force_refactor,
                                                     int lu_update_status,
                                                     int repeat_pattern,
                                                     int lu_num_updates,
                                                     double growth_factor,
                                                     double growth_threshold) {
    const int force_refactor_allowed =
        lp_refactor_policy_phase1_small_pivot_refactor_allowed(force_refactor,
                                                               repeat_pattern,
                                                               lu_num_updates);

    if (!isfinite(pivot) || fabs(pivot) < RALPH_PIVOT_TOL) {
        return LP_BASIS_ACTION_ABORT;
    }
    if (lu_update_status <= -3) {
        return LP_BASIS_ACTION_ABORT;
    }
    if (lu_update_status == -2) {
        return LP_BASIS_ACTION_REPAIR;
    }
    if (lu_update_status == -1) {
        return LP_BASIS_ACTION_REFACTOR;
    }
    if (force_refactor_allowed ||
        repeat_pattern >= RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER ||
        growth_factor > growth_threshold) {
        return LP_BASIS_ACTION_REFACTOR;
    }
    return LP_BASIS_ACTION_UPDATE;
}

const char* lp_refactor_policy_phase1_no_pivot_force_reason_string(int reason) {
    switch ((LPPhase1NoPivotForceReason)reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
            return "ratio_breakdown";
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            return "dir_skip";
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            return "pivot_fail";
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            return "unknown";
    }
}

int lp_refactor_policy_phase1_no_pivot_force_threshold(int m,
                                                       int degenerate_count) {
    int threshold = PHASE1_NO_PIVOT_FORCE_BASE_TRIGGER;
    if (m >= 1200) threshold -= 8;
    if (degenerate_count >= 80) threshold -= 8;
    if (degenerate_count >= 160) threshold -= 8;
    if (threshold < PHASE1_NO_PIVOT_FORCE_MIN_TRIGGER) {
        threshold = PHASE1_NO_PIVOT_FORCE_MIN_TRIGGER;
    }
    return threshold;
}

int lp_refactor_policy_phase1_no_pivot_force_transition(int m,
                                                        int degenerate_count,
                                                        int streak,
                                                        int cooldown,
                                                        int *next_streak,
                                                        int *next_cooldown) {
    int threshold = 0;

    if (streak < 0) streak = 0;
    if (cooldown < 0) cooldown = 0;

    if (cooldown > 0) {
        if (next_streak) *next_streak = streak;
        if (next_cooldown) *next_cooldown = cooldown;
        return 0;
    }
    if (m < PHASE1_NO_PIVOT_FORCE_MIN_M) {
        if (next_streak) *next_streak = streak;
        if (next_cooldown) *next_cooldown = cooldown;
        return 0;
    }

    threshold = lp_refactor_policy_phase1_no_pivot_force_threshold(m, degenerate_count);
    if (streak < threshold) {
        if (next_streak) *next_streak = streak;
        if (next_cooldown) *next_cooldown = cooldown;
        return 0;
    }

    if (next_streak) *next_streak = 0;
    if (next_cooldown) *next_cooldown = PHASE1_NO_PIVOT_FORCE_COOLDOWN_UPDATES;
    return 1;
}

int lp_refactor_policy_phase1_no_pivot_ladder_refactor_threshold(
    int m,
    int degenerate_count,
    int reason,
    int force_pivot_mode_active) {
    int threshold;
    switch ((LPPhase1NoPivotForceReason)reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_DIR_SKIP;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_PIVOT_FAIL;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_RATIO;
            break;
    }

    if (m >= 2500) threshold += 4;
    else if (m >= 1200) threshold += 2;
    if (degenerate_count >= 160) threshold -= 2;
    else if (degenerate_count >= 80) threshold -= 1;
    if (force_pivot_mode_active && threshold > PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN) {
        threshold -= 2;
    }
    if ((LPPhase1NoPivotForceReason)reason == LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL &&
        threshold > 6) {
        threshold = 6;
    }
    if (threshold < PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN) {
        threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN;
    }
    return threshold;
}

int lp_refactor_policy_phase1_no_pivot_ladder_step(
    int m,
    int degenerate_count,
    int reason,
    int no_pivot_streak,
    int no_progress_streak,
    int force_pivot_mode_active,
    int *refactor_threshold_out) {
    int refactor_threshold = lp_refactor_policy_phase1_no_pivot_ladder_refactor_threshold(
        m, degenerate_count, reason, force_pivot_mode_active);
    int rescue_start = PHASE1_NO_PIVOT_LADDER_RESCUE_START;
    int rescue_period = PHASE1_NO_PIVOT_LADDER_RESCUE_PERIOD;
    int streak_rescue_start = PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_START;
    int streak_rescue_period = PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_PERIOD;
    int fast_escalation = 0;

    if (no_pivot_streak < 0) no_pivot_streak = 0;
    if (no_progress_streak < 0) no_progress_streak = 0;

    if (m >= 1200 && degenerate_count >= 80 &&
        (LPPhase1NoPivotForceReason)reason != LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL) {
        fast_escalation = 1;
        if (refactor_threshold > PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN + 1) {
            refactor_threshold -= 1;
        }
        rescue_start = 2;
        rescue_period = 3;
        streak_rescue_start = 6;
        streak_rescue_period = 3;
    }
    if ((LPPhase1NoPivotForceReason)reason == LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL) {
        rescue_start = 2;
        rescue_period = 2;
        streak_rescue_start = 4;
        streak_rescue_period = 2;
    }
    if (refactor_threshold_out) *refactor_threshold_out = refactor_threshold;

    if (no_progress_streak >= refactor_threshold ||
        no_pivot_streak >= refactor_threshold * 2 ||
        (fast_escalation &&
         no_progress_streak >= rescue_start + 2 &&
         no_pivot_streak >= refactor_threshold)) {
        return 2;
    }
    if (fast_escalation &&
        (LPPhase1NoPivotForceReason)reason ==
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN &&
        no_pivot_streak >= refactor_threshold) {
        return 2;
    }

    if (no_progress_streak >= rescue_start &&
        rescue_period > 0 &&
        (no_progress_streak % rescue_period) == 0) {
        return 1;
    }
    if (no_pivot_streak >= streak_rescue_start &&
        streak_rescue_period > 0 &&
        (no_pivot_streak % streak_rescue_period) == 0) {
        return 1;
    }

    return 0;
}

int lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(
    int dir_skip_event_streak) {
    if (dir_skip_event_streak < PHASE1_DIR_SKIP_LADDER_RESCUE_START) return 0;
    if (PHASE1_DIR_SKIP_LADDER_RESCUE_PERIOD <= 0) return 0;
    return (dir_skip_event_streak % PHASE1_DIR_SKIP_LADDER_RESCUE_PERIOD) == 0;
}

int lp_refactor_policy_phase1_dir_skip_force_pivot_threshold(
    int m,
    int degenerate_count) {
    int threshold = PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_TRIGGER;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) threshold -= 16;
    if (m >= 1200) threshold -= 8;
    if (degenerate_count >= 80) threshold -= 8;
    if (degenerate_count >= 160) threshold -= 8;
    if (threshold < PHASE1_DIR_SKIP_FORCE_PIVOT_MIN_TRIGGER) {
        threshold = PHASE1_DIR_SKIP_FORCE_PIVOT_MIN_TRIGGER;
    }
    return threshold;
}

int lp_refactor_policy_phase1_dir_skip_force_pivot_budget(
    int m,
    int degenerate_count) {
    int budget = PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_BUDGET;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) budget += 4;
    if (m >= 1200) budget += 4;
    if (degenerate_count >= 160) budget += 8;
    if (budget > PHASE1_DIR_SKIP_FORCE_PIVOT_MAX_BUDGET) {
        budget = PHASE1_DIR_SKIP_FORCE_PIVOT_MAX_BUDGET;
    }
    return budget;
}

int lp_refactor_policy_phase1_activate_force_pivot_mode(
    int m,
    int degenerate_count,
    int dir_skip_event_streak,
    int force_pivot_attempt_budget,
    int queue_force_pending,
    int *next_dir_skip_event_streak,
    int *next_force_pivot_attempt_budget,
    int *next_force_pending,
    int *next_force_reason) {
    int streak = dir_skip_event_streak;
    int budget = force_pivot_attempt_budget;
    int threshold;

    if (streak < 0) streak = 0;
    if (budget < 0) budget = 0;

    if (budget > 0) {
        if (next_dir_skip_event_streak) *next_dir_skip_event_streak = streak;
        if (next_force_pivot_attempt_budget) *next_force_pivot_attempt_budget = budget;
        return 0;
    }

    threshold = lp_refactor_policy_phase1_dir_skip_force_pivot_threshold(
        m, degenerate_count);
    if (streak < threshold) {
        if (next_dir_skip_event_streak) *next_dir_skip_event_streak = streak;
        if (next_force_pivot_attempt_budget) *next_force_pivot_attempt_budget = budget;
        return 0;
    }

    budget = lp_refactor_policy_phase1_dir_skip_force_pivot_budget(
        m, degenerate_count);
    streak = 0;
    if (next_dir_skip_event_streak) *next_dir_skip_event_streak = streak;
    if (next_force_pivot_attempt_budget) *next_force_pivot_attempt_budget = budget;
    if (next_force_pending) *next_force_pending = queue_force_pending ? 1 : 0;
    if (queue_force_pending && next_force_reason) {
        *next_force_reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
    }
    return 1;
}

int lp_refactor_policy_phase1_window_pressure_force_pivot_budget(
    int m,
    int degenerate_count,
    int window_events,
    int window_failed_stabilize,
    int window_dir_skip,
    int window_local_memory_fail,
    int window_alternations,
    int force_pivot_attempt_budget,
    int *reject_reason_out) {
    int trigger = PHASE1_WINDOW_FORCE_PIVOT_BASE_TRIGGER;

    if (reject_reason_out) {
        *reject_reason_out = LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_NONE;
    }
    if (force_pivot_attempt_budget > 0) return 0;
    if (window_events < 0) window_events = 0;
    if (window_failed_stabilize < 0) window_failed_stabilize = 0;
    if (window_dir_skip < 0) window_dir_skip = 0;
    if (window_local_memory_fail < 0) window_local_memory_fail = 0;
    if (window_alternations < 0) window_alternations = 0;

    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) trigger -= 64;
    trigger = clamp_int_range(trigger,
                              PHASE1_WINDOW_FORCE_PIVOT_MIN_TRIGGER,
                              PHASE1_WINDOW_FORCE_PIVOT_BASE_TRIGGER);
    if (window_events < trigger) {
        if (reject_reason_out) {
            *reject_reason_out = LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_UNDER_TRIGGER;
        }
        return 0;
    }
    if (window_failed_stabilize * 2 < window_events) {
        if (reject_reason_out) {
            *reject_reason_out = LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_FAILED_SHARE;
        }
        return 0;
    }
    if (window_dir_skip * 2 < window_events) {
        if (reject_reason_out) {
            *reject_reason_out = LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_DIR_SKIP_SHARE;
        }
        return 0;
    }
    if (window_local_memory_fail < PHASE1_WINDOW_FORCE_PIVOT_LOCAL_FAIL_TRIGGER) {
        if (reject_reason_out) {
            *reject_reason_out = LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_LOCAL_FAIL;
        }
        return 0;
    }
    if (window_alternations + 2 < window_events) {
        if (reject_reason_out) {
            *reject_reason_out = LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_ALTERNATION;
        }
        return 0;
    }

    return lp_refactor_policy_phase1_dir_skip_force_pivot_budget(
        m, degenerate_count);
}

static int phase1_dir_escape_trigger_streak(int m, int degenerate_count) {
    int trigger = PHASE1_DIR_ESCAPE_BASE_TRIGGER;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) trigger -= 8;
    if (degenerate_count >= 80) trigger -= 8;
    if (degenerate_count >= 160) trigger -= 8;
    if (trigger < PHASE1_DIR_ESCAPE_MIN_TRIGGER) {
        trigger = PHASE1_DIR_ESCAPE_MIN_TRIGGER;
    }
    return trigger;
}

static int phase1_dir_escape_no_progress_trigger(int m, int degenerate_count) {
    int trigger = PHASE1_DIR_ESCAPE_BASE_NO_PROGRESS_TRIGGER;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) trigger -= 2;
    if (degenerate_count >= 80) trigger -= 2;
    if (degenerate_count >= 160) trigger -= 2;
    if (trigger < PHASE1_DIR_ESCAPE_MIN_NO_PROGRESS_TRIGGER) {
        trigger = PHASE1_DIR_ESCAPE_MIN_NO_PROGRESS_TRIGGER;
    }
    return trigger;
}

static int phase1_dir_escape_cooldown_updates(int m, int degenerate_count) {
    int cooldown = PHASE1_DIR_ESCAPE_BASE_COOLDOWN_UPDATES;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) cooldown += 16;
    if (degenerate_count >= 80) cooldown += 16;
    if (degenerate_count >= 160) cooldown += 32;
    if (cooldown > PHASE1_DIR_ESCAPE_MAX_COOLDOWN_UPDATES) {
        cooldown = PHASE1_DIR_ESCAPE_MAX_COOLDOWN_UPDATES;
    }
    return cooldown;
}

int lp_refactor_policy_phase1_dir_stabilize_escape_gate_plan(
    int m,
    int degenerate_count,
    int dir_skip_event_streak,
    int no_progress_streak,
    int escape_cooldown,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int *next_escape_cooldown,
    int *triggered,
    int *hard_bypass) {
    int next_cooldown = escape_cooldown;
    int local_triggered = 0;
    int local_hard_bypass = 0;
    int suppress = 0;
    int streak_trigger;
    int no_progress_trigger;
    int chronic_treadmill = 0;

    if (next_cooldown < 0) next_cooldown = 0;
    if (force_lu_health && !force_extreme_dir && m >= PHASE1_DIR_ESCAPE_MIN_M) {
        streak_trigger = phase1_dir_escape_trigger_streak(m, degenerate_count);
        no_progress_trigger = phase1_dir_escape_no_progress_trigger(m, degenerate_count);
        chronic_treadmill =
            ((dir_skip_event_streak >= streak_trigger &&
              no_progress_streak >= no_progress_trigger) ||
             (no_progress_streak >= no_progress_trigger * 3));
        if (lu_hard_trigger) {
            if (next_cooldown > 0 || chronic_treadmill) {
                local_hard_bypass = 1;
            }
        } else if (next_cooldown > 0) {
            suppress = 1;
        } else if (chronic_treadmill) {
            next_cooldown = phase1_dir_escape_cooldown_updates(
                m, degenerate_count);
            local_triggered = 1;
            suppress = 1;
        }
    }

    if (next_escape_cooldown) *next_escape_cooldown = next_cooldown;
    if (triggered) *triggered = local_triggered;
    if (hard_bypass) *hard_bypass = local_hard_bypass;
    return suppress;
}

int lp_refactor_policy_phase1_force_pivot_refactor_relax_plan(
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
    if (!force_pivot_mode_active) return 0;
    if (force_extreme_dir || force_lu_health || lu_hard_trigger) return 0;
    if (m < PHASE1_FORCE_PIVOT_RELAX_MIN_M) return 0;
    if (degenerate_count < PHASE1_FORCE_PIVOT_RELAX_DEGEN_TRIGGER) return 0;
    if (no_progress_streak > PHASE1_FORCE_PIVOT_RELAX_MAX_NO_PROGRESS) return 0;
    if (dual_rescue_fail_streak > 0) return 0;
    if (dual_rescue_attempts < PHASE1_FORCE_PIVOT_RELAX_RESCUE_MIN_ATTEMPTS) return 0;
    if (dual_rescue_successes < 0) return 0;
    if (dual_rescue_successes > dual_rescue_attempts) return 0;
    return (dual_rescue_successes * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_DEN) >=
           (dual_rescue_attempts * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_NUM);
}

int lp_refactor_policy_phase1_force_extreme_refactor_relax_plan(
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
    if (!force_extreme_dir) return 0;
    if (force_lu_health || lu_hard_trigger) return 0;
    if (!(dir_inf_ratio > 0.0)) return 0;
    if (dir_inf_ratio > PHASE1_FORCE_EXTREME_RELAX_MAX_RATIO) return 0;
    if (m < PHASE1_FORCE_EXTREME_RELAX_MIN_M) return 0;
    if (degenerate_count < PHASE1_FORCE_EXTREME_RELAX_DEGEN_TRIGGER) return 0;
    if (no_progress_streak > PHASE1_FORCE_EXTREME_RELAX_MAX_NO_PROGRESS) return 0;
    if (dual_rescue_fail_streak > 0) return 0;
    if (dual_rescue_attempts < PHASE1_FORCE_PIVOT_RELAX_RESCUE_MIN_ATTEMPTS) return 0;
    if (dual_rescue_successes < 0) return 0;
    if (dual_rescue_successes > dual_rescue_attempts) return 0;
    return (dual_rescue_successes * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_DEN) >=
           (dual_rescue_attempts * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_NUM);
}

int lp_refactor_policy_phase1_force_extreme_tiny_theta_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak) {
    if (!force_extreme_dir) return 0;
    if (force_lu_health || lu_hard_trigger) return 0;
    if (!(dir_inf_ratio > 0.0)) return 0;
    if (m < PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_MIN_M) return 0;
    (void)degenerate_count;
    (void)no_progress_streak;
    if (tiny_theta_followup_streak <
        PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_MIN_STREAK) return 0;
    return 1;
}

int lp_refactor_policy_phase1_force_extreme_bound_flip_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int bound_flip_followup_streak) {
    if (!force_extreme_dir) return 0;
    if (force_lu_health || lu_hard_trigger) return 0;
    if (!(dir_inf_ratio > 0.0)) return 0;
    if (m < PHASE1_FORCE_EXTREME_BOUND_FLIP_RELAX_MIN_M) return 0;
    (void)degenerate_count;
    (void)no_progress_streak;
    if (bound_flip_followup_streak <
        PHASE1_FORCE_EXTREME_BOUND_FLIP_RELAX_MIN_STREAK) return 0;
    return 1;
}

int lp_refactor_policy_phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak,
    double pivot_ratio) {
    if (!force_extreme_dir) return 0;
    if (force_lu_health || lu_hard_trigger) return 0;
    if (!(dir_inf_ratio > 0.0)) return 0;
    if (!(pivot_ratio > 0.0)) return 0;
    if (pivot_ratio > PHASE1_FORCE_EXTREME_CATA_TINY_THETA_RELAX_MAX_PIVOT_RATIO) {
        return 0;
    }
    if (m < PHASE1_FORCE_EXTREME_CATA_TINY_THETA_RELAX_MIN_M) return 0;
    (void)degenerate_count;
    (void)no_progress_streak;
    if (tiny_theta_followup_streak <
        PHASE1_FORCE_EXTREME_CATA_TINY_THETA_RELAX_MIN_STREAK) return 0;
    return 1;
}

int lp_refactor_policy_phase1_soft_lu_policy_cooldown_updates(
    int m,
    int degenerate_count,
    int periodic_interval) {
    int cooldown = PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_UPDATES;
    if (m < PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_M) return 0;
    if (degenerate_count < PHASE1_SOFT_LU_POLICY_COOLDOWN_DEGEN_TRIGGER) return 0;
    if (periodic_interval > 0) {
        int half = periodic_interval / 2;
        if (half > cooldown) cooldown = half;
    }
    if (cooldown > PHASE1_SOFT_LU_POLICY_COOLDOWN_MAX_UPDATES) {
        cooldown = PHASE1_SOFT_LU_POLICY_COOLDOWN_MAX_UPDATES;
    }
    return cooldown;
}

static int phase1_reinvert_pressure_event(int no_pivot_streak,
                                          int no_progress_streak,
                                          int ratio_breakdown_count,
                                          int dir_skip_no_recompute_streak) {
    if (no_pivot_streak >= PHASE1_REINVERT_PRESSURE_NO_PIVOT_THRESHOLD &&
        no_progress_streak >= (PHASE1_REINVERT_PRESSURE_NO_PROGRESS_THRESHOLD / 2)) {
        return 1;
    }
    if (no_progress_streak >= PHASE1_REINVERT_PRESSURE_NO_PROGRESS_THRESHOLD) return 1;
    if (dir_skip_no_recompute_streak >= PHASE1_REINVERT_PRESSURE_DIR_SKIP_THRESHOLD) return 1;
    if (ratio_breakdown_count >= PHASE1_REINVERT_PRESSURE_RATIO_BREAKDOWN_THRESHOLD) return 1;
    return 0;
}

void lp_refactor_policy_phase1_reinvert_pressure_safety_step(
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
    int last_iter;
    int burst;
    int demoted;
    int demotions;
    int pressure_event = 0;

    if (!last_iter_io || !burst_io || !demoted_io || !demotions_io) return;

    if (iter < 0) iter = 0;
    if (no_pivot_streak < 0) no_pivot_streak = 0;
    if (no_progress_streak < 0) no_progress_streak = 0;
    if (ratio_breakdown_count < 0) ratio_breakdown_count = 0;
    if (dir_skip_no_recompute_streak < 0) dir_skip_no_recompute_streak = 0;

    last_iter = *last_iter_io;
    burst = *burst_io;
    demoted = *demoted_io ? 1 : 0;
    demotions = *demotions_io;
    if (burst < 0) burst = 0;
    if (last_iter < -1) last_iter = -1;
    if (demotions < 0) demotions = 0;

    if (demoted) {
        if (last_iter < 0) {
            last_iter = iter;
        } else if ((iter - last_iter) >= PHASE1_REINVERT_DEMOTE_COOLDOWN_ITERS) {
            demoted = 0;
            burst = 0;
            last_iter = -1;
        }
        *last_iter_io = last_iter;
        *burst_io = burst;
        *demoted_io = demoted;
        *demotions_io = demotions;
        return;
    }

    if (hard_lu_trigger) {
        if (last_iter >= 0 && (iter - last_iter) > PHASE1_REINVERT_PRESSURE_WINDOW_ITERS) {
            burst = 0;
        }
        *last_iter_io = last_iter;
        *burst_io = burst;
        *demoted_io = demoted;
        *demotions_io = demotions;
        return;
    }

    pressure_event = phase1_reinvert_pressure_event(no_pivot_streak,
                                                    no_progress_streak,
                                                    ratio_breakdown_count,
                                                    dir_skip_no_recompute_streak);
    if (pressure_event) {
        if (last_iter >= 0 && (iter - last_iter) <= PHASE1_REINVERT_PRESSURE_WINDOW_ITERS) {
            burst++;
        } else {
            burst = 1;
        }
        last_iter = iter;
    } else if (last_iter >= 0 && (iter - last_iter) > PHASE1_REINVERT_PRESSURE_WINDOW_ITERS) {
        burst = 0;
    }

    if (burst >= PHASE1_REINVERT_PRESSURE_DEMOTE_COUNT) {
        demoted = 1;
        demotions++;
        burst = 0;
        last_iter = iter;
    }

    *last_iter_io = last_iter;
    *burst_io = burst;
    *demoted_io = demoted;
    *demotions_io = demotions;
}

int lp_refactor_policy_phase1_stagnation_escape_decision(
    int window_iters,
    double obj_delta,
    double obj_anchor,
    int retry_defers,
    int no_pivot_events,
    int update_recovery_refactors,
    int refactors,
    int recompute_ratio_breakdown,
    int recompute_dir_skip,
    int recompute_dir_refactor,
    int recompute_pivot_fail,
    int recompute_perturb,
    int cooldown_remaining) {
    int retry_pressure;
    int update_pressure;
    int recompute_total;
    int recompute_hot;
    int recompute_pressure;
    double obj_tol;
    int objective_flat;

    if (window_iters < PHASE1_STAGNATION_WINDOW_ITERS) return 0;
    if (cooldown_remaining > 0) return 0;
    if (retry_defers < 0) retry_defers = 0;
    if (no_pivot_events < 0) no_pivot_events = 0;
    if (update_recovery_refactors < 0) update_recovery_refactors = 0;
    if (refactors < 0) refactors = 0;
    if (recompute_ratio_breakdown < 0) recompute_ratio_breakdown = 0;
    if (recompute_dir_skip < 0) recompute_dir_skip = 0;
    if (recompute_dir_refactor < 0) recompute_dir_refactor = 0;
    if (recompute_pivot_fail < 0) recompute_pivot_fail = 0;
    if (recompute_perturb < 0) recompute_perturb = 0;

    obj_tol = PHASE1_STAGNATION_OBJ_ABS_TOL +
              PHASE1_STAGNATION_OBJ_REL_TOL * (1.0 + fabs(obj_anchor));
    objective_flat = fabs(obj_delta) <= obj_tol;

    retry_pressure =
        no_pivot_events >= PHASE1_STAGNATION_MIN_NO_PIVOT_EVENTS &&
        retry_defers >= PHASE1_STAGNATION_MIN_RETRY_DEFERS &&
        retry_defers * PHASE1_STAGNATION_RETRY_RATIO_DEN >=
            no_pivot_events * PHASE1_STAGNATION_RETRY_RATIO_NUM;
    update_pressure =
        refactors >= PHASE1_STAGNATION_MIN_REFACTORS &&
        update_recovery_refactors >= PHASE1_STAGNATION_MIN_UPDATE_RECOVERY &&
        update_recovery_refactors * PHASE1_STAGNATION_UPDATE_RATIO_DEN >=
            refactors * PHASE1_STAGNATION_UPDATE_RATIO_NUM;

    recompute_total = recompute_ratio_breakdown +
                      recompute_dir_skip +
                      recompute_dir_refactor +
                      recompute_pivot_fail +
                      recompute_perturb;
    recompute_hot = recompute_ratio_breakdown +
                    recompute_dir_skip +
                    recompute_pivot_fail;
    recompute_pressure =
        recompute_total >= PHASE1_STAGNATION_MIN_RECOMPUTES &&
        recompute_hot * PHASE1_STAGNATION_RECOMPUTE_HOT_DEN >=
            recompute_total * PHASE1_STAGNATION_RECOMPUTE_HOT_NUM;

    if (!objective_flat) return 0;
    if (!recompute_pressure) return 0;
    return retry_pressure || update_pressure;
}

int lp_refactor_policy_phase1_degen_threshold(int m) {
    return (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M)
        ? PHASE1_DEGEN_THRESHOLD_LARGE
        : PHASE1_DEGEN_THRESHOLD_DEFAULT;
}

int lp_refactor_policy_phase1_stall_threshold(int m) {
    return (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M)
        ? PHASE1_STALL_THRESHOLD_LARGE
        : PHASE1_STALL_THRESHOLD_DEFAULT;
}

int lp_refactor_policy_phase1_recompute_interval(void) {
    return PHASE1_RECOMPUTE_INTERVAL;
}

double lp_refactor_policy_phase1_stall_obj_tol(double last_obj) {
    if (!isfinite(last_obj)) last_obj = 0.0;
    return PHASE1_STALL_OBJ_REL_TOL * (1.0 + fabs(last_obj));
}

int lp_refactor_policy_phase1_ratio_breakdown_limit(int m,
                                                    int same_entering_streak) {
    int ratio_breakdown_limit = RALPH_PHASE1_RATIO_BREAKDOWN_LIMIT;

    if (same_entering_streak < 0) same_entering_streak = 0;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M &&
        same_entering_streak >= PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_THRESHOLD) {
        int tightened_limit =
            RALPH_PHASE1_RATIO_BREAKDOWN_LIMIT / PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_DIVISOR;
        if (tightened_limit < 4) tightened_limit = 4;
        if (ratio_breakdown_limit > tightened_limit) {
            ratio_breakdown_limit = tightened_limit;
        }
    }
    return ratio_breakdown_limit;
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

int lp_refactor_policy_dense_spike_min_updates_override(int phase,
                                                        int m) {
    if (phase == 2 &&
        m >= DENSE_SPIKE_WARMUP_PHASE2_MEDIUM_MIN_M &&
        m < DENSE_SPIKE_WARMUP_PHASE2_MEDIUM_MAX_M) {
        return DENSE_SPIKE_WARMUP_PHASE2_MEDIUM_MIN_UPDATES;
    }
    return 0;
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
    if (degenerate_count < lu_soft_cost_gate_min_degen(phase, m)) return 0;
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
