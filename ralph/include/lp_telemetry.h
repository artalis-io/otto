/*
 * lp_telemetry.h - the telemetry snapshot types and the two calls that fill
 * them.
 *
 * Moved verbatim out of lp.h, which was 3,074 lines and is included by 98
 * files. Telemetry was 45% of it. These two structs are the half that can
 * actually leave: nothing embeds them, so lp.h does not include this header
 * and the 91 files that never ask for a snapshot no longer parse 745 lines
 * describing one.
 *
 * The other half stays. LPSolverTelemetryState and LUTelemetryState are
 * members of SimplexSolver and LUFactorization by value, so every consumer of
 * those needs their full definition and moving them would buy nothing but a
 * second file to open. Narrowing that is a different change: it means making
 * the state opaque and reaching it through accessors, which is a behavioural
 * question about hot-path field access rather than a mechanical move.
 */
#ifndef LP_TELEMETRY_H
#define LP_TELEMETRY_H

#include "lp.h"

typedef struct {
    double perf_primal_setup_ms;
    double perf_dual_ms;
    double perf_phase1_ms;
    double perf_transition_ms;
    double perf_phase2_ms;
    double perf_pricing_ms;
    double perf_ratio_ms;
    double perf_pivot_ms;
    double perf_refactor_ms;
    double perf_ftran_ms;
    double perf_btran_ms;
    double perf_ftran_base_ms;
    double perf_ftran_update_apply_ms;
    int perf_ftran_update_apply_calls;
    double perf_btran_base_ms;
    double perf_btran_update_apply_ms;
    int perf_btran_update_apply_calls;
    int perf_ftran_calls;
    int perf_btran_calls;
    int perf_ftran_nnz_samples;
    int perf_btran_nnz_samples;
    long long perf_ftran_rhs_nnz_total;
    long long perf_ftran_sol_nnz_total;
    long long perf_btran_rhs_nnz_total;
    long long perf_btran_sol_nnz_total;
    double perf_lu_update_ms;
    double perf_compute_solution_ms;
    double perf_compute_rc_ms;
    double perf_refactor_all_ms;
    int perf_refactor_count;
    double perf_refactor_last_ms;
    double perf_refactor_max_ms;
    int perf_refactor_last_reason;
    int perf_refactor_next_reason;
    int perf_refactor_reason_setup;
    int perf_refactor_reason_transition;
    int perf_refactor_reason_periodic;
    int perf_refactor_reason_ratio_recovery;
    int perf_refactor_reason_pivot_recovery;
    int perf_refactor_reason_forced_small_pivot;
    int perf_refactor_reason_update_recovery;
    int perf_refactor_reason_direction_stabilize;
    int perf_refactor_reason_infeas_cleanup;
    int perf_refactor_reason_other;
    int perf_refactor_periodic_policy;
    int perf_refactor_periodic_lu_health;
    int perf_refactor_safety_forced;
    int perf_basis_fastpath_hits;
    int perf_basis_cols_rewritten;
    unsigned long long perf_basis_tail_shift_bytes;
    int perf_refactor_last_m;
    int perf_refactor_last_k;
    int perf_refactor_last_nnz_B;
    int perf_refactor_factorize_failures;
    int perf_refactor_repair_successes;
    int perf_refactor_repair_failures;
    int perf_refactor_last_factorize_failure_reason;
    int perf_refactor_last_sparse_numeric_failure_reason;
    int perf_refactor_last_repair_status;
    int perf_phase1_refactor_factorize_failures;
    int perf_phase1_refactor_repair_successes;
    int perf_phase1_refactor_repair_failures;
    int perf_phase2_refactor_factorize_failures;
    int perf_phase2_refactor_repair_successes;
    int perf_phase2_refactor_repair_failures;

    double perf_phase1_pricing_ms;
    double perf_phase1_ratio_ms;
    double perf_phase1_pivot_ms;
    double perf_phase1_refactor_ms;
    double perf_phase1_compute_solution_ms;
    double perf_phase1_compute_rc_ms;
    int perf_phase1_pricing_calls;
    int perf_phase1_ratio_calls;
    int perf_phase1_pivot_calls;
    int perf_phase1_refactor_calls;
    int perf_phase1_compute_solution_calls;
    int perf_phase1_compute_rc_calls;
    int perf_phase1_compute_solution_ctx_other;
    int perf_phase1_compute_solution_ctx_recompute_full;
    int perf_phase1_compute_solution_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_solution_ctx_init;
    int perf_phase1_compute_solution_ctx_no_entering_cleanup;
    int perf_phase1_compute_solution_ctx_infeas_cleanup;
    int perf_phase1_compute_solution_ctx_refactor_fail_continue;
    int perf_phase1_compute_solution_ctx_refactor_failure_recovery;
    int perf_phase1_compute_solution_ctx_refactor_success;
    int perf_phase1_compute_solution_ctx_drift_refresh;
    int perf_phase1_compute_solution_ctx_dual_rescue;
    int perf_phase1_compute_rc_ctx_other;
    int perf_phase1_compute_rc_ctx_recompute_full;
    int perf_phase1_compute_rc_ctx_recompute_rc_only;
    int perf_phase1_compute_rc_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_rc_ctx_init;
    int perf_phase1_compute_rc_ctx_infeas_cleanup;
    int perf_phase1_compute_rc_ctx_refactor_fail_continue;
    int perf_phase1_compute_rc_ctx_refactor_failure_recovery;
    int perf_phase1_compute_rc_ctx_refactor_success;
    int perf_phase1_compute_rc_ctx_drift_refresh;
    int perf_phase1_compute_rc_ctx_dual_rescue;
    int perf_phase1_entering_exclusions;
    int perf_phase1_entering_exclusion_repeats;
    int perf_phase1_entering_exclusion_hits;
    int perf_phase1_entering_exclusion_reroutes;
    int perf_phase1_entering_exclusion_no_alt;
    int perf_phase1_refactor_periodic_policy;
    int perf_phase1_refactor_periodic_lu_health;
    int perf_phase1_refactor_safety_forced;
    int perf_phase1_dir_stabilize_force_extreme_dir;
    int perf_phase1_dir_stabilize_force_lu_health;
    int perf_phase1_dir_stabilize_cooldown_candidates;
    int perf_phase1_dir_stabilize_ratio_le_3;
    int perf_phase1_dir_stabilize_ratio_le_10;
    int perf_phase1_dir_stabilize_ratio_le_30;
    int perf_phase1_dir_stabilize_ratio_le_100;
    int perf_phase1_dir_stabilize_ratio_gt_100;
    int perf_phase1_dir_stabilize_ratio_gt_300;
    int perf_phase1_dir_stabilize_ratio_gt_1000;
    int perf_phase1_dir_stabilize_skip_rc_only;
    int perf_phase1_dir_stabilize_skip_full;
    int perf_phase1_dir_stabilize_skip_no_recompute;
    int perf_phase1_dir_stabilize_skip_guard_refresh;
    int perf_phase1_dir_stabilize_escape_gate_triggers;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode;
    int perf_phase1_dir_stabilize_escape_gate_hard_bypass;
    int perf_phase1_dir_stabilize_refactor_from_no_pivot_force;
    int perf_phase1_dir_stabilize_refactor_from_force_extreme_dir;
    int perf_phase1_dir_stabilize_refactor_from_force_lu_health;
    int perf_phase1_dir_stabilize_refactor_from_force_pivot_mode;
    int perf_phase1_dir_stabilize_refactor_from_ladder_force;
    int perf_phase1_force_pivot_budget_dir_event_seen;
    int perf_phase1_force_pivot_budget_pivot_spend;
    int perf_phase1_force_pivot_relax_applied;
    int perf_phase1_force_extreme_relax_applied;
    int perf_phase1_force_extreme_bound_flip_relax_applied;
    int perf_phase1_force_extreme_catastrophic_tiny_theta_relax_applied;
    int perf_phase1_force_extreme_tiny_theta_relax_applied;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_force_lu_health;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_force_pivot_mode;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_ladder_force;
    int perf_phase1_force_extreme_tiny_theta_relax_next_failed_stabilize;
    int perf_phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown;
    int perf_phase1_force_extreme_tiny_theta_relax_next_pivot_fail;
    int perf_phase1_force_extreme_tiny_theta_relax_next_pivot_success;
    int perf_phase1_recompute_after_ratio_breakdown;
    int perf_phase1_recompute_after_dir_skip;
    int perf_phase1_recompute_after_dir_refactor;
    int perf_phase1_recompute_after_pivot_fail_recovery;
    int perf_phase1_recompute_after_perturb;
    int perf_phase1_recompute_rc_only_calls;
    int perf_phase1_recompute_rc_guard_forced_full;
    int perf_phase1_cleanup_attempts;
    int perf_phase1_cleanup_accepted;
    int perf_phase1_cleanup_rejected;
    int perf_phase1_cleanup_candidate_probe_rejects;
    int perf_phase1_progress_window_refactors;
    int perf_phase1_progress_window_cleanups;
    int perf_phase1_progress_window_perturbs;
    int perf_phase1_ratio_breakdown_retries;
    int perf_phase1_ratio_breakdown_escalations;
    int perf_phase1_pivot_fail_recovery_exclusions;
    int perf_phase1_no_pivot_events;
    int perf_phase1_no_pivot_forced_refactor;
    int perf_phase1_no_pivot_forced_ratio_breakdown;
    int perf_phase1_no_pivot_forced_dir_skip;
    int perf_phase1_no_pivot_forced_pivot_fail;
    int perf_phase1_no_pivot_events_ratio_breakdown;
    int perf_phase1_no_pivot_events_dir_skip;
    int perf_phase1_no_pivot_events_pivot_fail;
    int perf_phase1_no_pivot_no_progress_events;
    int perf_phase1_no_pivot_ladder_retry_defers;
    int perf_phase1_no_pivot_ladder_retry_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_retry_dir_skip;
    int perf_phase1_no_pivot_ladder_retry_pivot_fail;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts;
    int perf_phase1_no_pivot_ladder_dual_rescue_successes;
    int perf_phase1_no_pivot_ladder_dual_rescue_failures;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail;
    int perf_phase1_no_pivot_ladder_forced_refactors;
    int perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_forced_refactors_dir_skip;
    int perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail;
    int perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks;
    int perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces;
    int perf_phase1_direct_dual_rescue_attempts;
    int perf_phase1_direct_dual_rescue_successes;
    int perf_phase1_direct_dual_rescue_failures;
    int perf_phase1_direct_dual_rescue_guard_cooldown_blocks;
    int perf_phase1_direct_dual_rescue_guard_fail_cap_blocks;
    int perf_phase1_dual_rescue_exit_time_limit;
    int perf_phase1_dual_rescue_exit_bad_numerics;
    int perf_phase1_dual_rescue_exit_no_progress;
    int perf_phase1_dual_rescue_exit_no_entering;
    int perf_phase1_dual_rescue_exit_pivot_refactor_failure;
    int perf_phase1_dual_rescue_exit_periodic_refactor_failure;
    int perf_phase1_dual_rescue_exit_max_iters;
    int perf_phase1_dual_rescue_exit_alloc_failure;
    int perf_phase1_soft_lu_policy_cooldown_defers;
    int perf_phase1_dir_skip_same_entering_repeats;
    int perf_phase1_dir_skip_same_entering_max_streak;
    int perf_phase1_failed_stabilize_events;
    int perf_phase1_failed_stabilize_primary_failures;
    int perf_phase1_failed_stabilize_alternate_failures;
    int perf_phase1_failed_stabilize_same_entering_repeats;
    int perf_phase1_failed_stabilize_same_entering_max_streak;
    int perf_phase1_failed_stabilize_retry_penalty_arms;
    int perf_phase1_failed_stabilize_retry_penalty_alt_found;
    int perf_phase1_failed_stabilize_retry_penalty_no_alt;
    int perf_phase1_failed_stabilize_retry_penalty_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_penalty_alt_failed;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak;
    int perf_phase1_failed_stabilize_retry_local_memory_arms;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_found;
    int perf_phase1_failed_stabilize_retry_local_memory_no_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_failed;
    int perf_phase1_failed_stabilize_retry_pool_samples;
    int perf_phase1_failed_stabilize_retry_pool_eligible_total;
    int perf_phase1_failed_stabilize_retry_pool_eligible_max;
    int perf_phase1_failed_stabilize_retry_pool_singleton_samples;
    int perf_phase1_failed_stabilize_retry_pool_best_differs_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4;
    int perf_phase1_failed_stabilize_retry_shadow_samples;
    int perf_phase1_failed_stabilize_retry_shadow_ratio_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_stable;
    int perf_phase1_failed_stabilize_retry_shadow_dir_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_shadow_guard_arms;
    int perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor;
    int perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize;
    int perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_success;
    int perf_phase1_force_extreme_followup_stabilized;
    int perf_phase1_force_extreme_followup_ratio_breakdown;
    int perf_phase1_force_extreme_followup_failed_stabilize;
    int perf_phase1_force_extreme_followup_post_dir_skip_retry;
    int perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue;
    int perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor;
    int perf_phase1_force_extreme_followup_next_failed_stabilize;
    int perf_phase1_force_extreme_followup_next_ratio_breakdown;
    int perf_phase1_force_extreme_followup_next_pivot_fail;
    int perf_phase1_force_extreme_followup_next_pivot_success;
    int perf_phase1_force_extreme_followup_dir_samples;
    int perf_phase1_force_extreme_followup_dir_bound_geometry;
    int perf_phase1_force_extreme_followup_dir_bound_flip;
    int perf_phase1_force_extreme_followup_dir_tiny_theta;
    int perf_phase1_force_extreme_followup_dir_weak_leaving;
    int perf_phase1_force_extreme_followup_dir_ftran_shape;
    int perf_phase1_force_extreme_followup_dir_nnz_total;
    int perf_phase1_force_extreme_followup_dir_nnz_max;
    double perf_phase1_force_extreme_followup_dir_inf_total;
    double perf_phase1_force_extreme_followup_dir_inf_max;
    double perf_phase1_force_extreme_followup_pivot_abs_total;
    double perf_phase1_force_extreme_followup_pivot_abs_max;
    double perf_phase1_force_extreme_followup_theta_total;
    double perf_phase1_force_extreme_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland;
    int perf_phase1_failed_stabilize_retry_dir_fail_shape_samples;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_total;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_arms;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_failed;
    int perf_phase1_failed_stabilize_retry_dir_guard_arms;
    int perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions;
    int perf_phase1_window_pressure_windows_started;
    int perf_phase1_window_pressure_progress_resets;
    int perf_phase1_window_pressure_force_pivot_arms;
    int perf_phase1_window_pressure_force_pivot_blocked_pending;
    int perf_phase1_window_pressure_force_pivot_blocked_budget;
    int perf_phase1_window_pressure_force_pivot_reject_under_trigger;
    int perf_phase1_window_pressure_force_pivot_reject_failed_share;
    int perf_phase1_window_pressure_force_pivot_reject_dir_skip_share;
    int perf_phase1_window_pressure_force_pivot_reject_local_fail;
    int perf_phase1_window_pressure_force_pivot_reject_alternation;
    int perf_phase1_window_pressure_event_total;
    int perf_phase1_window_pressure_failed_stabilize_total;
    int perf_phase1_window_pressure_dir_skip_total;
    int perf_phase1_window_pressure_local_memory_fail_total;
    int perf_phase1_window_pressure_alternation_total;
    int perf_phase1_window_pressure_event_max;
    int perf_phase1_window_pressure_failed_stabilize_max;
    int perf_phase1_window_pressure_dir_skip_max;
    int perf_phase1_window_pressure_local_memory_fail_max;
    int perf_phase1_window_pressure_alternation_max;

    double perf_phase2_pricing_ms;
    double perf_phase2_ratio_ms;
    double perf_phase2_pivot_ms;
    double perf_phase2_refactor_ms;
    double perf_phase2_compute_solution_ms;
    double perf_phase2_compute_rc_ms;
    int perf_phase2_pricing_calls;
    int perf_phase2_ratio_calls;
    int perf_phase2_pivot_calls;
    int perf_phase2_refactor_calls;
    int perf_phase2_compute_solution_calls;
    int perf_phase2_compute_rc_calls;
    int perf_phase2_refactor_periodic_policy;
    int perf_phase2_refactor_periodic_lu_health;
    int perf_phase2_refactor_safety_forced;
    int perf_phase2_degenerate_episodes;
    int perf_phase2_degenerate_streak_max;
    int perf_phase2_theta_le_1e_9;
    int perf_phase2_theta_le_1e_6;
    int perf_phase2_theta_le_1e_3;
    int perf_phase2_theta_gt_1e_3;
    int perf_phase2_weak_pivot_samples;
    double perf_phase2_weak_pivot_ratio_total;
    double perf_phase2_weak_pivot_ratio_min;
    int perf_phase2_weak_pivot_ratio_le_1e_8;
    int perf_phase2_weak_pivot_ratio_le_1e_6;
    int perf_phase2_weak_pivot_ratio_le_1e_4;
    int perf_phase2_weak_pivot_ratio_gt_1e_4;
    int perf_phase2_repeat_entering_events;
    int perf_phase2_repeat_entering_max_streak;
    int perf_phase2_repeat_leaving_events;
    int perf_phase2_repeat_leaving_max_streak;
    int perf_phase2_bland_pricing_iters;
    int perf_phase2_adaptive_devex_partial_iters;
    int perf_phase2_bland_enter_episodes;
    int perf_phase2_bland_exit_episodes;
    int perf_phase2_perturb_applied;
    int perf_phase2_devex_reset_count;
    int perf_phase2_devex_age_max;
    int perf_phase2_degen_refactor_calls;
    int perf_phase2_degen_refactor_ratio_recovery;
    int perf_phase2_degen_refactor_pivot_recovery;
    int perf_phase2_degen_refactor_periodic_policy;
    int perf_phase2_degen_refactor_periodic_lu_health;
    int perf_phase2_degen_refactor_safety_forced;
    int perf_phase2_degen_escape_triggers;
    int perf_dual_ratio_no_entering;
    int perf_dual_theta_nonpositive;
    int perf_dual_pivot_reject_small;
    int perf_dual_bound_flip_applied;
    int perf_dual_bound_flip_startup;
    int perf_dual_bound_flip_iterative;
    int perf_dual_lu_hard_trigger;

    double periodic_feedback_bias_phase1;
    double periodic_feedback_bias_phase2;
    int periodic_feedback_last_reason_phase1;
    int periodic_feedback_last_reason_phase2;
    int periodic_feedback_last_interval_phase1;
    int periodic_feedback_last_interval_phase2;
    int periodic_feedback_hint_interval_phase1;
    int periodic_feedback_hint_interval_phase2;
    double periodic_feedback_hint_pressure_phase1;
    double periodic_feedback_hint_pressure_phase2;
    int soft_lu_cost_gate_enabled;
    int soft_lu_cost_gate_defers_phase1;
    int soft_lu_cost_gate_defers_phase2;
    int soft_lu_consecutive_defers_phase1;
    int soft_lu_consecutive_defers_phase2;
    int soft_lu_defer_cap_forced_phase1;
    int soft_lu_defer_cap_forced_phase2;
    int periodic_cost_gate_enabled;
    int periodic_cost_gate_defers_phase1;
    int periodic_cost_gate_defers_phase2;
    int periodic_cost_consecutive_defers_phase1;
    int periodic_cost_consecutive_defers_phase2;
    int periodic_cost_defer_cap_forced_phase1;
    int periodic_cost_defer_cap_forced_phase2;
    int periodic_cost_gate_checks_phase1;
    int periodic_cost_gate_checks_phase2;
    int periodic_cost_gate_block_small_m_phase1;
    int periodic_cost_gate_block_small_m_phase2;
    int periodic_cost_gate_block_invalid_inputs_phase1;
    int periodic_cost_gate_block_invalid_inputs_phase2;
    int periodic_cost_gate_block_warmup_phase1;
    int periodic_cost_gate_block_warmup_phase2;
    int periodic_cost_gate_block_invalid_cost_phase1;
    int periodic_cost_gate_block_invalid_cost_phase2;
    int periodic_cost_gate_block_ratio_phase1;
    int periodic_cost_gate_block_ratio_phase2;
    int periodic_cost_gate_block_update_reserve_phase1;
    int periodic_cost_gate_block_update_reserve_phase2;
    int periodic_cost_gate_last_reason_phase1;
    int periodic_cost_gate_last_reason_phase2;
    int periodic_cost_iter_samples_phase1;
    int periodic_cost_iter_samples_phase2;
    int periodic_cost_refactor_samples_phase1;
    int periodic_cost_refactor_samples_phase2;
    double soft_lu_refactor_cost_ewma_phase1;
    double soft_lu_refactor_cost_ewma_phase2;
    double soft_lu_iter_cost_ewma_phase1;
    double soft_lu_iter_cost_ewma_phase2;
    int basis_governor_mode;
    int reinvert_controller_mode;
    int reinvert_dual_control_demoted;
    int reinvert_dual_control_demotions;
    int reinvert_dual_hard_trigger_last_total;
    int reinvert_dual_hard_trigger_last_iter;
    int reinvert_dual_hard_trigger_burst;
    int reinvert_phase1_control_demoted;
    int reinvert_phase1_control_demotions;
    int reinvert_phase1_pressure_last_iter;
    int reinvert_phase1_pressure_burst;
    int phase1_stagnation_escape_cooldown;
    int phase1_stagnation_escape_triggers;
    int phase1_stagnation_escape_successes;
    int phase1_stagnation_escape_failures;
    int phase1_stagnation_escape_cooldown_blocks;
    int phase1_stagnation_last_window_iters;
    double phase1_stagnation_last_obj_delta;
    double phase1_stagnation_last_retry_defer_ratio;
    double phase1_stagnation_last_update_recovery_ratio;
    int phase1_stagnation_last_retry_defers;
    int phase1_stagnation_last_no_pivot_events;
    int phase1_stagnation_last_update_recovery_refactors;
    int phase1_stagnation_last_refactors;
    int phase1_stagnation_last_recompute_ratio;
    int phase1_stagnation_last_recompute_dir_skip;
    int phase1_stagnation_last_recompute_dir_refactor;
    int phase1_stagnation_last_recompute_pivot_fail;
    int phase1_stagnation_last_recompute_perturb;
    int shadow_refactor_yes_phase1;
    int shadow_refactor_yes_phase2;
    int shadow_refactor_yes_dual;
    int shadow_refactor_no_phase1;
    int shadow_refactor_no_phase2;
    int shadow_refactor_no_dual;
    int shadow_backend_pick_markowitz;
    int shadow_backend_pick_supernode;
    int shadow_backend_pick_dense;
    int shadow_disagree_primal_refactor;
    int shadow_disagree_dual_refactor;
    int shadow_disagree_lu_backend;
    int reinvert_shadow_checks_phase1;
    int reinvert_shadow_checks_phase2;
    int reinvert_shadow_checks_dual;
    int reinvert_shadow_suggest_allow_phase1;
    int reinvert_shadow_suggest_allow_phase2;
    int reinvert_shadow_suggest_allow_dual;
    int reinvert_shadow_suggest_defer_phase1;
    int reinvert_shadow_suggest_defer_phase2;
    int reinvert_shadow_suggest_defer_dual;
    int reinvert_shadow_suggest_force_phase1;
    int reinvert_shadow_suggest_force_phase2;
    int reinvert_shadow_suggest_force_dual;
    int reinvert_shadow_actual_refactor_yes_phase1;
    int reinvert_shadow_actual_refactor_yes_phase2;
    int reinvert_shadow_actual_refactor_yes_dual;
    int reinvert_shadow_actual_refactor_no_phase1;
    int reinvert_shadow_actual_refactor_no_phase2;
    int reinvert_shadow_actual_refactor_no_dual;
    int reinvert_shadow_disagree_phase1;
    int reinvert_shadow_disagree_phase2;
    int reinvert_shadow_disagree_dual;
    int reinvert_shadow_last_reason_phase1;
    int reinvert_shadow_last_reason_phase2;
    int reinvert_shadow_last_reason_dual;
} LPSolverTelemetrySnapshot;

