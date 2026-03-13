/*
 * Tests for solver-side LP telemetry helpers.
 *
 * Verifies:
 * 1) solver reset behavior
 * 2) refactor + basis event accounting
 * 3) refactor reason classifier
 * 4) solver snapshot export
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "lp_refactor_policy.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

#define ASSERT_ULL_EQ(a, b, msg) do { \
    tests_run++; \
    if ((unsigned long long)(a) == (unsigned long long)(b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%llu != %llu)\n", msg, \
               (unsigned long long)(a), (unsigned long long)(b)); \
    } \
} while (0)

#define ASSERT_DBL_EQ(a, b, msg) do { \
    tests_run++; \
    if (fabs((double)(a) - (double)(b)) <= 1e-12) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12f != %.12f)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

static void test_solver_reset_and_refactor_accounting(void) {
    printf("  telemetry/solver: reset + refactor accounting...\n");

    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));
    solver.telemetry_enabled = 1;

    solver.telemetry.perf_pricing_ms = 12.0;
    solver.telemetry.perf_refactor_count = 3;
    solver.telemetry.perf_ftran_calls = 9;
    solver.telemetry.perf_btran_calls = 7;
    solver.telemetry.perf_ftran_nnz_samples = 6;
    solver.telemetry.perf_btran_nnz_samples = 5;
    solver.telemetry.perf_ftran_rhs_nnz_total = 111;
    solver.telemetry.perf_ftran_sol_nnz_total = 222;
    solver.telemetry.perf_btran_rhs_nnz_total = 333;
    solver.telemetry.perf_btran_sol_nnz_total = 444;
    solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir = 5;
    solver.telemetry.perf_phase1_dir_stabilize_force_lu_health = 4;
    solver.telemetry.perf_phase1_dir_stabilize_cooldown_candidates = 8;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_3 = 2;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_10 = 2;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_30 = 2;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_100 = 1;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_100 = 1;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_300 = 1;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_1000 = 1;
    solver.telemetry.perf_phase1_dir_stabilize_skip_rc_only = 6;
    solver.telemetry.perf_phase1_dir_stabilize_skip_full = 2;
    solver.telemetry.perf_phase1_dir_stabilize_skip_no_recompute = 7;
    solver.telemetry.perf_phase1_dir_stabilize_skip_guard_refresh = 3;
    solver.telemetry.perf_phase1_force_extreme_tiny_theta_relax_applied = 4;
    solver.telemetry.perf_phase1_recompute_after_ratio_breakdown = 4;
    solver.telemetry.perf_phase1_recompute_after_dir_skip = 3;
    solver.telemetry.perf_phase1_recompute_after_dir_refactor = 2;
    solver.telemetry.perf_phase1_recompute_after_pivot_fail_recovery = 1;
    solver.telemetry.perf_phase1_recompute_after_perturb = 5;
    solver.telemetry.perf_phase1_recompute_rc_only_calls = 9;
    solver.telemetry.perf_phase1_compute_solution_ctx_refactor_success = 4;
    solver.telemetry.perf_phase1_compute_solution_ctx_other = 3;
    solver.telemetry.perf_phase1_compute_solution_ctx_dual_rescue = 8;
    solver.telemetry.perf_phase1_compute_rc_ctx_recompute_rc_only = 7;
    solver.telemetry.perf_phase1_compute_rc_ctx_init = 2;
    solver.telemetry.perf_phase1_compute_rc_ctx_dual_rescue = 6;
    solver.telemetry.perf_phase1_entering_exclusions = 9;
    solver.telemetry.perf_phase1_entering_exclusion_repeats = 4;
    solver.telemetry.perf_phase1_entering_exclusion_hits = 6;
    solver.telemetry.perf_phase1_entering_exclusion_reroutes = 5;
    solver.telemetry.perf_phase1_entering_exclusion_no_alt = 1;
    solver.telemetry.perf_phase1_recompute_rc_guard_forced_full = 4;
    solver.telemetry.perf_phase1_ratio_breakdown_retries = 6;
    solver.telemetry.perf_phase1_ratio_breakdown_escalations = 2;
    solver.telemetry.perf_phase1_pivot_fail_recovery_exclusions = 3;
    solver.telemetry.perf_phase1_no_pivot_events = 11;
    solver.telemetry.perf_phase1_no_pivot_forced_refactor = 4;
    solver.telemetry.perf_phase1_no_pivot_forced_ratio_breakdown = 2;
    solver.telemetry.perf_phase1_no_pivot_forced_dir_skip = 1;
    solver.telemetry.perf_phase1_no_pivot_forced_pivot_fail = 1;
    solver.telemetry.perf_phase1_direct_dual_rescue_attempts = 6;
    solver.telemetry.perf_phase1_direct_dual_rescue_successes = 2;
    solver.telemetry.perf_phase1_direct_dual_rescue_failures = 4;
    solver.telemetry.perf_phase1_direct_dual_rescue_guard_cooldown_blocks = 3;
    solver.telemetry.perf_phase1_direct_dual_rescue_guard_fail_cap_blocks = 1;
    solver.telemetry.perf_phase1_soft_lu_policy_cooldown_defers = 5;
    solver.telemetry.perf_phase1_dir_skip_same_entering_repeats = 7;
    solver.telemetry.perf_phase1_dir_skip_same_entering_max_streak = 3;
    solver.telemetry.perf_phase1_failed_stabilize_events = 8;
    solver.telemetry.perf_phase1_failed_stabilize_same_entering_repeats = 5;
    solver.telemetry.perf_phase1_failed_stabilize_same_entering_max_streak = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_arms = 6;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_found = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_no_alt = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_failed = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_arms = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_found = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_no_alt = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_failed = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_samples = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_total = 12;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_max = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_singleton_samples = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_best_differs_samples = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_samples = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total = 6.5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max = 4.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2 = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_samples = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_ratio_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_stable = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total = 25;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max = 11;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_total = 15.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max = 8.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total = 4.5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max = 2.5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_arms = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_arms = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total = 41;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max = 32;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_failed = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_dir_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed = 0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_shape_samples = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_total = 27;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max = 12;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total = 42000.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max = 18000.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total = 15.5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max = 8.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_arms = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_arms = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions = 2;
    solver.telemetry.perf_phase1_window_pressure_windows_started = 4;
    solver.telemetry.perf_phase1_window_pressure_progress_resets = 3;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_arms = 2;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_pending = 1;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_budget = 2;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_under_trigger = 3;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_failed_share = 4;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_dir_skip_share = 5;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_local_fail = 6;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_alternation = 7;
    solver.telemetry.perf_phase1_window_pressure_event_total = 17;
    solver.telemetry.perf_phase1_window_pressure_failed_stabilize_total = 9;
    solver.telemetry.perf_phase1_window_pressure_dir_skip_total = 8;
    solver.telemetry.perf_phase1_window_pressure_local_memory_fail_total = 6;
    solver.telemetry.perf_phase1_window_pressure_alternation_total = 11;
    solver.telemetry.perf_phase1_window_pressure_event_max = 7;
    solver.telemetry.perf_phase1_window_pressure_failed_stabilize_max = 4;
    solver.telemetry.perf_phase1_window_pressure_dir_skip_max = 3;
    solver.telemetry.perf_phase1_window_pressure_local_memory_fail_max = 2;
    solver.telemetry.perf_phase1_window_pressure_alternation_max = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_arms = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_pivot_success = 6;
    solver.telemetry.perf_phase1_force_extreme_followup_stabilized = 3;
    solver.telemetry.perf_phase1_force_extreme_followup_ratio_breakdown = 2;
    solver.telemetry.perf_phase1_force_extreme_followup_failed_stabilize = 5;
    solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_retry = 7;
    solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue = 4;
    solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor = 1;
    solver.telemetry.perf_phase1_force_extreme_followup_next_failed_stabilize = 6;
    solver.telemetry.perf_phase1_force_extreme_followup_next_ratio_breakdown = 3;
    solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_fail = 2;
    solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_success = 8;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_samples = 4;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_geometry = 1;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_flip = 1;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_tiny_theta = 0;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_weak_leaving = 2;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_ftran_shape = 1;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_total = 77;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_max = 31;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_total = 12345.0;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_max = 6789.0;
    solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_total = 9.75;
    solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_max = 6.5;
    solver.telemetry.perf_phase1_force_extreme_followup_theta_total = 0.015;
    solver.telemetry.perf_phase1_force_extreme_followup_theta_max = 0.01;
    solver.telemetry.perf_dual_bound_flip_applied = 9;
    solver.telemetry.perf_dual_bound_flip_startup = 4;
    solver.telemetry.perf_dual_bound_flip_iterative = 5;
    solver.telemetry.perf_reinvert_shadow_checks_phase1 = 7;
    solver.telemetry.perf_reinvert_shadow_disagree_phase2 = 3;
    solver.telemetry.perf_reinvert_shadow_last_reason_dual = LP_REINVERT_REASON_COST_DAMPEN;
    solver.policy.refactor_next_reason = RALPH_REFACTOR_REASON_SETUP;
    solver.telemetry.perf_basis_fastpath_hits = 7;
    solver.policy.periodic_feedback_phase2.bias = 0.2;
    solver.policy.soft_lu_cost_gate_enabled = 0;
    solver.policy.soft_lu_cost_gate_phase2.defers = 5;
    solver.policy.soft_lu_cost_gate_phase2.consecutive_defers = 3;
    solver.policy.soft_lu_cost_gate_phase2.defer_cap_forced = 4;
    solver.policy.soft_lu_cost_gate_phase2.refactor_cost_ewma = 9.5;
    solver.policy.periodic_cost_gate_enabled = 0;
    solver.policy.periodic_cost_gate_phase2.defers = 6;
    solver.policy.periodic_cost_gate_phase2.consecutive_defers = 2;
    solver.policy.periodic_cost_gate_phase2.defer_cap_forced = 3;
    solver.policy.periodic_cost_gate_phase2.checks = 9;
    solver.policy.periodic_cost_gate_phase2.block_ratio = 4;
    solver.policy.periodic_cost_gate_phase2.block_warmup = 2;
    solver.policy.periodic_cost_gate_phase2.last_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO;
    solver.policy.periodic_cost_gate_phase2.iter_samples = 12;
    solver.policy.periodic_cost_gate_phase2.refactor_samples = 3;
    solver.policy.basis_governor_mode = LP_BASIS_GOV_MODE_CONTROL_PHASE2;
    solver.policy.reinvert_controller_mode = LP_REINVERT_MODE_CONTROL_PHASE1;
    solver.policy.reinvert_dual.control_demoted = 1;
    solver.policy.reinvert_dual.control_demotions = 2;
    solver.policy.reinvert_dual.hard_trigger_last_total = 9;
    solver.policy.reinvert_dual.hard_trigger_last_iter = 77;
    solver.policy.reinvert_dual.hard_trigger_burst = 4;
    solver.policy.reinvert_phase1.control_demoted = 1;
    solver.policy.reinvert_phase1.control_demotions = 3;
    solver.policy.reinvert_phase1.pressure_last_iter = 41;
    solver.policy.reinvert_phase1.pressure_burst = 5;
    solver.policy.phase1_stagnation.window_start_iter = 19;
    solver.policy.phase1_stagnation.window_start_obj = 13.5;
    solver.policy.phase1_stagnation.window_retry_base = 7;
    solver.policy.phase1_stagnation.window_no_pivot_base = 9;
    solver.policy.phase1_stagnation.window_refactor_base = 5;
    solver.policy.phase1_stagnation.window_update_recovery_base = 3;
    solver.policy.phase1_stagnation.window_recompute_ratio_base = 2;
    solver.policy.phase1_stagnation.window_recompute_dir_skip_base = 4;
    solver.policy.phase1_stagnation.window_recompute_dir_refactor_base = 1;
    solver.policy.phase1_stagnation.window_recompute_pivot_fail_base = 6;
    solver.policy.phase1_stagnation.window_recompute_perturb_base = 8;
    solver.policy.phase1_stagnation.escape_cooldown = 11;
    solver.policy.phase1_stagnation.escape_triggers = 2;
    solver.policy.phase1_stagnation.escape_successes = 1;
    solver.policy.phase1_stagnation.escape_failures = 1;
    solver.policy.phase1_stagnation.escape_cooldown_blocks = 3;
    solver.policy.phase1_stagnation.last_window_iters = 96;
    solver.policy.phase1_stagnation.last_obj_delta = 0.125;
    solver.policy.phase1_stagnation.last_retry_defer_ratio = 0.75;
    solver.policy.phase1_stagnation.last_update_recovery_ratio = 0.5;
    solver.policy.phase1_stagnation.last_retry_defers = 12;
    solver.policy.phase1_stagnation.last_no_pivot_events = 16;
    solver.policy.phase1_stagnation.last_update_recovery_refactors = 4;
    solver.policy.phase1_stagnation.last_refactors = 8;
    solver.policy.phase1_stagnation.last_recompute_ratio = 9;
    solver.policy.phase1_stagnation.last_recompute_dir_skip = 7;
    solver.policy.phase1_stagnation.last_recompute_dir_refactor = 3;
    solver.policy.phase1_stagnation.last_recompute_pivot_fail = 2;
    solver.policy.phase1_stagnation.last_recompute_perturb = 1;
    lp_basis_governor_set_mode(&solver.policy.basis_governor,
                               solver.policy.basis_governor_mode);
    solver.policy.basis_governor.shadow_refactor_yes_phase1 = 4;
    solver.policy.basis_governor.shadow_disagree_lu_backend = 2;

    lp_telemetry_reset_solver(&solver);

    ASSERT_DBL_EQ(solver.telemetry.perf_pricing_ms, 0.0, "reset: perf_pricing_ms");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_count, 0, "reset: refactor_count");
    ASSERT_INT_EQ(solver.telemetry.perf_ftran_calls, 0, "reset: ftran calls");
    ASSERT_INT_EQ(solver.telemetry.perf_btran_calls, 0, "reset: btran calls");
    ASSERT_DBL_EQ(solver.telemetry.perf_ftran_base_ms, 0.0, "reset: ftran base ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_ftran_update_apply_ms, 0.0,
                  "reset: ftran update apply ms");
    ASSERT_INT_EQ(solver.telemetry.perf_ftran_update_apply_calls, 0,
                  "reset: ftran update apply calls");
    ASSERT_DBL_EQ(solver.telemetry.perf_btran_base_ms, 0.0, "reset: btran base ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_btran_update_apply_ms, 0.0,
                  "reset: btran update apply ms");
    ASSERT_INT_EQ(solver.telemetry.perf_btran_update_apply_calls, 0,
                  "reset: btran update apply calls");
    ASSERT_INT_EQ(solver.telemetry.perf_ftran_nnz_samples, 0, "reset: ftran nnz samples");
    ASSERT_INT_EQ(solver.telemetry.perf_btran_nnz_samples, 0, "reset: btran nnz samples");
    ASSERT_ULL_EQ((unsigned long long)solver.telemetry.perf_ftran_rhs_nnz_total, 0ULL,
                  "reset: ftran rhs nnz total");
    ASSERT_ULL_EQ((unsigned long long)solver.telemetry.perf_ftran_sol_nnz_total, 0ULL,
                  "reset: ftran sol nnz total");
    ASSERT_ULL_EQ((unsigned long long)solver.telemetry.perf_btran_rhs_nnz_total, 0ULL,
                  "reset: btran rhs nnz total");
    ASSERT_ULL_EQ((unsigned long long)solver.telemetry.perf_btran_sol_nnz_total, 0ULL,
                  "reset: btran sol nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir, 0,
                  "reset: phase1 dir force extreme");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_lu_health, 0,
                  "reset: phase1 dir force lu health");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_cooldown_candidates, 0,
                  "reset: phase1 dir cooldown candidates");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_3, 0,
                  "reset: phase1 dir ratio <=3");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_10, 0,
                  "reset: phase1 dir ratio <=10");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_30, 0,
                  "reset: phase1 dir ratio <=30");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_100, 0,
                  "reset: phase1 dir ratio <=100");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_100, 0,
                  "reset: phase1 dir ratio >100");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_300, 0,
                  "reset: phase1 dir ratio >300");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_1000, 0,
                  "reset: phase1 dir ratio >1000");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_rc_only, 0,
                  "reset: phase1 dir skip rc-only");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_full, 0,
                  "reset: phase1 dir skip full");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_no_recompute, 0,
                  "reset: phase1 dir skip no recompute");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_guard_refresh, 0,
                  "reset: phase1 dir skip guard refresh");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_tiny_theta_relax_applied, 0,
                  "reset: phase1 force extreme tiny-theta relax");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_ratio_breakdown, 0,
                  "reset: phase1 recompute ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_dir_skip, 0,
                  "reset: phase1 recompute dir skip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_dir_refactor, 0,
                  "reset: phase1 recompute dir refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_pivot_fail_recovery, 0,
                  "reset: phase1 recompute pivot fail recovery");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_perturb, 0,
                  "reset: phase1 recompute perturb");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_rc_only_calls, 0,
                  "reset: phase1 recompute rc-only calls");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_compute_solution_ctx_refactor_success, 0,
                  "reset: phase1 compute_solution refactor success ctx");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_compute_solution_ctx_other, 0,
                  "reset: phase1 compute_solution other ctx");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_compute_solution_ctx_dual_rescue, 0,
                  "reset: phase1 compute_solution dual rescue ctx");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_compute_rc_ctx_recompute_rc_only, 0,
                  "reset: phase1 compute_rc rc-only ctx");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_compute_rc_ctx_init, 0,
                  "reset: phase1 compute_rc init ctx");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_compute_rc_ctx_dual_rescue, 0,
                  "reset: phase1 compute_rc dual rescue ctx");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_entering_exclusions, 0,
                  "reset: phase1 entering exclusions");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_entering_exclusion_repeats, 0,
                  "reset: phase1 entering exclusion repeats");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_entering_exclusion_hits, 0,
                  "reset: phase1 entering exclusion hits");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_entering_exclusion_reroutes, 0,
                  "reset: phase1 entering exclusion reroutes");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_entering_exclusion_no_alt, 0,
                  "reset: phase1 entering exclusion no alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_rc_guard_forced_full, 0,
                  "reset: phase1 recompute rc-only guard forced");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_ratio_breakdown_retries, 0,
                  "reset: phase1 ratio breakdown retries");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_ratio_breakdown_escalations, 0,
                  "reset: phase1 ratio breakdown escalations");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_pivot_fail_recovery_exclusions, 0,
                  "reset: phase1 pivot fail recovery exclusions");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_events, 0,
                  "reset: phase1 no-pivot events");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_refactor, 0,
                  "reset: phase1 no-pivot forced refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_ratio_breakdown, 0,
                  "reset: phase1 no-pivot forced ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_dir_skip, 0,
                  "reset: phase1 no-pivot forced dir skip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_pivot_fail, 0,
                  "reset: phase1 no-pivot forced pivot fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_direct_dual_rescue_attempts, 0,
                  "reset: phase1 direct dual rescue attempts");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_direct_dual_rescue_successes, 0,
                  "reset: phase1 direct dual rescue successes");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_direct_dual_rescue_failures, 0,
                  "reset: phase1 direct dual rescue failures");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_direct_dual_rescue_guard_cooldown_blocks, 0,
                  "reset: phase1 direct dual rescue guard cooldown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_direct_dual_rescue_guard_fail_cap_blocks, 0,
                  "reset: phase1 direct dual rescue guard fail cap");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_soft_lu_policy_cooldown_defers, 0,
                  "reset: phase1 soft-lu periodic cooldown defers");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_skip_same_entering_repeats, 0,
                  "reset: phase1 dir-skip same-entering repeats");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_skip_same_entering_max_streak, 0,
                  "reset: phase1 dir-skip same-entering max streak");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_events, 0,
                  "reset: phase1 failed stabilize events");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_primary_failures, 0,
                  "reset: phase1 failed stabilize primary failures");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_alternate_failures, 0,
                  "reset: phase1 failed stabilize alternate failures");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_same_entering_repeats, 0,
                  "reset: phase1 failed stabilize same-entering repeats");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_same_entering_max_streak, 0,
                  "reset: phase1 failed stabilize same-entering max streak");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_arms, 0,
                  "reset: phase1 failed stabilize retry arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_found, 0,
                  "reset: phase1 failed stabilize retry alternate found");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_no_alt, 0,
                  "reset: phase1 failed stabilize retry no-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized, 0,
                  "reset: phase1 failed stabilize retry alternate stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_failed, 0,
                  "reset: phase1 failed stabilize retry alternate failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats, 0,
                  "reset: phase1 failed stabilize retry same-alt repeats");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak, 0,
                  "reset: phase1 failed stabilize retry same-alt max streak");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_arms, 0,
                  "reset: phase1 failed stabilize retry local-memory arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_found, 0,
                  "reset: phase1 failed stabilize retry local-memory alt found");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_no_alt, 0,
                  "reset: phase1 failed stabilize retry local-memory no-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt, 0,
                  "reset: phase1 failed stabilize retry local-memory fallback same-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized, 0,
                  "reset: phase1 failed stabilize retry local-memory alt stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_failed, 0,
                  "reset: phase1 failed stabilize retry local-memory alt failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_samples, 0,
                  "reset: phase1 failed stabilize retry pool samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_total, 0,
                  "reset: phase1 failed stabilize retry pool eligible total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_max, 0,
                  "reset: phase1 failed stabilize retry pool eligible max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_singleton_samples, 0,
                  "reset: phase1 failed stabilize retry pool singleton samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_best_differs_samples, 0,
                  "reset: phase1 failed stabilize retry pool best differs");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_samples, 0,
                  "reset: phase1 failed stabilize retry selector eval samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples, 0,
                  "reset: phase1 failed stabilize retry selector eval best differs");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total, 0.0,
                  "reset: phase1 failed stabilize retry selector eval score ratio total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max, 0.0,
                  "reset: phase1 failed stabilize retry selector eval score ratio max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2, 0,
                  "reset: phase1 failed stabilize retry selector eval score ratio >=2");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4, 0,
                  "reset: phase1 failed stabilize retry selector eval score ratio >=4");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_samples, 0,
                  "reset: phase1 failed stabilize retry shadow samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_ratio_failed, 0,
                  "reset: phase1 failed stabilize retry shadow ratio failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_stable, 0,
                  "reset: phase1 failed stabilize retry shadow dir stable");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_failed, 0,
                  "reset: phase1 failed stabilize retry shadow dir failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total, 0,
                  "reset: phase1 failed stabilize retry shadow dir nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max, 0,
                  "reset: phase1 failed stabilize retry shadow dir nnz max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_total, 0.0,
                  "reset: phase1 failed stabilize retry shadow dir inf total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max, 0.0,
                  "reset: phase1 failed stabilize retry shadow dir inf max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total, 0.0,
                  "reset: phase1 failed stabilize retry shadow pivot abs total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max, 0.0,
                  "reset: phase1 failed stabilize retry shadow pivot abs max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_arms, 0,
                  "reset: phase1 failed stabilize retry selector bland arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_arms, 0,
                  "reset: phase1 failed stabilize retry selector guarded arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total, 0,
                  "reset: phase1 failed stabilize retry selector guarded eligible total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max, 0,
                  "reset: phase1 failed stabilize retry selector guarded eligible max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized, 0,
                  "reset: phase1 failed stabilize retry selector bland stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_failed, 0,
                  "reset: phase1 failed stabilize retry selector bland failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed, 0,
                  "reset: phase1 failed stabilize retry selector bland ratio failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_dir_failed, 0,
                  "reset: phase1 failed stabilize retry selector bland dir failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized, 0,
                  "reset: phase1 failed stabilize retry selector guarded stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed, 0,
                  "reset: phase1 failed stabilize retry selector guarded failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed, 0,
                  "reset: phase1 failed stabilize retry selector guarded ratio failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed, 0,
                  "reset: phase1 failed stabilize retry selector guarded dir failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland, 0,
                  "reset: phase1 failed stabilize retry selector guarded fallback");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_shape_samples, 0,
                  "reset: phase1 failed stabilize retry dir-fail shape samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_total, 0,
                  "reset: phase1 failed stabilize retry dir-fail nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max, 0,
                  "reset: phase1 failed stabilize retry dir-fail nnz max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total, 0.0,
                  "reset: phase1 failed stabilize retry dir-fail dir-inf total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max, 0.0,
                  "reset: phase1 failed stabilize retry dir-fail dir-inf max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total, 0.0,
                  "reset: phase1 failed stabilize retry dir-fail pivot abs total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max, 0.0,
                  "reset: phase1 failed stabilize retry dir-fail pivot abs max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30, 0,
                  "reset: phase1 failed stabilize retry dir-fail inf ratio <=30");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100, 0,
                  "reset: phase1 failed stabilize retry dir-fail inf ratio <=100");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000, 0,
                  "reset: phase1 failed stabilize retry dir-fail inf ratio <=1000");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000, 0,
                  "reset: phase1 failed stabilize retry dir-fail inf ratio >1000");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8, 0,
                  "reset: phase1 failed stabilize retry dir-fail pivot ratio <=1e-8");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6, 0,
                  "reset: phase1 failed stabilize retry dir-fail pivot ratio <=1e-6");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4, 0,
                  "reset: phase1 failed stabilize retry dir-fail pivot ratio <=1e-4");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4, 0,
                  "reset: phase1 failed stabilize retry dir-fail pivot ratio >1e-4");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_arms, 0,
                  "reset: phase1 failed stabilize retry dir second-chance arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt, 0,
                  "reset: phase1 failed stabilize retry dir second-chance no-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized, 0,
                  "reset: phase1 failed stabilize retry dir second-chance stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_failed, 0,
                  "reset: phase1 failed stabilize retry dir second-chance failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_arms, 0,
                  "reset: phase1 failed stabilize retry dir guard arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions, 0,
                  "reset: phase1 failed stabilize retry dir guard original exclusions");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_windows_started, 0,
                  "reset: phase1 window pressure windows started");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_progress_resets, 0,
                  "reset: phase1 window pressure progress resets");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_arms, 0,
                  "reset: phase1 window pressure force-pivot arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_pending, 0,
                  "reset: phase1 window pressure force-pivot blocked pending");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_budget, 0,
                  "reset: phase1 window pressure force-pivot blocked budget");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_under_trigger, 0,
                  "reset: phase1 window pressure force-pivot reject under trigger");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_failed_share, 0,
                  "reset: phase1 window pressure force-pivot reject failed share");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_dir_skip_share, 0,
                  "reset: phase1 window pressure force-pivot reject dir-skip share");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_local_fail, 0,
                  "reset: phase1 window pressure force-pivot reject local fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_alternation, 0,
                  "reset: phase1 window pressure force-pivot reject alternation");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_event_total, 0,
                  "reset: phase1 window pressure event total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_failed_stabilize_total, 0,
                  "reset: phase1 window pressure failed-stabilize total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_dir_skip_total, 0,
                  "reset: phase1 window pressure dir-skip total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_local_memory_fail_total, 0,
                  "reset: phase1 window pressure local-memory-fail total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_alternation_total, 0,
                  "reset: phase1 window pressure alternation total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_event_max, 0,
                  "reset: phase1 window pressure event max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_failed_stabilize_max, 0,
                  "reset: phase1 window pressure failed-stabilize max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_dir_skip_max, 0,
                  "reset: phase1 window pressure dir-skip max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_local_memory_fail_max, 0,
                  "reset: phase1 window pressure local-memory-fail max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_alternation_max, 0,
                  "reset: phase1 window pressure alternation max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_arms, 0,
                  "reset: phase1 failed stabilize retry shadow guard arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions, 0,
                  "reset: phase1 failed stabilize retry shadow guard original exclusions");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_stabilized, 0,
                  "reset: phase1 force-extreme followup stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_ratio_breakdown, 0,
                  "reset: phase1 force-extreme followup ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_failed_stabilize, 0,
                  "reset: phase1 force-extreme followup failed stabilize");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_retry, 0,
                  "reset: phase1 force-extreme post dir-skip retry");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue, 0,
                  "reset: phase1 force-extreme post dir-skip dual rescue");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor, 0,
                  "reset: phase1 force-extreme post dir-skip forced refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_failed_stabilize, 0,
                  "reset: phase1 force-extreme next failed-stabilize");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_ratio_breakdown, 0,
                  "reset: phase1 force-extreme next ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_fail, 0,
                  "reset: phase1 force-extreme next pivot fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_success, 0,
                  "reset: phase1 force-extreme next pivot success");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_samples, 0,
                  "reset: phase1 force-extreme followup dir samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_geometry, 0,
                  "reset: phase1 force-extreme followup dir bound geometry");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_flip, 0,
                  "reset: phase1 force-extreme followup dir bound flip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_tiny_theta, 0,
                  "reset: phase1 force-extreme followup dir tiny theta");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_weak_leaving, 0,
                  "reset: phase1 force-extreme followup dir weak leaving");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_ftran_shape, 0,
                  "reset: phase1 force-extreme followup dir ftran shape");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_total, 0,
                  "reset: phase1 force-extreme followup dir nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_max, 0,
                  "reset: phase1 force-extreme followup dir nnz max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_total, 0.0,
                  "reset: phase1 force-extreme followup dir inf total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_max, 0.0,
                  "reset: phase1 force-extreme followup dir inf max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_total, 0.0,
                  "reset: phase1 force-extreme followup pivot abs total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_max, 0.0,
                  "reset: phase1 force-extreme followup pivot abs max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_theta_total, 0.0,
                  "reset: phase1 force-extreme followup theta total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_theta_max, 0.0,
                  "reset: phase1 force-extreme followup theta max");
    ASSERT_INT_EQ(solver.telemetry.perf_dual_bound_flip_applied, 0,
                  "reset: dual bound-flip aggregate");
    ASSERT_INT_EQ(solver.telemetry.perf_dual_bound_flip_startup, 0,
                  "reset: dual bound-flip startup");
    ASSERT_INT_EQ(solver.telemetry.perf_dual_bound_flip_iterative, 0,
                  "reset: dual bound-flip iterative");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_checks_phase1, 0,
                  "reset: reinvert shadow checks phase1");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_disagree_phase2, 0,
                  "reset: reinvert shadow disagree phase2");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_last_reason_dual,
                  LP_REINVERT_REASON_NONE,
                  "reset: reinvert shadow last reason dual");
    ASSERT_INT_EQ(solver.policy.refactor_next_reason, RALPH_REFACTOR_REASON_OTHER,
                  "reset: next reason");
    ASSERT_INT_EQ(solver.telemetry.perf_basis_fastpath_hits, 0, "reset: basis_fastpath_hits");
    ASSERT_DBL_EQ(solver.policy.periodic_feedback_phase2.bias, 0.0,
                  "reset: periodic feedback phase2");
    ASSERT_INT_EQ(solver.policy.soft_lu_cost_gate_enabled, 1,
                  "reset: soft lu cost gate enabled");
    ASSERT_INT_EQ(solver.policy.soft_lu_cost_gate_phase2.defers, 0,
                  "reset: soft lu defers phase2");
    ASSERT_INT_EQ(solver.policy.soft_lu_cost_gate_phase2.consecutive_defers, 0,
                  "reset: soft lu consecutive defers phase2");
    ASSERT_INT_EQ(solver.policy.soft_lu_cost_gate_phase2.defer_cap_forced, 0,
                  "reset: soft lu cap forced phase2");
    ASSERT_DBL_EQ(solver.policy.soft_lu_cost_gate_phase2.refactor_cost_ewma, 0.0,
                  "reset: soft lu refactor ewma phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_enabled, 1,
                  "reset: periodic cost gate enabled");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.defers, 0,
                  "reset: periodic cost gate defers phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.consecutive_defers, 0,
                  "reset: periodic cost consecutive defers phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.defer_cap_forced, 0,
                  "reset: periodic cost cap forced phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.checks, 0,
                  "reset: periodic cost checks phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.block_ratio, 0,
                  "reset: periodic cost ratio block phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.block_warmup, 0,
                  "reset: periodic cost warmup block phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.last_reason,
                  LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE,
                  "reset: periodic cost last reason phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.iter_samples, 0,
                  "reset: periodic cost iter samples phase2");
    ASSERT_INT_EQ(solver.policy.periodic_cost_gate_phase2.refactor_samples, 0,
                  "reset: periodic cost refactor samples phase2");
    ASSERT_INT_EQ(solver.policy.basis_governor_mode, LP_BASIS_GOV_MODE_CONTROL_PHASE2,
                  "reset: basis governor mode preserved");
    ASSERT_INT_EQ(solver.policy.reinvert_controller_mode, LP_REINVERT_MODE_CONTROL_PHASE1,
                  "reset: reinvert controller mode preserved");
    ASSERT_INT_EQ(solver.policy.reinvert_dual.control_demoted, 0,
                  "reset: dual reinvert control demoted");
    ASSERT_INT_EQ(solver.policy.reinvert_dual.control_demotions, 0,
                  "reset: dual reinvert control demotions");
    ASSERT_INT_EQ(solver.policy.reinvert_dual.hard_trigger_last_total, 0,
                  "reset: dual reinvert hard-trigger last total");
    ASSERT_INT_EQ(solver.policy.reinvert_dual.hard_trigger_last_iter, -1,
                  "reset: dual reinvert hard-trigger last iter");
    ASSERT_INT_EQ(solver.policy.reinvert_dual.hard_trigger_burst, 0,
                  "reset: dual reinvert hard-trigger burst");
    ASSERT_INT_EQ(solver.policy.reinvert_phase1.control_demoted, 0,
                  "reset: phase1 reinvert control demoted");
    ASSERT_INT_EQ(solver.policy.reinvert_phase1.control_demotions, 0,
                  "reset: phase1 reinvert control demotions");
    ASSERT_INT_EQ(solver.policy.reinvert_phase1.pressure_last_iter, -1,
                  "reset: phase1 reinvert pressure last iter");
    ASSERT_INT_EQ(solver.policy.reinvert_phase1.pressure_burst, 0,
                  "reset: phase1 reinvert pressure burst");
    ASSERT_INT_EQ(solver.policy.phase1_stagnation.window_start_iter, -1,
                  "reset: phase1 stagnation window start iter");
    ASSERT_DBL_EQ(solver.policy.phase1_stagnation.window_start_obj, 0.0,
                  "reset: phase1 stagnation window start obj");
    ASSERT_INT_EQ(solver.policy.phase1_stagnation.escape_cooldown, 0,
                  "reset: phase1 stagnation cooldown");
    ASSERT_INT_EQ(solver.policy.phase1_stagnation.escape_triggers, 0,
                  "reset: phase1 stagnation escape triggers");
    ASSERT_INT_EQ(solver.policy.phase1_stagnation.escape_successes, 0,
                  "reset: phase1 stagnation escape successes");
    ASSERT_INT_EQ(solver.policy.phase1_stagnation.escape_failures, 0,
                  "reset: phase1 stagnation escape failures");
    ASSERT_INT_EQ(solver.policy.phase1_stagnation.escape_cooldown_blocks, 0,
                  "reset: phase1 stagnation cooldown blocks");
    ASSERT_INT_EQ(solver.policy.phase1_stagnation.last_window_iters, 0,
                  "reset: phase1 stagnation last window iters");
    ASSERT_DBL_EQ(solver.policy.phase1_stagnation.last_obj_delta, 0.0,
                  "reset: phase1 stagnation last obj delta");
    ASSERT_DBL_EQ(solver.policy.phase1_stagnation.last_retry_defer_ratio, 0.0,
                  "reset: phase1 stagnation retry ratio");
    ASSERT_DBL_EQ(solver.policy.phase1_stagnation.last_update_recovery_ratio, 0.0,
                  "reset: phase1 stagnation update ratio");
    ASSERT_INT_EQ(lp_basis_governor_get_mode(&solver.policy.basis_governor),
                  LP_BASIS_GOV_MODE_CONTROL_PHASE2,
                  "reset: governor state mode preserved");
    ASSERT_INT_EQ(solver.policy.basis_governor.shadow_refactor_yes_phase1, 0,
                  "reset: basis governor yes phase1");
    ASSERT_INT_EQ(solver.policy.basis_governor.shadow_disagree_lu_backend, 0,
                  "reset: basis governor lu disagreement");

    lp_telemetry_add_ftran_ms(&solver, 1.25);
    lp_telemetry_add_btran_ms(&solver, 0.75);
    lp_telemetry_add_ftran_base_ms(&solver, 0.50);
    lp_telemetry_add_ftran_update_apply_ms(&solver, 0.20);
    lp_telemetry_add_btran_base_ms(&solver, 0.30);
    lp_telemetry_add_btran_update_apply_ms(&solver, 0.15);
    lp_telemetry_record_ftran_nnz(&solver, 3, 17);
    lp_telemetry_record_btran_nnz(&solver, 1, 9);
    lp_telemetry_record_ftran_nnz(&solver, -1, 4);  /* ignored invalid sample */
    ASSERT_INT_EQ(solver.telemetry.perf_ftran_calls, 1, "ftran: call count");
    ASSERT_INT_EQ(solver.telemetry.perf_btran_calls, 1, "btran: call count");
    ASSERT_DBL_EQ(solver.telemetry.perf_ftran_base_ms, 0.50, "ftran: base ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_ftran_update_apply_ms, 0.20,
                  "ftran: update apply ms");
    ASSERT_INT_EQ(solver.telemetry.perf_ftran_update_apply_calls, 1,
                  "ftran: update apply calls");
    ASSERT_DBL_EQ(solver.telemetry.perf_btran_base_ms, 0.30, "btran: base ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_btran_update_apply_ms, 0.15,
                  "btran: update apply ms");
    ASSERT_INT_EQ(solver.telemetry.perf_btran_update_apply_calls, 1,
                  "btran: update apply calls");
    ASSERT_INT_EQ(solver.telemetry.perf_ftran_nnz_samples, 1, "ftran: nnz sample count");
    ASSERT_INT_EQ(solver.telemetry.perf_btran_nnz_samples, 1, "btran: nnz sample count");
    ASSERT_INT_EQ((int)solver.telemetry.perf_ftran_rhs_nnz_total, 3, "ftran: rhs nnz total");
    ASSERT_INT_EQ((int)solver.telemetry.perf_ftran_sol_nnz_total, 17, "ftran: sol nnz total");
    ASSERT_INT_EQ((int)solver.telemetry.perf_btran_rhs_nnz_total, 1, "btran: rhs nnz total");
    ASSERT_INT_EQ((int)solver.telemetry.perf_btran_sol_nnz_total, 9, "btran: sol nnz total");

    lp_telemetry_record_basis_build(&solver, 1, 2, 128ULL);
    ASSERT_INT_EQ(solver.telemetry.perf_basis_fastpath_hits, 1, "basis: fastpath hit");
    ASSERT_INT_EQ(solver.telemetry.perf_basis_cols_rewritten, 2, "basis: cols rewritten");
    ASSERT_ULL_EQ(solver.telemetry.perf_basis_tail_shift_bytes, 128ULL, "basis: tail shift bytes");

    lp_telemetry_set_refactor_next_reason(&solver, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
    {
        int reason = -1;
        lp_telemetry_begin_refactor(&solver, &reason);
        ASSERT_INT_EQ(reason, RALPH_REFACTOR_REASON_RATIO_RECOVERY,
                      "begin_refactor returns staged reason");
        ASSERT_INT_EQ(solver.policy.refactor_next_reason, RALPH_REFACTOR_REASON_OTHER,
                      "begin_refactor clears staged reason");

        lp_telemetry_record_refactor(&solver, 2, reason, 5.5, 100, 80, 1234);
    }

    ASSERT_INT_EQ(solver.telemetry.perf_refactor_count, 1, "record: refactor count");
    ASSERT_DBL_EQ(solver.telemetry.perf_refactor_all_ms, 5.5, "record: refactor all ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_refactor_last_ms, 5.5, "record: refactor last ms");
    ASSERT_DBL_EQ(solver.telemetry.perf_refactor_max_ms, 5.5, "record: refactor max ms");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_reason_ratio_recovery, 1,
                  "record: ratio recovery reason");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_safety_forced, 1, "record: safety forced global");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_refactor_calls, 1, "record: phase2 refactor calls");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase2_refactor_ms, 5.5, "record: phase2 refactor ms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_refactor_safety_forced, 1,
                  "record: phase2 safety forced");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_last_m, 100, "record: last m");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_last_k, 80, "record: last k");
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_last_nnz_B, 1234, "record: last nnz_B");

    lp_telemetry_record_refactor(&solver, 1, RALPH_REFACTOR_REASON_SETUP, 2.0, 40, 20, 300);
    ASSERT_INT_EQ(solver.telemetry.perf_refactor_reason_setup, 1, "record: setup reason");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_refactor_calls, 1, "record: phase1 refactor calls");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_refactor_ms, 2.0, "record: phase1 refactor ms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_refactor_safety_forced, 0,
                  "record: phase1 safety remains 0 for setup");

    lp_telemetry_record_phase1_dir_stabilize_force(&solver, 1, 0);
    lp_telemetry_record_phase1_dir_stabilize_force(&solver, 0, 1);
    lp_telemetry_record_phase1_dir_stabilize_force(&solver, 1, 1);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir, 2,
                  "record: phase1 dir force extreme count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_force_lu_health, 2,
                  "record: phase1 dir force lu health count");

    lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(&solver, 2.0);
    lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(&solver, 8.0);
    lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(&solver, 20.0);
    lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(&solver, 70.0);
    lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(&solver, 140.0);
    lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(&solver, 350.0);
    lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(&solver, 1400.0);
    lp_telemetry_record_phase1_dir_stabilize_skip(&solver, 0);
    lp_telemetry_record_phase1_dir_stabilize_skip(&solver, 0);
    lp_telemetry_record_phase1_dir_stabilize_skip(&solver, 1);
    lp_telemetry_record_phase1_dir_stabilize_skip_no_recompute(&solver);
    lp_telemetry_record_phase1_dir_stabilize_skip_no_recompute(&solver);
    lp_telemetry_record_phase1_dir_stabilize_skip_guard_refresh(&solver);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_cooldown_candidates, 7,
                  "record: phase1 dir cooldown candidate count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_3, 1,
                  "record: phase1 dir ratio <=3 count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_10, 1,
                  "record: phase1 dir ratio <=10 count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_30, 1,
                  "record: phase1 dir ratio <=30 count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_le_100, 1,
                  "record: phase1 dir ratio <=100 count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_100, 3,
                  "record: phase1 dir ratio >100 count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_300, 2,
                  "record: phase1 dir ratio >300 count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_1000, 1,
                  "record: phase1 dir ratio >1000 count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_rc_only, 2,
                  "record: phase1 dir skip rc-only count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_full, 1,
                  "record: phase1 dir skip full count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_no_recompute, 2,
                  "record: phase1 dir skip no recompute count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_dir_stabilize_skip_guard_refresh, 1,
                  "record: phase1 dir skip guard refresh count");
    lp_telemetry_record_phase1_failed_stabilize_site(&solver, 0);
    lp_telemetry_record_phase1_failed_stabilize_site(&solver, 1);
    lp_telemetry_record_phase1_failed_stabilize_site(&solver, 1);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_primary_failures, 1,
                  "record: phase1 failed stabilize primary failures");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_alternate_failures, 2,
                  "record: phase1 failed stabilize alternate failures");
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_arm(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_arm(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_alternate(&solver, 0, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_alternate(&solver, 1, 3);
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_no_alt(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(&solver, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(&solver, 0);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_arms, 2,
                  "record: phase1 failed-stabilize retry arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_found, 2,
                  "record: phase1 failed-stabilize retry alternate found");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_no_alt, 1,
                  "record: phase1 failed-stabilize retry no-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized, 1,
                  "record: phase1 failed-stabilize retry alternate stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_failed, 1,
                  "record: phase1 failed-stabilize retry alternate failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats, 1,
                  "record: phase1 failed-stabilize retry same alternate repeats");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak, 3,
                  "record: phase1 failed-stabilize retry same alternate max streak");
    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_arm(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_arm(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_alternate(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_no_alt(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_fallback_same_alt(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(&solver, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(&solver, 0);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_arms, 2,
                  "record: phase1 failed-stabilize retry local-memory arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_found, 1,
                  "record: phase1 failed-stabilize retry local-memory alt found");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_no_alt, 1,
                  "record: phase1 failed-stabilize retry local-memory no-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt, 1,
                  "record: phase1 failed-stabilize retry local-memory fallback same-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized, 1,
                  "record: phase1 failed-stabilize retry local-memory alt stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_failed, 1,
                  "record: phase1 failed-stabilize retry local-memory alt failed");
    lp_telemetry_record_phase1_failed_stabilize_retry_pool_sample(&solver, 1, 0);
    lp_telemetry_record_phase1_failed_stabilize_retry_pool_sample(&solver, 4, 1);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_samples, 2,
                  "record: phase1 failed-stabilize retry pool samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_total, 5,
                  "record: phase1 failed-stabilize retry pool eligible total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_max, 4,
                  "record: phase1 failed-stabilize retry pool eligible max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_singleton_samples, 1,
                  "record: phase1 failed-stabilize retry pool singleton samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_pool_best_differs_samples, 1,
                  "record: phase1 failed-stabilize retry pool best differs");
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_eval(&solver, 0, 3.0, 3.0);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_eval(&solver, 1, 2.0, 8.0);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_samples, 2,
                  "record: phase1 failed-stabilize retry selector eval samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples, 1,
                  "record: phase1 failed-stabilize retry selector eval best differs");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total, 5.0,
                  "record: phase1 failed-stabilize retry selector eval score ratio total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max, 4.0,
                  "record: phase1 failed-stabilize retry selector eval score ratio max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2, 1,
                  "record: phase1 failed-stabilize retry selector eval score ratio >=2");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4, 1,
                  "record: phase1 failed-stabilize retry selector eval score ratio >=4");
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow(&solver, 0, 0, 0.0, 0, 0.0);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow(&solver, 1, 1, 3.0, 7, 0.5);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow(&solver, 1, 0, 9.0, 11, 0.25);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_samples, 3,
                  "record: phase1 failed-stabilize retry shadow samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_ratio_failed, 1,
                  "record: phase1 failed-stabilize retry shadow ratio failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_stable, 1,
                  "record: phase1 failed-stabilize retry shadow dir stable");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_failed, 1,
                  "record: phase1 failed-stabilize retry shadow dir failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total, 18,
                  "record: phase1 failed-stabilize retry shadow dir nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max, 11,
                  "record: phase1 failed-stabilize retry shadow dir nnz max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_total, 12.0,
                  "record: phase1 failed-stabilize retry shadow dir inf total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max, 9.0,
                  "record: phase1 failed-stabilize retry shadow dir inf max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total, 0.75,
                  "record: phase1 failed-stabilize retry shadow pivot abs total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max, 0.5,
                  "record: phase1 failed-stabilize retry shadow pivot abs max");
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(&solver, 0, 3);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(&solver, 1, 19);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(&solver, 0, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(&solver, 0, 0);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(&solver, 1, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(&solver, 1, 0);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_ratio_failure(&solver, 0);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_dir_failure(&solver, 0);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_ratio_failure(&solver, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_dir_failure(&solver, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_selector_guarded_fallback(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_fail_shape(&solver, 250000.0, 80, 0.75);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_fail_shape(&solver, 25000000.0, 120, 5.0);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_arm(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_no_alt(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_outcome(&solver, 1);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_outcome(&solver, 0);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_arm(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_original_exclusion(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_arm(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_original_exclusion(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_failed_stabilize(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_fail(&solver);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_success(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_stabilized(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_ratio_breakdown(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_failed_stabilize(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_retry(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_dual_rescue(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_forced_refactor(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_next_failed_stabilize(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_next_ratio_breakdown(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_next_pivot_fail(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_next_pivot_success(&solver);
    lp_telemetry_record_phase1_force_extreme_followup_direction(
        &solver, -2, 1.0e-2, 2.0e5, 48, 2.0e-2);
    lp_telemetry_record_phase1_force_extreme_followup_direction(
        &solver, 7, 1.0e-2, 3.0e5, 64, 3.0e-2);
    lp_telemetry_record_phase1_force_extreme_followup_direction(
        &solver, 9, 1.0e-2, 4.0e5, 80, 1.0e-2);
    lp_telemetry_record_phase1_force_extreme_followup_direction(
        &solver, 11, 1.0e-2, 5.0e5, 96, 5.0);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_arms, 1,
                  "record: phase1 failed-stabilize retry selector bland arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_arms, 1,
                  "record: phase1 failed-stabilize retry selector guarded arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total, 19,
                  "record: phase1 failed-stabilize retry selector guarded eligible total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max, 19,
                  "record: phase1 failed-stabilize retry selector guarded eligible max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized, 1,
                  "record: phase1 failed-stabilize retry selector bland stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_failed, 1,
                  "record: phase1 failed-stabilize retry selector bland failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed, 1,
                  "record: phase1 failed-stabilize retry selector bland ratio failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_dir_failed, 1,
                  "record: phase1 failed-stabilize retry selector bland dir failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized, 1,
                  "record: phase1 failed-stabilize retry selector guarded stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed, 1,
                  "record: phase1 failed-stabilize retry selector guarded failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed, 1,
                  "record: phase1 failed-stabilize retry selector guarded ratio failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed, 1,
                  "record: phase1 failed-stabilize retry selector guarded dir failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland, 1,
                  "record: phase1 failed-stabilize retry selector guarded fallback");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_shape_samples, 2,
                  "record: phase1 failed-stabilize retry dir-fail shape samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_total, 200,
                  "record: phase1 failed-stabilize retry dir-fail nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max, 120,
                  "record: phase1 failed-stabilize retry dir-fail nnz max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total, 25250000.0,
                  "record: phase1 failed-stabilize retry dir-fail dir-inf total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max, 25000000.0,
                  "record: phase1 failed-stabilize retry dir-fail dir-inf max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total, 5.75,
                  "record: phase1 failed-stabilize retry dir-fail pivot abs total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max, 5.0,
                  "record: phase1 failed-stabilize retry dir-fail pivot abs max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30, 1,
                  "record: phase1 failed-stabilize retry dir-fail inf ratio <=30");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100, 0,
                  "record: phase1 failed-stabilize retry dir-fail inf ratio <=100");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000, 0,
                  "record: phase1 failed-stabilize retry dir-fail inf ratio <=1000");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000, 1,
                  "record: phase1 failed-stabilize retry dir-fail inf ratio >1000");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8, 0,
                  "record: phase1 failed-stabilize retry dir-fail pivot ratio <=1e-8");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6, 1,
                  "record: phase1 failed-stabilize retry dir-fail pivot ratio <=1e-6");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4, 1,
                  "record: phase1 failed-stabilize retry dir-fail pivot ratio <=1e-4");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4, 0,
                  "record: phase1 failed-stabilize retry dir-fail pivot ratio >1e-4");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_arms, 1,
                  "record: phase1 failed-stabilize retry dir second-chance arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt, 1,
                  "record: phase1 failed-stabilize retry dir second-chance no-alt");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized, 1,
                  "record: phase1 failed-stabilize retry dir second-chance stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_failed, 1,
                  "record: phase1 failed-stabilize retry dir second-chance failed");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_arms, 1,
                  "record: phase1 failed-stabilize retry dir guard arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions, 1,
                  "record: phase1 failed-stabilize retry dir guard original exclusions");
    lp_telemetry_record_phase1_window_pressure_event(
        &solver, 1, 0, 1, 0, 1, 1, 0, 1, 0);
    lp_telemetry_record_phase1_window_pressure_event(
        &solver, 0, 1, 0, 1, 2, 1, 1, 1, 1);
    lp_telemetry_record_phase1_window_pressure_event(
        &solver, 1, 0, 0, 1, 3, 2, 1, 1, 2);
    lp_telemetry_record_phase1_window_pressure_progress_reset(&solver);
    lp_telemetry_record_phase1_window_pressure_force_pivot_arm(&solver);
    lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_pending(&solver);
    lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_budget(&solver);
    lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
        &solver, LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_UNDER_TRIGGER);
    lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
        &solver, LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_FAILED_SHARE);
    lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
        &solver, LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_DIR_SKIP_SHARE);
    lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
        &solver, LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_LOCAL_FAIL);
    lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
        &solver, LP_PHASE1_WINDOW_FORCE_PIVOT_REJECT_ALTERNATION);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_windows_started, 1,
                  "record: phase1 window pressure windows started");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_progress_resets, 1,
                  "record: phase1 window pressure progress resets");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_arms, 1,
                  "record: phase1 window pressure force-pivot arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_pending, 1,
                  "record: phase1 window pressure force-pivot blocked pending");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_budget, 1,
                  "record: phase1 window pressure force-pivot blocked budget");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_under_trigger, 1,
                  "record: phase1 window pressure force-pivot reject under trigger");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_failed_share, 1,
                  "record: phase1 window pressure force-pivot reject failed share");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_dir_skip_share, 1,
                  "record: phase1 window pressure force-pivot reject dir-skip share");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_local_fail, 1,
                  "record: phase1 window pressure force-pivot reject local fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_alternation, 1,
                  "record: phase1 window pressure force-pivot reject alternation");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_event_total, 3,
                  "record: phase1 window pressure event total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_failed_stabilize_total, 2,
                  "record: phase1 window pressure failed-stabilize total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_dir_skip_total, 1,
                  "record: phase1 window pressure dir-skip total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_local_memory_fail_total, 1,
                  "record: phase1 window pressure local-memory-fail total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_alternation_total, 2,
                  "record: phase1 window pressure alternation total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_event_max, 3,
                  "record: phase1 window pressure event max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_failed_stabilize_max, 2,
                  "record: phase1 window pressure failed-stabilize max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_dir_skip_max, 1,
                  "record: phase1 window pressure dir-skip max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_local_memory_fail_max, 1,
                  "record: phase1 window pressure local-memory-fail max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_window_pressure_alternation_max, 2,
                  "record: phase1 window pressure alternation max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_arms, 1,
                  "record: phase1 failed-stabilize retry shadow guard arms");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions, 1,
                  "record: phase1 failed-stabilize retry shadow guard original exclusions");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry, 1,
                  "record: phase1 shadow post dir-skip retry");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue, 1,
                  "record: phase1 shadow post dir-skip dual rescue");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor, 1,
                  "record: phase1 shadow post dir-skip forced refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize, 1,
                  "record: phase1 shadow next failed-stabilize");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown, 1,
                  "record: phase1 shadow next ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail, 1,
                  "record: phase1 shadow next pivot fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_pivot_success, 1,
                  "record: phase1 shadow next pivot success");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_stabilized, 1,
                  "record: phase1 force-extreme followup stabilized");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_ratio_breakdown, 1,
                  "record: phase1 force-extreme followup ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_failed_stabilize, 1,
                  "record: phase1 force-extreme followup failed stabilize");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_retry, 1,
                  "record: phase1 force-extreme post dir-skip retry");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue, 1,
                  "record: phase1 force-extreme post dir-skip dual rescue");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor, 1,
                  "record: phase1 force-extreme post dir-skip forced refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_failed_stabilize, 1,
                  "record: phase1 force-extreme next failed-stabilize");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_ratio_breakdown, 1,
                  "record: phase1 force-extreme next ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_fail, 1,
                  "record: phase1 force-extreme next pivot fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_success, 1,
                  "record: phase1 force-extreme next pivot success");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_samples, 4,
                  "record: phase1 force-extreme followup dir samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_geometry, 1,
                  "record: phase1 force-extreme followup dir bound geometry");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_flip, 1,
                  "record: phase1 force-extreme followup dir bound flip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_tiny_theta, 0,
                  "record: phase1 force-extreme followup dir tiny theta");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_weak_leaving, 2,
                  "record: phase1 force-extreme followup dir weak leaving");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_ftran_shape, 1,
                  "record: phase1 force-extreme followup dir ftran shape");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_total, 288,
                  "record: phase1 force-extreme followup dir nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_max, 96,
                  "record: phase1 force-extreme followup dir nnz max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_total, 1400000.0,
                  "record: phase1 force-extreme followup dir inf total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_max, 500000.0,
                  "record: phase1 force-extreme followup dir inf max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_total, 5.06,
                  "record: phase1 force-extreme followup pivot abs total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_max, 5.0,
                  "record: phase1 force-extreme followup pivot abs max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_theta_total, 0.04,
                  "record: phase1 force-extreme followup theta total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_force_extreme_followup_theta_max, 0.01,
                  "record: phase1 force-extreme followup theta max");
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_followup_direction(
        &solver, -2, 1.0e-9, 2.0e5, 48, 2.0e-2);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_followup_direction(
        &solver, 7, 1.0e-9, 3.0e5, 64, 3.0e-2);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_followup_direction(
        &solver, 9, 1.0e-3, 4.0e5, 80, 1.0e-2);
    lp_telemetry_record_phase1_failed_stabilize_retry_shadow_followup_direction(
        &solver, 11, 1.0e-2, 5.0e5, 96, 5.0);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples, 4,
                  "record: phase1 shadow followup dir samples");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry, 2,
                  "record: phase1 shadow followup dir bound geometry");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip, 1,
                  "record: phase1 shadow followup dir bound flip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta, 2,
                  "record: phase1 shadow followup dir tiny theta");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving, 1,
                  "record: phase1 shadow followup dir weak leaving");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape, 1,
                  "record: phase1 shadow followup dir ftran shape");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total, 288,
                  "record: phase1 shadow followup dir nnz total");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max, 96,
                  "record: phase1 shadow followup dir nnz max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total, 1400000.0,
                  "record: phase1 shadow followup dir inf total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max, 500000.0,
                  "record: phase1 shadow followup dir inf max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total, 5.06,
                  "record: phase1 shadow followup pivot abs total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max, 5.0,
                  "record: phase1 shadow followup pivot abs max");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_theta_total, 0.011000002,
                  "record: phase1 shadow followup theta total");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_theta_max, 0.01,
                  "record: phase1 shadow followup theta max");

    lp_telemetry_record_phase1_recompute(&solver, LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN);
    lp_telemetry_record_phase1_recompute(&solver, LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
    lp_telemetry_record_phase1_recompute(&solver, LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
    lp_telemetry_record_phase1_recompute(&solver, LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR);
    lp_telemetry_record_phase1_recompute(&solver, LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
    lp_telemetry_record_phase1_recompute(&solver, LP_PHASE1_RECOMPUTE_REASON_PERTURB);
    lp_telemetry_record_phase1_recompute(&solver, LP_PHASE1_RECOMPUTE_REASON_PERTURB);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_ratio_breakdown, 1,
                  "record: phase1 recompute ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_dir_skip, 2,
                  "record: phase1 recompute dir skip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_dir_refactor, 1,
                  "record: phase1 recompute dir refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_pivot_fail_recovery, 1,
                  "record: phase1 recompute pivot fail recovery");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_after_perturb, 2,
                  "record: phase1 recompute perturb");
    lp_telemetry_record_phase1_recompute_rc_only(&solver);
    lp_telemetry_record_phase1_recompute_rc_only(&solver);
    lp_telemetry_record_phase1_recompute_guard_forced_full(&solver);
    lp_telemetry_record_phase1_ratio_breakdown_retry(&solver);
    lp_telemetry_record_phase1_ratio_breakdown_retry(&solver);
    lp_telemetry_record_phase1_ratio_breakdown_escalation(&solver);
    lp_telemetry_record_phase1_pivot_fail_recovery_exclusion(&solver);
    lp_telemetry_record_phase1_pivot_fail_recovery_exclusion(&solver);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_rc_only_calls, 2,
                  "record: phase1 recompute rc-only calls");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_recompute_rc_guard_forced_full, 1,
                  "record: phase1 recompute rc-only guard forced");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_ratio_breakdown_retries, 2,
                  "record: phase1 ratio breakdown retries");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_ratio_breakdown_escalations, 1,
                  "record: phase1 ratio breakdown escalations");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_pivot_fail_recovery_exclusions, 2,
                  "record: phase1 pivot fail recovery exclusions");
    lp_telemetry_record_phase1_no_pivot_event(
        &solver, LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN);
    lp_telemetry_record_phase1_no_pivot_event(
        &solver, LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
    lp_telemetry_record_phase1_no_pivot_event(
        &solver, LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL);
    lp_telemetry_record_phase1_no_pivot_force(&solver,
                                              LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN);
    lp_telemetry_record_phase1_no_pivot_force(&solver,
                                              LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
    lp_telemetry_record_phase1_no_pivot_force(&solver,
                                              LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL);
    lp_telemetry_record_phase1_soft_lu_policy_cooldown_defer(&solver);
    lp_telemetry_record_phase1_soft_lu_policy_cooldown_defer(&solver);
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_events, 3,
                  "record: phase1 no-pivot events");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_events_ratio_breakdown, 1,
                  "record: phase1 no-pivot events ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_events_dir_skip, 1,
                  "record: phase1 no-pivot events dir skip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_events_pivot_fail, 1,
                  "record: phase1 no-pivot events pivot fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_refactor, 3,
                  "record: phase1 no-pivot forced refactors");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_ratio_breakdown, 1,
                  "record: phase1 no-pivot force ratio breakdown");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_dir_skip, 1,
                  "record: phase1 no-pivot force dir skip");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_no_pivot_forced_pivot_fail, 1,
                  "record: phase1 no-pivot force pivot fail");
    ASSERT_INT_EQ(solver.telemetry.perf_phase1_soft_lu_policy_cooldown_defers, 2,
                  "record: phase1 soft-lu periodic cooldown defers");

    lp_telemetry_record_dual_bound_flip_applied_startup(&solver, 3);
    lp_telemetry_record_dual_bound_flip_applied_iterative(&solver, 2);
    lp_telemetry_record_dual_bound_flip_applied(&solver, 4);
    ASSERT_INT_EQ(solver.telemetry.perf_dual_bound_flip_applied, 9,
                  "record: dual bound flips aggregate");
    ASSERT_INT_EQ(solver.telemetry.perf_dual_bound_flip_startup, 3,
                  "record: dual bound flips startup");
    ASSERT_INT_EQ(solver.telemetry.perf_dual_bound_flip_iterative, 2,
                  "record: dual bound flips iterative");

    lp_telemetry_record_reinvert_shadow(&solver,
                                        1,
                                        LP_REINVERT_DECISION_DEFER,
                                        LP_REINVERT_REASON_COST_DAMPEN,
                                        0,
                                        1);
    lp_telemetry_record_reinvert_shadow(&solver,
                                        2,
                                        LP_REINVERT_DECISION_FORCE,
                                        LP_REINVERT_REASON_HARD_LU_HEALTH,
                                        1,
                                        1);
    lp_telemetry_record_reinvert_shadow(&solver,
                                        0,
                                        LP_REINVERT_DECISION_ALLOW,
                                        LP_REINVERT_REASON_PERIODIC_CADENCE,
                                        0,
                                        0);
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_checks_phase1, 1,
                  "record: reinvert checks phase1");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_suggest_defer_phase1, 1,
                  "record: reinvert suggest defer phase1");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_actual_refactor_yes_phase1, 1,
                  "record: reinvert actual yes phase1");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_disagree_phase1, 1,
                  "record: reinvert disagree phase1");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_checks_phase2, 1,
                  "record: reinvert checks phase2");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_suggest_force_phase2, 1,
                  "record: reinvert suggest force phase2");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_disagree_phase2, 0,
                  "record: reinvert disagree phase2");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_checks_dual, 1,
                  "record: reinvert checks dual");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_suggest_allow_dual, 1,
                  "record: reinvert suggest allow dual");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_actual_refactor_no_dual, 1,
                  "record: reinvert actual no dual");
    ASSERT_INT_EQ(solver.telemetry.perf_reinvert_shadow_disagree_dual, 0,
                  "record: reinvert disagree dual");
}

