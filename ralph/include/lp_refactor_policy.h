#ifndef LP_REFACTOR_POLICY_H
#define LP_REFACTOR_POLICY_H

typedef struct {
    int interval;
    int min_update_age;
    double interval_pressure;
    double run_pressure;
} LPPeriodicRefactorPolicy;

typedef struct {
    int hard_trigger;
    int soft_trigger;
    int soft_breach_streak_next;
    int soft_breach_threshold;
    int soft_min_update_age;
    int refactor_now;
} LPLUHealthRefactorDecision;

typedef enum {
    LP_PERIODIC_COST_DAMPEN_DEFER = 0,
    LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE = 1,
    LP_PERIODIC_COST_DAMPEN_BLOCK_SMALL_M = 2,
    LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_INPUTS = 3,
    LP_PERIODIC_COST_DAMPEN_BLOCK_WARMUP = 4,
    LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_COST = 5,
    LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO = 6,
    LP_PERIODIC_COST_DAMPEN_BLOCK_UPDATE_RESERVE = 7
} LPPeriodicCostDampenReason;

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
                                                               double feedback_bias);

int lp_refactor_policy_should_run_metrics(int iter,
                                          int num_updates,
                                          const LPPeriodicRefactorPolicy *policy,
                                          int use_bland,
                                          int degenerate_count);

int lp_refactor_policy_phase2_cooldown_eligible(int m,
                                                int degenerate_count,
                                                int spike_pool_used,
                                                int spike_pool_capacity,
                                                double cond_estimate,
                                                double growth_factor);

int lp_refactor_policy_phase2_periodic_recompute_interval(
    int m,
    int use_bland,
    int degenerate_count,
    int spike_pool_used,
    int spike_pool_capacity,
    double cond_estimate,
    double growth_factor);

int lp_refactor_policy_phase2_cooldown_window_updates(int interval);

int lp_refactor_policy_phase1_cooldown_eligible(int m,
                                                int degenerate_count,
                                                int periodic_policy_refactor_count,
                                                int spike_pool_used,
                                                int spike_pool_capacity,
                                                double cond_estimate,
                                                double growth_factor);

int lp_refactor_policy_phase1_cooldown_window_updates(int interval);

int lp_refactor_policy_phase1_dir_stabilize_cooldown_updates(int m,
                                                             int degenerate_count,
                                                             int repeat_streak);

int lp_refactor_policy_phase1_dir_skip_allow_rc_only(int m,
                                                     int degenerate_count,
                                                     int no_pivot_streak);

int lp_refactor_policy_phase1_dir_stabilize_force_extreme_ratio(
    double dir_inf_ratio,
    int cooldown_active);

int lp_refactor_policy_phase1_dir_stabilize_should_defer_moderate(
    double dir_inf_ratio,
    int cooldown_active,
    int lu_health_triggered,
    int pending_repeat);

LPLUHealthRefactorDecision lp_refactor_policy_lu_health_refactor_decision(
    int m,
    int use_ft_updates,
    int num_updates,
    int max_updates,
    int spike_pool_used,
    int spike_pool_capacity,
    double cond_estimate,
    double growth_factor,
    int soft_breach_streak);

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
                                                      double iter_cost_ewma_ms);

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
                                                         int iter_cost_samples);

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
    int iter_cost_samples);

const char* lp_refactor_policy_periodic_cost_dampen_reason_string(
    LPPeriodicCostDampenReason reason);

#endif
