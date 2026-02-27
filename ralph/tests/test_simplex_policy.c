#include <stdio.h>
#include "lp.h"
#include "lp_refactor_policy.h"

/* Internal test hook from simplex.c */
int simplex_choose_basis_action_for_test(double pivot,
                                         int force_refactor,
                                         int lu_update_status,
                                         int lu_reason,
                                         int repeat_pattern,
                                         double growth_factor);

/* Internal scheduler test hook from simplex.c */
int simplex_periodic_refactor_plan_for_test(int phase,
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
                                            int periodic_policy_cooldown,
                                            double periodic_policy_pressure_decay,
                                            int *interval_out,
                                            double *pressure_out);

/* Internal LU health hysteresis test hook from simplex.c */
int simplex_lu_health_refactor_plan_for_test(int m,
                                             int use_ft_updates,
                                             int num_updates,
                                             int max_updates,
                                             int spike_pool_used,
                                             int spike_pool_capacity,
                                             double cond_estimate,
                                             double growth_factor,
                                             int soft_breach_streak,
                                             int *hard_trigger_out,
                                             int *soft_trigger_out,
                                             int *next_streak_out,
                                             int *soft_threshold_out,
                                             int *soft_min_update_age_out);

/* Internal soft LU defer cap/gate test hook from simplex.c */
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
                                        int *next_consecutive_defers_out);

/* Internal policy-periodic cost defer cap/gate hook from simplex.c */
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
                                              int *next_consecutive_defers_out);

int simplex_phase1_no_pivot_force_plan_for_test(int m,
                                                 int degenerate_count,
                                                 int streak,
                                                 int cooldown,
                                                 int reason,
                                                 int *next_streak_out,
                                                 int *next_cooldown_out);

int simplex_phase1_soft_lu_policy_cooldown_plan_for_test(
    int m,
    int degenerate_count,
    int periodic_interval,
    int lu_soft_cost_deferred,
    int periodic_policy_cooldown,
    int *next_cooldown_out);

enum {
    EXPECT_UPDATE = 0,
    EXPECT_REFACTOR = 1,
    EXPECT_REPAIR = 2,
    EXPECT_ABORT = 3
};

typedef struct {
    const char *name;
    double pivot;
    int force_refactor;
    int lu_update_status;
    int lu_reason;
    int repeat_pattern;
    double growth_factor;
    int expected_action;
} PolicyCase;

static int run_case(const PolicyCase *tc) {
    int got = simplex_choose_basis_action_for_test(tc->pivot,
                                                   tc->force_refactor,
                                                   tc->lu_update_status,
                                                   tc->lu_reason,
                                                   tc->repeat_pattern,
                                                   tc->growth_factor);
    if (got != tc->expected_action) {
        fprintf(stderr, "FAIL: %s (expected=%d got=%d)\n",
                tc->name, tc->expected_action, got);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

typedef struct {
    const char *name;
    int phase;
    int iter;
    int m;
    int max_updates;
    int num_updates;
    int spike_pool_used;
    int spike_pool_capacity;
    double cond_estimate;
    double growth_factor;
    int use_bland;
    int degenerate_count;
    double feedback_bias;
    int periodic_policy_cooldown;
    double periodic_policy_pressure_decay;
    int expected_run;
    int expected_interval;
    double min_pressure;
    double max_pressure;
} SchedulerCase;

static int run_scheduler_case(const SchedulerCase *tc) {
    int interval = -1;
    double pressure = -1.0;
    int should_run = simplex_periodic_refactor_plan_for_test(tc->phase,
                                                              tc->iter,
                                                              tc->m,
                                                              tc->max_updates,
                                                              tc->num_updates,
                                                              tc->spike_pool_used,
                                                              tc->spike_pool_capacity,
                                                              tc->cond_estimate,
                                                              tc->growth_factor,
                                                              tc->use_bland,
                                                              tc->degenerate_count,
                                                              tc->feedback_bias,
                                                              tc->periodic_policy_cooldown,
                                                              tc->periodic_policy_pressure_decay,
                                                              &interval,
                                                              &pressure);
    if (interval != tc->expected_interval) {
        fprintf(stderr, "FAIL: %s (expected interval=%d got=%d)\n",
                tc->name, tc->expected_interval, interval);
        return 0;
    }
    if (should_run != tc->expected_run) {
        fprintf(stderr, "FAIL: %s (expected should_run=%d got=%d)\n",
                tc->name, tc->expected_run, should_run);
        return 0;
    }
    if (pressure < tc->min_pressure || pressure > tc->max_pressure) {
        fprintf(stderr, "FAIL: %s (expected pressure in [%.3f, %.3f], got %.6f)\n",
                tc->name, tc->min_pressure, tc->max_pressure, pressure);
        return 0;
    }

    printf("PASS: %s\n", tc->name);
    return 1;
}

typedef struct {
    const char *name;
    int m;
    int use_ft_updates;
    int num_updates;
    int max_updates;
    int spike_pool_used;
    int spike_pool_capacity;
    double cond_estimate;
    double growth_factor;
    int soft_breach_streak;
    int expected_refactor;
    int expected_hard;
    int expected_soft;
    int expected_next_streak;
} LUHealthCase;

typedef struct {
    const char *name;
    int phase;
    int m;
    int use_bland;
    int degenerate_count;
    int num_updates;
    int max_updates;
    int spike_pool_used;
    int spike_pool_capacity;
    double cond_estimate;
    double growth_factor;
    double refactor_cost_ewma_ms;
    double iter_cost_ewma_ms;
    int refactor_cost_samples;
    int iter_cost_samples;
    int consecutive_defers;
    int expected_defer;
    int expected_cap;
    int expected_cap_blocked;
    int expected_next_consecutive;
} SoftLUDeferCase;

typedef struct {
    const char *name;
    int phase;
    int m;
    int use_bland;
    int degenerate_count;
    int num_updates;
    int max_updates;
    int spike_pool_used;
    int spike_pool_capacity;
    double cond_estimate;
    double growth_factor;
    double refactor_cost_ewma_ms;
    double iter_cost_ewma_ms;
    int refactor_cost_samples;
    int iter_cost_samples;
    int consecutive_defers;
    int expected_defer;
    int expected_cap;
    int expected_cap_blocked;
    int expected_next_consecutive;
    int expected_reason;
} PeriodicCostDeferCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int repeat_streak;
    int expected_cooldown;
} DirStabilizeCooldownCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int no_pivot_streak;
    int expected_allow;
} DirSkipRcOnlyCase;

