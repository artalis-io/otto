/*
 * Ralph - LP Solver Telemetry Helpers
 *
 * Solver-level reset, refactor bookkeeping, and snapshot export.
 */

#include <string.h>
#include "lp.h"
#include "lp_refactor_policy.h"

static int solver_telemetry_enabled(const SimplexSolver *solver) {
    return solver && solver->telemetry_enabled;
}

void lp_telemetry_reset_solver(SimplexSolver *solver) {
    int governor_mode = LP_BASIS_GOV_MODE_OFF;
    int reinvert_mode = LP_REINVERT_MODE_SHADOW;
    if (!solver) return;
    governor_mode = solver->policy.basis_governor_mode;
    if (!lp_basis_governor_mode_is_valid(governor_mode)) {
        governor_mode = LP_BASIS_GOV_MODE_OFF;
    }
    reinvert_mode = solver->policy.reinvert_controller_mode;
    if (!lp_reinvert_controller_mode_is_valid(reinvert_mode)) {
        reinvert_mode = LP_REINVERT_MODE_SHADOW;
    }
    solver->telemetry.perf_primal_setup_ms = 0.0;
    solver->telemetry.perf_dual_ms = 0.0;
    solver->telemetry.perf_phase1_ms = 0.0;
    solver->telemetry.perf_transition_ms = 0.0;
    solver->telemetry.perf_phase2_ms = 0.0;
    solver->telemetry.perf_pricing_ms = 0.0;
    solver->telemetry.perf_ratio_ms = 0.0;
    solver->telemetry.perf_pivot_ms = 0.0;
    solver->telemetry.perf_refactor_ms = 0.0;
    solver->telemetry.perf_ftran_ms = 0.0;
    solver->telemetry.perf_btran_ms = 0.0;
    solver->telemetry.perf_ftran_base_ms = 0.0;
    solver->telemetry.perf_ftran_update_apply_ms = 0.0;
    solver->telemetry.perf_ftran_update_apply_calls = 0;
    solver->telemetry.perf_btran_base_ms = 0.0;
    solver->telemetry.perf_btran_update_apply_ms = 0.0;
    solver->telemetry.perf_btran_update_apply_calls = 0;
    solver->telemetry.perf_ftran_calls = 0;
    solver->telemetry.perf_btran_calls = 0;
    solver->telemetry.perf_ftran_nnz_samples = 0;
    solver->telemetry.perf_btran_nnz_samples = 0;
    solver->telemetry.perf_ftran_rhs_nnz_total = 0;
    solver->telemetry.perf_ftran_sol_nnz_total = 0;
    solver->telemetry.perf_btran_rhs_nnz_total = 0;
    solver->telemetry.perf_btran_sol_nnz_total = 0;
    solver->telemetry.perf_lu_update_ms = 0.0;
    solver->telemetry.perf_compute_solution_ms = 0.0;
    solver->telemetry.perf_compute_rc_ms = 0.0;
    solver->telemetry.perf_refactor_all_ms = 0.0;
    solver->telemetry.perf_refactor_count = 0;
    solver->telemetry.perf_refactor_last_ms = 0.0;
    solver->telemetry.perf_refactor_max_ms = 0.0;
    solver->telemetry.perf_refactor_last_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->policy.refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->telemetry.perf_refactor_reason_setup = 0;
    solver->telemetry.perf_refactor_reason_transition = 0;
    solver->telemetry.perf_refactor_reason_periodic = 0;
    solver->telemetry.perf_refactor_reason_ratio_recovery = 0;
    solver->telemetry.perf_refactor_reason_pivot_recovery = 0;
    solver->telemetry.perf_refactor_reason_forced_small_pivot = 0;
    solver->telemetry.perf_refactor_reason_update_recovery = 0;
    solver->telemetry.perf_refactor_reason_direction_stabilize = 0;
    solver->telemetry.perf_refactor_reason_infeas_cleanup = 0;
    solver->telemetry.perf_refactor_reason_other = 0;
    solver->telemetry.perf_refactor_periodic_policy = 0;
    solver->telemetry.perf_refactor_periodic_lu_health = 0;
    solver->telemetry.perf_refactor_safety_forced = 0;
    solver->telemetry.perf_basis_fastpath_hits = 0;
    solver->telemetry.perf_basis_cols_rewritten = 0;
    solver->telemetry.perf_basis_tail_shift_bytes = 0ULL;
    solver->telemetry.perf_refactor_last_m = 0;
    solver->telemetry.perf_refactor_last_k = 0;
    solver->telemetry.perf_refactor_last_nnz_B = 0;

    solver->telemetry.perf_phase1_pricing_ms = 0.0;
    solver->telemetry.perf_phase1_ratio_ms = 0.0;
    solver->telemetry.perf_phase1_pivot_ms = 0.0;
    solver->telemetry.perf_phase1_refactor_ms = 0.0;
    solver->telemetry.perf_phase1_compute_solution_ms = 0.0;
    solver->telemetry.perf_phase1_compute_rc_ms = 0.0;
    solver->telemetry.perf_phase1_pricing_calls = 0;
    solver->telemetry.perf_phase1_ratio_calls = 0;
    solver->telemetry.perf_phase1_pivot_calls = 0;
    solver->telemetry.perf_phase1_refactor_calls = 0;
    solver->telemetry.perf_phase1_compute_solution_calls = 0;
    solver->telemetry.perf_phase1_compute_rc_calls = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_other = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_recompute_full = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_recompute_guard_forced_full = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_init = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_no_entering_cleanup = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_infeas_cleanup = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_refactor_fail_continue = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_refactor_failure_recovery = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_refactor_success = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_drift_refresh = 0;
    solver->telemetry.perf_phase1_compute_solution_ctx_dual_rescue = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_other = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_recompute_full = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_recompute_rc_only = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_recompute_guard_forced_full = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_init = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_infeas_cleanup = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_refactor_fail_continue = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_refactor_failure_recovery = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_refactor_success = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_drift_refresh = 0;
    solver->telemetry.perf_phase1_compute_rc_ctx_dual_rescue = 0;
    solver->telemetry.perf_phase1_entering_exclusions = 0;
    solver->telemetry.perf_phase1_entering_exclusion_repeats = 0;
    solver->telemetry.perf_phase1_entering_exclusion_hits = 0;
    solver->telemetry.perf_phase1_entering_exclusion_reroutes = 0;
    solver->telemetry.perf_phase1_entering_exclusion_no_alt = 0;
    solver->telemetry.perf_phase1_refactor_periodic_policy = 0;
    solver->telemetry.perf_phase1_refactor_periodic_lu_health = 0;
    solver->telemetry.perf_phase1_refactor_safety_forced = 0;
    solver->telemetry.perf_phase1_dir_stabilize_force_extreme_dir = 0;
    solver->telemetry.perf_phase1_dir_stabilize_force_lu_health = 0;
    solver->telemetry.perf_phase1_dir_stabilize_cooldown_candidates = 0;
    solver->telemetry.perf_phase1_dir_stabilize_ratio_le_3 = 0;
    solver->telemetry.perf_phase1_dir_stabilize_ratio_le_10 = 0;
    solver->telemetry.perf_phase1_dir_stabilize_ratio_le_30 = 0;
    solver->telemetry.perf_phase1_dir_stabilize_ratio_le_100 = 0;
    solver->telemetry.perf_phase1_dir_stabilize_ratio_gt_100 = 0;
    solver->telemetry.perf_phase1_dir_stabilize_ratio_gt_300 = 0;
    solver->telemetry.perf_phase1_dir_stabilize_ratio_gt_1000 = 0;
    solver->telemetry.perf_phase1_dir_stabilize_skip_rc_only = 0;
    solver->telemetry.perf_phase1_dir_stabilize_skip_full = 0;
    solver->telemetry.perf_phase1_dir_stabilize_skip_no_recompute = 0;
    solver->telemetry.perf_phase1_dir_stabilize_skip_guard_refresh = 0;
    solver->telemetry.perf_phase1_dir_stabilize_escape_gate_triggers = 0;
    solver->telemetry.perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health = 0;
    solver->telemetry.perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode = 0;
    solver->telemetry.perf_phase1_dir_stabilize_escape_gate_hard_bypass = 0;
    solver->telemetry.perf_phase1_dir_stabilize_refactor_from_no_pivot_force = 0;
    solver->telemetry.perf_phase1_dir_stabilize_refactor_from_force_extreme_dir = 0;
    solver->telemetry.perf_phase1_dir_stabilize_refactor_from_force_lu_health = 0;
    solver->telemetry.perf_phase1_dir_stabilize_refactor_from_force_pivot_mode = 0;
    solver->telemetry.perf_phase1_dir_stabilize_refactor_from_ladder_force = 0;
    solver->telemetry.perf_phase1_force_pivot_relax_applied = 0;
    solver->telemetry.perf_phase1_force_extreme_relax_applied = 0;
    solver->telemetry.perf_phase1_recompute_after_ratio_breakdown = 0;
    solver->telemetry.perf_phase1_recompute_after_dir_skip = 0;
    solver->telemetry.perf_phase1_recompute_after_dir_refactor = 0;
    solver->telemetry.perf_phase1_recompute_after_pivot_fail_recovery = 0;
    solver->telemetry.perf_phase1_recompute_after_perturb = 0;
    solver->telemetry.perf_phase1_recompute_rc_only_calls = 0;
    solver->telemetry.perf_phase1_recompute_rc_guard_forced_full = 0;
    solver->telemetry.perf_phase1_ratio_breakdown_retries = 0;
    solver->telemetry.perf_phase1_ratio_breakdown_escalations = 0;
    solver->telemetry.perf_phase1_pivot_fail_recovery_exclusions = 0;
    solver->telemetry.perf_phase1_no_pivot_events = 0;
    solver->telemetry.perf_phase1_no_pivot_forced_refactor = 0;
    solver->telemetry.perf_phase1_no_pivot_forced_ratio_breakdown = 0;
    solver->telemetry.perf_phase1_no_pivot_forced_dir_skip = 0;
    solver->telemetry.perf_phase1_no_pivot_forced_pivot_fail = 0;
    solver->telemetry.perf_phase1_no_pivot_events_ratio_breakdown = 0;
    solver->telemetry.perf_phase1_no_pivot_events_dir_skip = 0;
    solver->telemetry.perf_phase1_no_pivot_events_pivot_fail = 0;
    solver->telemetry.perf_phase1_no_pivot_no_progress_events = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_retry_defers = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_retry_ratio_breakdown = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_retry_dir_skip = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_retry_pivot_fail = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_successes = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_failures = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors_dir_skip = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks = 0;
    solver->telemetry.perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces = 0;
    solver->telemetry.perf_phase1_direct_dual_rescue_attempts = 0;
    solver->telemetry.perf_phase1_direct_dual_rescue_successes = 0;
    solver->telemetry.perf_phase1_direct_dual_rescue_failures = 0;
    solver->telemetry.perf_phase1_direct_dual_rescue_guard_cooldown_blocks = 0;
    solver->telemetry.perf_phase1_direct_dual_rescue_guard_fail_cap_blocks = 0;
    solver->telemetry.perf_phase1_soft_lu_policy_cooldown_defers = 0;
    solver->telemetry.perf_phase1_dir_skip_same_entering_repeats = 0;
    solver->telemetry.perf_phase1_dir_skip_same_entering_max_streak = 0;
    solver->telemetry.perf_phase1_failed_stabilize_events = 0;
    solver->telemetry.perf_phase1_failed_stabilize_primary_failures = 0;
    solver->telemetry.perf_phase1_failed_stabilize_alternate_failures = 0;
    solver->telemetry.perf_phase1_failed_stabilize_same_entering_repeats = 0;
    solver->telemetry.perf_phase1_failed_stabilize_same_entering_max_streak = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_arms = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_found = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_no_alt = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_failed = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_arms = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_found = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_no_alt = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized = 0;
    solver->telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_failed = 0;

    solver->telemetry.perf_phase2_pricing_ms = 0.0;
    solver->telemetry.perf_phase2_ratio_ms = 0.0;
    solver->telemetry.perf_phase2_pivot_ms = 0.0;
    solver->telemetry.perf_phase2_refactor_ms = 0.0;
    solver->telemetry.perf_phase2_compute_solution_ms = 0.0;
    solver->telemetry.perf_phase2_compute_rc_ms = 0.0;
    solver->telemetry.perf_phase2_pricing_calls = 0;
    solver->telemetry.perf_phase2_ratio_calls = 0;
    solver->telemetry.perf_phase2_pivot_calls = 0;
    solver->telemetry.perf_phase2_refactor_calls = 0;
    solver->telemetry.perf_phase2_compute_solution_calls = 0;
    solver->telemetry.perf_phase2_compute_rc_calls = 0;
    solver->telemetry.perf_phase2_refactor_periodic_policy = 0;
    solver->telemetry.perf_phase2_refactor_periodic_lu_health = 0;
    solver->telemetry.perf_phase2_refactor_safety_forced = 0;
    solver->telemetry.perf_dual_ratio_no_entering = 0;
    solver->telemetry.perf_dual_theta_nonpositive = 0;
    solver->telemetry.perf_dual_pivot_reject_small = 0;
    solver->telemetry.perf_dual_bound_flip_applied = 0;
    solver->telemetry.perf_dual_bound_flip_startup = 0;
    solver->telemetry.perf_dual_bound_flip_iterative = 0;
    solver->telemetry.perf_dual_lu_hard_trigger = 0;
    solver->telemetry.perf_reinvert_shadow_checks_phase1 = 0;
    solver->telemetry.perf_reinvert_shadow_checks_phase2 = 0;
    solver->telemetry.perf_reinvert_shadow_checks_dual = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_allow_phase1 = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_allow_phase2 = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_allow_dual = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_defer_phase1 = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_defer_phase2 = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_defer_dual = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_force_phase1 = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_force_phase2 = 0;
    solver->telemetry.perf_reinvert_shadow_suggest_force_dual = 0;
    solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_phase1 = 0;
    solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_phase2 = 0;
    solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_dual = 0;
    solver->telemetry.perf_reinvert_shadow_actual_refactor_no_phase1 = 0;
    solver->telemetry.perf_reinvert_shadow_actual_refactor_no_phase2 = 0;
    solver->telemetry.perf_reinvert_shadow_actual_refactor_no_dual = 0;
    solver->telemetry.perf_reinvert_shadow_disagree_phase1 = 0;
    solver->telemetry.perf_reinvert_shadow_disagree_phase2 = 0;
    solver->telemetry.perf_reinvert_shadow_disagree_dual = 0;
    solver->telemetry.perf_reinvert_shadow_last_reason_phase1 = LP_REINVERT_REASON_NONE;
    solver->telemetry.perf_reinvert_shadow_last_reason_phase2 = LP_REINVERT_REASON_NONE;
    solver->telemetry.perf_reinvert_shadow_last_reason_dual = LP_REINVERT_REASON_NONE;

    solver->policy.periodic_feedback_phase1.bias = 0.0;
    solver->policy.periodic_feedback_phase2.bias = 0.0;
    solver->policy.periodic_feedback_phase1.last_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->policy.periodic_feedback_phase2.last_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->policy.periodic_feedback_phase1.last_interval = 0;
    solver->policy.periodic_feedback_phase2.last_interval = 0;
    solver->policy.periodic_feedback_phase1.hint_interval = 0;
    solver->policy.periodic_feedback_phase2.hint_interval = 0;
    solver->policy.periodic_feedback_phase1.hint_pressure = 0.0;
    solver->policy.periodic_feedback_phase2.hint_pressure = 0.0;
    solver->policy.soft_lu_cost_gate_enabled = 1;
    solver->policy.soft_lu_cost_gate_phase1.defers = 0;
    solver->policy.soft_lu_cost_gate_phase2.defers = 0;
    solver->policy.soft_lu_cost_gate_phase1.consecutive_defers = 0;
    solver->policy.soft_lu_cost_gate_phase2.consecutive_defers = 0;
    solver->policy.soft_lu_cost_gate_phase1.defer_cap_forced = 0;
    solver->policy.soft_lu_cost_gate_phase2.defer_cap_forced = 0;
    solver->policy.periodic_cost_gate_enabled = 1;
    solver->policy.periodic_cost_gate_phase1.defers = 0;
    solver->policy.periodic_cost_gate_phase2.defers = 0;
    solver->policy.periodic_cost_gate_phase1.consecutive_defers = 0;
    solver->policy.periodic_cost_gate_phase2.consecutive_defers = 0;
    solver->policy.periodic_cost_gate_phase1.defer_cap_forced = 0;
    solver->policy.periodic_cost_gate_phase2.defer_cap_forced = 0;
    solver->policy.periodic_cost_gate_phase1.checks = 0;
    solver->policy.periodic_cost_gate_phase2.checks = 0;
    solver->policy.periodic_cost_gate_phase1.block_small_m = 0;
    solver->policy.periodic_cost_gate_phase2.block_small_m = 0;
    solver->policy.periodic_cost_gate_phase1.block_invalid_inputs = 0;
    solver->policy.periodic_cost_gate_phase2.block_invalid_inputs = 0;
    solver->policy.periodic_cost_gate_phase1.block_warmup = 0;
    solver->policy.periodic_cost_gate_phase2.block_warmup = 0;
    solver->policy.periodic_cost_gate_phase1.block_invalid_cost = 0;
    solver->policy.periodic_cost_gate_phase2.block_invalid_cost = 0;
    solver->policy.periodic_cost_gate_phase1.block_ratio = 0;
    solver->policy.periodic_cost_gate_phase2.block_ratio = 0;
    solver->policy.periodic_cost_gate_phase1.block_update_reserve = 0;
    solver->policy.periodic_cost_gate_phase2.block_update_reserve = 0;
    solver->policy.periodic_cost_gate_phase1.last_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE;
    solver->policy.periodic_cost_gate_phase2.last_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE;
    solver->policy.periodic_cost_gate_phase1.iter_samples = 0;
    solver->policy.periodic_cost_gate_phase2.iter_samples = 0;
    solver->policy.periodic_cost_gate_phase1.refactor_samples = 0;
    solver->policy.periodic_cost_gate_phase2.refactor_samples = 0;
    solver->policy.soft_lu_cost_gate_phase1.refactor_cost_ewma = 0.0;
    solver->policy.soft_lu_cost_gate_phase2.refactor_cost_ewma = 0.0;
    solver->policy.soft_lu_cost_gate_phase1.iter_cost_ewma = 0.0;
    solver->policy.soft_lu_cost_gate_phase2.iter_cost_ewma = 0.0;
    lp_reinvert_controller_state_reset(&solver->policy.reinvert_state_phase1);
    lp_reinvert_controller_state_reset(&solver->policy.reinvert_state_phase2);
    lp_reinvert_controller_state_reset(&solver->policy.reinvert_state_dual);
    solver->policy.reinvert_dual.last_hot_ms = 0.0;
    solver->policy.reinvert_dual.control_demoted = 0;
    solver->policy.reinvert_dual.control_demotions = 0;
    solver->policy.reinvert_dual.hard_trigger_last_total = 0;
    solver->policy.reinvert_dual.hard_trigger_last_iter = -1;
    solver->policy.reinvert_dual.hard_trigger_burst = 0;
    solver->policy.reinvert_phase1.control_demoted = 0;
    solver->policy.reinvert_phase1.control_demotions = 0;
    solver->policy.reinvert_phase1.pressure_last_iter = -1;
    solver->policy.reinvert_phase1.pressure_burst = 0;
    solver->policy.phase1_stagnation.window_start_iter = -1;
    solver->policy.phase1_stagnation.window_start_obj = 0.0;
    solver->policy.phase1_stagnation.window_retry_base = 0;
    solver->policy.phase1_stagnation.window_no_pivot_base = 0;
    solver->policy.phase1_stagnation.window_refactor_base = 0;
    solver->policy.phase1_stagnation.window_update_recovery_base = 0;
    solver->policy.phase1_stagnation.window_recompute_ratio_base = 0;
    solver->policy.phase1_stagnation.window_recompute_dir_skip_base = 0;
    solver->policy.phase1_stagnation.window_recompute_dir_refactor_base = 0;
    solver->policy.phase1_stagnation.window_recompute_pivot_fail_base = 0;
    solver->policy.phase1_stagnation.window_recompute_perturb_base = 0;
    solver->policy.phase1_stagnation.escape_cooldown = 0;
    solver->policy.phase1_stagnation.escape_triggers = 0;
    solver->policy.phase1_stagnation.escape_successes = 0;
    solver->policy.phase1_stagnation.escape_failures = 0;
    solver->policy.phase1_stagnation.escape_cooldown_blocks = 0;
    solver->policy.phase1_stagnation.last_window_iters = 0;
    solver->policy.phase1_stagnation.last_obj_delta = 0.0;
    solver->policy.phase1_stagnation.last_retry_defer_ratio = 0.0;
    solver->policy.phase1_stagnation.last_update_recovery_ratio = 0.0;
    solver->policy.phase1_stagnation.last_retry_defers = 0;
    solver->policy.phase1_stagnation.last_no_pivot_events = 0;
    solver->policy.phase1_stagnation.last_update_recovery_refactors = 0;
    solver->policy.phase1_stagnation.last_refactors = 0;
    solver->policy.phase1_stagnation.last_recompute_ratio = 0;
    solver->policy.phase1_stagnation.last_recompute_dir_skip = 0;
    solver->policy.phase1_stagnation.last_recompute_dir_refactor = 0;
    solver->policy.phase1_stagnation.last_recompute_pivot_fail = 0;
    solver->policy.phase1_stagnation.last_recompute_perturb = 0;
    solver->policy.basis_governor_mode = governor_mode;
    solver->policy.reinvert_controller_mode = reinvert_mode;
    lp_basis_governor_begin_solve(&solver->policy.basis_governor);
    lp_basis_governor_set_mode(&solver->policy.basis_governor, governor_mode);
}

