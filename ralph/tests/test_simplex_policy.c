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

int simplex_phase1_no_pivot_ladder_plan_for_test(int m,
                                                  int degenerate_count,
                                                  int reason,
                                                  int no_pivot_streak,
                                                  int no_progress_streak,
                                                  int force_pivot_mode_active,
                                                  int *refactor_threshold_out);
int simplex_phase1_no_pivot_ladder_rescue_guard_plan_for_test(
    int ladder_step,
    int rescue_cooldown_iters,
    int rescue_fail_streak);
int simplex_phase1_dir_skip_rescue_cadence_plan_for_test(
    int dir_skip_event_streak);

int simplex_phase1_force_pivot_mode_plan_for_test(int m,
                                                   int degenerate_count,
                                                   int dir_skip_no_recompute_streak,
                                                   int active_budget,
                                                   int *next_streak_out,
                                                   int *next_budget_out);

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
    int *hard_bypass_out);

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
    int dual_rescue_fail_streak);

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
    int dual_rescue_fail_streak);

int simplex_phase1_soft_lu_policy_cooldown_plan_for_test(
    int m,
    int degenerate_count,
    int periodic_interval,
    int lu_soft_cost_deferred,
    int periodic_policy_cooldown,
    int *next_cooldown_out);

int simplex_smcp_excl_should_skip_for_test(int smcp_excl,
                                           int var_status,
                                           double lb,
                                           double ub,
                                           double tol_bnd);
int simplex_smcp_shift_allows_perturb_for_test(int smcp_shift);

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
    int smcp_excl;
    int var_status;
    double lb;
    double ub;
    double tol_bnd;
    int expected_skip;
} SmcpExclCase;

static int run_smcp_excl_case(const SmcpExclCase *tc) {
    int skip = simplex_smcp_excl_should_skip_for_test(tc->smcp_excl,
                                                      tc->var_status,
                                                      tc->lb,
                                                      tc->ub,
                                                      tc->tol_bnd);
    if (skip != tc->expected_skip) {
        fprintf(stderr, "FAIL: %s (expected=%d got=%d)\n",
                tc->name, tc->expected_skip, skip);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_smcp_shift_case(const char *name, int smcp_shift, int expected_allow) {
    int allow = simplex_smcp_shift_allows_perturb_for_test(smcp_shift);
    if (allow != expected_allow) {
        fprintf(stderr, "FAIL: %s (expected=%d got=%d)\n", name, expected_allow, allow);
        return 0;
    }
    printf("PASS: %s\n", name);
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
    int m;
    int degenerate_count;
    int no_pivot_streak;
    int no_recompute_streak;
    int expected_skip;
} DirSkipNoRecomputeCase;

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
    int reason;
    int no_pivot_streak;
    int no_progress_streak;
    int force_pivot_mode_active;
    int expected_step;
    int min_threshold;
    int max_threshold;
} NoPivotLadderCase;

typedef struct {
    const char *name;
    int ladder_step;
    int rescue_cooldown_iters;
    int rescue_fail_streak;
    int expected_step;
} NoPivotLadderGuardCase;

typedef struct {
    const char *name;
    int dir_skip_event_streak;
    int expected_due;
} DirSkipRescueCadenceCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int dir_skip_no_recompute_streak;
    int active_budget;
    int expected_activate;
    int expected_next_streak;
    int expected_next_budget;
} ForcePivotModeCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int dir_skip_event_streak;
    int no_progress_streak;
    int escape_cooldown;
    int force_extreme_dir;
    int force_lu_health;
    int lu_hard_trigger;
    int expected_suppress;
    int expected_triggered;
    int expected_hard_bypass;
    int expected_next_cooldown;
} DirStabilizeEscapeGateCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int no_progress_streak;
    int force_pivot_mode_active;
    int force_extreme_dir;
    int force_lu_health;
    int lu_hard_trigger;
    int dual_rescue_attempts;
    int dual_rescue_successes;
    int dual_rescue_fail_streak;
    int expected_relax;
} ForcePivotRelaxCase;