typedef struct {
    int mkz_enabled;
    int sn_enabled;
    int mkz_calls;
    int mkz_successes;
    int mkz_failures;
    int mkz_last_failure;
    int mkz_dense_fallbacks;
    int mkz_fail_workspace;
    int mkz_fail_pool;
    int mkz_fail_singular;
    int mkz_fail_capacity;
    int mkz_singular_retry_attempts;
    int mkz_singular_retry_successes;
    int mkz_singular_retry_failures;
    int mkz_reserved_fallback_attempts;
    int mkz_reserved_fallback_accepts;
    int mkz_reserved_fallback_rejects;
    int mkz_circuit_trips;
    int mkz_circuit_skips;
    int mkz_circuit_resets;
    int mkz_global_skip_trips;
    int mkz_global_skip_skips;
    int mkz_global_skip_resets;
    int mkz_profile_retry_attempts;
    int mkz_profile_retry_successes;
    int mkz_profile_retry_failures;
    int mkz_profile_retry_fail_identity_sep;
    int mkz_profile_retry_fail_backend_exhausted;
    int mkz_profile_retry_fail_pathological;
    uint64_t mkz_primary_scan_entries;
    uint64_t mkz_rescue_scan_entries;
    uint64_t mkz_reserved_scan_entries;
    uint64_t mkz_update_existing_entries;
    uint64_t mkz_update_fill_candidates;
    uint64_t mkz_hint_fallback_scans;
    uint64_t mkz_hint_fallback_scan_entries;
    uint64_t mkz_affected_columns_total;
    uint64_t mkz_affected_columns_max;
    uint64_t mkz_col_max_scan_entries;
    int mkz_high_cond_count;
    double mkz_worst_cond;

    int sparse_dense_fallbacks;
    int used_dense_fallback_last;
    int sparse_fallback_last_reason;
    int sparse_fallback_reason_small_matrix;
    int sparse_fallback_reason_symbolic;
    int sparse_fallback_reason_numeric;
    int sparse_numeric_last_failure_reason;
    int sparse_numeric_fail_identity_sep;
    int sparse_numeric_fail_backend_exhausted;
    int sparse_numeric_fail_pathological;
    int numeric_full_retry_attempts;
    int numeric_full_retry_successes;
    int numeric_full_retry_failures;
    int identity_sep_failures;
    int symbolic_failures;
    int symbolic_fail_workspace;
    int symbolic_fail_unmatched_no_reserved;
    int symbolic_fail_inconsistent_identity;
    int symbolic_full_retry_attempts;
    int symbolic_full_retry_successes;
    int symbolic_full_retry_numeric_failures;
    int symbolic_full_retry_mkz_attempts;
    int symbolic_full_retry_mkz_successes;
    int symbolic_full_retry_mkz_failures;
    int numeric_backend_markowitz;
    int numeric_backend_supernode;
    int numeric_backend_dense_ge;
    int backend_policy_luf_ft;
    int backend_policy_cbg;
    int backend_policy_cgr;
    int backend_policy_last;
    int update_path_ft;
    int update_path_eta;
    int update_path_bg_compat;
    int update_path_gr_compat;
    int identity_sep_retry_lane_dense_chosen;
    int identity_sep_retry_lane_supernode_chosen;
    int identity_sep_retry_lane_dense_successes;
    int identity_sep_retry_lane_supernode_successes;
    int sn_cost_gate_trips;
    int sn_cost_gate_skips;
    int sn_cost_gate_resets;
    int refactor_need_checks;
    int refactor_need_triggers;
    int refactor_need_last_reason;
    int refactor_need_reason_max_updates;
    int refactor_need_reason_growth_guard;
    int refactor_need_reason_avg_spike_density;
    int refactor_need_reason_cond_severe;
    int refactor_need_reason_cond_adaptive_limit;
    int refactor_need_reason_spike_pool_warn;
    int refactor_need_reason_spike_work;
    int refactor_need_reason_spike_diag_quality; /* N2: spike diag ratio > 1e8 */
    int update_fail_bad_input;
    int update_fail_max_updates;
    int update_fail_singular_update;
    int update_fail_update_pivot_too_small;
    int update_fail_spike_pool_full;
    int update_fail_dense_spike_reject;
    int update_fail_eta_alloc;

    int sn_calls;
    int sn_successes;
    int num_updates;
    int max_updates;
    int last_failure_reason;
    int last_refactor_trigger_reason;

    int perf_factorize_calls;
    int perf_last_basis_nnz;
    int perf_last_m;
    int perf_last_k;
    int perf_symbolic_calls;
    int perf_symbolic_cache_hits;
    int perf_symbolic_cache_misses;
    double perf_last_symbolic_ms;
    double perf_last_sparse_numeric_ms;
    double perf_last_dense_ge_numeric_ms;
    double perf_last_supernode_numeric_ms;
    double perf_last_dense_factorize_ms;
    double perf_last_a_struct_build_ms;
    double perf_last_markowitz_numeric_ms;
    double perf_last_identity_placement_ms;
    double perf_last_coo_to_csc_ms;
    double perf_total_symbolic_ms;
    double perf_total_sparse_numeric_ms;
    double perf_total_dense_ge_numeric_ms;
    double perf_total_supernode_numeric_ms;
    double perf_total_dense_factorize_ms;
    double perf_total_a_struct_build_ms;
    double perf_total_markowitz_numeric_ms;
    double perf_total_identity_placement_ms;
    double perf_total_coo_to_csc_ms;
    int perf_update_apply_forward_calls;
    int perf_update_apply_backward_calls;
    int perf_compact_factor_calls;
    int perf_compact_solve_calls;
    double perf_total_update_apply_forward_ms;
    double perf_total_update_apply_backward_ms;
    double perf_total_compact_factor_ms;
    double perf_total_compact_solve_ms;
    uint64_t perf_sn_phase_samples;
    double perf_sn_panel_factor_ms;
    double perf_sn_panel_pivot_search_ms;
    double perf_sn_panel_swap_scatter_ms;
    double perf_sn_panel_eliminate_ms;
    uint64_t perf_sn_panel_pivot_search_calls;
    uint64_t perf_sn_panel_pivot_search_entries_total;
    uint64_t perf_sn_panel_pivot_search_size1_calls;
    double perf_sn_panel_pivot_search_size1_ms;
    uint64_t perf_sn_panel_pivot_search_size2_calls;
    double perf_sn_panel_pivot_search_size2_ms;
    uint64_t perf_sn_panel_pivot_search_size3_4_calls;
    double perf_sn_panel_pivot_search_size3_4_ms;
    uint64_t perf_sn_panel_pivot_search_size5_8_calls;
    double perf_sn_panel_pivot_search_size5_8_ms;
    uint64_t perf_sn_panel_pivot_search_size9p_calls;
    double perf_sn_panel_pivot_search_size9p_ms;
    uint64_t perf_sn_panel_pivot_search_reserved_present_calls;
    uint64_t perf_sn_panel_pivot_search_reserved_present_entries;
    double perf_sn_panel_pivot_search_reserved_present_ms;
    uint64_t perf_sn_panel_pivot_search_reserved_alt_chosen_calls;
    double perf_sn_panel_pivot_search_reserved_alt_chosen_ms;
    uint64_t perf_sn_size1_u_emit_calls;
    double perf_sn_size1_u_emit_ms;
    uint64_t perf_sn_size1_update_scan_calls;
    double perf_sn_size1_update_scan_ms;
    uint64_t perf_sn_size1_update_apply_calls;
    double perf_sn_size1_update_apply_ms;
    double perf_sn_size1_update_row_gather_ms;
    double perf_sn_size1_update_col_indirection_ms;
    double perf_sn_size1_update_outer_product_ms;
    uint64_t perf_sn_size1_update_full_calls;
    double perf_sn_size1_update_full_ms;
    uint64_t perf_sn_size1_update_cols1_calls;
    double perf_sn_size1_update_cols1_ms;
    uint64_t perf_sn_size1_update_cols2_calls;
    double perf_sn_size1_update_cols2_ms;
    uint64_t perf_sn_size1_update_cols3_calls;
    double perf_sn_size1_update_cols3_ms;
    uint64_t perf_sn_size1_update_cols4_calls;
    double perf_sn_size1_update_cols4_ms;
    uint64_t perf_sn_size1_update_cols5p_calls;
    double perf_sn_size1_update_cols5p_ms;
    uint64_t perf_sn_size1_update_cols5p_rows1_8_calls;
    double perf_sn_size1_update_cols5p_rows1_8_ms;
    uint64_t perf_sn_size1_update_cols5p_rows9_32_calls;
    double perf_sn_size1_update_cols5p_rows9_32_ms;
    uint64_t perf_sn_size1_update_cols5p_rows33_128_calls;
    double perf_sn_size1_update_cols5p_rows33_128_ms;
    uint64_t perf_sn_size1_update_cols5p_rows129p_calls;
    double perf_sn_size1_update_cols5p_rows129p_ms;
    double perf_sn_u_emit_ms;
    double perf_sn_active_set_ms;
    double perf_sn_pack_blocks_ms;
    double perf_sn_full_update_ms;
    double perf_sn_compact_update_ms;
    uint64_t perf_sn_active_row_scan_entries;
    uint64_t perf_sn_active_col_scan_entries;
    uint64_t perf_sn_trailing_rows_total;
    uint64_t perf_sn_trailing_cols_total;
    uint64_t perf_sn_active_rows_total;
    uint64_t perf_sn_active_cols_total;
    uint64_t perf_sn_pack_l_entries_total;
    uint64_t perf_sn_pack_u_entries_total;
    uint64_t perf_sn_dense_triplets_total;
    uint64_t perf_sn_compact_triplets_total;
    uint64_t perf_sn_full_update_calls;
    uint64_t perf_sn_compact_update_calls;
    uint64_t perf_sn_skipped_update_calls;
    uint64_t perf_sn_compact_cols1_calls;
    uint64_t perf_sn_compact_cols1_rows_total;
    double perf_sn_compact_cols1_ms;
    uint64_t perf_sn_compact_cols2_calls;
    uint64_t perf_sn_compact_cols2_rows_total;
    double perf_sn_compact_cols2_ms;
    uint64_t perf_sn_compact_cols3_calls;
    uint64_t perf_sn_compact_cols3_rows_total;
    double perf_sn_compact_cols3_ms;
    uint64_t perf_sn_compact_cols4_calls;
    uint64_t perf_sn_compact_cols4_rows_total;
    double perf_sn_compact_cols4_ms;
    uint64_t perf_sn_compact_cols5p_calls;
    uint64_t perf_sn_compact_cols5p_rows_total;
    double perf_sn_compact_cols5p_ms;
} LUTelemetrySnapshot;