void lp_telemetry_record_basis_build(SimplexSolver *owner,
                                     int fastpath_hit,
                                     int cols_rewritten,
                                     unsigned long long tail_shift_bytes) {
    if (!solver_telemetry_enabled(owner)) return;
    if (fastpath_hit) owner->telemetry.perf_basis_fastpath_hits++;
    if (cols_rewritten > 0) owner->telemetry.perf_basis_cols_rewritten += cols_rewritten;
    owner->telemetry.perf_basis_tail_shift_bytes += tail_shift_bytes;
}

void lp_telemetry_begin_refactor(SimplexSolver *owner, int *reason_out) {
    if (!reason_out) return;
    *reason_out = RALPH_REFACTOR_REASON_OTHER;
    if (!owner) return;
    *reason_out = owner->policy.refactor_next_reason;
    owner->policy.refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
}

void lp_telemetry_set_refactor_next_reason(SimplexSolver *owner, int reason) {
    if (!owner) return;
    owner->policy.refactor_next_reason = reason;
}

void lp_telemetry_record_refactor(SimplexSolver *owner,
                                  int phase,
                                  int reason,
                                  double elapsed_ms,
                                  int m,
                                  int lu_last_k,
                                  int lu_last_basis_nnz) {
    if (!solver_telemetry_enabled(owner)) return;

    owner->telemetry.perf_refactor_all_ms += elapsed_ms;
    owner->telemetry.perf_refactor_count++;
    owner->telemetry.perf_refactor_last_ms = elapsed_ms;
    if (elapsed_ms > owner->telemetry.perf_refactor_max_ms) {
        owner->telemetry.perf_refactor_max_ms = elapsed_ms;
    }
    owner->telemetry.perf_refactor_last_reason = reason;

    switch ((RalphRefactorReason)reason) {
        case RALPH_REFACTOR_REASON_SETUP:
            owner->telemetry.perf_refactor_reason_setup++;
            break;
        case RALPH_REFACTOR_REASON_PHASE_TRANSITION:
            owner->telemetry.perf_refactor_reason_transition++;
            break;
        case RALPH_REFACTOR_REASON_PERIODIC:
            owner->telemetry.perf_refactor_reason_periodic++;
            break;
        case RALPH_REFACTOR_REASON_RATIO_RECOVERY:
            owner->telemetry.perf_refactor_reason_ratio_recovery++;
            break;
        case RALPH_REFACTOR_REASON_PIVOT_RECOVERY:
            owner->telemetry.perf_refactor_reason_pivot_recovery++;
            break;
        case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT:
            owner->telemetry.perf_refactor_reason_forced_small_pivot++;
            break;
        case RALPH_REFACTOR_REASON_UPDATE_RECOVERY:
            owner->telemetry.perf_refactor_reason_update_recovery++;
            break;
        case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE:
            owner->telemetry.perf_refactor_reason_direction_stabilize++;
            break;
        case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP:
            owner->telemetry.perf_refactor_reason_infeas_cleanup++;
            break;
        case RALPH_REFACTOR_REASON_OTHER:
        default:
            owner->telemetry.perf_refactor_reason_other++;
            break;
    }

    if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
        owner->telemetry.perf_refactor_safety_forced++;
    }

    owner->telemetry.perf_refactor_last_m = m;
    owner->telemetry.perf_refactor_last_k = lu_last_k;
    owner->telemetry.perf_refactor_last_nnz_B = lu_last_basis_nnz;

    if (phase == 1) {
        owner->telemetry.perf_phase1_refactor_ms += elapsed_ms;
        owner->telemetry.perf_phase1_refactor_calls++;
        if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
            owner->telemetry.perf_phase1_refactor_safety_forced++;
        }
    } else if (phase == 2) {
        owner->telemetry.perf_phase2_refactor_ms += elapsed_ms;
        owner->telemetry.perf_phase2_refactor_calls++;
        if (lp_telemetry_refactor_reason_is_safety_forced(reason)) {
            owner->telemetry.perf_phase2_refactor_safety_forced++;
        }
    }
}