static void test_refactor_reason_classifier(void) {
    printf("  telemetry/solver: refactor reason classifier...\n");

    ASSERT(lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_RATIO_RECOVERY),
           "classifier: ratio recovery is safety-forced");
    ASSERT(lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_PIVOT_RECOVERY),
           "classifier: pivot recovery is safety-forced");
    ASSERT(!lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_SETUP),
           "classifier: setup is not safety-forced");
    ASSERT(!lp_telemetry_refactor_reason_is_safety_forced(RALPH_REFACTOR_REASON_PERIODIC),
           "classifier: periodic is not safety-forced");
}

static void test_solver_snapshot(void) {
    printf("  telemetry/solver: snapshot...\n");

    SimplexSolver solver;
    LPSolverTelemetrySnapshot snap;
    memset(&solver, 0, sizeof(solver));

    solver.telemetry.perf_phase2_ms = 42.25;
    solver.telemetry.perf_ftran_calls = 31;
    solver.telemetry.perf_btran_calls = 19;
    solver.telemetry.perf_ftran_base_ms = 12.5;
    solver.telemetry.perf_ftran_update_apply_ms = 3.75;
    solver.telemetry.perf_ftran_update_apply_calls = 11;
    solver.telemetry.perf_btran_base_ms = 8.5;
    solver.telemetry.perf_btran_update_apply_ms = 2.25;
    solver.telemetry.perf_btran_update_apply_calls = 9;
    solver.telemetry.perf_ftran_nnz_samples = 29;
    solver.telemetry.perf_btran_nnz_samples = 17;
    solver.telemetry.perf_ftran_rhs_nnz_total = 377;
    solver.telemetry.perf_ftran_sol_nnz_total = 2441;
    solver.telemetry.perf_btran_rhs_nnz_total = 121;
    solver.telemetry.perf_btran_sol_nnz_total = 1303;
    solver.telemetry.perf_refactor_reason_periodic = 11;
    solver.telemetry.perf_basis_tail_shift_bytes = 4096ULL;
    solver.telemetry.perf_phase1_pricing_calls = 17;
    solver.telemetry.perf_phase1_dir_stabilize_force_extreme_dir = 6;
    solver.telemetry.perf_phase1_dir_stabilize_force_lu_health = 9;
    solver.telemetry.perf_phase1_dir_stabilize_cooldown_candidates = 12;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_3 = 1;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_10 = 2;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_30 = 3;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_le_100 = 4;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_100 = 2;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_300 = 1;
    solver.telemetry.perf_phase1_dir_stabilize_ratio_gt_1000 = 1;
    solver.telemetry.perf_phase1_dir_stabilize_skip_rc_only = 10;
    solver.telemetry.perf_phase1_dir_stabilize_skip_full = 4;
    solver.telemetry.perf_phase1_dir_stabilize_skip_no_recompute = 12;
    solver.telemetry.perf_phase1_dir_stabilize_skip_guard_refresh = 3;
    solver.telemetry.perf_phase1_force_extreme_tiny_theta_relax_applied = 7;
    solver.telemetry.perf_phase1_recompute_after_ratio_breakdown = 12;
    solver.telemetry.perf_phase1_recompute_after_dir_skip = 7;
    solver.telemetry.perf_phase1_recompute_after_dir_refactor = 5;
    solver.telemetry.perf_phase1_recompute_after_pivot_fail_recovery = 4;
    solver.telemetry.perf_phase1_recompute_after_perturb = 3;
    solver.telemetry.perf_phase1_recompute_rc_only_calls = 11;
    solver.telemetry.perf_phase1_compute_solution_ctx_refactor_success = 6;
    solver.telemetry.perf_phase1_compute_solution_ctx_no_entering_cleanup = 2;
    solver.telemetry.perf_phase1_compute_solution_ctx_dual_rescue = 13;
    solver.telemetry.perf_phase1_compute_rc_ctx_recompute_full = 9;
    solver.telemetry.perf_phase1_compute_rc_ctx_recompute_rc_only = 11;
    solver.telemetry.perf_phase1_compute_rc_ctx_dual_rescue = 5;
    solver.telemetry.perf_phase1_entering_exclusions = 17;
    solver.telemetry.perf_phase1_entering_exclusion_repeats = 6;
    solver.telemetry.perf_phase1_entering_exclusion_hits = 12;
    solver.telemetry.perf_phase1_entering_exclusion_reroutes = 9;
    solver.telemetry.perf_phase1_entering_exclusion_no_alt = 3;
    solver.telemetry.perf_phase1_recompute_rc_guard_forced_full = 2;
    solver.telemetry.perf_phase1_ratio_breakdown_retries = 14;
    solver.telemetry.perf_phase1_ratio_breakdown_escalations = 3;
    solver.telemetry.perf_phase1_pivot_fail_recovery_exclusions = 5;
    solver.telemetry.perf_phase1_no_pivot_events = 21;
    solver.telemetry.perf_phase1_no_pivot_forced_refactor = 6;
    solver.telemetry.perf_phase1_no_pivot_forced_ratio_breakdown = 2;
    solver.telemetry.perf_phase1_no_pivot_forced_dir_skip = 3;
    solver.telemetry.perf_phase1_no_pivot_forced_pivot_fail = 1;
    solver.telemetry.perf_phase1_direct_dual_rescue_attempts = 7;
    solver.telemetry.perf_phase1_direct_dual_rescue_successes = 2;
    solver.telemetry.perf_phase1_direct_dual_rescue_failures = 5;
    solver.telemetry.perf_phase1_direct_dual_rescue_guard_cooldown_blocks = 4;
    solver.telemetry.perf_phase1_direct_dual_rescue_guard_fail_cap_blocks = 1;
    solver.telemetry.perf_phase1_soft_lu_policy_cooldown_defers = 4;
    solver.telemetry.perf_phase1_dir_skip_same_entering_repeats = 15;
    solver.telemetry.perf_phase1_dir_skip_same_entering_max_streak = 5;
    solver.telemetry.perf_phase1_failed_stabilize_events = 14;
    solver.telemetry.perf_phase1_failed_stabilize_primary_failures = 6;
    solver.telemetry.perf_phase1_failed_stabilize_alternate_failures = 8;
    solver.telemetry.perf_phase1_failed_stabilize_same_entering_repeats = 8;
    solver.telemetry.perf_phase1_failed_stabilize_same_entering_max_streak = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_arms = 9;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_found = 7;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_no_alt = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_alt_failed = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_arms = 6;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_found = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_no_alt = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_local_memory_alt_failed = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_samples = 7;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_total = 19;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_eligible_max = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_singleton_samples = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_pool_best_differs_samples = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_samples = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total = 13.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max = 5.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2 = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4 = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_samples = 6;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_ratio_failed = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_stable = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total = 44;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max = 17;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_total = 29.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max = 12.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total = 7.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max = 3.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_arms = 6;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_arms = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total = 71;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max = 28;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_alt_failed = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_bland_dir_failed = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_shape_samples = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_total = 43;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max = 17;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total = 71000.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max = 21000.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total = 19.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max = 6.5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30 = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100 = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000 = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000 = 5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8 = 6;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6 = 7;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4 = 8;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4 = 9;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_arms = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_second_chance_failed = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_arms = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions = 3;
    solver.telemetry.perf_phase1_window_pressure_windows_started = 8;
    solver.telemetry.perf_phase1_window_pressure_progress_resets = 6;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_arms = 4;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_pending = 2;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_blocked_budget = 3;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_under_trigger = 4;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_failed_share = 5;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_dir_skip_share = 6;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_local_fail = 7;
    solver.telemetry.perf_phase1_window_pressure_force_pivot_reject_alternation = 8;
    solver.telemetry.perf_phase1_window_pressure_event_total = 29;
    solver.telemetry.perf_phase1_window_pressure_failed_stabilize_total = 15;
    solver.telemetry.perf_phase1_window_pressure_dir_skip_total = 14;
    solver.telemetry.perf_phase1_window_pressure_local_memory_fail_total = 9;
    solver.telemetry.perf_phase1_window_pressure_alternation_total = 18;
    solver.telemetry.perf_phase1_window_pressure_event_max = 11;
    solver.telemetry.perf_phase1_window_pressure_failed_stabilize_max = 6;
    solver.telemetry.perf_phase1_window_pressure_dir_skip_max = 5;
    solver.telemetry.perf_phase1_window_pressure_local_memory_fail_max = 4;
    solver.telemetry.perf_phase1_window_pressure_alternation_max = 9;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_arms = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry = 8;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize = 7;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown = 4;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_next_pivot_success = 5;
    solver.telemetry.perf_phase1_force_extreme_followup_stabilized = 9;
    solver.telemetry.perf_phase1_force_extreme_followup_ratio_breakdown = 4;
    solver.telemetry.perf_phase1_force_extreme_followup_failed_stabilize = 8;
    solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_retry = 11;
    solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue = 3;
    solver.telemetry.perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor = 2;
    solver.telemetry.perf_phase1_force_extreme_followup_next_failed_stabilize = 10;
    solver.telemetry.perf_phase1_force_extreme_followup_next_ratio_breakdown = 6;
    solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_fail = 1;
    solver.telemetry.perf_phase1_force_extreme_followup_next_pivot_success = 7;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_samples = 12;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_geometry = 3;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_bound_flip = 1;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_tiny_theta = 2;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_weak_leaving = 5;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_ftran_shape = 4;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_total = 155;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_nnz_max = 44;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_total = 830000.0;
    solver.telemetry.perf_phase1_force_extreme_followup_dir_inf_max = 410000.0;
    solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_total = 13.5;
    solver.telemetry.perf_phase1_force_extreme_followup_pivot_abs_max = 6.75;
    solver.telemetry.perf_phase1_force_extreme_followup_theta_total = 0.024;
    solver.telemetry.perf_phase1_force_extreme_followup_theta_max = 0.008;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples = 6;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta = 2;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving = 3;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape = 1;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total = 81;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max = 21;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total = 915000.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max = 410000.0;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total = 8.75;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max = 4.5;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_theta_total = 0.0065;
    solver.telemetry.perf_phase1_failed_stabilize_retry_shadow_followup_theta_max = 0.0030;
    solver.telemetry.perf_dual_bound_flip_applied = 13;
    solver.telemetry.perf_dual_bound_flip_startup = 5;
    solver.telemetry.perf_dual_bound_flip_iterative = 8;
    solver.telemetry.perf_reinvert_shadow_checks_phase1 = 17;
    solver.telemetry.perf_reinvert_shadow_suggest_defer_phase1 = 9;
    solver.telemetry.perf_reinvert_shadow_actual_refactor_yes_phase1 = 4;
    solver.telemetry.perf_reinvert_shadow_disagree_phase1 = 2;
    solver.telemetry.perf_reinvert_shadow_last_reason_phase1 = LP_REINVERT_REASON_COST_DAMPEN;
    solver.policy.periodic_feedback_phase2.hint_pressure = 0.55;
    solver.policy.soft_lu_cost_gate_enabled = 1;
    solver.policy.soft_lu_cost_gate_phase1.defers = 3;
    solver.policy.soft_lu_cost_gate_phase1.consecutive_defers = 2;
    solver.policy.soft_lu_cost_gate_phase1.defer_cap_forced = 1;
    solver.policy.soft_lu_cost_gate_phase2.refactor_cost_ewma = 7.25;
    solver.policy.periodic_cost_gate_enabled = 1;
    solver.policy.periodic_cost_gate_phase1.defers = 4;
    solver.policy.periodic_cost_gate_phase1.consecutive_defers = 1;
    solver.policy.periodic_cost_gate_phase1.defer_cap_forced = 2;
    solver.policy.periodic_cost_gate_phase1.checks = 7;
    solver.policy.periodic_cost_gate_phase1.block_ratio = 3;
    solver.policy.periodic_cost_gate_phase1.block_warmup = 1;
    solver.policy.periodic_cost_gate_phase1.last_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO;
    solver.policy.periodic_cost_gate_phase1.iter_samples = 19;
    solver.policy.periodic_cost_gate_phase1.refactor_samples = 4;
    solver.policy.basis_governor_mode = LP_BASIS_GOV_MODE_SHADOW;
    solver.policy.reinvert_controller_mode = LP_REINVERT_MODE_CONTROL_ALL;
    solver.policy.reinvert_dual.control_demoted = 1;
    solver.policy.reinvert_dual.control_demotions = 1;
    solver.policy.reinvert_dual.hard_trigger_last_total = 13;
    solver.policy.reinvert_dual.hard_trigger_last_iter = 101;
    solver.policy.reinvert_dual.hard_trigger_burst = 7;
    solver.policy.reinvert_phase1.control_demoted = 1;
    solver.policy.reinvert_phase1.control_demotions = 2;
    solver.policy.reinvert_phase1.pressure_last_iter = 87;
    solver.policy.reinvert_phase1.pressure_burst = 6;
    solver.policy.phase1_stagnation.escape_cooldown = 23;
    solver.policy.phase1_stagnation.escape_triggers = 5;
    solver.policy.phase1_stagnation.escape_successes = 4;
    solver.policy.phase1_stagnation.escape_failures = 1;
    solver.policy.phase1_stagnation.escape_cooldown_blocks = 2;
    solver.policy.phase1_stagnation.last_window_iters = 96;
    solver.policy.phase1_stagnation.last_obj_delta = 0.00012;
    solver.policy.phase1_stagnation.last_retry_defer_ratio = 0.85;
    solver.policy.phase1_stagnation.last_update_recovery_ratio = 0.6;
    solver.policy.phase1_stagnation.last_retry_defers = 17;
    solver.policy.phase1_stagnation.last_no_pivot_events = 20;
    solver.policy.phase1_stagnation.last_update_recovery_refactors = 6;
    solver.policy.phase1_stagnation.last_refactors = 8;
    solver.policy.phase1_stagnation.last_recompute_ratio = 9;
    solver.policy.phase1_stagnation.last_recompute_dir_skip = 7;
    solver.policy.phase1_stagnation.last_recompute_dir_refactor = 2;
    solver.policy.phase1_stagnation.last_recompute_pivot_fail = 1;
    solver.policy.phase1_stagnation.last_recompute_perturb = 0;
    lp_basis_governor_set_mode(&solver.policy.basis_governor,
                               solver.policy.basis_governor_mode);
    solver.policy.basis_governor.shadow_refactor_yes_phase1 = 8;
    solver.policy.basis_governor.shadow_disagree_primal_refactor = 3;

    lp_telemetry_snapshot_solver(&solver, &snap);

    ASSERT_DBL_EQ(snap.perf_phase2_ms, 42.25, "solver_snapshot: phase2_ms");
    ASSERT_INT_EQ(snap.perf_ftran_calls, 31, "solver_snapshot: ftran calls");
    ASSERT_INT_EQ(snap.perf_btran_calls, 19, "solver_snapshot: btran calls");
    ASSERT_DBL_EQ(snap.perf_ftran_base_ms, 12.5, "solver_snapshot: ftran base ms");
    ASSERT_DBL_EQ(snap.perf_ftran_update_apply_ms, 3.75,
                  "solver_snapshot: ftran update apply ms");
    ASSERT_INT_EQ(snap.perf_ftran_update_apply_calls, 11,
                  "solver_snapshot: ftran update apply calls");
    ASSERT_DBL_EQ(snap.perf_btran_base_ms, 8.5, "solver_snapshot: btran base ms");
    ASSERT_DBL_EQ(snap.perf_btran_update_apply_ms, 2.25,
                  "solver_snapshot: btran update apply ms");
    ASSERT_INT_EQ(snap.perf_btran_update_apply_calls, 9,
                  "solver_snapshot: btran update apply calls");
    ASSERT_INT_EQ(snap.perf_ftran_nnz_samples, 29, "solver_snapshot: ftran nnz samples");
    ASSERT_INT_EQ(snap.perf_btran_nnz_samples, 17, "solver_snapshot: btran nnz samples");
    ASSERT_INT_EQ((int)snap.perf_ftran_rhs_nnz_total, 377, "solver_snapshot: ftran rhs nnz total");
    ASSERT_INT_EQ((int)snap.perf_ftran_sol_nnz_total, 2441, "solver_snapshot: ftran sol nnz total");
    ASSERT_INT_EQ((int)snap.perf_btran_rhs_nnz_total, 121, "solver_snapshot: btran rhs nnz total");
    ASSERT_INT_EQ((int)snap.perf_btran_sol_nnz_total, 1303, "solver_snapshot: btran sol nnz total");
    ASSERT_INT_EQ(snap.perf_refactor_reason_periodic, 11,
                  "solver_snapshot: refactor_reason_periodic");
    ASSERT_ULL_EQ(snap.perf_basis_tail_shift_bytes, 4096ULL,
                  "solver_snapshot: basis_tail_shift_bytes");
    ASSERT_INT_EQ(snap.perf_phase1_pricing_calls, 17,
                  "solver_snapshot: phase1_pricing_calls");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_force_extreme_dir, 6,
                  "solver_snapshot: phase1 dir force extreme");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_force_lu_health, 9,
                  "solver_snapshot: phase1 dir force lu health");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_cooldown_candidates, 12,
                  "solver_snapshot: phase1 dir cooldown candidates");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_ratio_le_3, 1,
                  "solver_snapshot: phase1 dir ratio <=3");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_ratio_le_10, 2,
                  "solver_snapshot: phase1 dir ratio <=10");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_ratio_le_30, 3,
                  "solver_snapshot: phase1 dir ratio <=30");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_ratio_le_100, 4,
                  "solver_snapshot: phase1 dir ratio <=100");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_ratio_gt_100, 2,
                  "solver_snapshot: phase1 dir ratio >100");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_ratio_gt_300, 1,
                  "solver_snapshot: phase1 dir ratio >300");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_ratio_gt_1000, 1,
                  "solver_snapshot: phase1 dir ratio >1000");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_skip_rc_only, 10,
                  "solver_snapshot: phase1 dir skip rc-only");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_skip_full, 4,
                  "solver_snapshot: phase1 dir skip full");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_skip_no_recompute, 12,
                  "solver_snapshot: phase1 dir skip no recompute");
    ASSERT_INT_EQ(snap.perf_phase1_dir_stabilize_skip_guard_refresh, 3,
                  "solver_snapshot: phase1 dir skip guard refresh");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_tiny_theta_relax_applied, 7,
                  "solver_snapshot: phase1 force extreme tiny-theta relax");
    ASSERT_INT_EQ(snap.perf_phase1_recompute_after_ratio_breakdown, 12,
                  "solver_snapshot: phase1 recompute ratio breakdown");
    ASSERT_INT_EQ(snap.perf_phase1_recompute_after_dir_skip, 7,
                  "solver_snapshot: phase1 recompute dir skip");
    ASSERT_INT_EQ(snap.perf_phase1_recompute_after_dir_refactor, 5,
                  "solver_snapshot: phase1 recompute dir refactor");
    ASSERT_INT_EQ(snap.perf_phase1_recompute_after_pivot_fail_recovery, 4,
                  "solver_snapshot: phase1 recompute pivot fail recovery");
    ASSERT_INT_EQ(snap.perf_phase1_recompute_after_perturb, 3,
                  "solver_snapshot: phase1 recompute perturb");
    ASSERT_INT_EQ(snap.perf_phase1_recompute_rc_only_calls, 11,
                  "solver_snapshot: phase1 recompute rc-only calls");
    ASSERT_INT_EQ(snap.perf_phase1_compute_solution_ctx_refactor_success, 6,
                  "solver_snapshot: phase1 compute_solution refactor success ctx");
    ASSERT_INT_EQ(snap.perf_phase1_compute_solution_ctx_no_entering_cleanup, 2,
                  "solver_snapshot: phase1 compute_solution no-entering cleanup ctx");
    ASSERT_INT_EQ(snap.perf_phase1_compute_solution_ctx_dual_rescue, 13,
                  "solver_snapshot: phase1 compute_solution dual rescue ctx");
    ASSERT_INT_EQ(snap.perf_phase1_compute_rc_ctx_recompute_full, 9,
                  "solver_snapshot: phase1 compute_rc recompute full ctx");
    ASSERT_INT_EQ(snap.perf_phase1_compute_rc_ctx_recompute_rc_only, 11,
                  "solver_snapshot: phase1 compute_rc rc-only ctx");
    ASSERT_INT_EQ(snap.perf_phase1_compute_rc_ctx_dual_rescue, 5,
                  "solver_snapshot: phase1 compute_rc dual rescue ctx");
    ASSERT_INT_EQ(snap.perf_phase1_entering_exclusions, 17,
                  "solver_snapshot: phase1 entering exclusions");
    ASSERT_INT_EQ(snap.perf_phase1_entering_exclusion_repeats, 6,
                  "solver_snapshot: phase1 entering exclusion repeats");
    ASSERT_INT_EQ(snap.perf_phase1_entering_exclusion_hits, 12,
                  "solver_snapshot: phase1 entering exclusion hits");
    ASSERT_INT_EQ(snap.perf_phase1_entering_exclusion_reroutes, 9,
                  "solver_snapshot: phase1 entering exclusion reroutes");
    ASSERT_INT_EQ(snap.perf_phase1_entering_exclusion_no_alt, 3,
                  "solver_snapshot: phase1 entering exclusion no alt");
    ASSERT_INT_EQ(snap.perf_phase1_recompute_rc_guard_forced_full, 2,
                  "solver_snapshot: phase1 recompute rc-only guard forced");
    ASSERT_INT_EQ(snap.perf_phase1_ratio_breakdown_retries, 14,
                  "solver_snapshot: phase1 ratio breakdown retries");
    ASSERT_INT_EQ(snap.perf_phase1_ratio_breakdown_escalations, 3,
                  "solver_snapshot: phase1 ratio breakdown escalations");
    ASSERT_INT_EQ(snap.perf_phase1_pivot_fail_recovery_exclusions, 5,
                  "solver_snapshot: phase1 pivot fail recovery exclusions");
    ASSERT_INT_EQ(snap.perf_phase1_no_pivot_events, 21,
                  "solver_snapshot: phase1 no-pivot events");
    ASSERT_INT_EQ(snap.perf_phase1_no_pivot_forced_refactor, 6,
                  "solver_snapshot: phase1 no-pivot forced refactor");
    ASSERT_INT_EQ(snap.perf_phase1_no_pivot_forced_ratio_breakdown, 2,
                  "solver_snapshot: phase1 no-pivot force ratio breakdown");
    ASSERT_INT_EQ(snap.perf_phase1_no_pivot_forced_dir_skip, 3,
                  "solver_snapshot: phase1 no-pivot force dir skip");
    ASSERT_INT_EQ(snap.perf_phase1_no_pivot_forced_pivot_fail, 1,
                  "solver_snapshot: phase1 no-pivot force pivot fail");
    ASSERT_INT_EQ(snap.perf_phase1_direct_dual_rescue_attempts, 7,
                  "solver_snapshot: phase1 direct dual rescue attempts");
    ASSERT_INT_EQ(snap.perf_phase1_direct_dual_rescue_successes, 2,
                  "solver_snapshot: phase1 direct dual rescue successes");
    ASSERT_INT_EQ(snap.perf_phase1_direct_dual_rescue_failures, 5,
                  "solver_snapshot: phase1 direct dual rescue failures");
    ASSERT_INT_EQ(snap.perf_phase1_direct_dual_rescue_guard_cooldown_blocks, 4,
                  "solver_snapshot: phase1 direct dual rescue guard cooldown");
    ASSERT_INT_EQ(snap.perf_phase1_direct_dual_rescue_guard_fail_cap_blocks, 1,
                  "solver_snapshot: phase1 direct dual rescue guard fail cap");
    ASSERT_INT_EQ(snap.perf_phase1_soft_lu_policy_cooldown_defers, 4,
                  "solver_snapshot: phase1 soft-lu periodic cooldown defers");
    ASSERT_INT_EQ(snap.perf_phase1_dir_skip_same_entering_repeats, 15,
                  "solver_snapshot: phase1 dir-skip same-entering repeats");
    ASSERT_INT_EQ(snap.perf_phase1_dir_skip_same_entering_max_streak, 5,
                  "solver_snapshot: phase1 dir-skip same-entering max streak");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_events, 14,
                  "solver_snapshot: phase1 failed stabilize events");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_primary_failures, 6,
                  "solver_snapshot: phase1 failed stabilize primary failures");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_alternate_failures, 8,
                  "solver_snapshot: phase1 failed stabilize alternate failures");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_same_entering_repeats, 8,
                  "solver_snapshot: phase1 failed stabilize same-entering repeats");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_same_entering_max_streak, 4,
                  "solver_snapshot: phase1 failed stabilize same-entering max streak");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_penalty_arms, 9,
                  "solver_snapshot: phase1 failed stabilize retry arms");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_penalty_alt_found, 7,
                  "solver_snapshot: phase1 failed stabilize retry alternate found");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_penalty_no_alt, 2,
                  "solver_snapshot: phase1 failed stabilize retry no-alt");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized, 3,
                  "solver_snapshot: phase1 failed stabilize retry alternate stabilized");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_penalty_alt_failed, 4,
                  "solver_snapshot: phase1 failed stabilize retry alternate failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats, 5,
                  "solver_snapshot: phase1 failed stabilize retry same alternate repeats");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak, 3,
                  "solver_snapshot: phase1 failed stabilize retry same alternate max streak");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_local_memory_arms, 6,
                  "solver_snapshot: phase1 failed stabilize retry local-memory arms");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_local_memory_alt_found, 4,
                  "solver_snapshot: phase1 failed stabilize retry local-memory alt found");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_local_memory_no_alt, 2,
                  "solver_snapshot: phase1 failed stabilize retry local-memory no-alt");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt, 2,
                  "solver_snapshot: phase1 failed stabilize retry local-memory fallback same-alt");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized, 1,
                  "solver_snapshot: phase1 failed stabilize retry local-memory alt stabilized");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_local_memory_alt_failed, 3,
                  "solver_snapshot: phase1 failed stabilize retry local-memory alt failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_pool_samples, 7,
                  "solver_snapshot: phase1 failed stabilize retry pool samples");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_pool_eligible_total, 19,
                  "solver_snapshot: phase1 failed stabilize retry pool eligible total");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_pool_eligible_max, 5,
                  "solver_snapshot: phase1 failed stabilize retry pool eligible max");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_pool_singleton_samples, 2,
                  "solver_snapshot: phase1 failed stabilize retry pool singleton samples");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_pool_best_differs_samples, 3,
                  "solver_snapshot: phase1 failed stabilize retry pool best differs");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_eval_samples, 5,
                  "solver_snapshot: phase1 failed stabilize retry selector eval samples");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples, 4,
                  "solver_snapshot: phase1 failed stabilize retry selector eval best differs");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total, 13.0,
                  "solver_snapshot: phase1 failed stabilize retry selector eval score ratio total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max, 5.0,
                  "solver_snapshot: phase1 failed stabilize retry selector eval score ratio max");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2, 3,
                  "solver_snapshot: phase1 failed stabilize retry selector eval score ratio >=2");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4, 1,
                  "solver_snapshot: phase1 failed stabilize retry selector eval score ratio >=4");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_samples, 6,
                  "solver_snapshot: phase1 failed stabilize retry shadow samples");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_ratio_failed, 2,
                  "solver_snapshot: phase1 failed stabilize retry shadow ratio failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_dir_stable, 3,
                  "solver_snapshot: phase1 failed stabilize retry shadow dir stable");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_dir_failed, 1,
                  "solver_snapshot: phase1 failed stabilize retry shadow dir failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total, 44,
                  "solver_snapshot: phase1 failed stabilize retry shadow dir nnz total");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max, 17,
                  "solver_snapshot: phase1 failed stabilize retry shadow dir nnz max");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_dir_inf_total, 29.0,
                  "solver_snapshot: phase1 failed stabilize retry shadow dir inf total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max, 12.0,
                  "solver_snapshot: phase1 failed stabilize retry shadow dir inf max");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total, 7.0,
                  "solver_snapshot: phase1 failed stabilize retry shadow pivot abs total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max, 3.0,
                  "solver_snapshot: phase1 failed stabilize retry shadow pivot abs max");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_bland_arms, 6,
                  "solver_snapshot: phase1 failed stabilize retry selector bland arms");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_arms, 4,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded arms");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total, 71,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded eligible total");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max, 28,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded eligible max");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized, 2,
                  "solver_snapshot: phase1 failed stabilize retry selector bland stabilized");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_bland_alt_failed, 4,
                  "solver_snapshot: phase1 failed stabilize retry selector bland failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed, 1,
                  "solver_snapshot: phase1 failed stabilize retry selector bland ratio failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_bland_dir_failed, 3,
                  "solver_snapshot: phase1 failed stabilize retry selector bland dir failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized, 1,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded stabilized");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed, 3,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed, 2,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded ratio failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed, 1,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded dir failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland, 2,
                  "solver_snapshot: phase1 failed stabilize retry selector guarded fallback");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_shape_samples, 5,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail shape samples");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_nnz_total, 43,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail nnz total");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max, 17,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail nnz max");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total, 71000.0,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail dir-inf total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max, 21000.0,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail dir-inf max");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total, 19.0,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail pivot abs total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max, 6.5,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail pivot abs max");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30, 2,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail inf ratio <=30");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100, 3,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail inf ratio <=100");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000, 4,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail inf ratio <=1000");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000, 5,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail inf ratio >1000");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8, 6,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail pivot ratio <=1e-8");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6, 7,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail pivot ratio <=1e-6");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4, 8,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail pivot ratio <=1e-4");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4, 9,
                  "solver_snapshot: phase1 failed stabilize retry dir-fail pivot ratio >1e-4");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_second_chance_arms, 3,
                  "solver_snapshot: phase1 failed stabilize retry dir second-chance arms");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt, 1,
                  "solver_snapshot: phase1 failed stabilize retry dir second-chance no-alt");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized, 1,
                  "solver_snapshot: phase1 failed stabilize retry dir second-chance stabilized");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_second_chance_failed, 2,
                  "solver_snapshot: phase1 failed stabilize retry dir second-chance failed");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_guard_arms, 4,
                  "solver_snapshot: phase1 failed stabilize retry dir guard arms");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions, 3,
                  "solver_snapshot: phase1 failed stabilize retry dir guard original exclusions");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_windows_started, 8,
                  "solver_snapshot: phase1 window pressure windows started");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_progress_resets, 6,
                  "solver_snapshot: phase1 window pressure progress resets");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_arms, 4,
                  "solver_snapshot: phase1 window pressure force-pivot arms");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_blocked_pending, 2,
                  "solver_snapshot: phase1 window pressure force-pivot blocked pending");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_blocked_budget, 3,
                  "solver_snapshot: phase1 window pressure force-pivot blocked budget");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_reject_under_trigger, 4,
                  "solver_snapshot: phase1 window pressure force-pivot reject under trigger");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_reject_failed_share, 5,
                  "solver_snapshot: phase1 window pressure force-pivot reject failed share");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_reject_dir_skip_share, 6,
                  "solver_snapshot: phase1 window pressure force-pivot reject dir-skip share");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_reject_local_fail, 7,
                  "solver_snapshot: phase1 window pressure force-pivot reject local fail");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_force_pivot_reject_alternation, 8,
                  "solver_snapshot: phase1 window pressure force-pivot reject alternation");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_event_total, 29,
                  "solver_snapshot: phase1 window pressure event total");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_failed_stabilize_total, 15,
                  "solver_snapshot: phase1 window pressure failed-stabilize total");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_dir_skip_total, 14,
                  "solver_snapshot: phase1 window pressure dir-skip total");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_local_memory_fail_total, 9,
                  "solver_snapshot: phase1 window pressure local-memory-fail total");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_alternation_total, 18,
                  "solver_snapshot: phase1 window pressure alternation total");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_event_max, 11,
                  "solver_snapshot: phase1 window pressure event max");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_failed_stabilize_max, 6,
                  "solver_snapshot: phase1 window pressure failed-stabilize max");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_dir_skip_max, 5,
                  "solver_snapshot: phase1 window pressure dir-skip max");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_local_memory_fail_max, 4,
                  "solver_snapshot: phase1 window pressure local-memory-fail max");
    ASSERT_INT_EQ(snap.perf_phase1_window_pressure_alternation_max, 9,
                  "solver_snapshot: phase1 window pressure alternation max");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_guard_arms, 2,
                  "solver_snapshot: phase1 failed stabilize retry shadow guard arms");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions, 1,
                  "solver_snapshot: phase1 failed stabilize retry shadow guard original exclusions");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry, 8,
                  "solver_snapshot: phase1 shadow post dir-skip retry");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue, 3,
                  "solver_snapshot: phase1 shadow post dir-skip dual rescue");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor, 2,
                  "solver_snapshot: phase1 shadow post dir-skip forced refactor");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize, 7,
                  "solver_snapshot: phase1 shadow next failed-stabilize");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown, 4,
                  "solver_snapshot: phase1 shadow next ratio breakdown");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail, 3,
                  "solver_snapshot: phase1 shadow next pivot fail");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_next_pivot_success, 5,
                  "solver_snapshot: phase1 shadow next pivot success");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_stabilized, 9,
                  "solver_snapshot: phase1 force-extreme followup stabilized");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_ratio_breakdown, 4,
                  "solver_snapshot: phase1 force-extreme followup ratio breakdown");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_failed_stabilize, 8,
                  "solver_snapshot: phase1 force-extreme followup failed stabilize");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_post_dir_skip_retry, 11,
                  "solver_snapshot: phase1 force-extreme post dir-skip retry");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue, 3,
                  "solver_snapshot: phase1 force-extreme post dir-skip dual rescue");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor, 2,
                  "solver_snapshot: phase1 force-extreme post dir-skip forced refactor");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_next_failed_stabilize, 10,
                  "solver_snapshot: phase1 force-extreme next failed-stabilize");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_next_ratio_breakdown, 6,
                  "solver_snapshot: phase1 force-extreme next ratio breakdown");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_next_pivot_fail, 1,
                  "solver_snapshot: phase1 force-extreme next pivot fail");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_next_pivot_success, 7,
                  "solver_snapshot: phase1 force-extreme next pivot success");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_samples, 12,
                  "solver_snapshot: phase1 force-extreme followup dir samples");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_bound_geometry, 3,
                  "solver_snapshot: phase1 force-extreme followup dir bound geometry");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_bound_flip, 1,
                  "solver_snapshot: phase1 force-extreme followup dir bound flip");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_tiny_theta, 2,
                  "solver_snapshot: phase1 force-extreme followup dir tiny theta");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_weak_leaving, 5,
                  "solver_snapshot: phase1 force-extreme followup dir weak leaving");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_ftran_shape, 4,
                  "solver_snapshot: phase1 force-extreme followup dir ftran shape");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_nnz_total, 155,
                  "solver_snapshot: phase1 force-extreme followup dir nnz total");
    ASSERT_INT_EQ(snap.perf_phase1_force_extreme_followup_dir_nnz_max, 44,
                  "solver_snapshot: phase1 force-extreme followup dir nnz max");
    ASSERT_DBL_EQ(snap.perf_phase1_force_extreme_followup_dir_inf_total, 830000.0,
                  "solver_snapshot: phase1 force-extreme followup dir inf total");
    ASSERT_DBL_EQ(snap.perf_phase1_force_extreme_followup_dir_inf_max, 410000.0,
                  "solver_snapshot: phase1 force-extreme followup dir inf max");
    ASSERT_DBL_EQ(snap.perf_phase1_force_extreme_followup_pivot_abs_total, 13.5,
                  "solver_snapshot: phase1 force-extreme followup pivot abs total");
    ASSERT_DBL_EQ(snap.perf_phase1_force_extreme_followup_pivot_abs_max, 6.75,
                  "solver_snapshot: phase1 force-extreme followup pivot abs max");
    ASSERT_DBL_EQ(snap.perf_phase1_force_extreme_followup_theta_total, 0.024,
                  "solver_snapshot: phase1 force-extreme followup theta total");
    ASSERT_DBL_EQ(snap.perf_phase1_force_extreme_followup_theta_max, 0.008,
                  "solver_snapshot: phase1 force-extreme followup theta max");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples, 6,
                  "solver_snapshot: phase1 shadow followup dir samples");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry, 2,
                  "solver_snapshot: phase1 shadow followup dir bound geometry");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip, 1,
                  "solver_snapshot: phase1 shadow followup dir bound flip");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta, 2,
                  "solver_snapshot: phase1 shadow followup dir tiny theta");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving, 3,
                  "solver_snapshot: phase1 shadow followup dir weak leaving");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape, 1,
                  "solver_snapshot: phase1 shadow followup dir ftran shape");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total, 81,
                  "solver_snapshot: phase1 shadow followup dir nnz total");
    ASSERT_INT_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max, 21,
                  "solver_snapshot: phase1 shadow followup dir nnz max");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total, 915000.0,
                  "solver_snapshot: phase1 shadow followup dir inf total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max, 410000.0,
                  "solver_snapshot: phase1 shadow followup dir inf max");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total, 8.75,
                  "solver_snapshot: phase1 shadow followup pivot abs total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max, 4.5,
                  "solver_snapshot: phase1 shadow followup pivot abs max");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_theta_total, 0.0065,
                  "solver_snapshot: phase1 shadow followup theta total");
    ASSERT_DBL_EQ(snap.perf_phase1_failed_stabilize_retry_shadow_followup_theta_max, 0.0030,
                  "solver_snapshot: phase1 shadow followup theta max");
    ASSERT_INT_EQ(snap.perf_dual_bound_flip_applied, 13,
                  "solver_snapshot: dual bound flips aggregate");
    ASSERT_INT_EQ(snap.perf_dual_bound_flip_startup, 5,
                  "solver_snapshot: dual bound flips startup");
    ASSERT_INT_EQ(snap.perf_dual_bound_flip_iterative, 8,
                  "solver_snapshot: dual bound flips iterative");
    ASSERT_INT_EQ(snap.reinvert_shadow_checks_phase1, 17,
                  "solver_snapshot: reinvert shadow checks phase1");
    ASSERT_INT_EQ(snap.reinvert_shadow_suggest_defer_phase1, 9,
                  "solver_snapshot: reinvert suggest defer phase1");
    ASSERT_INT_EQ(snap.reinvert_shadow_actual_refactor_yes_phase1, 4,
                  "solver_snapshot: reinvert actual yes phase1");
    ASSERT_INT_EQ(snap.reinvert_shadow_disagree_phase1, 2,
                  "solver_snapshot: reinvert disagree phase1");
    ASSERT_INT_EQ(snap.reinvert_shadow_last_reason_phase1, LP_REINVERT_REASON_COST_DAMPEN,
                  "solver_snapshot: reinvert last reason phase1");
    ASSERT_DBL_EQ(snap.periodic_feedback_hint_pressure_phase2, 0.55,
                  "solver_snapshot: feedback pressure phase2");
    ASSERT_INT_EQ(snap.soft_lu_cost_gate_enabled, 1,
                  "solver_snapshot: soft lu gate enabled");
    ASSERT_INT_EQ(snap.soft_lu_cost_gate_defers_phase1, 3,
                  "solver_snapshot: soft lu defers phase1");
    ASSERT_INT_EQ(snap.soft_lu_consecutive_defers_phase1, 2,
                  "solver_snapshot: soft lu consecutive defers phase1");
    ASSERT_INT_EQ(snap.soft_lu_defer_cap_forced_phase1, 1,
                  "solver_snapshot: soft lu cap forced phase1");
    ASSERT_DBL_EQ(snap.soft_lu_refactor_cost_ewma_phase2, 7.25,
                  "solver_snapshot: soft lu refactor ewma phase2");
    ASSERT_INT_EQ(snap.periodic_cost_gate_enabled, 1,
                  "solver_snapshot: periodic cost gate enabled");
    ASSERT_INT_EQ(snap.periodic_cost_gate_defers_phase1, 4,
                  "solver_snapshot: periodic cost defers phase1");
    ASSERT_INT_EQ(snap.periodic_cost_consecutive_defers_phase1, 1,
                  "solver_snapshot: periodic cost consecutive defers phase1");
    ASSERT_INT_EQ(snap.periodic_cost_defer_cap_forced_phase1, 2,
                  "solver_snapshot: periodic cost cap forced phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_checks_phase1, 7,
                  "solver_snapshot: periodic cost checks phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_block_ratio_phase1, 3,
                  "solver_snapshot: periodic cost ratio block phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_block_warmup_phase1, 1,
                  "solver_snapshot: periodic cost warmup block phase1");
    ASSERT_INT_EQ(snap.periodic_cost_gate_last_reason_phase1,
                  LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO,
                  "solver_snapshot: periodic cost last reason phase1");
    ASSERT_INT_EQ(snap.periodic_cost_iter_samples_phase1, 19,
                  "solver_snapshot: periodic cost iter samples phase1");
    ASSERT_INT_EQ(snap.periodic_cost_refactor_samples_phase1, 4,
                  "solver_snapshot: periodic cost refactor samples phase1");
    ASSERT_INT_EQ(snap.basis_governor_mode, LP_BASIS_GOV_MODE_SHADOW,
                  "solver_snapshot: basis governor mode");
    ASSERT_INT_EQ(snap.reinvert_controller_mode, LP_REINVERT_MODE_CONTROL_ALL,
                  "solver_snapshot: reinvert controller mode");
    ASSERT_INT_EQ(snap.reinvert_dual_control_demoted, 1,
                  "solver_snapshot: dual reinvert control demoted");
    ASSERT_INT_EQ(snap.reinvert_dual_control_demotions, 1,
                  "solver_snapshot: dual reinvert control demotions");
    ASSERT_INT_EQ(snap.reinvert_dual_hard_trigger_last_total, 13,
                  "solver_snapshot: dual reinvert hard-trigger last total");
    ASSERT_INT_EQ(snap.reinvert_dual_hard_trigger_last_iter, 101,
                  "solver_snapshot: dual reinvert hard-trigger last iter");
    ASSERT_INT_EQ(snap.reinvert_dual_hard_trigger_burst, 7,
                  "solver_snapshot: dual reinvert hard-trigger burst");
    ASSERT_INT_EQ(snap.reinvert_phase1_control_demoted, 1,
                  "solver_snapshot: phase1 reinvert control demoted");
    ASSERT_INT_EQ(snap.reinvert_phase1_control_demotions, 2,
                  "solver_snapshot: phase1 reinvert control demotions");
    ASSERT_INT_EQ(snap.reinvert_phase1_pressure_last_iter, 87,
                  "solver_snapshot: phase1 reinvert pressure last iter");
    ASSERT_INT_EQ(snap.reinvert_phase1_pressure_burst, 6,
                  "solver_snapshot: phase1 reinvert pressure burst");
    ASSERT_INT_EQ(snap.phase1_stagnation_escape_cooldown, 23,
                  "solver_snapshot: phase1 stagnation cooldown");
    ASSERT_INT_EQ(snap.phase1_stagnation_escape_triggers, 5,
                  "solver_snapshot: phase1 stagnation escape triggers");
    ASSERT_INT_EQ(snap.phase1_stagnation_escape_successes, 4,
                  "solver_snapshot: phase1 stagnation escape successes");
    ASSERT_INT_EQ(snap.phase1_stagnation_escape_failures, 1,
                  "solver_snapshot: phase1 stagnation escape failures");
    ASSERT_INT_EQ(snap.phase1_stagnation_escape_cooldown_blocks, 2,
                  "solver_snapshot: phase1 stagnation cooldown blocks");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_window_iters, 96,
                  "solver_snapshot: phase1 stagnation last window iters");
    ASSERT_DBL_EQ(snap.phase1_stagnation_last_obj_delta, 0.00012,
                  "solver_snapshot: phase1 stagnation obj delta");
    ASSERT_DBL_EQ(snap.phase1_stagnation_last_retry_defer_ratio, 0.85,
                  "solver_snapshot: phase1 stagnation retry ratio");
    ASSERT_DBL_EQ(snap.phase1_stagnation_last_update_recovery_ratio, 0.6,
                  "solver_snapshot: phase1 stagnation update ratio");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_retry_defers, 17,
                  "solver_snapshot: phase1 stagnation retry defers");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_no_pivot_events, 20,
                  "solver_snapshot: phase1 stagnation no-pivot events");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_update_recovery_refactors, 6,
                  "solver_snapshot: phase1 stagnation update refactors");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_refactors, 8,
                  "solver_snapshot: phase1 stagnation refactors");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_recompute_ratio, 9,
                  "solver_snapshot: phase1 stagnation recompute ratio");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_recompute_dir_skip, 7,
                  "solver_snapshot: phase1 stagnation recompute dir skip");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_recompute_dir_refactor, 2,
                  "solver_snapshot: phase1 stagnation recompute dir refactor");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_recompute_pivot_fail, 1,
                  "solver_snapshot: phase1 stagnation recompute pivot fail");
    ASSERT_INT_EQ(snap.phase1_stagnation_last_recompute_perturb, 0,
                  "solver_snapshot: phase1 stagnation recompute perturb");
    ASSERT_INT_EQ(snap.shadow_refactor_yes_phase1, 8,
                  "solver_snapshot: shadow refactor yes phase1");
    ASSERT_INT_EQ(snap.shadow_disagree_primal_refactor, 3,
                  "solver_snapshot: shadow disagree primal refactor");
}