typedef struct {
    const char *name;
    int m;
    int degenerate_count;
    int no_progress_streak;
    double dir_inf_ratio;
    int force_extreme_dir;
    int force_lu_health;
    int lu_hard_trigger;
    int dual_rescue_attempts;
    int dual_rescue_successes;
    int dual_rescue_fail_streak;
    int expected_relax;
} ForceExtremeRelaxCase;

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

static int run_dir_skip_no_recompute_case(const DirSkipNoRecomputeCase *tc) {
    int skip = lp_refactor_policy_phase1_dir_skip_should_skip_recompute(
        tc->m,
        tc->degenerate_count,
        tc->no_pivot_streak,
        tc->no_recompute_streak);
    if (skip != tc->expected_skip) {
        fprintf(stderr, "FAIL: %s (expected skip=%d got=%d)\n",
                tc->name, tc->expected_skip, skip);
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

static int run_no_pivot_ladder_case(const NoPivotLadderCase *tc) {
    int threshold = -1;
    int step = simplex_phase1_no_pivot_ladder_plan_for_test(
        tc->m,
        tc->degenerate_count,
        tc->reason,
        tc->no_pivot_streak,
        tc->no_progress_streak,
        tc->force_pivot_mode_active,
        &threshold);
    if (step != tc->expected_step) {
        fprintf(stderr, "FAIL: %s (expected step=%d got=%d)\n",
                tc->name, tc->expected_step, step);
        return 0;
    }
    if (threshold < tc->min_threshold || threshold > tc->max_threshold) {
        fprintf(stderr, "FAIL: %s (expected threshold in [%d,%d] got=%d)\n",
                tc->name, tc->min_threshold, tc->max_threshold, threshold);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_no_pivot_ladder_guard_case(const NoPivotLadderGuardCase *tc) {
    int step = simplex_phase1_no_pivot_ladder_rescue_guard_plan_for_test(
        tc->ladder_step,
        tc->rescue_cooldown_iters,
        tc->rescue_fail_streak);
    if (step != tc->expected_step) {
        fprintf(stderr, "FAIL: %s (expected step=%d got=%d)\n",
                tc->name, tc->expected_step, step);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_dir_skip_rescue_cadence_case(const DirSkipRescueCadenceCase *tc) {
    int due = simplex_phase1_dir_skip_rescue_cadence_plan_for_test(
        tc->dir_skip_event_streak);
    if (due != tc->expected_due) {
        fprintf(stderr, "FAIL: %s (expected due=%d got=%d)\n",
                tc->name, tc->expected_due, due);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_force_pivot_mode_case(const ForcePivotModeCase *tc) {
    int next_streak = -1;
    int next_budget = -1;
    int activate = simplex_phase1_force_pivot_mode_plan_for_test(
        tc->m,
        tc->degenerate_count,
        tc->dir_skip_no_recompute_streak,
        tc->active_budget,
        &next_streak,
        &next_budget);
    if (activate != tc->expected_activate) {
        fprintf(stderr, "FAIL: %s (expected activate=%d got=%d)\n",
                tc->name, tc->expected_activate, activate);
        return 0;
    }
    if (next_streak != tc->expected_next_streak) {
        fprintf(stderr, "FAIL: %s (expected next_streak=%d got=%d)\n",
                tc->name, tc->expected_next_streak, next_streak);
        return 0;
    }
    if (next_budget != tc->expected_next_budget) {
        fprintf(stderr, "FAIL: %s (expected next_budget=%d got=%d)\n",
                tc->name, tc->expected_next_budget, next_budget);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_dir_stabilize_escape_gate_case(
    const DirStabilizeEscapeGateCase *tc) {
    int next_cooldown = -1;
    int triggered = -1;
    int hard_bypass = -1;
    int suppress = simplex_phase1_dir_stabilize_escape_gate_plan_for_test(
        tc->m,
        tc->degenerate_count,
        tc->dir_skip_event_streak,
        tc->no_progress_streak,
        tc->escape_cooldown,
        tc->force_extreme_dir,
        tc->force_lu_health,
        tc->lu_hard_trigger,
        &next_cooldown,
        &triggered,
        &hard_bypass);
    if (suppress != tc->expected_suppress) {
        fprintf(stderr, "FAIL: %s (expected suppress=%d got=%d)\n",
                tc->name, tc->expected_suppress, suppress);
        return 0;
    }
    if (triggered != tc->expected_triggered) {
        fprintf(stderr, "FAIL: %s (expected triggered=%d got=%d)\n",
                tc->name, tc->expected_triggered, triggered);
        return 0;
    }
    if (hard_bypass != tc->expected_hard_bypass) {
        fprintf(stderr, "FAIL: %s (expected hard_bypass=%d got=%d)\n",
                tc->name, tc->expected_hard_bypass, hard_bypass);
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

static int run_force_pivot_relax_case(const ForcePivotRelaxCase *tc) {
    int relax = simplex_phase1_force_pivot_relax_plan_for_test(
        tc->m,
        tc->degenerate_count,
        tc->no_progress_streak,
        tc->force_pivot_mode_active,
        tc->force_extreme_dir,
        tc->force_lu_health,
        tc->lu_hard_trigger,
        tc->dual_rescue_attempts,
        tc->dual_rescue_successes,
        tc->dual_rescue_fail_streak);
    if (relax != tc->expected_relax) {
        fprintf(stderr, "FAIL: %s (expected relax=%d got=%d)\n",
                tc->name, tc->expected_relax, relax);
        return 0;
    }
    printf("PASS: %s\n", tc->name);
    return 1;
}

static int run_force_extreme_relax_case(const ForceExtremeRelaxCase *tc) {
    int relax = simplex_phase1_force_extreme_relax_plan_for_test(
        tc->m,
        tc->degenerate_count,
        tc->no_progress_streak,
        tc->dir_inf_ratio,
        tc->force_extreme_dir,
        tc->force_lu_health,
        tc->lu_hard_trigger,
        tc->dual_rescue_attempts,
        tc->dual_rescue_successes,
        tc->dual_rescue_fail_streak);
    if (relax != tc->expected_relax) {
        fprintf(stderr, "FAIL: %s (expected relax=%d got=%d)\n",
                tc->name, tc->expected_relax, relax);
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
    const DirSkipNoRecomputeCase dir_skip_no_recompute_cases[] = {
        {
            .name = "phase1 dir-skip no-recompute disabled when rc-only not allowed",
            .m = 500,
            .degenerate_count = 100,
            .no_pivot_streak = 20,
            .no_recompute_streak = 0,
            .expected_skip = 0
        },
        {
            .name = "phase1 dir-skip no-recompute allowed under guard budget",
            .m = 900,
            .degenerate_count = 40,
            .no_pivot_streak = 1,
            .no_recompute_streak = 3,
            .expected_skip = 1
        },
        {
            .name = "phase1 dir-skip no-recompute blocked at guard boundary",
            .m = 900,
            .degenerate_count = 5,
            .no_pivot_streak = 12,
            .no_recompute_streak = 8,
            .expected_skip = 0
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
    const NoPivotLadderCase no_pivot_ladder_cases[] = {
        {
            .name = "phase1 no-pivot ladder defers early ratio breakdown events",
            .m = 1500,
            .degenerate_count = 120,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            .no_pivot_streak = 3,
            .no_progress_streak = 2,
            .force_pivot_mode_active = 0,
            .expected_step = 0,
            .min_threshold = 10,
            .max_threshold = 14
        },
        {
            .name = "phase1 no-pivot ladder escalates to dual rescue before refactor",
            .m = 1500,
            .degenerate_count = 120,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            .no_pivot_streak = 8,
            .no_progress_streak = 6,
            .force_pivot_mode_active = 0,
            .expected_step = 1,
            .min_threshold = 10,
            .max_threshold = 14
        },
        {
            .name = "phase1 no-pivot ladder forces refactor on sustained no-progress",
            .m = 1500,
            .degenerate_count = 120,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            .no_pivot_streak = 12,
            .no_progress_streak = 12,
            .force_pivot_mode_active = 0,
            .expected_step = 2,
            .min_threshold = 10,
            .max_threshold = 14
        },
        {
            .name = "phase1 no-pivot ladder can trigger rescue from no-pivot streak",
            .m = 1500,
            .degenerate_count = 120,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            .no_pivot_streak = 12,
            .no_progress_streak = 1,
            .force_pivot_mode_active = 0,
            .expected_step = 1,
            .min_threshold = 10,
            .max_threshold = 14
        },
        {
            .name = "phase1 no-pivot ladder keeps pivot-fail path aggressive",
            .m = 1500,
            .degenerate_count = 120,
            .reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
            .no_pivot_streak = 6,
            .no_progress_streak = 5,
            .force_pivot_mode_active = 1,
            .expected_step = 2,
            .min_threshold = 3,
            .max_threshold = 6
        }
    };
    const NoPivotLadderGuardCase no_pivot_ladder_guard_cases[] = {
        {
            .name = "phase1 no-pivot ladder rescue guard blocks on cooldown",
            .ladder_step = 1,
            .rescue_cooldown_iters = 3,
            .rescue_fail_streak = 0,
            .expected_step = 0
        },
        {
            .name = "phase1 no-pivot ladder rescue guard forces refactor on fail cap",
            .ladder_step = 1,
            .rescue_cooldown_iters = 0,
            .rescue_fail_streak = 3,
            .expected_step = 2
        },
        {
            .name = "phase1 no-pivot ladder rescue guard allows rescue when clear",
            .ladder_step = 1,
            .rescue_cooldown_iters = 0,
            .rescue_fail_streak = 0,
            .expected_step = 1
        }
    };
    const DirSkipRescueCadenceCase dir_skip_rescue_cadence_cases[] = {
        {
            .name = "phase1 dir-skip rescue cadence not due before threshold",
            .dir_skip_event_streak = 23,
            .expected_due = 0
        },
        {
            .name = "phase1 dir-skip rescue cadence due at threshold multiple",
            .dir_skip_event_streak = 24,
            .expected_due = 1
        },
        {
            .name = "phase1 dir-skip rescue cadence repeats on period",
            .dir_skip_event_streak = 36,
            .expected_due = 1
        },
        {
            .name = "phase1 dir-skip rescue cadence off period",
            .dir_skip_event_streak = 35,
            .expected_due = 0
        }
    };
    const ForcePivotModeCase force_pivot_mode_cases[] = {
        {
            .name = "force-pivot mode activates after repeated dir-skip no-recompute",
            .m = 1500,
            .degenerate_count = 120,
            .dir_skip_no_recompute_streak = 32,
            .active_budget = 0,
            .expected_activate = 1,
            .expected_next_streak = 0,
            .expected_next_budget = 20
        },
        {
            .name = "force-pivot mode does not activate below threshold",
            .m = 1500,
            .degenerate_count = 120,
            .dir_skip_no_recompute_streak = 31,
            .active_budget = 0,
            .expected_activate = 0,
            .expected_next_streak = 31,
            .expected_next_budget = 0
        },
        {
            .name = "force-pivot mode keeps active budget without re-arming",
            .m = 1500,
            .degenerate_count = 120,
            .dir_skip_no_recompute_streak = 64,
            .active_budget = 3,
            .expected_activate = 0,
            .expected_next_streak = 64,
            .expected_next_budget = 3
        }
    };
    const DirStabilizeEscapeGateCase dir_stabilize_escape_gate_cases[] = {
        {
            .name = "dir-stabilize escape gate suppresses lu-health force on chronic dir-skip treadmill",
            .m = 233,
            .degenerate_count = 120,
            .dir_skip_event_streak = 40,
            .no_progress_streak = 8,
            .escape_cooldown = 0,
            .force_extreme_dir = 0,
            .force_lu_health = 1,
            .lu_hard_trigger = 0,
            .expected_suppress = 1,
            .expected_triggered = 1,
            .expected_hard_bypass = 0,
            .expected_next_cooldown = 64
        },
        {
            .name = "dir-stabilize escape gate can trigger from no-progress streak alone",
            .m = 233,
            .degenerate_count = 120,
            .dir_skip_event_streak = 0,
            .no_progress_streak = 24,
            .escape_cooldown = 0,
            .force_extreme_dir = 0,
            .force_lu_health = 1,
            .lu_hard_trigger = 0,
            .expected_suppress = 1,
            .expected_triggered = 1,
            .expected_hard_bypass = 0,
            .expected_next_cooldown = 64
        },
        {
            .name = "dir-stabilize escape gate keeps suppressing while cooldown active",
            .m = 233,
            .degenerate_count = 120,
            .dir_skip_event_streak = 8,
            .no_progress_streak = 2,
            .escape_cooldown = 20,
            .force_extreme_dir = 0,
            .force_lu_health = 1,
            .lu_hard_trigger = 0,
            .expected_suppress = 1,
            .expected_triggered = 0,
            .expected_hard_bypass = 0,
            .expected_next_cooldown = 20
        },
        {
            .name = "dir-stabilize escape gate bypasses suppression on hard lu-health trigger",
            .m = 233,
            .degenerate_count = 120,
            .dir_skip_event_streak = 40,
            .no_progress_streak = 8,
            .escape_cooldown = 0,
            .force_extreme_dir = 0,
            .force_lu_health = 1,
            .lu_hard_trigger = 1,
            .expected_suppress = 0,
            .expected_triggered = 0,
            .expected_hard_bypass = 1,
            .expected_next_cooldown = 0
        },
        {
            .name = "dir-stabilize escape gate never suppresses extreme-direction force",
            .m = 233,
            .degenerate_count = 120,
            .dir_skip_event_streak = 40,
            .no_progress_streak = 8,
            .escape_cooldown = 0,
            .force_extreme_dir = 1,
            .force_lu_health = 1,
            .lu_hard_trigger = 0,
            .expected_suppress = 0,
            .expected_triggered = 0,
            .expected_hard_bypass = 0,
            .expected_next_cooldown = 0
        }
    };
    const ForcePivotRelaxCase force_pivot_relax_cases[] = {
        {
            .name = "force-pivot relax applies on large degenerate run with strong dual-rescue success",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .force_pivot_mode_active = 1,
            .force_extreme_dir = 0,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 19,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 1
        },
        {
            .name = "force-pivot relax blocked by LU hard trigger",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .force_pivot_mode_active = 1,
            .force_extreme_dir = 0,
            .force_lu_health = 0,
            .lu_hard_trigger = 1,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 20,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
        },
        {
            .name = "force-pivot relax blocked by weak dual-rescue success rate",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .force_pivot_mode_active = 1,
            .force_extreme_dir = 0,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 12,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
        },
        {
            .name = "force-pivot relax blocked by sustained no-progress streak",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 16,
            .force_pivot_mode_active = 1,
            .force_extreme_dir = 0,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 20,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
        },
        {
            .name = "force-pivot relax blocked on small matrices",
            .m = 500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .force_pivot_mode_active = 1,
            .force_extreme_dir = 0,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 20,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
        }
    };
    const ForceExtremeRelaxCase force_extreme_relax_cases[] = {
        {
            .name = "force-extreme relax applies for moderate extreme ratio under stable LU and strong dual rescue",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .dir_inf_ratio = 150.0,
            .force_extreme_dir = 1,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 19,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 1
        },
        {
            .name = "force-extreme relax blocked for severe direction ratio",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .dir_inf_ratio = 450.0,
            .force_extreme_dir = 1,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 20,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
        },
        {
            .name = "force-extreme relax blocked by LU-health trigger",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .dir_inf_ratio = 150.0,
            .force_extreme_dir = 1,
            .force_lu_health = 1,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 20,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
        },
        {
            .name = "force-extreme relax blocked by poor dual-rescue effectiveness",
            .m = 1500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .dir_inf_ratio = 150.0,
            .force_extreme_dir = 1,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 12,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
        },
        {
            .name = "force-extreme relax blocked on small matrices",
            .m = 500,
            .degenerate_count = 120,
            .no_progress_streak = 6,
            .dir_inf_ratio = 150.0,
            .force_extreme_dir = 1,
            .force_lu_health = 0,
            .lu_hard_trigger = 0,
            .dual_rescue_attempts = 20,
            .dual_rescue_successes = 20,
            .dual_rescue_fail_streak = 0,
            .expected_relax = 0
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
    const SmcpExclCase smcp_excl_cases[] = {
        {
            .name = "smcp excl skips explicit fixed status",
            .smcp_excl = 0,
            .var_status = (int)RALPH_FIXED,
            .lb = 2.0,
            .ub = 2.0,
            .tol_bnd = 1e-7,
            .expected_skip = 1
        },
        {
            .name = "smcp excl skips boxed non-basic when enabled",
            .smcp_excl = 1,
            .var_status = (int)RALPH_NONBASIC_LOWER,
            .lb = 5.0,
            .ub = 5.0 + 5e-13,
            .tol_bnd = 1e-7,
            .expected_skip = 1
        },
        {
            .name = "smcp excl keeps boxed non-basic when disabled",
            .smcp_excl = 0,
            .var_status = (int)RALPH_NONBASIC_LOWER,
            .lb = 5.0,
            .ub = 5.0 + 5e-13,
            .tol_bnd = 1e-7,
            .expected_skip = 0
        },
        {
            .name = "smcp excl keeps wide-interval non-basic when enabled",
            .smcp_excl = 1,
            .var_status = (int)RALPH_NONBASIC_UPPER,
            .lb = -1.0,
            .ub = 3.0,
            .tol_bnd = 1e-7,
            .expected_skip = 0
        }
    };
    const int smcp_shift_cases[][2] = {
        {0, 0},
        {1, 1}
    };
    const char *smcp_shift_case_names[] = {
        "smcp shift disables perturbation when off",
        "smcp shift enables perturbation when on"
    };

    int pass = 0;
    int total_policy = (int)(sizeof(cases) / sizeof(cases[0]));
    int total_sched = (int)(sizeof(scheduler_cases) / sizeof(scheduler_cases[0]));
    int total_lu_health = (int)(sizeof(lu_health_cases) / sizeof(lu_health_cases[0]));
    int total_soft_lu_defer = (int)(sizeof(soft_lu_defer_cases) / sizeof(soft_lu_defer_cases[0]));
    int total_periodic_cost_defer = (int)(sizeof(periodic_cost_defer_cases) / sizeof(periodic_cost_defer_cases[0]));
    int total_dir_stabilize = (int)(sizeof(dir_stabilize_cooldown_cases) / sizeof(dir_stabilize_cooldown_cases[0]));
    int total_dir_skip_rc_only = (int)(sizeof(dir_skip_rc_only_cases) / sizeof(dir_skip_rc_only_cases[0]));
    int total_dir_skip_no_recompute =
        (int)(sizeof(dir_skip_no_recompute_cases) /
              sizeof(dir_skip_no_recompute_cases[0]));
    int total_dir_force = (int)(sizeof(dir_stabilize_force_cases) / sizeof(dir_stabilize_force_cases[0]));
    int total_dir_moderate = (int)(sizeof(dir_stabilize_moderate_cases) / sizeof(dir_stabilize_moderate_cases[0]));
    int total_no_pivot = (int)(sizeof(no_pivot_force_cases) / sizeof(no_pivot_force_cases[0]));
    int total_no_pivot_ladder = (int)(sizeof(no_pivot_ladder_cases) / sizeof(no_pivot_ladder_cases[0]));
    int total_no_pivot_ladder_guard = (int)(sizeof(no_pivot_ladder_guard_cases) / sizeof(no_pivot_ladder_guard_cases[0]));
    int total_dir_skip_rescue_cadence = (int)(sizeof(dir_skip_rescue_cadence_cases) / sizeof(dir_skip_rescue_cadence_cases[0]));
    int total_force_pivot_mode = (int)(sizeof(force_pivot_mode_cases) / sizeof(force_pivot_mode_cases[0]));
    int total_dir_escape_gate =
        (int)(sizeof(dir_stabilize_escape_gate_cases) /
              sizeof(dir_stabilize_escape_gate_cases[0]));
    int total_force_pivot_relax =
        (int)(sizeof(force_pivot_relax_cases) /
              sizeof(force_pivot_relax_cases[0]));
    int total_force_extreme_relax =
        (int)(sizeof(force_extreme_relax_cases) /
              sizeof(force_extreme_relax_cases[0]));
    int total_soft_lu_policy_cd = (int)(sizeof(soft_lu_policy_cooldown_cases) / sizeof(soft_lu_policy_cooldown_cases[0]));
    int total_phase2_recompute_interval =
        (int)(sizeof(phase2_recompute_interval_cases) /
              sizeof(phase2_recompute_interval_cases[0]));
    int total_smcp_excl =
        (int)(sizeof(smcp_excl_cases) / sizeof(smcp_excl_cases[0]));
    int total_smcp_shift =
        (int)(sizeof(smcp_shift_cases) / sizeof(smcp_shift_cases[0]));
    int total = total_policy + total_sched + total_lu_health + total_soft_lu_defer +
                total_periodic_cost_defer + total_dir_stabilize + total_dir_force +
                total_dir_moderate + total_no_pivot + total_no_pivot_ladder +
                total_no_pivot_ladder_guard +
                total_dir_skip_rescue_cadence +
                total_force_pivot_mode +
                total_dir_escape_gate +
                total_force_pivot_relax +
                total_force_extreme_relax +
                total_soft_lu_policy_cd +
                total_phase2_recompute_interval;
    total += total_dir_skip_rc_only;
    total += total_dir_skip_no_recompute;
    total += total_smcp_excl;
    total += total_smcp_shift;

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
    for (int i = 0; i < total_dir_skip_no_recompute; i++) {
        pass += run_dir_skip_no_recompute_case(&dir_skip_no_recompute_cases[i]);
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
    for (int i = 0; i < total_no_pivot_ladder; i++) {
        pass += run_no_pivot_ladder_case(&no_pivot_ladder_cases[i]);
    }
    for (int i = 0; i < total_no_pivot_ladder_guard; i++) {
        pass += run_no_pivot_ladder_guard_case(&no_pivot_ladder_guard_cases[i]);
    }
    for (int i = 0; i < total_dir_skip_rescue_cadence; i++) {
        pass += run_dir_skip_rescue_cadence_case(&dir_skip_rescue_cadence_cases[i]);
    }
    for (int i = 0; i < total_force_pivot_mode; i++) {
        pass += run_force_pivot_mode_case(&force_pivot_mode_cases[i]);
    }
    for (int i = 0; i < total_dir_escape_gate; i++) {
        pass += run_dir_stabilize_escape_gate_case(&dir_stabilize_escape_gate_cases[i]);
    }
    for (int i = 0; i < total_force_pivot_relax; i++) {
        pass += run_force_pivot_relax_case(&force_pivot_relax_cases[i]);
    }
    for (int i = 0; i < total_force_extreme_relax; i++) {
        pass += run_force_extreme_relax_case(&force_extreme_relax_cases[i]);
    }
    for (int i = 0; i < total_soft_lu_policy_cd; i++) {
        pass += run_soft_lu_policy_cooldown_case(&soft_lu_policy_cooldown_cases[i]);
    }
    for (int i = 0; i < total_phase2_recompute_interval; i++) {
        pass += run_phase2_recompute_interval_case(&phase2_recompute_interval_cases[i]);
    }
    for (int i = 0; i < total_smcp_excl; i++) {
        pass += run_smcp_excl_case(&smcp_excl_cases[i]);
    }
    for (int i = 0; i < total_smcp_shift; i++) {
        pass += run_smcp_shift_case(smcp_shift_case_names[i],
                                    smcp_shift_cases[i][0],
                                    smcp_shift_cases[i][1]);
    }

    printf("\nPolicy cases passed: %d/%d\n", pass, total);
    return (pass == total) ? 0 : 1;
}
