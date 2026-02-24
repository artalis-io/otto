#ifndef LP_REFACTOR_POLICY_H
#define LP_REFACTOR_POLICY_H

typedef struct {
    int interval;
    int min_update_age;
    double interval_pressure;
    double run_pressure;
} LPPeriodicRefactorPolicy;

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

int lp_refactor_policy_phase2_cooldown_window_updates(int interval);

int lp_refactor_policy_phase1_cooldown_eligible(int m,
                                                int degenerate_count,
                                                int periodic_policy_refactor_count,
                                                int spike_pool_used,
                                                int spike_pool_capacity,
                                                double cond_estimate,
                                                double growth_factor);

int lp_refactor_policy_phase1_cooldown_window_updates(int interval);

#endif