void lp_telemetry_snapshot_solver(const SimplexSolver *solver,
                                  LPSolverTelemetrySnapshot *out);
void lp_telemetry_snapshot_lu(const LUFactorization *lu,
                              LUTelemetrySnapshot *out);


/* The recording API, moved out of lp.h.
 *
 * 217 of the 313 functions lp.h declared were these, against roughly 96
 * genuine solver entry points -- lu_*, the tableau and pricing calls, the
 * ratio tests. A reader opening lp.h to find the solver interface was
 * reading a telemetry header with a solver in it.
 *
 * What stays behind in lp.h is LPSolverTelemetryState and LUTelemetryState,
 * which are members of SimplexSolver and LUFactorization by value. Those
 * cannot move without making the state opaque and going through accessors,
 * and they are written on the pricing and refactorisation paths -- a
 * question about field access, not a place to put a struct. */
/* Telemetry helpers */
double lp_telemetry_now_ms(void);
double lp_telemetry_timer_start(void);
double lp_telemetry_timer_elapsed_ms(double start_ms);
int lp_telemetry_refactor_reason_is_safety_forced(int reason);
void lp_telemetry_reset_solver(SimplexSolver *solver);
void lp_telemetry_reset_lu(LUFactorization *lu);
void lp_telemetry_prepare_lu_factorize(LUFactorization *lu, const SparseMatrix *B);
void lp_telemetry_record_basis_build(SimplexSolver *owner,
                                     int fastpath_hit,
                                     int cols_rewritten,
                                     unsigned long long tail_shift_bytes);