void lp_telemetry_record_refactor_with_lu(SimplexSolver *owner,
                                          int phase,
                                          int reason,
                                          double elapsed_ms,
                                          int m,
                                          const LUFactorization *lu) {
    int lu_last_k = lu ? lu->telemetry.perf_last_k : 0;
    int lu_last_basis_nnz = lu ? lu->telemetry.perf_last_basis_nnz : 0;
    lp_telemetry_record_refactor(owner,
                                 phase,
                                 reason,
                                 elapsed_ms,
                                 m,
                                 lu_last_k,
                                 lu_last_basis_nnz);
}

void lp_telemetry_record_refactor_with_lu_timed(SimplexSolver *owner,
                                                int phase,
                                                int reason,
                                                double start_ms,
                                                int m,
                                                const LUFactorization *lu) {
    lp_telemetry_record_refactor_with_lu(owner,
                                         phase,
                                         reason,
                                         lp_telemetry_timer_elapsed_ms(start_ms),
                                         m,
                                         lu);
}

#define COPY_SOLVER_FIELD(field) out->field = solver->telemetry.field
void lp_telemetry_snapshot_solver(const SimplexSolver *solver,
                                  LPSolverTelemetrySnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!solver) return;

    COPY_SOLVER_FIELD(perf_primal_setup_ms);
    COPY_SOLVER_FIELD(perf_dual_ms);
    COPY_SOLVER_FIELD(perf_phase1_ms);
    COPY_SOLVER_FIELD(perf_transition_ms);
    COPY_SOLVER_FIELD(perf_phase2_ms);
    COPY_SOLVER_FIELD(perf_pricing_ms);
    COPY_SOLVER_FIELD(perf_ratio_ms);
    COPY_SOLVER_FIELD(perf_pivot_ms);
    COPY_SOLVER_FIELD(perf_refactor_ms);
    COPY_SOLVER_FIELD(perf_ftran_ms);
    COPY_SOLVER_FIELD(perf_btran_ms);
    COPY_SOLVER_FIELD(perf_ftran_base_ms);
    COPY_SOLVER_FIELD(perf_ftran_update_apply_ms);
    COPY_SOLVER_FIELD(perf_ftran_update_apply_calls);
    COPY_SOLVER_FIELD(perf_btran_base_ms);
    COPY_SOLVER_FIELD(perf_btran_update_apply_ms);
    COPY_SOLVER_FIELD(perf_btran_update_apply_calls);
    COPY_SOLVER_FIELD(perf_ftran_calls);
    COPY_SOLVER_FIELD(perf_btran_calls);
    COPY_SOLVER_FIELD(perf_ftran_nnz_samples);
    COPY_SOLVER_FIELD(perf_btran_nnz_samples);
    COPY_SOLVER_FIELD(perf_ftran_rhs_nnz_total);
    COPY_SOLVER_FIELD(perf_ftran_sol_nnz_total);
    COPY_SOLVER_FIELD(perf_btran_rhs_nnz_total);
    COPY_SOLVER_FIELD(perf_btran_sol_nnz_total);
    COPY_SOLVER_FIELD(perf_lu_update_ms);
    COPY_SOLVER_FIELD(perf_compute_solution_ms);
    COPY_SOLVER_FIELD(perf_compute_rc_ms);
    COPY_SOLVER_FIELD(perf_refactor_all_ms);
    COPY_SOLVER_FIELD(perf_refactor_count);
    COPY_SOLVER_FIELD(perf_refactor_last_ms);
    COPY_SOLVER_FIELD(perf_refactor_max_ms);
    COPY_SOLVER_FIELD(perf_refactor_last_reason);
    out->perf_refactor_next_reason = solver->policy.refactor_next_reason;
    COPY_SOLVER_FIELD(perf_refactor_reason_setup);
    COPY_SOLVER_FIELD(perf_refactor_reason_transition);
    COPY_SOLVER_FIELD(perf_refactor_reason_periodic);
    COPY_SOLVER_FIELD(perf_refactor_reason_ratio_recovery);
    COPY_SOLVER_FIELD(perf_refactor_reason_pivot_recovery);
    COPY_SOLVER_FIELD(perf_refactor_reason_forced_small_pivot);
    COPY_SOLVER_FIELD(perf_refactor_reason_update_recovery);
    COPY_SOLVER_FIELD(perf_refactor_reason_direction_stabilize);
    COPY_SOLVER_FIELD(perf_refactor_reason_infeas_cleanup);
    COPY_SOLVER_FIELD(perf_refactor_reason_other);
    COPY_SOLVER_FIELD(perf_refactor_periodic_policy);
    COPY_SOLVER_FIELD(perf_refactor_periodic_lu_health);
    COPY_SOLVER_FIELD(perf_refactor_safety_forced);
    COPY_SOLVER_FIELD(perf_basis_fastpath_hits);
    COPY_SOLVER_FIELD(perf_basis_cols_rewritten);
    COPY_SOLVER_FIELD(perf_basis_tail_shift_bytes);
    COPY_SOLVER_FIELD(perf_refactor_last_m);
    COPY_SOLVER_FIELD(perf_refactor_last_k);
    COPY_SOLVER_FIELD(perf_refactor_last_nnz_B);

    COPY_SOLVER_FIELD(perf_phase1_pricing_ms);
    COPY_SOLVER_FIELD(perf_phase1_ratio_ms);
    COPY_SOLVER_FIELD(perf_phase1_pivot_ms);
    COPY_SOLVER_FIELD(perf_phase1_refactor_ms);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ms);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ms);
    COPY_SOLVER_FIELD(perf_phase1_pricing_calls);
    COPY_SOLVER_FIELD(perf_phase1_ratio_calls);
    COPY_SOLVER_FIELD(perf_phase1_pivot_calls);
    COPY_SOLVER_FIELD(perf_phase1_refactor_calls);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_calls);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_calls);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_other);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_recompute_full);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_recompute_guard_forced_full);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_init);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_no_entering_cleanup);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_infeas_cleanup);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_refactor_fail_continue);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_refactor_failure_recovery);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_refactor_success);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_drift_refresh);
    COPY_SOLVER_FIELD(perf_phase1_compute_solution_ctx_dual_rescue);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_other);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_recompute_full);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_recompute_rc_only);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_recompute_guard_forced_full);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_init);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_infeas_cleanup);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_refactor_fail_continue);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_refactor_failure_recovery);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_refactor_success);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_drift_refresh);
    COPY_SOLVER_FIELD(perf_phase1_compute_rc_ctx_dual_rescue);
    COPY_SOLVER_FIELD(perf_phase1_entering_exclusions);
    COPY_SOLVER_FIELD(perf_phase1_entering_exclusion_repeats);
    COPY_SOLVER_FIELD(perf_phase1_entering_exclusion_hits);
    COPY_SOLVER_FIELD(perf_phase1_entering_exclusion_reroutes);
    COPY_SOLVER_FIELD(perf_phase1_entering_exclusion_no_alt);
    COPY_SOLVER_FIELD(perf_phase1_refactor_periodic_policy);
    COPY_SOLVER_FIELD(perf_phase1_refactor_periodic_lu_health);
    COPY_SOLVER_FIELD(perf_phase1_refactor_safety_forced);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_force_extreme_dir);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_force_lu_health);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_cooldown_candidates);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_ratio_le_3);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_ratio_le_10);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_ratio_le_30);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_ratio_le_100);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_ratio_gt_100);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_ratio_gt_300);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_ratio_gt_1000);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_skip_rc_only);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_skip_full);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_skip_no_recompute);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_skip_guard_refresh);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_escape_gate_triggers);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_escape_gate_hard_bypass);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_refactor_from_no_pivot_force);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_refactor_from_force_extreme_dir);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_refactor_from_force_lu_health);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_refactor_from_force_pivot_mode);
    COPY_SOLVER_FIELD(perf_phase1_dir_stabilize_refactor_from_ladder_force);
    COPY_SOLVER_FIELD(perf_phase1_force_pivot_relax_applied);
    COPY_SOLVER_FIELD(perf_phase1_force_extreme_relax_applied);
    COPY_SOLVER_FIELD(perf_phase1_recompute_after_ratio_breakdown);
    COPY_SOLVER_FIELD(perf_phase1_recompute_after_dir_skip);
    COPY_SOLVER_FIELD(perf_phase1_recompute_after_dir_refactor);
    COPY_SOLVER_FIELD(perf_phase1_recompute_after_pivot_fail_recovery);
    COPY_SOLVER_FIELD(perf_phase1_recompute_after_perturb);
    COPY_SOLVER_FIELD(perf_phase1_recompute_rc_only_calls);
    COPY_SOLVER_FIELD(perf_phase1_recompute_rc_guard_forced_full);
    COPY_SOLVER_FIELD(perf_phase1_ratio_breakdown_retries);
    COPY_SOLVER_FIELD(perf_phase1_ratio_breakdown_escalations);
    COPY_SOLVER_FIELD(perf_phase1_pivot_fail_recovery_exclusions);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_events);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_forced_refactor);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_forced_ratio_breakdown);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_forced_dir_skip);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_forced_pivot_fail);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_events_ratio_breakdown);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_events_dir_skip);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_events_pivot_fail);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_no_progress_events);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_retry_defers);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_retry_ratio_breakdown);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_retry_dir_skip);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_retry_pivot_fail);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_dual_rescue_attempts);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_dual_rescue_successes);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_dual_rescue_failures);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_forced_refactors);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_forced_refactors_dir_skip);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks);
    COPY_SOLVER_FIELD(perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces);
    COPY_SOLVER_FIELD(perf_phase1_direct_dual_rescue_attempts);
    COPY_SOLVER_FIELD(perf_phase1_direct_dual_rescue_successes);
    COPY_SOLVER_FIELD(perf_phase1_direct_dual_rescue_failures);
    COPY_SOLVER_FIELD(perf_phase1_direct_dual_rescue_guard_cooldown_blocks);
    COPY_SOLVER_FIELD(perf_phase1_direct_dual_rescue_guard_fail_cap_blocks);
    COPY_SOLVER_FIELD(perf_phase1_soft_lu_policy_cooldown_defers);
    COPY_SOLVER_FIELD(perf_phase1_dir_skip_same_entering_repeats);
    COPY_SOLVER_FIELD(perf_phase1_dir_skip_same_entering_max_streak);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_events);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_primary_failures);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_alternate_failures);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_same_entering_repeats);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_same_entering_max_streak);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_penalty_arms);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_penalty_alt_found);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_penalty_no_alt);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_penalty_alt_stabilized);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_penalty_alt_failed);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_local_memory_arms);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_local_memory_alt_found);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_local_memory_no_alt);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized);
    COPY_SOLVER_FIELD(perf_phase1_failed_stabilize_retry_local_memory_alt_failed);

    COPY_SOLVER_FIELD(perf_phase2_pricing_ms);
    COPY_SOLVER_FIELD(perf_phase2_ratio_ms);
    COPY_SOLVER_FIELD(perf_phase2_pivot_ms);
    COPY_SOLVER_FIELD(perf_phase2_refactor_ms);
    COPY_SOLVER_FIELD(perf_phase2_compute_solution_ms);
    COPY_SOLVER_FIELD(perf_phase2_compute_rc_ms);
    COPY_SOLVER_FIELD(perf_phase2_pricing_calls);
    COPY_SOLVER_FIELD(perf_phase2_ratio_calls);
    COPY_SOLVER_FIELD(perf_phase2_pivot_calls);
    COPY_SOLVER_FIELD(perf_phase2_refactor_calls);
    COPY_SOLVER_FIELD(perf_phase2_compute_solution_calls);
    COPY_SOLVER_FIELD(perf_phase2_compute_rc_calls);
    COPY_SOLVER_FIELD(perf_phase2_refactor_periodic_policy);
    COPY_SOLVER_FIELD(perf_phase2_refactor_periodic_lu_health);
    COPY_SOLVER_FIELD(perf_phase2_refactor_safety_forced);
    COPY_SOLVER_FIELD(perf_dual_ratio_no_entering);
    COPY_SOLVER_FIELD(perf_dual_theta_nonpositive);
    COPY_SOLVER_FIELD(perf_dual_pivot_reject_small);
    COPY_SOLVER_FIELD(perf_dual_bound_flip_applied);
    COPY_SOLVER_FIELD(perf_dual_bound_flip_startup);
    COPY_SOLVER_FIELD(perf_dual_bound_flip_iterative);
    COPY_SOLVER_FIELD(perf_dual_lu_hard_trigger);

    out->periodic_feedback_bias_phase1 = solver->policy.periodic_feedback_phase1.bias;
    out->periodic_feedback_bias_phase2 = solver->policy.periodic_feedback_phase2.bias;
    out->periodic_feedback_last_reason_phase1 = solver->policy.periodic_feedback_phase1.last_reason;
    out->periodic_feedback_last_reason_phase2 = solver->policy.periodic_feedback_phase2.last_reason;
    out->periodic_feedback_last_interval_phase1 = solver->policy.periodic_feedback_phase1.last_interval;
    out->periodic_feedback_last_interval_phase2 = solver->policy.periodic_feedback_phase2.last_interval;
    out->periodic_feedback_hint_interval_phase1 = solver->policy.periodic_feedback_phase1.hint_interval;
    out->periodic_feedback_hint_interval_phase2 = solver->policy.periodic_feedback_phase2.hint_interval;
    out->periodic_feedback_hint_pressure_phase1 = solver->policy.periodic_feedback_phase1.hint_pressure;
    out->periodic_feedback_hint_pressure_phase2 = solver->policy.periodic_feedback_phase2.hint_pressure;
    out->soft_lu_cost_gate_enabled = solver->policy.soft_lu_cost_gate_enabled;
    out->soft_lu_cost_gate_defers_phase1 = solver->policy.soft_lu_cost_gate_phase1.defers;
    out->soft_lu_cost_gate_defers_phase2 = solver->policy.soft_lu_cost_gate_phase2.defers;
    out->soft_lu_consecutive_defers_phase1 = solver->policy.soft_lu_cost_gate_phase1.consecutive_defers;
    out->soft_lu_consecutive_defers_phase2 = solver->policy.soft_lu_cost_gate_phase2.consecutive_defers;
    out->soft_lu_defer_cap_forced_phase1 = solver->policy.soft_lu_cost_gate_phase1.defer_cap_forced;
    out->soft_lu_defer_cap_forced_phase2 = solver->policy.soft_lu_cost_gate_phase2.defer_cap_forced;
    out->periodic_cost_gate_enabled = solver->policy.periodic_cost_gate_enabled;
    out->periodic_cost_gate_defers_phase1 = solver->policy.periodic_cost_gate_phase1.defers;
    out->periodic_cost_gate_defers_phase2 = solver->policy.periodic_cost_gate_phase2.defers;
    out->periodic_cost_consecutive_defers_phase1 = solver->policy.periodic_cost_gate_phase1.consecutive_defers;
    out->periodic_cost_consecutive_defers_phase2 = solver->policy.periodic_cost_gate_phase2.consecutive_defers;
    out->periodic_cost_defer_cap_forced_phase1 = solver->policy.periodic_cost_gate_phase1.defer_cap_forced;
    out->periodic_cost_defer_cap_forced_phase2 = solver->policy.periodic_cost_gate_phase2.defer_cap_forced;
    out->periodic_cost_gate_checks_phase1 = solver->policy.periodic_cost_gate_phase1.checks;
    out->periodic_cost_gate_checks_phase2 = solver->policy.periodic_cost_gate_phase2.checks;
    out->periodic_cost_gate_block_small_m_phase1 = solver->policy.periodic_cost_gate_phase1.block_small_m;
    out->periodic_cost_gate_block_small_m_phase2 = solver->policy.periodic_cost_gate_phase2.block_small_m;
    out->periodic_cost_gate_block_invalid_inputs_phase1 = solver->policy.periodic_cost_gate_phase1.block_invalid_inputs;
    out->periodic_cost_gate_block_invalid_inputs_phase2 = solver->policy.periodic_cost_gate_phase2.block_invalid_inputs;
    out->periodic_cost_gate_block_warmup_phase1 = solver->policy.periodic_cost_gate_phase1.block_warmup;
    out->periodic_cost_gate_block_warmup_phase2 = solver->policy.periodic_cost_gate_phase2.block_warmup;
    out->periodic_cost_gate_block_invalid_cost_phase1 = solver->policy.periodic_cost_gate_phase1.block_invalid_cost;
    out->periodic_cost_gate_block_invalid_cost_phase2 = solver->policy.periodic_cost_gate_phase2.block_invalid_cost;
    out->periodic_cost_gate_block_ratio_phase1 = solver->policy.periodic_cost_gate_phase1.block_ratio;
    out->periodic_cost_gate_block_ratio_phase2 = solver->policy.periodic_cost_gate_phase2.block_ratio;
    out->periodic_cost_gate_block_update_reserve_phase1 = solver->policy.periodic_cost_gate_phase1.block_update_reserve;
    out->periodic_cost_gate_block_update_reserve_phase2 = solver->policy.periodic_cost_gate_phase2.block_update_reserve;
    out->periodic_cost_gate_last_reason_phase1 = solver->policy.periodic_cost_gate_phase1.last_reason;
    out->periodic_cost_gate_last_reason_phase2 = solver->policy.periodic_cost_gate_phase2.last_reason;
    out->periodic_cost_iter_samples_phase1 = solver->policy.periodic_cost_gate_phase1.iter_samples;
    out->periodic_cost_iter_samples_phase2 = solver->policy.periodic_cost_gate_phase2.iter_samples;
    out->periodic_cost_refactor_samples_phase1 = solver->policy.periodic_cost_gate_phase1.refactor_samples;
    out->periodic_cost_refactor_samples_phase2 = solver->policy.periodic_cost_gate_phase2.refactor_samples;
    out->soft_lu_refactor_cost_ewma_phase1 = solver->policy.soft_lu_cost_gate_phase1.refactor_cost_ewma;
    out->soft_lu_refactor_cost_ewma_phase2 = solver->policy.soft_lu_cost_gate_phase2.refactor_cost_ewma;
    out->soft_lu_iter_cost_ewma_phase1 = solver->policy.soft_lu_cost_gate_phase1.iter_cost_ewma;
    out->soft_lu_iter_cost_ewma_phase2 = solver->policy.soft_lu_cost_gate_phase2.iter_cost_ewma;
    out->basis_governor_mode = lp_basis_governor_get_mode(&solver->policy.basis_governor);
    out->reinvert_controller_mode = lp_reinvert_controller_mode_is_valid(
                                        solver->policy.reinvert_controller_mode)
                                        ? solver->policy.reinvert_controller_mode
                                        : LP_REINVERT_MODE_SHADOW;
    out->reinvert_dual_control_demoted = solver->policy.reinvert_dual.control_demoted;
    out->reinvert_dual_control_demotions = solver->policy.reinvert_dual.control_demotions;
    out->reinvert_dual_hard_trigger_last_total = solver->policy.reinvert_dual.hard_trigger_last_total;
    out->reinvert_dual_hard_trigger_last_iter = solver->policy.reinvert_dual.hard_trigger_last_iter;
    out->reinvert_dual_hard_trigger_burst = solver->policy.reinvert_dual.hard_trigger_burst;
    out->reinvert_phase1_control_demoted = solver->policy.reinvert_phase1.control_demoted;
    out->reinvert_phase1_control_demotions = solver->policy.reinvert_phase1.control_demotions;
    out->reinvert_phase1_pressure_last_iter = solver->policy.reinvert_phase1.pressure_last_iter;
    out->reinvert_phase1_pressure_burst = solver->policy.reinvert_phase1.pressure_burst;
    out->phase1_stagnation_escape_cooldown = solver->policy.phase1_stagnation.escape_cooldown;
    out->phase1_stagnation_escape_triggers = solver->policy.phase1_stagnation.escape_triggers;
    out->phase1_stagnation_escape_successes = solver->policy.phase1_stagnation.escape_successes;
    out->phase1_stagnation_escape_failures = solver->policy.phase1_stagnation.escape_failures;
    out->phase1_stagnation_escape_cooldown_blocks =
        solver->policy.phase1_stagnation.escape_cooldown_blocks;
    out->phase1_stagnation_last_window_iters = solver->policy.phase1_stagnation.last_window_iters;
    out->phase1_stagnation_last_obj_delta = solver->policy.phase1_stagnation.last_obj_delta;
    out->phase1_stagnation_last_retry_defer_ratio =
        solver->policy.phase1_stagnation.last_retry_defer_ratio;
    out->phase1_stagnation_last_update_recovery_ratio =
        solver->policy.phase1_stagnation.last_update_recovery_ratio;
    out->phase1_stagnation_last_retry_defers = solver->policy.phase1_stagnation.last_retry_defers;
    out->phase1_stagnation_last_no_pivot_events =
        solver->policy.phase1_stagnation.last_no_pivot_events;
    out->phase1_stagnation_last_update_recovery_refactors =
        solver->policy.phase1_stagnation.last_update_recovery_refactors;
    out->phase1_stagnation_last_refactors = solver->policy.phase1_stagnation.last_refactors;
    out->phase1_stagnation_last_recompute_ratio =
        solver->policy.phase1_stagnation.last_recompute_ratio;
    out->phase1_stagnation_last_recompute_dir_skip =
        solver->policy.phase1_stagnation.last_recompute_dir_skip;
    out->phase1_stagnation_last_recompute_dir_refactor =
        solver->policy.phase1_stagnation.last_recompute_dir_refactor;
    out->phase1_stagnation_last_recompute_pivot_fail =
        solver->policy.phase1_stagnation.last_recompute_pivot_fail;
    out->phase1_stagnation_last_recompute_perturb =
        solver->policy.phase1_stagnation.last_recompute_perturb;
    out->shadow_refactor_yes_phase1 = solver->policy.basis_governor.shadow_refactor_yes_phase1;
    out->shadow_refactor_yes_phase2 = solver->policy.basis_governor.shadow_refactor_yes_phase2;
    out->shadow_refactor_yes_dual = solver->policy.basis_governor.shadow_refactor_yes_dual;
    out->shadow_refactor_no_phase1 = solver->policy.basis_governor.shadow_refactor_no_phase1;
    out->shadow_refactor_no_phase2 = solver->policy.basis_governor.shadow_refactor_no_phase2;
    out->shadow_refactor_no_dual = solver->policy.basis_governor.shadow_refactor_no_dual;
    out->shadow_backend_pick_markowitz = solver->policy.basis_governor.shadow_backend_pick_markowitz;
    out->shadow_backend_pick_supernode = solver->policy.basis_governor.shadow_backend_pick_supernode;
    out->shadow_backend_pick_dense = solver->policy.basis_governor.shadow_backend_pick_dense;
    out->shadow_disagree_primal_refactor = solver->policy.basis_governor.shadow_disagree_primal_refactor;
    out->shadow_disagree_dual_refactor = solver->policy.basis_governor.shadow_disagree_dual_refactor;
    out->shadow_disagree_lu_backend = solver->policy.basis_governor.shadow_disagree_lu_backend;
    out->reinvert_shadow_checks_phase1 = solver->telemetry.perf_reinvert_shadow_checks_phase1;
    out->reinvert_shadow_checks_phase2 = solver->telemetry.perf_reinvert_shadow_checks_phase2;
    out->reinvert_shadow_checks_dual = solver->telemetry.perf_reinvert_shadow_checks_dual;
    out->reinvert_shadow_suggest_allow_phase1 = solver->telemetry.perf_reinvert_shadow_suggest_allow_phase1;
    out->reinvert_shadow_suggest_allow_phase2 = solver->telemetry.perf_reinvert_shadow_suggest_allow_phase2;
    out->reinvert_shadow_suggest_allow_dual = solver->telemetry.perf_reinvert_shadow_suggest_allow_dual;
    out->reinvert_shadow_suggest_defer_phase1 = solver->telemetry.perf_reinvert_shadow_suggest_defer_phase1;
    out->reinvert_shadow_suggest_defer_phase2 = solver->telemetry.perf_reinvert_shadow_suggest_defer_phase2;
    out->reinvert_shadow_suggest_defer_dual = solver->telemetry.perf_reinvert_shadow_suggest_defer_dual;
    out->reinvert_shadow_suggest_force_phase1 = solver->telemetry.perf_reinvert_shadow_suggest_force_phase1;
    out->reinvert_shadow_suggest_force_phase2 = solver->telemetry.perf_reinvert_shadow_suggest_force_phase2;
    out->reinvert_shadow_suggest_force_dual = solver->telemetry.perf_reinvert_shadow_suggest_force_dual;
    out->reinvert_shadow_actual_refactor_yes_phase1 = solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_phase1;
    out->reinvert_shadow_actual_refactor_yes_phase2 = solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_phase2;
    out->reinvert_shadow_actual_refactor_yes_dual = solver->telemetry.perf_reinvert_shadow_actual_refactor_yes_dual;
    out->reinvert_shadow_actual_refactor_no_phase1 = solver->telemetry.perf_reinvert_shadow_actual_refactor_no_phase1;
    out->reinvert_shadow_actual_refactor_no_phase2 = solver->telemetry.perf_reinvert_shadow_actual_refactor_no_phase2;
    out->reinvert_shadow_actual_refactor_no_dual = solver->telemetry.perf_reinvert_shadow_actual_refactor_no_dual;
    out->reinvert_shadow_disagree_phase1 = solver->telemetry.perf_reinvert_shadow_disagree_phase1;
    out->reinvert_shadow_disagree_phase2 = solver->telemetry.perf_reinvert_shadow_disagree_phase2;
    out->reinvert_shadow_disagree_dual = solver->telemetry.perf_reinvert_shadow_disagree_dual;
    out->reinvert_shadow_last_reason_phase1 = solver->telemetry.perf_reinvert_shadow_last_reason_phase1;
    out->reinvert_shadow_last_reason_phase2 = solver->telemetry.perf_reinvert_shadow_last_reason_phase2;
    out->reinvert_shadow_last_reason_dual = solver->telemetry.perf_reinvert_shadow_last_reason_dual;
}
#undef COPY_SOLVER_FIELD