static void test_phase2_degeneracy_helpers(void) {
    printf("  telemetry/solver: phase2 degeneracy helpers...\n");

    SimplexSolver solver;
    LPSolverTelemetrySnapshot snap;
    memset(&solver, 0, sizeof(solver));
    solver.telemetry_enabled = 1;

    lp_telemetry_record_phase2_pivot_geometry(&solver, 1e-10, 1000.0, 1e-7);
    lp_telemetry_record_phase2_pivot_geometry(&solver, 1e-5, 100.0, 1e-4);
    lp_telemetry_record_phase2_devex_reset(&solver, 17);
    lp_telemetry_record_phase2_devex_reset(&solver, 9);
    lp_telemetry_record_phase2_degenerate_refactor(
        &solver, RALPH_REFACTOR_REASON_PERIODIC, 1, 0);
    lp_telemetry_record_phase2_degenerate_refactor(
        &solver, RALPH_REFACTOR_REASON_PIVOT_RECOVERY, 0, 1);

    solver.telemetry.perf_phase2_repeat_entering_events = 5;
    solver.telemetry.perf_phase2_repeat_entering_max_streak = 3;
    solver.telemetry.perf_phase2_repeat_leaving_events = 4;
    solver.telemetry.perf_phase2_repeat_leaving_max_streak = 2;
    solver.telemetry.perf_phase2_bland_pricing_iters = 7;
    solver.telemetry.perf_phase2_adaptive_devex_partial_iters = 11;
    solver.telemetry.perf_phase2_bland_enter_episodes = 2;
    solver.telemetry.perf_phase2_bland_exit_episodes = 1;
    solver.telemetry.perf_phase2_perturb_applied = 3;
    solver.telemetry.perf_phase2_degenerate_episodes = 4;
    solver.telemetry.perf_phase2_degenerate_streak_max = 12;
    solver.telemetry.perf_phase2_degen_escape_triggers = 2;

    ASSERT_INT_EQ(solver.telemetry.perf_phase2_theta_le_1e_9, 1,
                  "phase2 helpers: theta <= 1e-9");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_theta_le_1e_6, 0,
                  "phase2 helpers: theta <= 1e-6");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_theta_le_1e_3, 1,
                  "phase2 helpers: theta <= 1e-3");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_weak_pivot_samples, 2,
                  "phase2 helpers: weak pivot samples");
    ASSERT_DBL_EQ(solver.telemetry.perf_phase2_weak_pivot_ratio_min, 1e-10,
                  "phase2 helpers: weak pivot ratio min");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_weak_pivot_ratio_le_1e_8, 1,
                  "phase2 helpers: weak pivot ratio <= 1e-8");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_weak_pivot_ratio_le_1e_6, 1,
                  "phase2 helpers: weak pivot ratio <= 1e-6");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_devex_reset_count, 2,
                  "phase2 helpers: devex reset count");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_devex_age_max, 17,
                  "phase2 helpers: devex age max");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_degen_refactor_calls, 2,
                  "phase2 helpers: degenerate refactor calls");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_degen_refactor_periodic_lu_health, 1,
                  "phase2 helpers: degenerate periodic lu-health refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_degen_refactor_pivot_recovery, 1,
                  "phase2 helpers: degenerate pivot recovery refactor");
    ASSERT_INT_EQ(solver.telemetry.perf_phase2_degen_refactor_safety_forced, 1,
                  "phase2 helpers: degenerate safety-forced refactor");

    lp_telemetry_snapshot_solver(&solver, &snap);
    ASSERT_INT_EQ(snap.perf_phase2_repeat_entering_events, 5,
                  "phase2 snapshot: repeat entering events");
    ASSERT_INT_EQ(snap.perf_phase2_repeat_leaving_max_streak, 2,
                  "phase2 snapshot: repeat leaving max streak");
    ASSERT_INT_EQ(snap.perf_phase2_bland_pricing_iters, 7,
                  "phase2 snapshot: bland pricing iterations");
    ASSERT_INT_EQ(snap.perf_phase2_adaptive_devex_partial_iters, 11,
                  "phase2 snapshot: adaptive devex partial iterations");
    ASSERT_INT_EQ(snap.perf_phase2_degenerate_episodes, 4,
                  "phase2 snapshot: degenerate episodes");
    ASSERT_INT_EQ(snap.perf_phase2_degenerate_streak_max, 12,
                  "phase2 snapshot: degenerate streak max");
    ASSERT_INT_EQ(snap.perf_phase2_perturb_applied, 3,
                  "phase2 snapshot: perturb applied");
    ASSERT_INT_EQ(snap.perf_phase2_degen_escape_triggers, 2,
                  "phase2 snapshot: degen escape triggers");
}

int main(void) {
    printf("=== LP Telemetry Solver Tests ===\n");

    test_solver_reset_and_refactor_accounting();
    test_refactor_reason_classifier();
    test_solver_snapshot();
    test_phase2_degeneracy_helpers();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