void lp_telemetry_begin_refactor(SimplexSolver *owner, int *reason_out);
void lp_telemetry_set_refactor_next_reason(SimplexSolver *owner, int reason);
void lp_telemetry_record_refactor(SimplexSolver *owner,
                                  int phase,
                                  int reason,
                                  double elapsed_ms,
                                  int m,
                                  int lu_last_k,
                                  int lu_last_basis_nnz);
void lp_telemetry_record_refactor_with_lu(SimplexSolver *owner,
                                          int phase,
                                          int reason,
                                          double elapsed_ms,
                                          int m,
                                          const LUFactorization *lu);
void lp_telemetry_record_refactor_with_lu_timed(SimplexSolver *owner,
                                                int phase,
                                                int reason,
                                                double start_ms,
                                                int m,
                                                const LUFactorization *lu);
void lp_telemetry_record_refactor_repair_outcome(
    SimplexSolver *owner,
    int phase,
    int original_lu_failure_reason,
    int original_sparse_numeric_failure_reason,
    int repair_status);
void lp_telemetry_add_solver_stage_ms(SimplexSolver *solver,
                                      LPSolverStage stage,
                                      double elapsed_ms);
void lp_telemetry_add_solver_stage_timed(SimplexSolver *solver,
                                         LPSolverStage stage,
                                         double start_ms);
