#include <stdio.h>
#include "lp.h"

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

    int pass = 0;
    int total_policy = (int)(sizeof(cases) / sizeof(cases[0]));
    int total_sched = (int)(sizeof(scheduler_cases) / sizeof(scheduler_cases[0]));
    int total_lu_health = (int)(sizeof(lu_health_cases) / sizeof(lu_health_cases[0]));
    int total = total_policy + total_sched + total_lu_health;

    for (int i = 0; i < total_policy; i++) {
        pass += run_case(&cases[i]);
    }
    for (int i = 0; i < total_sched; i++) {
        pass += run_scheduler_case(&scheduler_cases[i]);
    }
    for (int i = 0; i < total_lu_health; i++) {
        pass += run_lu_health_case(&lu_health_cases[i]);
    }

    printf("\nPolicy cases passed: %d/%d\n", pass, total);
    return (pass == total) ? 0 : 1;
}
