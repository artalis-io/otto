#ifndef LP_REFACTOR_POLICY_H
#define LP_REFACTOR_POLICY_H

typedef struct {
    int interval;
    int min_update_age;
    double interval_pressure;
    double run_pressure;
} LPPeriodicRefactorPolicy;

typedef struct {
    LPPeriodicRefactorPolicy policy;
    int cooldown_eligible;
    double effective_run_pressure;
    int should_run;
} LPPeriodicRefactorPlan;

typedef struct {
    int hard_trigger;
    int soft_trigger;
    int soft_breach_streak_next;
    int soft_breach_threshold;
    int soft_min_update_age;
    int refactor_now;
} LPLUHealthRefactorDecision;

typedef enum {
    LP_BASIS_ACTION_UPDATE = 0,
    LP_BASIS_ACTION_REFACTOR = 1,
    LP_BASIS_ACTION_REPAIR = 2,
    LP_BASIS_ACTION_ABORT = 3
} LPBasisAction;

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

typedef struct {
    double bias;
    int last_reason;
    int last_interval;
    int hint_interval;
    double hint_pressure;
} LPPeriodicFeedbackState;

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
    double periodic_policy_pressure_decay);

int lp_refactor_policy_should_run_metrics(int iter,
                                          int num_updates,
                                          const LPPeriodicRefactorPolicy *policy,
                                          int use_bland,
                                          int degenerate_count);

double lp_refactor_policy_periodic_pressure_effective(
    int cooldown_eligible,
    double run_pressure,
    double pressure_decay);

double lp_refactor_policy_periodic_pressure_decay_recover(int phase,
                                                          double pressure_decay);

double lp_refactor_policy_periodic_pressure_decay_penalty(int phase,
                                                          double pressure_decay);

int lp_refactor_policy_periodic_cooldown_tick(int cooldown_updates);

int lp_refactor_policy_periodic_cooldown_extend(int cooldown_updates,
                                                int candidate_updates);

void lp_refactor_policy_periodic_post_refactor_update(int phase,
                                                      int cooldown_eligible,
                                                      int periodic_interval,
                                                      int refactor_status,
                                                      int *cooldown_updates_io,
                                                      double *pressure_decay_io);

void lp_refactor_policy_periodic_feedback_set_hint(LPPeriodicFeedbackState *state,
                                                   int interval,
                                                   double run_pressure);

void lp_refactor_policy_periodic_feedback_record_refactor(
    LPPeriodicFeedbackState *state,
    int reason,
    int updates_before,
    int status);

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
                                                     int n,
                                                     int degenerate_count,
                                                     int no_pivot_streak);

int lp_refactor_policy_phase1_dir_skip_should_skip_recompute(
    int m,
    int n,
    int degenerate_count,
    int no_pivot_streak,
    int no_recompute_streak);

int lp_refactor_policy_phase1_dir_stabilize_force_extreme_ratio(
    double dir_inf_ratio,
    int cooldown_active);

int lp_refactor_policy_phase1_dir_stabilize_should_defer_moderate(
    double dir_inf_ratio,
    int cooldown_active,
    int lu_health_triggered,
    int pending_repeat);

int lp_refactor_policy_phase1_small_pivot_refactor_allowed(int force_refactor,
                                                           int repeat_pattern,
                                                           int lu_num_updates);

LPBasisAction lp_refactor_policy_choose_basis_action(double pivot,
                                                     int force_refactor,
                                                     int lu_update_status,
                                                     int repeat_pattern,
                                                     int lu_num_updates,
                                                     double growth_factor,
                                                     double growth_threshold);

const char* lp_refactor_policy_phase1_no_pivot_force_reason_string(int reason);

int lp_refactor_policy_phase1_no_pivot_force_threshold(int m,
                                                       int degenerate_count);

int lp_refactor_policy_phase1_no_pivot_force_transition(int m,
                                                        int degenerate_count,
                                                        int streak,
                                                        int cooldown,
                                                        int *next_streak,
                                                        int *next_cooldown);

int lp_refactor_policy_phase1_no_pivot_ladder_refactor_threshold(
    int m,
    int degenerate_count,
    int reason,
    int force_pivot_mode_active);

int lp_refactor_policy_phase1_no_pivot_ladder_step(
    int m,
    int degenerate_count,
    int reason,
    int no_pivot_streak,
    int no_progress_streak,
    int force_pivot_mode_active,
    int *refactor_threshold_out);

int lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(
    int dir_skip_event_streak);

int lp_refactor_policy_phase1_dir_skip_force_pivot_threshold(
    int m,
    int degenerate_count);

int lp_refactor_policy_phase1_dir_skip_force_pivot_budget(
    int m,
    int degenerate_count);

int lp_refactor_policy_phase1_activate_force_pivot_mode(
    int m,
    int degenerate_count,
    int dir_skip_event_streak,
    int force_pivot_attempt_budget,
    int queue_force_pending,
    int *next_dir_skip_event_streak,
    int *next_force_pivot_attempt_budget,
    int *next_force_pending,
    int *next_force_reason);

int lp_refactor_policy_phase1_window_pressure_force_pivot_budget(
    int m,
    int degenerate_count,
    int window_events,
    int window_failed_stabilize,
    int window_dir_skip,
    int window_local_memory_fail,
    int window_alternations,
    int force_pivot_attempt_budget,
    int *reject_reason_out);

#define LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_NONE 0
#define LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_UNDER_TRIGGER 1
#define LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_FAILED_SHARE 2
#define LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_DIR_SKIP_SHARE 3
#define LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_LOCAL_FAIL 4
#define LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_ALTERNATION 5

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
    int *hard_bypass);

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
    int dual_rescue_fail_streak);

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
    int dual_rescue_fail_streak);

int lp_refactor_policy_phase1_force_extreme_tiny_theta_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak);

int lp_refactor_policy_phase1_soft_lu_policy_cooldown_updates(
    int m,
    int degenerate_count,
    int periodic_interval);

void lp_refactor_policy_phase1_reinvert_pressure_safety_step(
    int iter,
    int no_pivot_streak,
    int no_progress_streak,
    int ratio_breakdown_count,
    int dir_skip_no_recompute_streak,
    int hard_lu_trigger,
    int *last_iter,
    int *burst,
    int *demoted,
    int *demotions);

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
    int cooldown_remaining);

int lp_refactor_policy_phase1_degen_threshold(int m);

int lp_refactor_policy_phase1_stall_threshold(int m);

int lp_refactor_policy_phase1_recompute_interval(void);

double lp_refactor_policy_phase1_stall_obj_tol(double last_obj);

int lp_refactor_policy_phase1_ratio_breakdown_limit(int m,
                                                    int same_entering_streak);

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