void lp_telemetry_add_refactor_runtime_ms(SimplexSolver *solver,
                                          double elapsed_ms);
void lp_telemetry_add_refactor_runtime_timed(SimplexSolver *solver,
                                             double start_ms);
void lp_telemetry_add_ftran_ms(SimplexSolver *solver,
                               double elapsed_ms);
void lp_telemetry_add_ftran_timed(SimplexSolver *solver,
                                  double start_ms);
void lp_telemetry_add_ftran_base_ms(SimplexSolver *solver,
                                    double elapsed_ms);
void lp_telemetry_add_ftran_base_timed(SimplexSolver *solver,
                                       double start_ms);
void lp_telemetry_add_ftran_update_apply_ms(SimplexSolver *solver,
                                            double elapsed_ms);
void lp_telemetry_add_ftran_update_apply_timed(SimplexSolver *solver,
                                               double start_ms);
void lp_telemetry_record_ftran_nnz(SimplexSolver *solver,
                                   int rhs_nnz,
                                   int sol_nnz);
void lp_telemetry_add_btran_ms(SimplexSolver *solver,
                               double elapsed_ms);
void lp_telemetry_add_btran_timed(SimplexSolver *solver,
                                  double start_ms);
void lp_telemetry_add_btran_base_ms(SimplexSolver *solver,
                                    double elapsed_ms);