typedef struct {
    const char *name;
    double dir_inf_ratio;
    int cooldown_active;
    int expected_force;
} DirStabilizeForceCase;

typedef struct {
    const char *name;
    double dir_inf_ratio;
    int cooldown_active;
    int lu_health_triggered;
    int pending_repeat;
    int expected_defer;
} DirStabilizeModerateCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int streak;
    int cooldown;
    int reason;
    int expected_force;
    int expected_next_streak;
    int expected_next_cooldown;
} NoPivotForceCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int periodic_interval;
    int lu_soft_cost_deferred;
    int periodic_policy_cooldown;
    int expected_applied;
    int expected_next_cooldown;
} SoftLUPolicyCooldownCase;

typedef struct {
    const char *name;
    int m;
    int use_bland;
    int degenerate_count;
    int spike_pool_used;
    int spike_pool_capacity;
    double cond_estimate;
    double growth_factor;
    int expected_interval;
} Phase2RecomputeIntervalCase;

static int run_lu_health_case(const LUHealthCase *tc) {
    int hard = -1;
    int soft = -1;
    int next_streak = -1;
    int soft_threshold = -1;
    int soft_min_update_age = -1;
    int refactor = simplex_lu_health_refactor_plan_for_test(tc->m,
                                                             tc->use_ft_updates,
                                                             tc->num_updates,
                                                             tc->max_updates,
                                                             tc->spike_pool_used,
                                                             tc->spike_pool_capacity,
                                                             tc->cond_estimate,
                                                             tc->growth_factor,
                                                             tc->soft_breach_streak,
                                                             &hard,
                                                             &soft,
                                                             &next_streak,
                                                             &soft_threshold,
                                                             &soft_min_update_age);
    if (refactor != tc->expected_refactor) {
        fprintf(stderr, "FAIL: %s (expected refactor=%d got=%d)\n",
                tc->name, tc->expected_refactor, refactor);
        return 0;
    }
    if (hard != tc->expected_hard) {
        fprintf(stderr, "FAIL: %s (expected hard=%d got=%d)\n",
                tc->name, tc->expected_hard, hard);
        return 0;
    }
    if (soft != tc->expected_soft) {
        fprintf(stderr, "FAIL: %s (expected soft=%d got=%d)\n",
                tc->name, tc->expected_soft, soft);
        return 0;
    }
    if (next_streak != tc->expected_next_streak) {
        fprintf(stderr, "FAIL: %s (expected next_streak=%d got=%d)\n",
                tc->name, tc->expected_next_streak, next_streak);
        return 0;
    }
    if (soft_threshold < 1) {
        fprintf(stderr, "FAIL: %s (soft threshold must be >=1, got=%d)\n",
                tc->name, soft_threshold);
        return 0;
    }
    if (soft_min_update_age < 1) {
        fprintf(stderr, "FAIL: %s (soft min update age must be >=1, got=%d)\n",
                tc->name, soft_min_update_age);
        return 0;
    }

    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_soft_lu_defer_case(const SoftLUDeferCase *tc) {
    int cap = -1;
    int cap_blocked = -1;
    int next_consecutive = -1;
    int defer = simplex_soft_lu_defer_plan_for_test(tc->phase,
                                                     tc->m,
                                                     tc->use_bland,
                                                     tc->degenerate_count,
                                                     tc->num_updates,
                                                     tc->max_updates,
                                                     tc->spike_pool_used,
                                                     tc->spike_pool_capacity,
                                                     tc->cond_estimate,
                                                     tc->growth_factor,
                                                     tc->refactor_cost_ewma_ms,
                                                     tc->iter_cost_ewma_ms,
                                                     tc->consecutive_defers,
                                                     &cap,
                                                     &cap_blocked,
                                                     &next_consecutive);
    if (defer != tc->expected_defer) {
        fprintf(stderr, "FAIL: %s (expected defer=%d got=%d)\n",
                tc->name, tc->expected_defer, defer);
        return 0;
    }
    if (cap != tc->expected_cap) {
        fprintf(stderr, "FAIL: %s (expected cap=%d got=%d)\n",
                tc->name, tc->expected_cap, cap);
        return 0;
    }
    if (cap_blocked != tc->expected_cap_blocked) {
        fprintf(stderr, "FAIL: %s (expected cap_blocked=%d got=%d)\n",
                tc->name, tc->expected_cap_blocked, cap_blocked);
        return 0;
    }
    if (next_consecutive != tc->expected_next_consecutive) {
        fprintf(stderr, "FAIL: %s (expected next_consecutive=%d got=%d)\n",
                tc->name, tc->expected_next_consecutive, next_consecutive);
        return 0;
    }

    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_periodic_cost_defer_case(const PeriodicCostDeferCase *tc) {
    int reason = -1;
    int cap = -1;
    int cap_blocked = -1;
    int next_consecutive = -1;
    int defer = simplex_periodic_cost_defer_plan_for_test(tc->phase,
                                                           tc->m,
                                                           tc->use_bland,
                                                           tc->degenerate_count,
                                                           tc->num_updates,
                                                           tc->max_updates,
                                                           tc->spike_pool_used,
                                                           tc->spike_pool_capacity,
                                                           tc->cond_estimate,
                                                           tc->growth_factor,
                                                           tc->refactor_cost_ewma_ms,
                                                           tc->iter_cost_ewma_ms,
                                                           tc->refactor_cost_samples,
                                                           tc->iter_cost_samples,
                                                           tc->consecutive_defers,
                                                           &reason,
                                                           &cap,
                                                           &cap_blocked,
                                                           &next_consecutive);
    if (defer != tc->expected_defer) {
        fprintf(stderr, "FAIL: %s (expected defer=%d got=%d)\n",
                tc->name, tc->expected_defer, defer);
        return 0;
    }
    if (cap != tc->expected_cap) {
        fprintf(stderr, "FAIL: %s (expected cap=%d got=%d)\n",
                tc->name, tc->expected_cap, cap);
        return 0;
    }
    if (cap_blocked != tc->expected_cap_blocked) {
        fprintf(stderr, "FAIL: %s (expected cap_blocked=%d got=%d)\n",
                tc->name, tc->expected_cap_blocked, cap_blocked);
        return 0;
    }
    if (next_consecutive != tc->expected_next_consecutive) {
        fprintf(stderr, "FAIL: %s (expected next_consecutive=%d got=%d)\n",
                tc->name, tc->expected_next_consecutive, next_consecutive);
        return 0;
    }
    if (reason != tc->expected_reason) {
        fprintf(stderr, "FAIL: %s (expected reason=%d got=%d)\n",
                tc->name, tc->expected_reason, reason);
        return 0;
    }

    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_dir_stabilize_cooldown_case(const DirStabilizeCooldownCase *tc) {
    int cooldown = lp_refactor_policy_phase1_dir_stabilize_cooldown_updates(tc->m,
                                                                             tc->degenerate_count,
                                                                             tc->repeat_streak);
    if (cooldown != tc->expected_cooldown) {
        fprintf(stderr, "FAIL: %s (expected cooldown=%d got=%d)\n",
                tc->name, tc->expected_cooldown, cooldown);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_dir_skip_rc_only_case(const DirSkipRcOnlyCase *tc) {
    int allow = lp_refactor_policy_phase1_dir_skip_allow_rc_only(
        tc->m, tc->degenerate_count, tc->no_pivot_streak);
    if (allow != tc->expected_allow) {
        fprintf(stderr, "FAIL: %s (expected allow=%d got=%d)\n",
                tc->name, tc->expected_allow, allow);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_dir_stabilize_force_case(const DirStabilizeForceCase *tc) {
    int force =
        lp_refactor_policy_phase1_dir_stabilize_force_extreme_ratio(
            tc->dir_inf_ratio,
            tc->cooldown_active);
    if (force != tc->expected_force) {
        fprintf(stderr, "FAIL: %s (expected force=%d got=%d)\n",
                tc->name, tc->expected_force, force);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_dir_stabilize_moderate_case(const DirStabilizeModerateCase *tc) {
    int defer =
        lp_refactor_policy_phase1_dir_stabilize_should_defer_moderate(
            tc->dir_inf_ratio,
            tc->cooldown_active,
            tc->lu_health_triggered,
            tc->pending_repeat);
    if (defer != tc->expected_defer) {
        fprintf(stderr, "FAIL: %s (expected defer=%d got=%d)\n",
                tc->name, tc->expected_defer, defer);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_no_pivot_force_case(const NoPivotForceCase *tc) {
    int next_streak = -1;
    int next_cooldown = -1;
    int force = simplex_phase1_no_pivot_force_plan_for_test(tc->m,
                                                             tc->degenerate_count,
                                                             tc->streak,
                                                             tc->cooldown,
                                                             tc->reason,
                                                             &next_streak,
                                                             &next_cooldown);
    if (force != tc->expected_force) {
        fprintf(stderr, "FAIL: %s (expected force=%d got=%d)\n",
                tc->name, tc->expected_force, force);
        return 0;
    }
    if (next_streak != tc->expected_next_streak) {
        fprintf(stderr, "FAIL: %s (expected next_streak=%d got=%d)\n",
                tc->name, tc->expected_next_streak, next_streak);
        return 0;
    }
    if (next_cooldown != tc->expected_next_cooldown) {
        fprintf(stderr, "FAIL: %s (expected next_cooldown=%d got=%d)\n",
                tc->name, tc->expected_next_cooldown, next_cooldown);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_soft_lu_policy_cooldown_case(const SoftLUPolicyCooldownCase *tc) {
    int next_cooldown = -1;
    int applied = simplex_phase1_soft_lu_policy_cooldown_plan_for_test(
        tc->m,
        tc->degenerate_count,
        tc->periodic_interval,
        tc->lu_soft_cost_deferred,
        tc->periodic_policy_cooldown,
        &next_cooldown);
    if (applied != tc->expected_applied) {
        fprintf(stderr, "FAIL: %s (expected applied=%d got=%d)\n",
                tc->name, tc->expected_applied, applied);
        return 0;
    }
    if (next_cooldown != tc->expected_next_cooldown) {
        fprintf(stderr, "FAIL: %s (expected next_cooldown=%d got=%d)\n",
                tc->name, tc->expected_next_cooldown, next_cooldown);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_phase2_recompute_interval_case(
    const Phase2RecomputeIntervalCase *tc) {
    int interval = lp_refactor_policy_phase2_periodic_recompute_interval(
        tc->m,
        tc->use_bland,
        tc->degenerate_count,
        tc->spike_pool_used,
        tc->spike_pool_capacity,
        tc->cond_estimate,
        tc->growth_factor);
    if (interval != tc->expected_interval) {
        fprintf(stderr, "FAIL: %s (expected interval=%d got=%d)\n",
                tc->name, tc->expected_interval, interval);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

int main(void) {
    const PolicyCase cases[] = {
        {
            .name = "default path uses LU update",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_UPDATE
        },
        {
            .name = "tiny pivot aborts immediately",
            .pivot = 1e-12,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_ABORT
        },
        {
            .name = "forced-refactor flag bypasses LU update",
            .pivot = 1e-2,
            .force_refactor = 1,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "repeat-pattern trigger forces refactor",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "growth-trigger forces refactor",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = 0,
            .lu_reason = LU_FAIL_NONE,
            .repeat_pattern = 0,
            .growth_factor = RALPH_LU_GROWTH_REFACTOR_THRESHOLD * 1.01,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "failed LU update escalates to refactor",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = -1,
            .lu_reason = LU_FAIL_MAX_UPDATES,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REFACTOR
        },
        {
            .name = "failed refactor escalates to repair",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = -2,
            .lu_reason = LU_FAIL_FACTOR_SINGULAR,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_REPAIR
        },
        {
            .name = "failed repair aborts",
            .pivot = 1e-2,
            .force_refactor = 0,
            .lu_update_status = -3,
            .lu_reason = LU_FAIL_FACTOR_SINGULAR,
            .repeat_pattern = 0,
            .growth_factor = 1.0,
            .expected_action = EXPECT_ABORT
        }
    };
    const SchedulerCase scheduler_cases[] = {
        {
            .name = "large phase2 basis clamps to min interval and runs",
            .phase = 2,
            .iter = 10,
            .m = 650,
            .max_updates = 100,
            .num_updates = 10,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .use_bland = 0,
            .degenerate_count = 0,
            .feedback_bias = 0.0,
            .expected_run = 1,
            .expected_interval = 10,
            .min_pressure = 0.99,
            .max_pressure = 1.0
        },
        {
            .name = "healthy medium phase2 basis relaxes interval and skips",
            .phase = 2,
            .iter = 45,
            .m = 250,
            .max_updates = 120,
            .num_updates = 45,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .use_bland = 0,
            .degenerate_count = 0,
            .feedback_bias = 0.0,
            .expected_run = 0,
            .expected_interval = 45,
            .min_pressure = 0.37,
            .max_pressure = 0.38
        },
        {
            .name = "update pressure eventually triggers periodic run",
            .phase = 2,
            .iter = 90,
            .m = 250,
            .max_updates = 120,
            .num_updates = 90,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .use_bland = 0,
            .degenerate_count = 0,
            .feedback_bias = 0.0,
            .expected_run = 1,
            .expected_interval = 45,
            .min_pressure = 0.75,
            .max_pressure = 0.75
        },
        {
            .name = "degeneracy pressure tightens medium phase2 interval",
            .phase = 2,
            .iter = 38,
            .m = 250,
            .max_updates = 120,
            .num_updates = 38,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .use_bland = 0,
            .degenerate_count = 10,
            .feedback_bias = 0.0,
            .expected_run = 1,
            .expected_interval = 38,
            .min_pressure = 0.50,
            .max_pressure = 0.50
        },
        {
            .name = "large phase1 basis clamps to min interval and runs",
            .phase = 1,
            .iter = 24,
            .m = 700,
            .max_updates = 100,
            .num_updates = 24,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .use_bland = 0,
            .degenerate_count = 0,
            .feedback_bias = 0.0,
            .expected_run = 1,
            .expected_interval = 24,
            .min_pressure = 0.99,
            .max_pressure = 1.0
        },
        {
            .name = "bland mode forces pressure to conservative schedule",
            .phase = 1,
            .iter = 30,
            .m = 120,
            .max_updates = 90,
            .num_updates = 30,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e2,
            .growth_factor = 1.0,
            .use_bland = 1,
            .degenerate_count = 0,
            .feedback_bias = 0.0,
            .expected_run = 1,
            .expected_interval = 30,
            .min_pressure = 1.0,
            .max_pressure = 1.0
        },
        {
            .name = "positive feedback tightens phase2 interval",
            .phase = 2,
            .iter = 12,
            .m = 250,
            .max_updates = 120,
            .num_updates = 12,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .use_bland = 0,
            .degenerate_count = 0,
            .feedback_bias = 0.20,
            .expected_run = 0,
            .expected_interval = 42,
            .min_pressure = 0.25,
            .max_pressure = 0.26
        },
        {
            .name = "negative feedback relaxes phase2 interval",
            .phase = 2,
            .iter = 12,
            .m = 250,
            .max_updates = 120,
            .num_updates = 12,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .use_bland = 0,
            .degenerate_count = 0,
            .feedback_bias = -0.20,
            .expected_run = 0,
            .expected_interval = 46,
            .min_pressure = 0.10,
            .max_pressure = 0.10
        },
        {
            .name = "xlarge degenerate phase2 relaxes min interval when LU health is stable",
            .phase = 2,
            .iter = 24,
            .m = 1503,
            .max_updates = 120,
            .num_updates = 24,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .use_bland = 0,
            .degenerate_count = 40,
            .feedback_bias = 0.0,
            .expected_run = 1,
            .expected_interval = 24,
            .min_pressure = 0.99,
            .max_pressure = 1.00
        },
        {
            .name = "xlarge degenerate phase2 keeps tight min interval when LU health is poor",
            .phase = 2,
            .iter = 10,
            .m = 1503,
            .max_updates = 120,
            .num_updates = 10,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e8,
            .growth_factor = 10.0,
            .use_bland = 0,
            .degenerate_count = 40,
            .feedback_bias = 0.0,
            .expected_run = 1,
            .expected_interval = 10,
            .min_pressure = 0.99,
            .max_pressure = 1.00
        },
        {
            .name = "phase2 policy cooldown skips one periodic cadence under long degeneracy",
            .phase = 2,
            .iter = 24,
            .m = 1503,
            .max_updates = 120,
            .num_updates = 24,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .use_bland = 0,
            .degenerate_count = 40,
            .feedback_bias = 0.0,
            .periodic_policy_cooldown = 12,
            .periodic_policy_pressure_decay = 0.0,
            .expected_run = 0,
            .expected_interval = 24,
            .min_pressure = 0.99,
            .max_pressure = 1.00
        },
        {
            .name = "phase2 pressure decay lowers effective periodic pressure",
            .phase = 2,
            .iter = 24,
            .m = 1503,
            .max_updates = 120,
            .num_updates = 24,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .use_bland = 0,
            .degenerate_count = 40,
            .feedback_bias = 0.0,
            .periodic_policy_cooldown = 0,
            .periodic_policy_pressure_decay = 0.24,
            .expected_run = 1,
            .expected_interval = 24,
            .min_pressure = 0.76,
            .max_pressure = 0.76
        },
        {
            .name = "phase1 policy cooldown skips one periodic cadence under large degeneracy",
            .phase = 1,
            .iter = 24,
            .m = 1503,
            .max_updates = 120,
            .num_updates = 24,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .use_bland = 0,
            .degenerate_count = 120,
            .feedback_bias = 0.0,
            .periodic_policy_cooldown = 24,
            .periodic_policy_pressure_decay = 0.0,
            .expected_run = 0,
            .expected_interval = 24,
            .min_pressure = 0.99,
            .max_pressure = 1.00
        },
        {
            .name = "phase1 pressure decay lowers effective periodic pressure",
            .phase = 1,
            .iter = 24,
            .m = 1503,
            .max_updates = 120,
            .num_updates = 24,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .use_bland = 0,
            .degenerate_count = 120,
            .feedback_bias = 0.0,
            .periodic_policy_cooldown = 0,
            .periodic_policy_pressure_decay = 0.24,
            .expected_run = 1,
            .expected_interval = 24,
            .min_pressure = 0.76,
            .max_pressure = 0.76
        }
    };
    const LUHealthCase lu_health_cases[] = {
        {
            .name = "lu hard trigger on max updates bypasses hysteresis",
            .m = 1500,
            .use_ft_updates = 1,
            .num_updates = 120,
            .max_updates = 120,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .soft_breach_streak = 0,
            .expected_refactor = 1,
            .expected_hard = 1,
            .expected_soft = 0,
            .expected_next_streak = 0
        },
        {
            .name = "lu hard trigger on severe cond ratio bypasses hysteresis",
            .m = 1500,
            .use_ft_updates = 1,
            .num_updates = 10,
            .max_updates = 120,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 2e8,
            .growth_factor = 100.0,
            .soft_breach_streak = 0,
            .expected_refactor = 1,
            .expected_hard = 1,
            .expected_soft = 0,
            .expected_next_streak = 0
        },
        {
            .name = "lu soft trigger requires consecutive breaches",
            .m = 1500,
            .use_ft_updates = 1,
            .num_updates = 70,
            .max_updates = 120,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e7,
            .growth_factor = 1.0,
            .soft_breach_streak = 0,
            .expected_refactor = 0,
            .expected_hard = 0,
            .expected_soft = 1,
            .expected_next_streak = 1
        },
        {
            .name = "lu soft trigger refactors after hysteresis streak",
            .m = 1500,
            .use_ft_updates = 1,
            .num_updates = 70,
            .max_updates = 120,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e7,
            .growth_factor = 1.0,
            .soft_breach_streak = 2,
            .expected_refactor = 1,
            .expected_hard = 0,
            .expected_soft = 1,
            .expected_next_streak = 3
        },
        {
            .name = "lu soft trigger respects min update age",
            .m = 1500,
            .use_ft_updates = 1,
            .num_updates = 12,
            .max_updates = 120,
            .spike_pool_used = 90,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e3,
            .growth_factor = 1.0,
            .soft_breach_streak = 1,
            .expected_refactor = 0,
            .expected_hard = 0,
            .expected_soft = 1,
            .expected_next_streak = 2
        }
    };
    const SoftLUDeferCase soft_lu_defer_cases[] = {
        {
            .name = "soft LU defer gate allows defer below cap",
            .phase = 2,
            .m = 1503,
            .use_bland = 0,
            .degenerate_count = 80,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e5,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 12.0,
            .iter_cost_ewma_ms = 1.0,
            .consecutive_defers = 0,
            .expected_defer = 1,
            .expected_cap = 4,
            .expected_cap_blocked = 0,
            .expected_next_consecutive = 1
        },
        {
            .name = "soft LU defer cap blocks excessive consecutive defers",
            .phase = 2,
            .m = 1503,
            .use_bland = 0,
            .degenerate_count = 80,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e5,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 12.0,
            .iter_cost_ewma_ms = 1.0,
            .consecutive_defers = 4,
            .expected_defer = 0,
            .expected_cap = 4,
            .expected_cap_blocked = 1,
            .expected_next_consecutive = 0
        },
        {
            .name = "soft LU defer gate disabled by poor LU health envelope",
            .phase = 2,
            .m = 1503,
            .use_bland = 0,
            .degenerate_count = 80,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e8,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 12.0,
            .iter_cost_ewma_ms = 1.0,
            .consecutive_defers = 2,
            .expected_defer = 0,
            .expected_cap = 4,
            .expected_cap_blocked = 0,
            .expected_next_consecutive = 0
        }
    };
    const PeriodicCostDeferCase periodic_cost_defer_cases[] = {
        {
            .name = "periodic cost gate allows defer below cap",
            .phase = 2,
            .m = 1503,
            .use_bland = 0,
            .degenerate_count = 80,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e5,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 12.0,
            .iter_cost_ewma_ms = 1.0,
            .refactor_cost_samples = 2,
            .iter_cost_samples = 16,
            .consecutive_defers = 0,
            .expected_defer = 1,
            .expected_cap = 3,
            .expected_cap_blocked = 0,
            .expected_next_consecutive = 1,
            .expected_reason = LP_PERIODIC_COST_DAMPEN_DEFER
        },
        {
            .name = "periodic cost defer cap blocks excessive defers",
            .phase = 2,
            .m = 1503,
            .use_bland = 0,
            .degenerate_count = 80,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e5,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 12.0,
            .iter_cost_ewma_ms = 1.0,
            .refactor_cost_samples = 2,
            .iter_cost_samples = 16,
            .consecutive_defers = 3,
            .expected_defer = 0,
            .expected_cap = 3,
            .expected_cap_blocked = 1,
            .expected_next_consecutive = 0,
            .expected_reason = LP_PERIODIC_COST_DAMPEN_DEFER
        },
        {
            .name = "periodic cost gate requires high refactor-to-iter ratio",
            .phase = 2,
            .m = 1503,
            .use_bland = 0,
            .degenerate_count = 80,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e5,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 5.0,
            .iter_cost_ewma_ms = 1.0,
            .refactor_cost_samples = 2,
            .iter_cost_samples = 16,
            .consecutive_defers = 2,
            .expected_defer = 0,
            .expected_cap = 3,
            .expected_cap_blocked = 0,
            .expected_next_consecutive = 0,
            .expected_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO
        },
        {
            .name = "periodic cost gate waits for warmup samples",
            .phase = 2,
            .m = 1503,
            .use_bland = 0,
            .degenerate_count = 80,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e5,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 12.0,
            .iter_cost_ewma_ms = 1.0,
            .refactor_cost_samples = 0,
            .iter_cost_samples = 16,
            .consecutive_defers = 0,
            .expected_defer = 0,
            .expected_cap = 3,
            .expected_cap_blocked = 0,
            .expected_next_consecutive = 0,
            .expected_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_WARMUP
        },
        {
            .name = "phase1 periodic cost cap is stricter",
            .phase = 1,
            .m = 1800,
            .use_bland = 0,
            .degenerate_count = 120,
            .num_updates = 60,
            .max_updates = 120,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e5,
            .growth_factor = 100.0,
            .refactor_cost_ewma_ms = 12.0,
            .iter_cost_ewma_ms = 1.0,
            .refactor_cost_samples = 2,
            .iter_cost_samples = 16,
            .consecutive_defers = 2,
            .expected_defer = 0,
            .expected_cap = 2,
            .expected_cap_blocked = 1,
            .expected_next_consecutive = 0,
            .expected_reason = LP_PERIODIC_COST_DAMPEN_DEFER
        }
    };
    const DirStabilizeCooldownCase dir_stabilize_cooldown_cases[] = {
        {
            .name = "phase1 dir-stabilize uses base cooldown on small matrices",
            .m = 350,
            .degenerate_count = 80,
            .repeat_streak = 8,
            .expected_cooldown = 8
        },
        {
            .name = "phase1 dir-stabilize uses base cooldown without repeat streak",
            .m = 900,
            .degenerate_count = 10,
            .repeat_streak = 1,
            .expected_cooldown = 8
        },
        {
            .name = "phase1 dir-stabilize escalates cooldown on repeated large-degenerate events",
            .m = 900,
            .degenerate_count = 80,
            .repeat_streak = 7,
            .expected_cooldown = 24
        },
        {
            .name = "phase1 dir-stabilize cooldown is capped",
            .m = 1500,
            .degenerate_count = 120,
            .repeat_streak = 40,
            .expected_cooldown = 64
        }
    };
    const DirSkipRcOnlyCase dir_skip_rc_only_cases[] = {
        {
            .name = "phase1 dir-skip rc-only disabled for small matrices",
            .m = 500,
            .degenerate_count = 100,
            .no_pivot_streak = 20,
            .expected_allow = 0
        },
        {
            .name = "phase1 dir-skip rc-only enabled by high degeneracy on large matrix",
            .m = 900,
            .degenerate_count = 40,
            .no_pivot_streak = 1,
            .expected_allow = 1
        },
        {
            .name = "phase1 dir-skip rc-only enabled by sustained no-pivot streak",
            .m = 900,
            .degenerate_count = 5,
            .no_pivot_streak = 12,
            .expected_allow = 1
        },
        {
            .name = "phase1 dir-skip rc-only disabled before no-pivot threshold",
            .m = 900,
            .degenerate_count = 5,
            .no_pivot_streak = 3,
            .expected_allow = 0
        }
    };
    const DirStabilizeForceCase dir_stabilize_force_cases[] = {
        {
            .name = "phase1 dir-force uses baseline threshold before cooldown",
            .dir_inf_ratio = 150.0,
            .cooldown_active = 0,
            .expected_force = 1
        },
        {
            .name = "phase1 dir-force suppresses moderate extremes during cooldown",
            .dir_inf_ratio = 150.0,
            .cooldown_active = 1,
            .expected_force = 0
        },
        {
            .name = "phase1 dir-force keeps cooldown guard below 1000x",
            .dir_inf_ratio = 350.0,
            .cooldown_active = 1,
            .expected_force = 0
        },
        {
            .name = "phase1 dir-force allows severe extremes during cooldown",
            .dir_inf_ratio = 1500.0,
            .cooldown_active = 1,
            .expected_force = 1
        }
    };
    const DirStabilizeModerateCase dir_stabilize_moderate_cases[] = {
        {
            .name = "phase1 moderate dir defers first event when LU health is good",
            .dir_inf_ratio = 20.0,
            .cooldown_active = 0,
            .lu_health_triggered = 0,
            .pending_repeat = 0,
            .expected_defer = 1
        },
        {
            .name = "phase1 moderate dir does not defer when repeat is pending",
            .dir_inf_ratio = 20.0,
            .cooldown_active = 0,
            .lu_health_triggered = 0,
            .pending_repeat = 1,
            .expected_defer = 0
        },
        {
            .name = "phase1 moderate dir does not defer when LU health requests refactor",
            .dir_inf_ratio = 20.0,
            .cooldown_active = 0,
            .lu_health_triggered = 1,
            .pending_repeat = 0,
            .expected_defer = 0
        },
        {
            .name = "phase1 moderate dir does not defer during cooldown",
            .dir_inf_ratio = 20.0,
            .cooldown_active = 1,
            .lu_health_triggered = 0,
            .pending_repeat = 0,
            .expected_defer = 0
        },
        {
            .name = "phase1 moderate dir does not defer high-ratio events",
            .dir_inf_ratio = 80.0,
            .cooldown_active = 0,
            .lu_health_triggered = 0,
            .pending_repeat = 0,
            .expected_defer = 0
        }
    };
    const NoPivotForceCase no_pivot_force_cases[] = {
        {
            .name = "phase1 no-pivot force triggers on large degenerate streak",
            .m = 1500,
            .degenerate_count = 120,
            .streak = 32,
            .cooldown = 0,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            .expected_force = 1,
            .expected_next_streak = 0,
            .expected_next_cooldown = 24
        },
        {
            .name = "phase1 no-pivot force does not trigger below threshold",
            .m = 1500,
            .degenerate_count = 120,
            .streak = 30,
            .cooldown = 0,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
            .expected_force = 0,
            .expected_next_streak = 31,
            .expected_next_cooldown = 0
        },
        {
            .name = "phase1 no-pivot force respects active cooldown",
            .m = 1500,
            .degenerate_count = 120,
            .streak = 40,
            .cooldown = 5,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
            .expected_force = 0,
            .expected_next_streak = 41,
            .expected_next_cooldown = 5
        }
    };
    const SoftLUPolicyCooldownCase soft_lu_policy_cooldown_cases[] = {
        {
            .name = "phase1 soft-lu defer applies periodic cooldown on large degenerate run",
            .m = 1500,
            .degenerate_count = 80,
            .periodic_interval = 24,
            .lu_soft_cost_deferred = 1,
            .periodic_policy_cooldown = 0,
            .expected_applied = 1,
            .expected_next_cooldown = 12
        },
        {
            .name = "phase1 soft-lu defer can extend existing periodic cooldown",
            .m = 1500,
            .degenerate_count = 80,
            .periodic_interval = 48,
            .lu_soft_cost_deferred = 1,
            .periodic_policy_cooldown = 20,
            .expected_applied = 1,
            .expected_next_cooldown = 24
        },
        {
            .name = "phase1 soft-lu cooldown inactive on small problems",
            .m = 500,
            .degenerate_count = 80,
            .periodic_interval = 24,
            .lu_soft_cost_deferred = 1,
            .periodic_policy_cooldown = 0,
            .expected_applied = 0,
            .expected_next_cooldown = 0
        }
    };
    const Phase2RecomputeIntervalCase phase2_recompute_interval_cases[] = {
        {
            .name = "phase2 periodic recompute keeps base interval on small matrix",
            .m = 900,
            .use_bland = 0,
            .degenerate_count = 200,
            .spike_pool_used = 0,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .expected_interval = 200
        },
        {
            .name = "phase2 periodic recompute relaxes cadence on large healthy degeneracy",
            .m = 1500,
            .use_bland = 0,
            .degenerate_count = 200,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .expected_interval = 286
        },
        {
            .name = "phase2 periodic recompute reaches max interval on extreme size+degeneracy",
            .m = 2600,
            .use_bland = 0,
            .degenerate_count = 400,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .expected_interval = 600
        },
        {
            .name = "phase2 periodic recompute blocks relaxation on poor LU health",
            .m = 2600,
            .use_bland = 0,
            .degenerate_count = 400,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e8,
            .growth_factor = 10.0,
            .expected_interval = 200
        },
        {
            .name = "phase2 periodic recompute blocks relaxation under bland mode",
            .m = 2600,
            .use_bland = 1,
            .degenerate_count = 400,
            .spike_pool_used = 10,
            .spike_pool_capacity = 100,
            .cond_estimate = 1e4,
            .growth_factor = 10.0,
            .expected_interval = 200
        }
    };

    int pass = 0;
    int total_policy = (int)(sizeof(cases) / sizeof(cases[0]));
    int total_sched = (int)(sizeof(scheduler_cases) / sizeof(scheduler_cases[0]));
    int total_lu_health = (int)(sizeof(lu_health_cases) / sizeof(lu_health_cases[0]));
    int total_soft_lu_defer = (int)(sizeof(soft_lu_defer_cases) / sizeof(soft_lu_defer_cases[0]));
    int total_periodic_cost_defer = (int)(sizeof(periodic_cost_defer_cases) / sizeof(periodic_cost_defer_cases[0]));
    int total_dir_stabilize = (int)(sizeof(dir_stabilize_cooldown_cases) / sizeof(dir_stabilize_cooldown_cases[0]));
    int total_dir_skip_rc_only = (int)(sizeof(dir_skip_rc_only_cases) / sizeof(dir_skip_rc_only_cases[0]));
    int total_dir_force = (int)(sizeof(dir_stabilize_force_cases) / sizeof(dir_stabilize_force_cases[0]));
    int total_dir_moderate = (int)(sizeof(dir_stabilize_moderate_cases) / sizeof(dir_stabilize_moderate_cases[0]));
    int total_no_pivot = (int)(sizeof(no_pivot_force_cases) / sizeof(no_pivot_force_cases[0]));
    int total_soft_lu_policy_cd = (int)(sizeof(soft_lu_policy_cooldown_cases) / sizeof(soft_lu_policy_cooldown_cases[0]));
    int total_phase2_recompute_interval =
        (int)(sizeof(phase2_recompute_interval_cases) /
              sizeof(phase2_recompute_interval_cases[0]));
    int total = total_policy + total_sched + total_lu_health + total_soft_lu_defer +
                total_periodic_cost_defer + total_dir_stabilize + total_dir_force +
                total_dir_moderate + total_no_pivot + total_soft_lu_policy_cd +
                total_phase2_recompute_interval;
    total += total_dir_skip_rc_only;

    for (int i = 0; i < total_policy; i++) {
        pass += run_case(&cases[i]);
    }
    for (int i = 0; i < total_sched; i++) {
        pass += run_scheduler_case(&scheduler_cases[i]);
    }
    for (int i = 0; i < total_lu_health; i++) {
        pass += run_lu_health_case(&lu_health_cases[i]);
    }
    for (int i = 0; i < total_soft_lu_defer; i++) {
        pass += run_soft_lu_defer_case(&soft_lu_defer_cases[i]);
    }
    for (int i = 0; i < total_periodic_cost_defer; i++) {
        pass += run_periodic_cost_defer_case(&periodic_cost_defer_cases[i]);
    }
    for (int i = 0; i < total_dir_stabilize; i++) {
        pass += run_dir_stabilize_cooldown_case(&dir_stabilize_cooldown_cases[i]);
    }
    for (int i = 0; i < total_dir_skip_rc_only; i++) {
        pass += run_dir_skip_rc_only_case(&dir_skip_rc_only_cases[i]);
    }
    for (int i = 0; i < total_dir_force; i++) {
        pass += run_dir_stabilize_force_case(&dir_stabilize_force_cases[i]);
    }
    for (int i = 0; i < total_dir_moderate; i++) {
        pass += run_dir_stabilize_moderate_case(&dir_stabilize_moderate_cases[i]);
    }
    for (int i = 0; i < total_no_pivot; i++) {
        pass += run_no_pivot_force_case(&no_pivot_force_cases[i]);
    }
    for (int i = 0; i < total_soft_lu_policy_cd; i++) {
        pass += run_soft_lu_policy_cooldown_case(&soft_lu_policy_cooldown_cases[i]);
    }
    for (int i = 0; i < total_phase2_recompute_interval; i++) {
        pass += run_phase2_recompute_interval_case(&phase2_recompute_interval_cases[i]);
    }

    printf("\nPolicy cases passed: %d/%d\n", pass, total);
    return (pass == total) ? 0 : 1;
}