void lp_telemetry_add_btran_base_timed(SimplexSolver *solver,
                                       double start_ms);
void lp_telemetry_add_btran_update_apply_ms(SimplexSolver *solver,
                                            double elapsed_ms);
void lp_telemetry_add_btran_update_apply_timed(SimplexSolver *solver,
                                               double start_ms);
void lp_telemetry_record_btran_nnz(SimplexSolver *solver,
                                   int rhs_nnz,
                                   int sol_nnz);
void lp_telemetry_add_lu_update_ms(SimplexSolver *solver,
                                   double elapsed_ms);
void lp_telemetry_add_lu_update_timed(SimplexSolver *solver,
                                      double start_ms);
void lp_telemetry_record_compute_solution(SimplexSolver *solver,
                                          int phase,
                                          double elapsed_ms);
void lp_telemetry_record_compute_solution_timed(SimplexSolver *solver,
                                                int phase,
                                                double start_ms);
void lp_telemetry_record_compute_reduced_costs(SimplexSolver *solver,
                                               int phase,
                                               double elapsed_ms);
void lp_telemetry_record_phase1_compute_solution_context(SimplexSolver *solver,
                                                         LPPhase1ComputeContext context);
void lp_telemetry_record_phase1_compute_rc_context(SimplexSolver *solver,
                                                   LPPhase1ComputeContext context);
void lp_telemetry_record_phase1_entering_exclusion(SimplexSolver *solver,
                                                   int repeated_slot);
void lp_telemetry_record_phase1_entering_exclusion_hit(SimplexSolver *solver,
                                                       int rerouted);
void lp_telemetry_record_phase1_dir_skip_entering(SimplexSolver *solver,
                                                  int same_entering,
                                                  int streak);
void lp_telemetry_record_phase1_failed_stabilize_entering(
    SimplexSolver *solver,
    int same_entering,
    int streak);
void lp_telemetry_record_phase1_failed_stabilize_site(
    SimplexSolver *solver,
    int used_alternate);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_alternate(
    SimplexSolver *solver,
    int same_alt,
    int streak);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_no_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(
    SimplexSolver *solver,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_alternate(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_no_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_fallback_same_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(
    SimplexSolver *solver,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_pool_sample(
    SimplexSolver *solver,
    int eligible_count,
    int best_differs_from_bland);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_eval(
    SimplexSolver *solver,
    int best_differs_from_bland,
    double bland_score,
    double best_score);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow(
    SimplexSolver *solver,
    int ratio_success,
    int dir_stable,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_original_exclusion(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_fail(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_success(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_stabilized(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_retry(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_dual_rescue(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_forced_refactor(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_pivot_fail(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_pivot_success(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_direction(
    SimplexSolver *solver,
    int leaving,
    double theta,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_followup_direction(
    SimplexSolver *solver,
    int leaving,
    double theta,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(
    SimplexSolver *solver,
    int used_guarded,
    int eligible_count);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(
    SimplexSolver *solver,
    int used_guarded,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_ratio_failure(
    SimplexSolver *solver,
    int used_guarded);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_dir_failure(
    SimplexSolver *solver,
    int used_guarded);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_guarded_fallback(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_fail_shape(
    SimplexSolver *solver,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_no_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_outcome(
    SimplexSolver *solver,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_original_exclusion(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_event(
    SimplexSolver *solver,
    int failed_stabilize_event,
    int dir_skip_event,
    int local_memory_fail_event,
    int alternated,
    int window_events,
    int window_failed_stabilize,
    int window_dir_skip,
    int window_local_memory_fail,
    int window_alternations);
void lp_telemetry_record_phase1_window_pressure_progress_reset(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_force_pivot_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
    SimplexSolver *solver,
    int reject_reason);
void lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_pending(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_budget(
    SimplexSolver *solver);
void lp_telemetry_record_phase2_pivot_geometry(SimplexSolver *solver,
                                               double theta,
                                               double dir_inf,
                                               double pivot_abs);
void lp_telemetry_record_phase2_devex_reset(SimplexSolver *solver,
                                            int devex_age);
void lp_telemetry_record_phase2_degenerate_refactor(SimplexSolver *solver,
                                                    int reason,
                                                    int lu_health_triggered,
                                                    int safety_forced);
void lp_telemetry_record_compute_reduced_costs_timed(SimplexSolver *solver,
                                                     int phase,
                                                     double start_ms);
void lp_telemetry_record_pricing(SimplexSolver *solver,
                                 int phase,
                                 double elapsed_ms);
void lp_telemetry_record_pricing_timed(SimplexSolver *solver,
                                       int phase,
                                       double start_ms);
void lp_telemetry_record_ratio(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms);
void lp_telemetry_record_ratio_timed(SimplexSolver *solver,
                                     int phase,
                                     double start_ms);
void lp_telemetry_record_pivot(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms);
void lp_telemetry_record_pivot_timed(SimplexSolver *solver,
                                     int phase,
                                     double start_ms);
void lp_telemetry_record_periodic_refactor_trigger(SimplexSolver *solver,
                                                   int phase,
                                                   int lu_health_triggered);
void lp_telemetry_record_phase1_dir_stabilize_force(SimplexSolver *solver,
                                                    int force_extreme_dir,
                                                    int force_lu_health);
void lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(
    SimplexSolver *solver,
    double dir_inf_ratio);
void lp_telemetry_record_phase1_dir_stabilize_skip(SimplexSolver *solver,
                                                   int used_full_recompute);
void lp_telemetry_record_phase1_dir_stabilize_skip_no_recompute(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_dir_stabilize_skip_guard_refresh(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_dir_stabilize_escape_gate(
    SimplexSolver *solver,
    int event);
void lp_telemetry_record_phase1_dir_stabilize_refactor_trigger(
    SimplexSolver *solver,
    int trigger);
void lp_telemetry_record_phase1_force_pivot_budget_dir_event_seen(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_pivot_budget_pivot_spend(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_pivot_relax(SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_relax(SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_bound_flip_relax(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_catastrophic_tiny_theta_relax(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_refactor(
    SimplexSolver *solver,
    int reason);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_pivot_fail(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_pivot_success(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_recompute(SimplexSolver *solver,
                                          LPPhase1RecomputeReason reason);
void lp_telemetry_record_phase1_recompute_rc_only(SimplexSolver *solver);
void lp_telemetry_record_phase1_recompute_guard_forced_full(SimplexSolver *solver);
void lp_telemetry_record_phase1_cleanup_attempt(SimplexSolver *solver);
void lp_telemetry_record_phase1_cleanup_accepted(SimplexSolver *solver);
void lp_telemetry_record_phase1_cleanup_rejected(SimplexSolver *solver);
void lp_telemetry_record_phase1_cleanup_candidate_probe_reject(SimplexSolver *solver);
void lp_telemetry_record_phase1_progress_window_refactor(SimplexSolver *solver);
void lp_telemetry_record_phase1_progress_window_cleanup(SimplexSolver *solver);
void lp_telemetry_record_phase1_progress_window_perturb(SimplexSolver *solver);
void lp_telemetry_record_phase1_ratio_breakdown_retry(SimplexSolver *solver);
void lp_telemetry_record_phase1_ratio_breakdown_escalation(SimplexSolver *solver);
void lp_telemetry_record_phase1_pivot_fail_recovery_exclusion(SimplexSolver *solver);
void lp_telemetry_record_phase1_no_pivot_event(SimplexSolver *solver,
                                               LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_force(SimplexSolver *solver,
                                               LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_no_progress(SimplexSolver *solver);
void lp_telemetry_record_phase1_no_pivot_ladder_retry(SimplexSolver *solver,
                                                      LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_ladder_dual_rescue(SimplexSolver *solver,
                                                             LPPhase1NoPivotForceReason reason,
                                                             int success);
void lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
    SimplexSolver *solver,
    LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(
    SimplexSolver *solver,
    int forced_refactor);
void lp_telemetry_record_phase1_direct_dual_rescue(SimplexSolver *solver,
                                                   int success);
void lp_telemetry_record_phase1_direct_dual_rescue_guard(
    SimplexSolver *solver,
    int fail_cap_block);

void lp_telemetry_record_phase1_dual_rescue_exit(
    SimplexSolver *solver,
    LPPhase1DualRescueExitReason reason);
void lp_telemetry_record_phase1_soft_lu_policy_cooldown_defer(
    SimplexSolver *solver);
void lp_telemetry_record_dual_ratio_no_entering(SimplexSolver *solver);
void lp_telemetry_record_dual_theta_nonpositive(SimplexSolver *solver);
void lp_telemetry_record_dual_pivot_reject_small(SimplexSolver *solver);
void lp_telemetry_record_dual_bound_flip_applied(SimplexSolver *solver,
                                                 int flips);
void lp_telemetry_record_dual_bound_flip_applied_startup(SimplexSolver *solver,
                                                         int flips);
void lp_telemetry_record_dual_bound_flip_applied_iterative(SimplexSolver *solver,
                                                           int flips);
void lp_telemetry_record_dual_lu_hard_trigger(SimplexSolver *solver);
void lp_telemetry_record_reinvert_shadow(SimplexSolver *solver,
                                         int phase,
                                         LPReinvertDecision suggested_decision,
                                         LPReinvertReason suggested_reason,
                                         int suggested_refactor,
                                         int actual_refactor);
void lp_telemetry_lu_record_dense_factorize_ms(LUFactorization *lu,
                                               double elapsed_ms);
void lp_telemetry_lu_record_dense_factorize_timed(LUFactorization *lu,
                                                  double start_ms);
void lp_telemetry_lu_record_update_apply_forward_ms(LUFactorization *lu,
                                                    double elapsed_ms);
void lp_telemetry_lu_record_update_apply_backward_ms(LUFactorization *lu,
                                                     double elapsed_ms);
void lp_telemetry_lu_record_compact_factor_ms(LUFactorization *lu,
                                              double elapsed_ms);
void lp_telemetry_lu_record_compact_solve_ms(LUFactorization *lu,
                                             double elapsed_ms);
void lp_telemetry_lu_record_symbolic_cache_hit(LUFactorization *lu);
void lp_telemetry_lu_record_symbolic_cache_miss(LUFactorization *lu);
void lp_telemetry_lu_record_symbolic_call(LUFactorization *lu,
                                          double elapsed_ms);
void lp_telemetry_lu_record_symbolic_call_timed(LUFactorization *lu,
                                                double start_ms);
void lp_telemetry_lu_mark_identity_sep_failure(LUFactorization *lu);
void lp_telemetry_lu_record_numeric_stages(LUFactorization *lu,
                                           int last_k,
                                           double a_struct_build_ms,
                                           double markowitz_numeric_ms,
                                           double supernode_numeric_ms,
                                           double dense_ge_numeric_ms,
                                           double identity_placement_ms,
                                           double coo_to_csc_ms);
void lp_telemetry_lu_mark_sparse_numeric_failure(LUFactorization *lu,
                                                 int reason);
void lp_telemetry_lu_set_sparse_fallback_reason(LUFactorization *lu,
                                                int reason);
void lp_telemetry_lu_mark_numeric_full_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_full_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_full_retry_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_failure(LUFactorization *lu,
                                           int reason);
void lp_telemetry_lu_mark_symbolic_full_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_numeric_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_mkz_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_mkz_success(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_mkz_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_backend_markowitz(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_backend_supernode(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_backend_dense_ge(LUFactorization *lu);
void lp_telemetry_lu_mark_identity_sep_retry_lane_chosen(LUFactorization *lu,
                                                         int lane);
void lp_telemetry_lu_mark_identity_sep_retry_lane_success(LUFactorization *lu,
                                                          int lane);
void lp_telemetry_lu_mark_sn_cost_gate_trip(LUFactorization *lu);
void lp_telemetry_lu_mark_sn_cost_gate_skip(LUFactorization *lu);
void lp_telemetry_lu_mark_sn_cost_gate_reset(LUFactorization *lu);
void lp_telemetry_lu_mark_sparse_success(LUFactorization *lu);
void lp_telemetry_lu_mark_dense_fallback(LUFactorization *lu);
void lp_telemetry_lu_clear_mkz_last_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_success(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_failure_reason(LUFactorization *lu,
                                             int rc);
void lp_telemetry_lu_mark_mkz_failure(LUFactorization *lu,
                                      int rc);
void lp_telemetry_lu_mark_mkz_singular_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_singular_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_singular_retry_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_reserved_fallback_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_reserved_fallback_accept(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_reserved_fallback_reject(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_circuit_trip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_circuit_skip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_circuit_reset(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_global_skip_trip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_global_skip_skip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_global_skip_reset(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_terminal_failure(LUFactorization *lu,
                                                             int reason);
void lp_telemetry_lu_add_mkz_scan_work(LUFactorization *lu,
                                       uint64_t primary_scan_entries,
                                       uint64_t rescue_scan_entries,
                                       uint64_t reserved_scan_entries,
                                       uint64_t update_existing_entries,
                                       uint64_t update_fill_candidates,
                                       uint64_t hint_fallback_scans,
                                       uint64_t hint_fallback_scan_entries);
void lp_telemetry_lu_add_mkz_colmax_work(LUFactorization *lu,
                                         uint64_t affected_columns,
                                         uint64_t affected_columns_max,
                                         uint64_t col_max_scan_entries);
void lp_telemetry_lu_add_supernode_work(LUFactorization *lu,
                                        uint64_t phase_samples,
                                        double panel_factor_ms,
                                        double panel_pivot_search_ms,
                                        double panel_swap_scatter_ms,
                                        double panel_eliminate_ms,
                                        uint64_t panel_pivot_search_calls,
                                        uint64_t panel_pivot_search_entries_total,
                                        uint64_t panel_pivot_search_size1_calls,
                                        double panel_pivot_search_size1_ms,
                                        uint64_t panel_pivot_search_size2_calls,
                                        double panel_pivot_search_size2_ms,
                                        uint64_t panel_pivot_search_size3_4_calls,
                                        double panel_pivot_search_size3_4_ms,
                                        uint64_t panel_pivot_search_size5_8_calls,
                                        double panel_pivot_search_size5_8_ms,
                                        uint64_t panel_pivot_search_size9p_calls,
                                        double panel_pivot_search_size9p_ms,
                                        uint64_t panel_pivot_search_reserved_present_calls,
                                        uint64_t panel_pivot_search_reserved_present_entries,
                                        double panel_pivot_search_reserved_present_ms,
                                        uint64_t panel_pivot_search_reserved_alt_chosen_calls,
                                        double panel_pivot_search_reserved_alt_chosen_ms,
                                        uint64_t size1_u_emit_calls,
                                        double size1_u_emit_ms,
                                        uint64_t size1_update_scan_calls,
                                        double size1_update_scan_ms,
                                        uint64_t size1_update_apply_calls,
                                        double size1_update_apply_ms,
                                        double size1_update_row_gather_ms,
                                        double size1_update_col_indirection_ms,
                                        double size1_update_outer_product_ms,
                                        uint64_t size1_update_full_calls,
                                        double size1_update_full_ms,
                                        uint64_t size1_update_cols1_calls,
                                        double size1_update_cols1_ms,
                                        uint64_t size1_update_cols2_calls,
                                        double size1_update_cols2_ms,
                                        uint64_t size1_update_cols3_calls,
                                        double size1_update_cols3_ms,
                                        uint64_t size1_update_cols4_calls,
                                        double size1_update_cols4_ms,
                                        uint64_t size1_update_cols5p_calls,
                                        double size1_update_cols5p_ms,
                                        uint64_t size1_update_cols5p_rows1_8_calls,
                                        double size1_update_cols5p_rows1_8_ms,
                                        uint64_t size1_update_cols5p_rows9_32_calls,
                                        double size1_update_cols5p_rows9_32_ms,
                                        uint64_t size1_update_cols5p_rows33_128_calls,
                                        double size1_update_cols5p_rows33_128_ms,
                                        uint64_t size1_update_cols5p_rows129p_calls,
                                        double size1_update_cols5p_rows129p_ms,
                                        double u_emit_ms,
                                        double active_set_ms,
                                        double pack_blocks_ms,
                                        double full_update_ms,
                                        double compact_update_ms,
                                        uint64_t active_row_scan_entries,
                                        uint64_t active_col_scan_entries,
                                        uint64_t trailing_rows_total,
                                        uint64_t trailing_cols_total,
                                        uint64_t active_rows_total,
                                        uint64_t active_cols_total,
                                        uint64_t pack_l_entries_total,
                                        uint64_t pack_u_entries_total,
                                        uint64_t dense_triplets_total,
                                        uint64_t compact_triplets_total,
                                        uint64_t full_update_calls,
                                        uint64_t compact_update_calls,
                                        uint64_t skipped_update_calls,
                                        uint64_t compact_cols1_calls,
                                        uint64_t compact_cols1_rows_total,
                                        double compact_cols1_ms,
                                        uint64_t compact_cols2_calls,
                                        uint64_t compact_cols2_rows_total,
                                        double compact_cols2_ms,
                                        uint64_t compact_cols3_calls,
                                        uint64_t compact_cols3_rows_total,
                                        double compact_cols3_ms,
                                        uint64_t compact_cols4_calls,
                                        uint64_t compact_cols4_rows_total,
                                        double compact_cols4_ms,
                                        uint64_t compact_cols5p_calls,
                                        uint64_t compact_cols5p_rows_total,
                                        double compact_cols5p_ms);

#endif /* LP_TELEMETRY_H */
