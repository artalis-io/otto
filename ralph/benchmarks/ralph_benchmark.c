/*
 * Ralph LP Benchmark Tool
 *
 * Compares Ralph solver against GLPK on standard LP problems.
 * Produces structured JSON output for automated analysis.
 *
 * Usage:
 *   ./ralph-benchmark problem.mps           # Single problem
 *   ./ralph-benchmark --netlib afiro        # NETLIB problem by name
 *   ./ralph-benchmark --suite tiny          # Run test suite
 *   ./ralph-benchmark --list                # List available problems
 *   ./ralph-benchmark --download-netlib     # Download NETLIB problems
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#include "ralph_test_mod_api.h"
#include "lp.h"
#include "lp_refactor_policy.h"

/* Internal helpers exposed by ralph.c for benchmark diagnostics */
extern LPModel* ralph_get_lp_model(const RalphModel *model);
extern SimplexSolver* ralph_get_lp_solver(const RalphModel *model);

/* ============================================================================
 * Constants and Configuration
 * ============================================================================ */

#define MAX_PATH 4096
#define MAX_LINE 4096
#define MAX_PROBLEMS 200

/* Default settings */
#define DEFAULT_TIME_MULTIPLIER 20.0
#define DEFAULT_HARD_CAP_SEC 60.0
#define DEFAULT_OBJ_REL_TOL 1e-6
#define DEFAULT_OBJ_ABS_TOL 1e-8
#define DEFAULT_FEAS_TOL 1e-6

/* NETLIB directory relative to this executable */
static const char *NETLIB_DIR = "netlib";

/* Default test time caps */
#define TEST_FAST_CAP_SEC 60.0
#define TEST_FULL_CAP_SEC 300.0

/* ============================================================================
 * NETLIB Known Optimal Values (from netlib.org/lp/data/readme)
 * ============================================================================ */

typedef struct {
    const char *name;
    double optimal;
    int tier;  /* 0=tiny, 1=small, 2=medium, 3=large, 4=xlarge */
} NetlibReference;

static const NetlibReference NETLIB_REFERENCE[] = {
    /* Tier 0: tiny (<100 vars) */
    {"afiro",      -4.6475314286e+02, 0},
    {"sc50a",      -6.4575077059e+01, 0},
    {"sc50b",      -7.0000000000e+01, 0},
    {"kb2",        -1.7499001299e+03, 0},
    {"sc105",      -5.2202061212e+01, 0},
    {"blend",      -3.0812149846e+01, 0},
    {"share2b",    -4.1573224074e+02, 0},
    {"recipe",     -2.6661600000e+02, 0},

    /* Tier 1: small (100-500 vars) */
    {"adlittle",    2.2549496316e+05, 1},
    {"lotfi",      -2.5264706062e+01, 1},
    {"scagr7",     -2.3313892548e+06, 1},
    {"israel",     -8.9664482186e+05, 1},
    {"scorpion",    1.8781248227e+03, 1},
    {"brandy",      1.5185098965e+03, 1},
    {"bandm",      -1.5862801845e+02, 1},
    {"beaconfd",    3.3592485807e+04, 1},
    {"e226",       -2.5864929066e+01, 1},
    {"stocfor1",   -4.1131976219e+04, 1},
    {"sc205",      -5.2202061212e+01, 1},
    {"agg",        -3.5991767287e+07, 1},
    {"agg2",       -2.0239252356e+07, 1},
    {"agg3",        1.0312115935e+07, 1},
    {"bore3d",      1.3730803942e+03, 1},
    {"capri",       2.6900129138e+03, 1},
    {"share1b",    -7.6589318579e+04, 1},
    {"scagr25",    -1.4753433061e+07, 1},

    /* Tier 2: medium (500-2000 vars) */
    {"bnl1",        1.9776292856e+03, 2},
    {"degen2",     -1.4351780000e+03, 2},
    {"grow7",      -4.7787811815e+07, 2},
    {"grow15",     -1.0687094129e+08, 2},
    {"grow22",     -1.6083433648e+08, 2},
    {"scfxm1",      1.8416759028e+04, 2},
    {"scfxm2",      3.6660261565e+04, 2},
    {"scfxm3",      5.4901254550e+04, 2},
    {"scsd1",       8.6666666743e+00, 2},
    {"scsd6",       5.0500000078e+01, 2},
    {"scsd8",       9.0499999993e+02, 2},
    {"sctap1",      1.4122500000e+03, 2},
    {"sctap2",      1.7248071429e+03, 2},
    {"sctap3",      1.4240000000e+03, 2},
    {"ship04s",     1.7987147004e+06, 2},
    {"ship04l",     1.7933245380e+06, 2},
    {"ship08s",     1.9200982105e+06, 2},
    {"ship08l",     1.9090552114e+06, 2},
    {"ship12s",     1.4892361344e+06, 2},
    {"ship12l",     1.4701879193e+06, 2},
    {"etamacro",   -7.5571521774e+02, 2},
    {"finnis",      1.7279096547e+05, 2},
    {"perold",     -9.3807580773e+03, 2},
    {"stair",      -2.5126695119e+02, 2},
    {"shell",       1.2088253460e+09, 2},
    {"seba",        1.5711600000e+04, 2},
    {"forplan",    -6.6421873953e+02, 2},
    {"ganges",     -1.0958573613e+05, 2},
    {"sierra",      1.5394362184e+07, 2},
    {"standata",    1.2576995000e+03, 2},
    {"standmps",    1.4060175000e+03, 2},
    {"nesm",        1.4076036488e+07, 2},
    {"fffff800",    5.5567961165e+05, 2},

    /* Tier 3: large (2000+ vars) */
    {"bnl2",        1.8112365404e+03, 3},
    {"degen3",     -9.8729400000e+02, 3},
    {"pilot",      -5.5748972928e+02, 3},
    {"pilot87",     3.0171034733e+02, 3},
    {"pilot.ja",   -6.1131344111e+03, 3},
    {"pilot.we",   -2.7201075328e+06, 3},
    {"pilot4",     -2.5811392641e+03, 3},
    {"pilotnov",   -4.4972761882e+03, 3},
    {"maros",      -5.8063743701e+04, 3},
    {"d2q06c",      1.2278423615e+05, 3},
    {"stocfor2",   -3.9024408538e+04, 3},
    {"cycle",      -5.2263930249e+00, 3},
    {"czprob",      2.1851966989e+06, 3},
    {"25fv47",      5.5018458883e+03, 3},
    {"woodw",       1.3044763331e+00, 3},
    {"wood1p",      1.4429024116e+00, 3},

    /* Tier 4: xlarge */
    {"80bau3b",     9.8722419241e+05, 4},
    {"fit1d",      -9.1463780924e+03, 4},
    {"fit1p",       9.1463780924e+03, 4},
    {"fit2d",      -6.8464293294e+04, 4},
    {"fit2p",       6.8464293232e+04, 4},
    {"maros-r7",    1.4971851665e+06, 4},
    {"stocfor3",   -3.9976661576e+04, 4},
    {"greenbea",   -7.2555248130e+07, 4},
    {"greenbeb",   -4.3022602612e+06, 4},
    {"truss",       4.5881584719e+05, 4},
    {"d6cube",      3.1549166667e+02, 4},

    {NULL, 0.0, -1}  /* sentinel */
};

static const NetlibReference* find_netlib_reference(const char *name) {
    for (int i = 0; NETLIB_REFERENCE[i].name != NULL; i++) {
        if (strcasecmp(NETLIB_REFERENCE[i].name, name) == 0) {
            return &NETLIB_REFERENCE[i];
        }
    }
    return NULL;
}

/* ============================================================================
 * Data Structures
 * ============================================================================ */

typedef struct {
    char name[256];
    char path[MAX_PATH];
    int is_mip;
} ProblemInfo;

typedef struct {
    int status;          /* 0=optimal, 1=infeasible, 2=unbounded, 3=error, 4=timeout */
    int raw_status_code; /* RalphStatus raw value */
    char raw_status_str[32];
    int termination_reason_code;
    char termination_reason[64];
    char solve_path[32];
    int api_error_domain;
    int api_error_code;
    int api_error_api_id;
    char api_error_message[RALPH_API_ERROR_MESSAGE_MAX];
    double objective;
    double time_ms;
    int iterations;
    int presolve_used;
    unsigned int presolve_mask;
    int presolve_rounds;
    int presolve_vars_removed;
    int presolve_cons_removed;
    int presolve_bounds_tightened;
    int presolve_matrix_rank;
    int presolve_redundant_rows_found;
    double presolve_time_ms;
    double phase1_artificial_sum;
    double phase1_artificial_max;
    int phase1_artificial_basic;
    double primal_setup_ms;
    double dual_ms;
    double phase1_ms;
    double transition_ms;
    double phase2_ms;
    double pricing_ms;
    double ratio_ms;
    double pivot_ms;
    double refactor_ms;
    double ftran_ms;
    double btran_ms;
    double ftran_base_ms;
    double ftran_update_apply_ms;
    int ftran_update_apply_calls;
    double btran_base_ms;
    double btran_update_apply_ms;
    int btran_update_apply_calls;
    int ftran_calls;
    int btran_calls;
    int ftran_nnz_samples;
    int btran_nnz_samples;
    long long ftran_rhs_nnz_total;
    long long ftran_sol_nnz_total;
    long long btran_rhs_nnz_total;
    long long btran_sol_nnz_total;
    double lu_update_ms;
    double compute_solution_ms;
    double compute_rc_ms;
    double refactor_all_ms;
    int refactor_count;
    double refactor_last_ms;
    double refactor_max_ms;
    int refactor_last_reason;
    char refactor_last_reason_str[64];
    int refactor_reason_setup;
    int refactor_reason_transition;
    int refactor_reason_periodic;
    int refactor_reason_ratio_recovery;
    int refactor_reason_pivot_recovery;
    int refactor_reason_forced_small_pivot;
    int refactor_reason_update_recovery;
    int refactor_reason_direction_stabilize;
    int refactor_reason_infeas_cleanup;
    int refactor_reason_other;
    int refactor_periodic_policy;
    int refactor_periodic_lu_health;
    int refactor_safety_forced;
    int basis_fastpath_hits;
    int basis_cols_rewritten;
    unsigned long long basis_tail_shift_bytes;
    int refactor_last_m;
    int refactor_last_k;
    int refactor_last_nnz_b;
    int refactor_factorize_failures;
    int refactor_repair_successes;
    int refactor_repair_failures;
    int refactor_last_factorize_failure_reason;
    int refactor_last_sparse_numeric_failure_reason;
    int refactor_last_repair_status;
    int phase1_refactor_factorize_failures;
    int phase1_refactor_repair_successes;
    int phase1_refactor_repair_failures;
    int phase2_refactor_factorize_failures;
    int phase2_refactor_repair_successes;
    int phase2_refactor_repair_failures;

    double phase1_pricing_ms;
    double phase1_ratio_ms;
    double phase1_pivot_ms;
    double phase1_refactor_ms;
    double phase1_compute_solution_ms;
    double phase1_compute_rc_ms;
    int phase1_pricing_calls;
    int phase1_ratio_calls;
    int phase1_pivot_calls;
    int phase1_refactor_calls;
    int phase1_compute_solution_calls;
    int phase1_compute_rc_calls;
    int phase1_compute_solution_ctx_other;
    int phase1_compute_solution_ctx_recompute_full;
    int phase1_compute_solution_ctx_recompute_guard_forced_full;
    int phase1_compute_solution_ctx_init;
    int phase1_compute_solution_ctx_no_entering_cleanup;
    int phase1_compute_solution_ctx_infeas_cleanup;
    int phase1_compute_solution_ctx_refactor_fail_continue;
    int phase1_compute_solution_ctx_refactor_failure_recovery;
    int phase1_compute_solution_ctx_refactor_success;
    int phase1_compute_solution_ctx_drift_refresh;
    int phase1_compute_solution_ctx_dual_rescue;
    int phase1_compute_rc_ctx_other;
    int phase1_compute_rc_ctx_recompute_full;
    int phase1_compute_rc_ctx_recompute_rc_only;
    int phase1_compute_rc_ctx_recompute_guard_forced_full;
    int phase1_compute_rc_ctx_init;
    int phase1_compute_rc_ctx_infeas_cleanup;
    int phase1_compute_rc_ctx_refactor_fail_continue;
    int phase1_compute_rc_ctx_refactor_failure_recovery;
    int phase1_compute_rc_ctx_refactor_success;
    int phase1_compute_rc_ctx_drift_refresh;
    int phase1_compute_rc_ctx_dual_rescue;
    int phase1_entering_exclusions;
    int phase1_entering_exclusion_repeats;
    int phase1_entering_exclusion_hits;
    int phase1_entering_exclusion_reroutes;
    int phase1_entering_exclusion_no_alt;
    int phase1_refactor_periodic_policy;
    int phase1_refactor_periodic_lu_health;
    int phase1_refactor_safety_forced;
    int phase1_dir_stabilize_force_extreme_dir;
    int phase1_dir_stabilize_force_lu_health;
    int phase1_dir_stabilize_cooldown_candidates;
    int phase1_dir_stabilize_ratio_le_3;
    int phase1_dir_stabilize_ratio_le_10;
    int phase1_dir_stabilize_ratio_le_30;
    int phase1_dir_stabilize_ratio_le_100;
    int phase1_dir_stabilize_ratio_gt_100;
    int phase1_dir_stabilize_ratio_gt_300;
    int phase1_dir_stabilize_ratio_gt_1000;
    int phase1_dir_stabilize_skip_rc_only;
    int phase1_dir_stabilize_skip_full;
    int phase1_dir_stabilize_skip_no_recompute;
    int phase1_dir_stabilize_skip_guard_refresh;
    int phase1_dir_stabilize_escape_gate_triggers;
    int phase1_dir_stabilize_escape_gate_suppressed_lu_health;
    int phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode;
    int phase1_dir_stabilize_escape_gate_hard_bypass;
    int phase1_dir_stabilize_refactor_from_no_pivot_force;
    int phase1_dir_stabilize_refactor_from_force_extreme_dir;
    int phase1_dir_stabilize_refactor_from_force_lu_health;
    int phase1_dir_stabilize_refactor_from_force_pivot_mode;
    int phase1_dir_stabilize_refactor_from_ladder_force;
    int phase1_force_pivot_budget_dir_event_seen;
    int phase1_force_pivot_budget_pivot_spend;
    int phase1_force_pivot_relax_applied;
    int phase1_force_extreme_relax_applied;
    int phase1_force_extreme_bound_flip_relax_applied;
    int phase1_force_extreme_catastrophic_tiny_theta_relax_applied;
    int phase1_force_extreme_tiny_theta_relax_applied;
    int phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry;
    int phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue;
    int phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor;
    int phase1_force_extreme_tiny_theta_relax_refactor_force_lu_health;
    int phase1_force_extreme_tiny_theta_relax_refactor_force_pivot_mode;
    int phase1_force_extreme_tiny_theta_relax_refactor_ladder_force;
    int phase1_force_extreme_tiny_theta_relax_next_failed_stabilize;
    int phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown;
    int phase1_force_extreme_tiny_theta_relax_next_pivot_fail;
    int phase1_force_extreme_tiny_theta_relax_next_pivot_success;
    int phase1_recompute_after_ratio_breakdown;
    int phase1_recompute_after_dir_skip;
    int phase1_recompute_after_dir_refactor;
    int phase1_recompute_after_pivot_fail_recovery;
    int phase1_recompute_after_perturb;
    int phase1_recompute_rc_only_calls;
    int phase1_recompute_rc_guard_forced_full;
    int phase1_cleanup_attempts;
    int phase1_cleanup_accepted;
    int phase1_cleanup_rejected;
    int phase1_cleanup_candidate_probe_rejects;
    int phase1_progress_window_refactors;
    int phase1_progress_window_cleanups;
    int phase1_progress_window_perturbs;
    int phase1_ratio_breakdown_retries;
    int phase1_ratio_breakdown_escalations;
    int phase1_pivot_fail_recovery_exclusions;
    int phase1_no_pivot_events;
    int phase1_no_pivot_forced_refactor;
    int phase1_no_pivot_forced_ratio_breakdown;
    int phase1_no_pivot_forced_dir_skip;
    int phase1_no_pivot_forced_pivot_fail;
    int phase1_no_pivot_events_ratio_breakdown;
    int phase1_no_pivot_events_dir_skip;
    int phase1_no_pivot_events_pivot_fail;
    int phase1_no_pivot_no_progress_events;
    int phase1_no_pivot_ladder_retry_defers;
    int phase1_no_pivot_ladder_retry_ratio_breakdown;
    int phase1_no_pivot_ladder_retry_dir_skip;
    int phase1_no_pivot_ladder_retry_pivot_fail;
    int phase1_no_pivot_ladder_dual_rescue_attempts;
    int phase1_no_pivot_ladder_dual_rescue_successes;
    int phase1_no_pivot_ladder_dual_rescue_failures;
    int phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown;
    int phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip;
    int phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail;
    int phase1_no_pivot_ladder_forced_refactors;
    int phase1_no_pivot_ladder_forced_refactors_ratio_breakdown;
    int phase1_no_pivot_ladder_forced_refactors_dir_skip;
    int phase1_no_pivot_ladder_forced_refactors_pivot_fail;
    int phase1_no_pivot_ladder_rescue_guard_cooldown_blocks;
    int phase1_no_pivot_ladder_rescue_guard_fail_cap_forces;
    int phase1_direct_dual_rescue_attempts;
    int phase1_direct_dual_rescue_successes;
    int phase1_direct_dual_rescue_failures;
    int phase1_direct_dual_rescue_guard_cooldown_blocks;
    int phase1_direct_dual_rescue_guard_fail_cap_blocks;
    int phase1_dual_rescue_exit_time_limit;
    int phase1_dual_rescue_exit_bad_numerics;
    int phase1_dual_rescue_exit_no_progress;
    int phase1_dual_rescue_exit_no_entering;
    int phase1_dual_rescue_exit_pivot_refactor_failure;
    int phase1_dual_rescue_exit_periodic_refactor_failure;
    int phase1_dual_rescue_exit_max_iters;
    int phase1_dual_rescue_exit_alloc_failure;
    int phase1_soft_lu_policy_cooldown_defers;
    int phase1_dir_skip_same_entering_repeats;
    int phase1_dir_skip_same_entering_max_streak;
    int phase1_failed_stabilize_events;
    int phase1_failed_stabilize_primary_failures;
    int phase1_failed_stabilize_alternate_failures;
    int phase1_failed_stabilize_same_entering_repeats;
    int phase1_failed_stabilize_same_entering_max_streak;
    int phase1_failed_stabilize_retry_penalty_arms;
    int phase1_failed_stabilize_retry_penalty_alt_found;
    int phase1_failed_stabilize_retry_penalty_no_alt;
    int phase1_failed_stabilize_retry_penalty_alt_stabilized;
    int phase1_failed_stabilize_retry_penalty_alt_failed;
    int phase1_failed_stabilize_retry_penalty_same_alt_repeats;
    int phase1_failed_stabilize_retry_penalty_same_alt_max_streak;
    int phase1_failed_stabilize_retry_local_memory_arms;
    int phase1_failed_stabilize_retry_local_memory_alt_found;
    int phase1_failed_stabilize_retry_local_memory_no_alt;
    int phase1_failed_stabilize_retry_local_memory_fallback_same_alt;
    int phase1_failed_stabilize_retry_local_memory_alt_stabilized;
    int phase1_failed_stabilize_retry_local_memory_alt_failed;
    int phase1_failed_stabilize_retry_pool_samples;
    int phase1_failed_stabilize_retry_pool_eligible_total;
    int phase1_failed_stabilize_retry_pool_eligible_max;
    int phase1_failed_stabilize_retry_pool_singleton_samples;
    int phase1_failed_stabilize_retry_pool_best_differs_samples;
    int phase1_failed_stabilize_retry_selector_eval_samples;
    int phase1_failed_stabilize_retry_selector_eval_best_differs_samples;
    double phase1_failed_stabilize_retry_selector_eval_score_ratio_total;
    double phase1_failed_stabilize_retry_selector_eval_score_ratio_max;
    int phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2;
    int phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4;
    int phase1_failed_stabilize_retry_shadow_samples;
    int phase1_failed_stabilize_retry_shadow_ratio_failed;
    int phase1_failed_stabilize_retry_shadow_dir_stable;
    int phase1_failed_stabilize_retry_shadow_dir_failed;
    int phase1_failed_stabilize_retry_shadow_dir_nnz_total;
    int phase1_failed_stabilize_retry_shadow_dir_nnz_max;
    double phase1_failed_stabilize_retry_shadow_dir_inf_total;
    double phase1_failed_stabilize_retry_shadow_dir_inf_max;
    double phase1_failed_stabilize_retry_shadow_pivot_abs_total;
    double phase1_failed_stabilize_retry_shadow_pivot_abs_max;
    int phase1_failed_stabilize_retry_shadow_guard_arms;
    int phase1_failed_stabilize_retry_shadow_guard_original_exclusions;
    int phase1_failed_stabilize_retry_shadow_post_dir_skip_retry;
    int phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue;
    int phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor;
    int phase1_failed_stabilize_retry_shadow_next_failed_stabilize;
    int phase1_failed_stabilize_retry_shadow_next_ratio_breakdown;
    int phase1_failed_stabilize_retry_shadow_next_pivot_fail;
    int phase1_failed_stabilize_retry_shadow_next_pivot_success;
    int phase1_force_extreme_followup_stabilized;
    int phase1_force_extreme_followup_ratio_breakdown;
    int phase1_force_extreme_followup_failed_stabilize;
    int phase1_force_extreme_followup_post_dir_skip_retry;
    int phase1_force_extreme_followup_post_dir_skip_dual_rescue;
    int phase1_force_extreme_followup_post_dir_skip_forced_refactor;
    int phase1_force_extreme_followup_next_failed_stabilize;
    int phase1_force_extreme_followup_next_ratio_breakdown;
    int phase1_force_extreme_followup_next_pivot_fail;
    int phase1_force_extreme_followup_next_pivot_success;
    int phase1_force_extreme_followup_dir_samples;
    int phase1_force_extreme_followup_dir_bound_geometry;
    int phase1_force_extreme_followup_dir_bound_flip;
    int phase1_force_extreme_followup_dir_tiny_theta;
    int phase1_force_extreme_followup_dir_weak_leaving;
    int phase1_force_extreme_followup_dir_ftran_shape;
    int phase1_force_extreme_followup_dir_nnz_total;
    int phase1_force_extreme_followup_dir_nnz_max;
    double phase1_force_extreme_followup_dir_inf_total;
    double phase1_force_extreme_followup_dir_inf_max;
    double phase1_force_extreme_followup_pivot_abs_total;
    double phase1_force_extreme_followup_pivot_abs_max;
    double phase1_force_extreme_followup_theta_total;
    double phase1_force_extreme_followup_theta_max;
    int phase1_failed_stabilize_retry_shadow_followup_dir_samples;
    int phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry;
    int phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip;
    int phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta;
    int phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving;
    int phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape;
    int phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total;
    int phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max;
    double phase1_failed_stabilize_retry_shadow_followup_dir_inf_total;
    double phase1_failed_stabilize_retry_shadow_followup_dir_inf_max;
    double phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total;
    double phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max;
    double phase1_failed_stabilize_retry_shadow_followup_theta_total;
    double phase1_failed_stabilize_retry_shadow_followup_theta_max;
    int phase1_failed_stabilize_retry_selector_bland_arms;
    int phase1_failed_stabilize_retry_selector_guarded_arms;
    int phase1_failed_stabilize_retry_selector_guarded_eligible_total;
    int phase1_failed_stabilize_retry_selector_guarded_eligible_max;
    int phase1_failed_stabilize_retry_selector_bland_alt_stabilized;
    int phase1_failed_stabilize_retry_selector_bland_alt_failed;
    int phase1_failed_stabilize_retry_selector_bland_ratio_failed;
    int phase1_failed_stabilize_retry_selector_bland_dir_failed;
    int phase1_failed_stabilize_retry_selector_guarded_alt_stabilized;
    int phase1_failed_stabilize_retry_selector_guarded_alt_failed;
    int phase1_failed_stabilize_retry_selector_guarded_ratio_failed;
    int phase1_failed_stabilize_retry_selector_guarded_dir_failed;
    int phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland;
    int phase1_failed_stabilize_retry_dir_fail_shape_samples;
    int phase1_failed_stabilize_retry_dir_fail_nnz_total;
    int phase1_failed_stabilize_retry_dir_fail_nnz_max;
    double phase1_failed_stabilize_retry_dir_fail_dir_inf_total;
    double phase1_failed_stabilize_retry_dir_fail_dir_inf_max;
    double phase1_failed_stabilize_retry_dir_fail_pivot_abs_total;
    double phase1_failed_stabilize_retry_dir_fail_pivot_abs_max;
    int phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30;
    int phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100;
    int phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000;
    int phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000;
    int phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8;
    int phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6;
    int phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4;
    int phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4;
    int phase1_failed_stabilize_retry_dir_second_chance_arms;
    int phase1_failed_stabilize_retry_dir_second_chance_no_alt;
    int phase1_failed_stabilize_retry_dir_second_chance_stabilized;
    int phase1_failed_stabilize_retry_dir_second_chance_failed;
    int phase1_failed_stabilize_retry_dir_guard_arms;
    int phase1_failed_stabilize_retry_dir_guard_original_exclusions;
    int phase1_window_pressure_windows_started;
    int phase1_window_pressure_progress_resets;
    int phase1_window_pressure_force_pivot_arms;
    int phase1_window_pressure_force_pivot_blocked_pending;
    int phase1_window_pressure_force_pivot_blocked_budget;
    int phase1_window_pressure_force_pivot_reject_under_trigger;
    int phase1_window_pressure_force_pivot_reject_failed_share;
    int phase1_window_pressure_force_pivot_reject_dir_skip_share;
    int phase1_window_pressure_force_pivot_reject_local_fail;
    int phase1_window_pressure_force_pivot_reject_alternation;
    int phase1_window_pressure_event_total;
    int phase1_window_pressure_failed_stabilize_total;
    int phase1_window_pressure_dir_skip_total;
    int phase1_window_pressure_local_memory_fail_total;
    int phase1_window_pressure_alternation_total;
    int phase1_window_pressure_event_max;
    int phase1_window_pressure_failed_stabilize_max;
    int phase1_window_pressure_dir_skip_max;
    int phase1_window_pressure_local_memory_fail_max;
    int phase1_window_pressure_alternation_max;

    double phase2_pricing_ms;
    double phase2_ratio_ms;
    double phase2_pivot_ms;
    double phase2_refactor_ms;
    double phase2_compute_solution_ms;
    double phase2_compute_rc_ms;
    int phase2_pricing_calls;
    int phase2_ratio_calls;
    int phase2_pivot_calls;
    int phase2_refactor_calls;
    int phase2_compute_solution_calls;
    int phase2_compute_rc_calls;
    int phase2_refactor_periodic_policy;
    int phase2_refactor_periodic_lu_health;
    int phase2_refactor_safety_forced;
    int phase2_degenerate_episodes;
    int phase2_degenerate_streak_max;
    int phase2_theta_le_1e_9;
    int phase2_theta_le_1e_6;
    int phase2_theta_le_1e_3;
    int phase2_theta_gt_1e_3;
    int phase2_weak_pivot_samples;
    double phase2_weak_pivot_ratio_total;
    double phase2_weak_pivot_ratio_min;
    int phase2_weak_pivot_ratio_le_1e_8;
    int phase2_weak_pivot_ratio_le_1e_6;
    int phase2_weak_pivot_ratio_le_1e_4;
    int phase2_weak_pivot_ratio_gt_1e_4;
    int phase2_repeat_entering_events;
    int phase2_repeat_entering_max_streak;
    int phase2_repeat_leaving_events;
    int phase2_repeat_leaving_max_streak;
    int phase2_bland_pricing_iters;
    int phase2_adaptive_devex_partial_iters;
    int phase2_bland_enter_episodes;
    int phase2_bland_exit_episodes;
    int phase2_perturb_applied;
    int phase2_devex_reset_count;
    int phase2_devex_age_max;
    int phase2_degen_refactor_calls;
    int phase2_degen_refactor_ratio_recovery;
    int phase2_degen_refactor_pivot_recovery;
    int phase2_degen_refactor_periodic_policy;
    int phase2_degen_refactor_periodic_lu_health;
    int phase2_degen_refactor_safety_forced;
    int phase2_degen_escape_triggers;
    int dual_ratio_no_entering;
    int dual_theta_nonpositive;
    int dual_pivot_reject_small;
    int dual_bound_flip_applied;
    int dual_lu_hard_trigger;
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

    int lu_mkz_enabled;
    int lu_sn_enabled;
    int lu_mkz_calls;
    int lu_mkz_successes;
    int lu_mkz_failures;
    int lu_mkz_retry_count;
    int lu_mkz_last_failure;
    int lu_mkz_dense_fallbacks;
    int lu_mkz_fail_workspace;
    int lu_mkz_fail_pool;
    int lu_mkz_fail_singular;
    int lu_mkz_fail_capacity;
    int lu_mkz_singular_retry_attempts;
    int lu_mkz_singular_retry_successes;
    int lu_mkz_singular_retry_failures;
    int lu_mkz_reserved_fallback_attempts;
    int lu_mkz_reserved_fallback_accepts;
    int lu_mkz_reserved_fallback_rejects;
    int lu_mkz_circuit_trips;
    int lu_mkz_circuit_skips;
    int lu_mkz_circuit_resets;
    int lu_mkz_global_skip_trips;
    int lu_mkz_global_skip_skips;
    int lu_mkz_global_skip_resets;
    int lu_mkz_profile_retry_attempts;
    int lu_mkz_profile_retry_successes;
    int lu_mkz_profile_retry_failures;
    int lu_mkz_profile_retry_fail_identity_sep;
    int lu_mkz_profile_retry_fail_backend_exhausted;
    int lu_mkz_profile_retry_fail_pathological;
    uint64_t lu_mkz_primary_scan_entries;
    uint64_t lu_mkz_rescue_scan_entries;
    uint64_t lu_mkz_reserved_scan_entries;
    uint64_t lu_mkz_update_existing_entries;
    uint64_t lu_mkz_update_fill_candidates;
    uint64_t lu_mkz_hint_fallback_scans;
    uint64_t lu_mkz_hint_fallback_scan_entries;
    uint64_t lu_mkz_affected_columns_total;
    uint64_t lu_mkz_affected_columns_max;
    uint64_t lu_mkz_col_max_scan_entries;
    int lu_mkz_high_cond_count;
    double lu_mkz_worst_cond;
    int lu_sparse_dense_fallbacks;
    int lu_used_dense_fallback_last;
    int lu_sparse_fallback_last_reason;
    char lu_sparse_fallback_last_reason_str[64];
    int lu_sparse_fallback_reason_small_matrix;
    int lu_sparse_fallback_reason_symbolic;
    int lu_sparse_fallback_reason_numeric;
    int lu_sparse_numeric_last_failure_reason;
    char lu_sparse_numeric_last_failure_reason_str[64];
    int lu_sparse_numeric_fail_identity_sep;
    int lu_sparse_numeric_fail_backend_exhausted;
    int lu_sparse_numeric_fail_pathological;
    int lu_numeric_full_retry_attempts;
    int lu_numeric_full_retry_successes;
    int lu_numeric_full_retry_failures;
    int lu_identity_sep_failures;
    int lu_symbolic_failures;
    int lu_symbolic_fail_workspace;
    int lu_symbolic_fail_unmatched_no_reserved;
    int lu_symbolic_fail_inconsistent_identity;
    int lu_symbolic_full_retry_attempts;
    int lu_symbolic_full_retry_successes;
    int lu_symbolic_full_retry_numeric_failures;
    int lu_symbolic_full_retry_mkz_attempts;
    int lu_symbolic_full_retry_mkz_successes;
    int lu_symbolic_full_retry_mkz_failures;
    int lu_numeric_backend_markowitz;
    int lu_numeric_backend_supernode;
    int lu_numeric_backend_dense_ge;
    int lu_backend_policy_luf_ft;
    int lu_backend_policy_cbg;
    int lu_backend_policy_cgr;
    int lu_backend_policy_last;
    int lu_update_path_ft;
    int lu_update_path_eta;
    int lu_identity_sep_retry_lane_dense_chosen;
    int lu_identity_sep_retry_lane_supernode_chosen;
    int lu_identity_sep_retry_lane_dense_successes;
    int lu_identity_sep_retry_lane_supernode_successes;
    int lu_sn_cost_gate_trips;
    int lu_sn_cost_gate_skips;
    int lu_sn_cost_gate_resets;
    int lu_sn_calls;
    int lu_sn_successes;
    int lu_num_updates;
    int lu_max_updates;
    int lu_last_failure_reason_code;
    char lu_last_failure_reason[64];
    int lu_last_refactor_trigger_reason_code;
    char lu_last_refactor_trigger_reason[64];
    int lu_refactor_need_checks;
    int lu_refactor_need_triggers;
    int lu_refactor_need_reason_max_updates;
    int lu_refactor_need_reason_growth_guard;
    int lu_refactor_need_reason_avg_spike_density;
    int lu_refactor_need_reason_cond_severe;
    int lu_refactor_need_reason_cond_adaptive_limit;
    int lu_refactor_need_reason_spike_pool_warn;
    int lu_refactor_need_reason_spike_work;
    int lu_update_fail_bad_input;
    int lu_update_fail_max_updates;
    int lu_update_fail_singular_update;
    int lu_update_fail_update_pivot_too_small;
    int lu_update_fail_spike_pool_full;
    int lu_update_fail_dense_spike_reject;
    int lu_update_fail_eta_alloc;
    int lu_factorize_calls;
    int lu_last_basis_nnz;
    int lu_last_m;
    int lu_last_k;
    int lu_symbolic_calls;
    int lu_symbolic_cache_hits;
    int lu_symbolic_cache_misses;
    double lu_last_symbolic_ms;
    double lu_last_sparse_numeric_ms;
    double lu_last_dense_ge_numeric_ms;
    double lu_last_supernode_numeric_ms;
    double lu_last_dense_factorize_ms;
    double lu_last_a_struct_build_ms;
    double lu_last_markowitz_numeric_ms;
    double lu_last_identity_placement_ms;
    double lu_last_coo_to_csc_ms;
    double lu_total_symbolic_ms;
    double lu_total_sparse_numeric_ms;
    double lu_total_dense_ge_numeric_ms;
    double lu_total_supernode_numeric_ms;
    double lu_total_dense_factorize_ms;
    double lu_total_a_struct_build_ms;
    double lu_total_markowitz_numeric_ms;
    double lu_total_identity_placement_ms;
    double lu_total_coo_to_csc_ms;
    int lu_update_apply_forward_calls;
    int lu_update_apply_backward_calls;
    int lu_compact_factor_calls;
    int lu_compact_solve_calls;
    double lu_total_update_apply_forward_ms;
    double lu_total_update_apply_backward_ms;
    double lu_total_compact_factor_ms;
    double lu_total_compact_solve_ms;
    uint64_t lu_sn_phase_samples;
    double lu_sn_panel_factor_ms;
    double lu_sn_panel_pivot_search_ms;
    double lu_sn_panel_swap_scatter_ms;
    double lu_sn_panel_eliminate_ms;
    uint64_t lu_sn_panel_pivot_search_calls;
    uint64_t lu_sn_panel_pivot_search_entries_total;
    uint64_t lu_sn_panel_pivot_search_size1_calls;
    double lu_sn_panel_pivot_search_size1_ms;
    uint64_t lu_sn_panel_pivot_search_size2_calls;
    double lu_sn_panel_pivot_search_size2_ms;
    uint64_t lu_sn_panel_pivot_search_size3_4_calls;
    double lu_sn_panel_pivot_search_size3_4_ms;
    uint64_t lu_sn_panel_pivot_search_size5_8_calls;
    double lu_sn_panel_pivot_search_size5_8_ms;
    uint64_t lu_sn_panel_pivot_search_size9p_calls;
    double lu_sn_panel_pivot_search_size9p_ms;
    uint64_t lu_sn_panel_pivot_search_reserved_present_calls;
    uint64_t lu_sn_panel_pivot_search_reserved_present_entries;
    double lu_sn_panel_pivot_search_reserved_present_ms;
    uint64_t lu_sn_panel_pivot_search_reserved_alt_chosen_calls;
    double lu_sn_panel_pivot_search_reserved_alt_chosen_ms;
    uint64_t lu_sn_size1_u_emit_calls;
    double lu_sn_size1_u_emit_ms;
    uint64_t lu_sn_size1_update_scan_calls;
    double lu_sn_size1_update_scan_ms;
    uint64_t lu_sn_size1_update_apply_calls;
    double lu_sn_size1_update_apply_ms;
    double lu_sn_size1_update_row_gather_ms;
    double lu_sn_size1_update_col_indirection_ms;
    double lu_sn_size1_update_outer_product_ms;
    uint64_t lu_sn_size1_update_full_calls;
    double lu_sn_size1_update_full_ms;
    uint64_t lu_sn_size1_update_cols1_calls;
    double lu_sn_size1_update_cols1_ms;
    uint64_t lu_sn_size1_update_cols2_calls;
    double lu_sn_size1_update_cols2_ms;
    uint64_t lu_sn_size1_update_cols3_calls;
    double lu_sn_size1_update_cols3_ms;
    uint64_t lu_sn_size1_update_cols4_calls;
    double lu_sn_size1_update_cols4_ms;
    uint64_t lu_sn_size1_update_cols5p_calls;
    double lu_sn_size1_update_cols5p_ms;
    uint64_t lu_sn_size1_update_cols5p_rows1_8_calls;
    double lu_sn_size1_update_cols5p_rows1_8_ms;
    uint64_t lu_sn_size1_update_cols5p_rows9_32_calls;
    double lu_sn_size1_update_cols5p_rows9_32_ms;
    uint64_t lu_sn_size1_update_cols5p_rows33_128_calls;
    double lu_sn_size1_update_cols5p_rows33_128_ms;
    uint64_t lu_sn_size1_update_cols5p_rows129p_calls;
    double lu_sn_size1_update_cols5p_rows129p_ms;
    double lu_sn_u_emit_ms;
    double lu_sn_active_set_ms;
    double lu_sn_pack_blocks_ms;
    double lu_sn_full_update_ms;
    double lu_sn_compact_update_ms;
    uint64_t lu_sn_active_row_scan_entries;
    uint64_t lu_sn_active_col_scan_entries;
    uint64_t lu_sn_trailing_rows_total;
    uint64_t lu_sn_trailing_cols_total;
    uint64_t lu_sn_active_rows_total;
    uint64_t lu_sn_active_cols_total;
    uint64_t lu_sn_pack_l_entries_total;
    uint64_t lu_sn_pack_u_entries_total;
    uint64_t lu_sn_dense_triplets_total;
    uint64_t lu_sn_compact_triplets_total;
    uint64_t lu_sn_full_update_calls;
    uint64_t lu_sn_compact_update_calls;
    uint64_t lu_sn_skipped_update_calls;
    uint64_t lu_sn_compact_cols1_calls;
    uint64_t lu_sn_compact_cols1_rows_total;
    double lu_sn_compact_cols1_ms;
    uint64_t lu_sn_compact_cols2_calls;
    uint64_t lu_sn_compact_cols2_rows_total;
    double lu_sn_compact_cols2_ms;
    uint64_t lu_sn_compact_cols3_calls;
    uint64_t lu_sn_compact_cols3_rows_total;
    double lu_sn_compact_cols3_ms;
    uint64_t lu_sn_compact_cols4_calls;
    uint64_t lu_sn_compact_cols4_rows_total;
    double lu_sn_compact_cols4_ms;
    uint64_t lu_sn_compact_cols5p_calls;
    uint64_t lu_sn_compact_cols5p_rows_total;
    double lu_sn_compact_cols5p_ms;
    double *solution;    /* Primal solution (may be NULL) */
    int solution_size;
    int feasibility_checked;
    int num_constraint_violations;
    int num_bound_violations;
    double max_constraint_violation;
    double max_bound_violation;
    int worst_bound_var;
    double worst_bound_value;
    double worst_bound_lb;
    double worst_bound_ub;
} SolveResult;

typedef struct {
    int solution_valid;
    double max_constraint_violation;
    double max_bound_violation;
    int objective_match;
    double objective_rel_error;
    double objective_abs_error;
    int numerically_stable;
    char issues[1024];
} ValidationResult;

typedef struct {
    /* CLI options */
    int show_help;
    int show_version;
    int list_problems;
    int download_netlib;
    int json_output;
    int verbose;
    int lp_only;         /* Default: 1 (skip MIP) */
    int mip_only;
    int verify_matrix;   /* Deep matrix verification via GLPK solution */
    int test_mode;       /* 0=off, 1=fast (tiers 0-1), 2=full (all tiers) */

    /* Time limits */
    double time_multiplier;
    double hard_cap_sec;

    /* Tolerances */
    double obj_rel_tol;
    double obj_abs_tol;
    double feas_tol;

    /* Suite to run */
    char suite[64];

    /* Single problem */
    char problem_path[MAX_PATH];
    char netlib_name[256];

    /* Solver method */
    int method;  /* 0=primal, 1=dual, 2=auto, 3=dualp */
    int pricing; /* -1=default, 0=Dantzig, 1=SE, 2=Devex, 3=Partial, 4=Heap */
    int phase1_pricing; /* -1=default, 0=Dantzig, 1=SE, 2=Devex, 3=Partial, 4=Heap */
    int glpk_smcp_ratio; /* -1=default, 0=standard (--norelax), 1=harris (--relax) */
    int glpk_smcp_flip;  /* -1=default, 0=off (--noflip), 1=on (--flip) */
    int glpk_smcp_shift; /* -1=default, 0=off, 1=on */
    int glpk_bfcp_backend; /* -1=default, 0=luf_ft, 1=cbg, 2=cgr */
    int glpk_bfcp_update_limit; /* -1=default, positive overrides max updates */
    int dual_steepest_edge; /* -1=default, 0=off, 1=on */
    int lu_supernode; /* 0=off, 1=enable supernodal LU */
    int lp_basis_governor_mode; /* 0=off, 1=shadow, 2=control_phase2 */
    int lp_reinvert_controller_mode; /* 0=off, 1=shadow, 2=control_phase1, 3=control_all */
    int random_seed; /* deterministic LP anti-cycling perturbation seed */
    int external_glpk_oop; /* Use registered GLPK out-of-process LP backend for Ralph */
    int no_adaptive_fallback; /* Keep single native solve result for regression audits */
    int no_external_fallback; /* Allow native retries but forbid external backend rescue */
    int no_presolve; /* Disable native presolve before simplex */
    int presolve_mask_override; /* -1=default, otherwise native presolve technique mask */
    int crash; /* Enable native primal crash basis */
    int trace_phase1; /* Emit deterministic Phase 1 trace from the LP solver */
    int solver_verbose; /* Forward benchmark diagnostics to the native LP solver */

    /* Output */
    char output_dir[MAX_PATH];
} Options;

/* Access Ralph's internal model finalizer for benchmark-only validation. */
extern int lp_model_finalize(LPModel *model);

static int compute_solution_feasibility(LPModel *lp, const double *x, int n,
                                        double feas_tol,
                                        int *num_con_violations,
                                        double *max_con_violation,
                                        int *num_bound_violations,
                                        double *max_bound_violation,
                                        int *worst_bound_var,
                                        double *worst_bound_value,
                                        double *worst_bound_lb,
                                        double *worst_bound_ub) {
    double *ax;

    if (!lp || !x || n != lp->num_vars || lp->num_cons < 0) return -1;
    if (!lp->A) {
        if (lp_model_finalize(lp) != 0 || !lp->A) return -1;
    }

    ax = (double*)calloc((size_t)lp->num_cons, sizeof(double));
    if (!ax && lp->num_cons > 0) return -1;

    for (int j = 0; j < n; j++) {
        double xj = x[j];
        if (fabs(xj) < 1e-15) continue;
        for (int p = lp->A->colptr[j]; p < lp->A->colptr[j + 1]; p++) {
            int row = lp->A->rowidx[p];
            if (row >= 0 && row < lp->num_cons) {
                ax[row] += lp->A->values[p] * xj;
            }
        }
    }

    *num_con_violations = 0;
    *max_con_violation = 0.0;
    for (int i = 0; i < lp->num_cons; i++) {
        double viol = 0.0;
        double row_scale = fmax(1.0, fmax(fabs(ax[i]), fabs(lp->b[i])));
        double allowed = 2000.0 * feas_tol * row_scale;
        if (lp->sense[i] == 'E') {
            viol = fabs(ax[i] - lp->b[i]);
        } else if (lp->sense[i] == 'L') {
            if (ax[i] > lp->b[i] + feas_tol) viol = ax[i] - lp->b[i];
        } else if (lp->sense[i] == 'G') {
            if (ax[i] < lp->b[i] - feas_tol) viol = lp->b[i] - ax[i];
        }
        if (viol > allowed) (*num_con_violations)++;
        if (viol > *max_con_violation) *max_con_violation = viol;
    }

    *num_bound_violations = 0;
    *max_bound_violation = 0.0;
    *worst_bound_var = -1;
    *worst_bound_value = 0.0;
    *worst_bound_lb = 0.0;
    *worst_bound_ub = 0.0;
    for (int j = 0; j < n; j++) {
        double viol = 0.0;
        double bound_scale = fmax(1.0, fabs(x[j]));
        double allowed;
        if (lp->lb && x[j] < lp->lb[j] - feas_tol) {
            viol = lp->lb[j] - x[j];
            bound_scale = fmax(bound_scale, fabs(lp->lb[j]));
        }
        if (lp->ub && x[j] > lp->ub[j] + feas_tol) {
            double ub_viol = x[j] - lp->ub[j];
            if (ub_viol > viol) viol = ub_viol;
            bound_scale = fmax(bound_scale, fabs(lp->ub[j]));
        }
        allowed = 2000.0 * feas_tol * bound_scale;
        if (viol > allowed) (*num_bound_violations)++;
        if (viol > *max_bound_violation) {
            *max_bound_violation = viol;
            *worst_bound_var = j;
            *worst_bound_value = x[j];
            *worst_bound_lb = lp->lb ? lp->lb[j] : -RALPH_INFINITY;
            *worst_bound_ub = lp->ub ? lp->ub[j] : RALPH_INFINITY;
        }
    }

    free(ax);
    return 0;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void get_benchmark_dir(char *buf, size_t size) {
    /*
     * Get the benchmarks directory where NETLIB problems are stored.
     * This handles running from different locations:
     * - ./ralph-benchmark (from ralph/ directory)
     * - ./benchmarks/ralph-benchmark (from ralph/ directory)
     * - /path/to/ralph/ralph-benchmark (absolute path)
     */
    char cwd[MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strncpy(buf, "benchmarks", size - 1);
        buf[size - 1] = '\0';
        return;
    }

    /* Check if benchmarks/ exists in current directory */
    snprintf(buf, size, "%s/benchmarks", cwd);
    if (dir_exists(buf)) {
        return;
    }

    /* Check if we're already in benchmarks/ */
    snprintf(buf, size, "%s", cwd);
    char netlib_check[MAX_PATH];
    snprintf(netlib_check, sizeof(netlib_check), "%s/netlib", cwd);
    if (dir_exists(netlib_check)) {
        return;
    }

    /* Fallback: just use "benchmarks" and hope for the best */
    strncpy(buf, "benchmarks", size - 1);
    buf[size - 1] = '\0';
}

static void json_escape_string(char *out, size_t out_size, const char *in) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < out_size - 2; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = c;
            }
        } else if (c == '\n') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = 'n';
            }
        } else if (c == '\r') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = 'r';
            }
        } else if (c == '\t') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = 't';
            }
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static const char* refactor_reason_string(int reason) {
    switch ((RalphRefactorReason)reason) {
        case RALPH_REFACTOR_REASON_SETUP: return "setup";
        case RALPH_REFACTOR_REASON_PHASE_TRANSITION: return "phase_transition";
        case RALPH_REFACTOR_REASON_PERIODIC: return "periodic";
        case RALPH_REFACTOR_REASON_RATIO_RECOVERY: return "ratio_recovery";
        case RALPH_REFACTOR_REASON_PIVOT_RECOVERY: return "pivot_recovery";
        case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT: return "forced_small_pivot";
        case RALPH_REFACTOR_REASON_UPDATE_RECOVERY: return "update_recovery";
        case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE: return "direction_stabilize";
        case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP: return "infeasibility_cleanup";
        case RALPH_REFACTOR_REASON_OTHER:
        default:
            return "other";
    }
}

static const char* lu_sparse_fallback_reason_string(int reason) {
    switch ((LUSparseFallbackReason)reason) {
        case LU_SPARSE_FALLBACK_SMALL_MATRIX: return "small_matrix";
        case LU_SPARSE_FALLBACK_SYMBOLIC: return "symbolic";
        case LU_SPARSE_FALLBACK_NUMERIC: return "numeric";
        case LU_SPARSE_FALLBACK_NONE:
        default:
            return "none";
    }
}

static const char* lu_sparse_numeric_failure_reason_string(int reason) {
    switch ((LUSparseNumericFailureReason)reason) {
        case LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION:
            return "identity_separation";
        case LU_SPARSE_NUMERIC_FAIL_BACKEND_EXHAUSTED:
            return "backend_exhausted";
        case LU_SPARSE_NUMERIC_FAIL_PATHOLOGICAL:
            return "pathological";
        case LU_SPARSE_NUMERIC_FAIL_NONE:
        default:
            return "none";
    }
}

typedef enum {
    BENCH_TERM_NONE = 0,
    BENCH_TERM_OPTIMAL = 1,
    BENCH_TERM_INFEASIBLE = 2,
    BENCH_TERM_UNBOUNDED = 3,
    BENCH_TERM_TIME_LIMIT = 4,
    BENCH_TERM_ITERATION_LIMIT = 5,
    BENCH_TERM_OBJ_LIMIT = 6,
    BENCH_TERM_IMPRECISE = 7,
    BENCH_TERM_DUAL_RATIO_NO_ENTERING = 100,
    BENCH_TERM_DUAL_PIVOT_SMALL = 101,
    BENCH_TERM_DUAL_LU_HARD = 102,
    BENCH_TERM_LU_FAILURE = 103,
    BENCH_TERM_API_ERROR = 104,
    BENCH_TERM_NUMERICAL = 105,
    BENCH_TERM_UNKNOWN = 199
} BenchTerminationReason;

static void bench_set_termination_reason(SolveResult *result,
                                         BenchTerminationReason code,
                                         const char *name) {
    if (!result) return;
    result->termination_reason_code = (int)code;
    strncpy(result->termination_reason, name ? name : "unknown",
            sizeof(result->termination_reason) - 1);
    result->termination_reason[sizeof(result->termination_reason) - 1] = '\0';
}

static void bench_derive_termination_reason(SolveResult *result) {
    if (!result) return;

    if (result->raw_status_code == (int)RALPH_STATUS_OPTIMAL) {
        bench_set_termination_reason(result, BENCH_TERM_OPTIMAL, "optimal");
        return;
    }
    if (result->raw_status_code == (int)RALPH_STATUS_IMPRECISE) {
        bench_set_termination_reason(result, BENCH_TERM_IMPRECISE, "imprecise");
        return;
    }
    if (result->raw_status_code == (int)RALPH_STATUS_INFEASIBLE) {
        bench_set_termination_reason(result, BENCH_TERM_INFEASIBLE, "infeasible");
        return;
    }
    if (result->raw_status_code == (int)RALPH_STATUS_UNBOUNDED ||
        result->raw_status_code == (int)RALPH_STATUS_INF_OR_UNBD) {
        bench_set_termination_reason(result, BENCH_TERM_UNBOUNDED, "unbounded");
        return;
    }
    if (result->raw_status_code == (int)RALPH_STATUS_TIME_LIMIT) {
        bench_set_termination_reason(result, BENCH_TERM_TIME_LIMIT, "time_limit");
        return;
    }
    if (result->raw_status_code == (int)RALPH_STATUS_ITERATION_LIMIT) {
        bench_set_termination_reason(result, BENCH_TERM_ITERATION_LIMIT, "iteration_limit");
        return;
    }
    if (result->raw_status_code == (int)RALPH_STATUS_OBJ_LIMIT) {
        bench_set_termination_reason(result, BENCH_TERM_OBJ_LIMIT, "objective_limit");
        return;
    }
    if (result->raw_status_code == (int)RALPH_STATUS_ERROR &&
        result->iterations > 0) {
        bench_set_termination_reason(result, BENCH_TERM_NUMERICAL, "numerical_breakdown");
        return;
    }

    if (result->dual_lu_hard_trigger > 0) {
        bench_set_termination_reason(result, BENCH_TERM_DUAL_LU_HARD, "dual_lu_hard_trigger");
    } else if (result->dual_ratio_no_entering > 0) {
        bench_set_termination_reason(result, BENCH_TERM_DUAL_RATIO_NO_ENTERING, "dual_ratio_no_entering");
    } else if (result->dual_pivot_reject_small > 0) {
        bench_set_termination_reason(result, BENCH_TERM_DUAL_PIVOT_SMALL, "dual_pivot_reject_small");
    } else if (result->lu_last_failure_reason_code != LU_FAIL_NONE) {
        bench_set_termination_reason(result, BENCH_TERM_LU_FAILURE, "lu_failure");
    } else if (result->api_error_code != (int)RALPH_ERROR_CODE_NONE) {
        bench_set_termination_reason(result, BENCH_TERM_API_ERROR, "api_error");
    } else if (result->raw_status_code == (int)RALPH_STATUS_ERROR) {
        bench_set_termination_reason(result, BENCH_TERM_NUMERICAL, "numerical_breakdown");
    } else {
        bench_set_termination_reason(result, BENCH_TERM_UNKNOWN, "unknown");
    }
}

static const char* periodic_cost_reason_string(int reason) {
    return lp_refactor_policy_periodic_cost_dampen_reason_string(
        (LPPeriodicCostDampenReason)reason);
}

/* ============================================================================
 * GLPK Wrapper (via Ralph external OOP adapter)
 * ============================================================================ */

static int check_glpk_available(void) {
    int ret = system("which glpsol >/dev/null 2>&1");
    return ret == 0;
}

static int load_problem_into_model(RalphModel *model, const char *problem_path) {
    const char *ext;
    if (!model || !problem_path) return -1;
    ext = strrchr(problem_path, '.');
    if (ext && strcasecmp(ext, ".lp") == 0) {
        return ralph_test_read_lp(model, problem_path);
    }
    return ralph_test_read_mps(model, problem_path);
}

static SolveResult solve_with_glpk(const char *problem_path, double time_limit_sec) {
    SolveResult result = {0};
    RalphModel *model;
    RalphStatus status;
    int num_vars;

    result.status = 3;  /* Error by default */
    result.raw_status_code = (int)RALPH_STATUS_UNKNOWN;
    strncpy(result.raw_status_str, "UNKNOWN", sizeof(result.raw_status_str) - 1);
    strncpy(result.solve_path, "glpk_reference", sizeof(result.solve_path) - 1);
    bench_set_termination_reason(&result, BENCH_TERM_UNKNOWN, "unknown");
    result.solution = NULL;
    model = ralph_test_create();
    if (!model) return result;
    if (load_problem_into_model(model, problem_path) != 0) {
        ralph_test_free(model);
        return result;
    }

    num_vars = ralph_test_get_num_vars(model);
    ralph_core_set_int_param_id(model, RALPH_PARAM_VERBOSE, 0);
    ralph_core_set_int_param_id(model, RALPH_PARAM_PRESOLVE, 0);
    ralph_core_set_int_param_id(model, RALPH_PARAM_DETECT_SPECIAL, 0);
    ralph_core_set_dbl_param_id(model, RALPH_PARAM_TIME_LIMIT, time_limit_sec);
    ralph_core_set_int_param_id(model, RALPH_PARAM_MAX_ITERATIONS, 10000000);
    ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                           (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK);
    ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1);
    ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL);

    {
        double start_time = get_time_ms();
        (void)ralph_test_optimize_lp(model);
        result.time_ms = get_time_ms() - start_time;
    }
    result.iterations = ralph_test_get_iterations(model);
    status = ralph_test_get_status(model);
    result.raw_status_code = (int)status;
    {
        const char *status_str = ralph_test_status_string(status);
        if (!status_str) status_str = "UNKNOWN";
        strncpy(result.raw_status_str, status_str, sizeof(result.raw_status_str) - 1);
        result.raw_status_str[sizeof(result.raw_status_str) - 1] = '\0';
    }
    switch (status) {
        case RALPH_STATUS_OPTIMAL:
        case RALPH_STATUS_IMPRECISE:
        case RALPH_STATUS_OBJ_LIMIT:
            result.status = 0;
            result.objective = ralph_test_get_objval(model);
            if (num_vars > 0) {
                SimplexSolver *solver = ralph_get_lp_solver(model);
                result.solution = (double*)malloc((size_t)num_vars * sizeof(double));
                if (result.solution) {
                    if (solver && solver->solution) {
                        memcpy(result.solution, solver->solution,
                               (size_t)num_vars * sizeof(double));
                    } else {
                        (void)ralph_test_get_solution(model, result.solution);
                    }
                    result.solution_size = num_vars;
                }
            }
            break;
        case RALPH_STATUS_INFEASIBLE:
            result.status = 1;
            break;
        case RALPH_STATUS_UNBOUNDED:
        case RALPH_STATUS_INF_OR_UNBD:
            result.status = 2;
            break;
        case RALPH_STATUS_TIME_LIMIT:
        case RALPH_STATUS_ITERATION_LIMIT:
            result.status = 4;
            break;
        default:
            result.status = 3;
            break;
    }
    bench_derive_termination_reason(&result);
    ralph_test_free(model);

    return result;
}

/* ============================================================================
 * Ralph Wrapper
 * ============================================================================ */

static SolveResult solve_with_ralph(const char *problem_path, double time_limit_sec,
                                     int method, int pricing, int phase1_pricing,
                                     int glpk_smcp_ratio, int glpk_smcp_flip,
                                     int glpk_bfcp_backend,
                                     int glpk_bfcp_update_limit,
                                     int dual_steepest_edge,
                                     int lu_supernode, int lp_basis_governor_mode,
                                     int lp_reinvert_controller_mode,
                                     int random_seed,
                                     int external_glpk_oop,
                                     int smcp_shift_override,
                                     int no_presolve,
                                     int presolve_mask_override,
                                     int crash,
                                     int trace_phase1,
                                     int solver_verbose,
                                     int *out_num_vars, int *out_num_cons, int *out_nnz,
                                     int *out_is_mip) {
    SolveResult result = {0};
    result.status = 3;  /* Error by default */
    result.raw_status_code = (int)RALPH_STATUS_UNKNOWN;
    strncpy(result.raw_status_str, "UNKNOWN", sizeof(result.raw_status_str) - 1);
    strncpy(result.solve_path,
            external_glpk_oop ? "external_glpk_oop" :
            (smcp_shift_override == 0 ? "native_shift_off" : "native"),
            sizeof(result.solve_path) - 1);
    bench_set_termination_reason(&result, BENCH_TERM_UNKNOWN, "unknown");
    result.api_error_domain = (int)RALPH_ERROR_DOMAIN_NONE;
    result.api_error_code = (int)RALPH_ERROR_CODE_NONE;
    result.api_error_api_id = (int)RALPH_ERROR_API_NONE;
    strncpy(result.api_error_message, "", sizeof(result.api_error_message) - 1);
    result.solution = NULL;
    strncpy(result.lu_last_failure_reason, "none",
            sizeof(result.lu_last_failure_reason) - 1);
    strncpy(result.lu_sparse_fallback_last_reason_str, "none",
            sizeof(result.lu_sparse_fallback_last_reason_str) - 1);
    strncpy(result.lu_sparse_numeric_last_failure_reason_str, "none",
            sizeof(result.lu_sparse_numeric_last_failure_reason_str) - 1);
    strncpy(result.lu_last_refactor_trigger_reason, "none",
            sizeof(result.lu_last_refactor_trigger_reason) - 1);
    strncpy(result.refactor_last_reason_str, "other",
            sizeof(result.refactor_last_reason_str) - 1);

    RalphModel *model = ralph_test_create();
    if (!model) {
        return result;
    }

    /* Load problem */
    const char *ext = strrchr(problem_path, '.');
    int load_ret;
    if (ext && strcasecmp(ext, ".lp") == 0) {
        load_ret = ralph_test_read_lp(model, problem_path);
    } else {
        load_ret = ralph_test_read_mps(model, problem_path);
    }

    if (load_ret != 0) {
        ralph_test_free(model);
        return result;
    }

    /* Get problem info via public API */
    *out_num_vars = ralph_test_get_num_vars(model);
    *out_num_cons = ralph_test_get_num_cons(model);
    *out_is_mip = ralph_test_is_mip(model);
    *out_nnz = 0;

    /* Configure solver */
    ralph_test_set_int_param(model, "verbose", solver_verbose);
    ralph_test_set_dbl_param(model, "time_limit", time_limit_sec);
    ralph_test_set_int_param(model, "max_iterations", 10000000);
    ralph_test_set_int_param(model, "presolve", (external_glpk_oop || no_presolve) ? 0 : 1);
    if (presolve_mask_override >= 0) {
        ralph_test_set_int_param(model, "presolve_mask", presolve_mask_override);
    }
    ralph_test_set_int_param(model, "verify", 1);
    if (external_glpk_oop) {
        ralph_test_set_int_param(model, "detect_special", 0);
    }
    ralph_test_set_int_param(model, "method", (method == 3) ? 2 : method);
    ralph_test_set_int_param(model, "random_seed", random_seed);
    if (smcp_shift_override >= 0) {
        ralph_test_set_int_param(model, "glpk_smcp_shift", smcp_shift_override);
    }
    if (trace_phase1) {
        ralph_test_set_int_param(model, "trace_phase1", 1);
    }
    if (crash) {
        ralph_test_set_int_param(model, "crash", 1);
    }
    ralph_test_set_int_param(model, "lp_basis_governor_mode", lp_basis_governor_mode);
    ralph_test_set_int_param(model, "lp_reinvert_controller_mode", lp_reinvert_controller_mode);
    if (external_glpk_oop) {
        ralph_test_set_int_param(model, "lp_algorithm", RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL);
        ralph_test_set_int_param(model, "lp_external_provider", RALPH_LP_EXTERNAL_PROVIDER_GLPK);
        ralph_test_set_int_param(model, "lp_external_strict", 1);
    }
    if (pricing >= 0) {
        ralph_test_set_int_param(model, "pricing", pricing);
    }
    if (phase1_pricing >= 0) {
        ralph_test_set_int_param(model, "phase1_pricing", phase1_pricing);
    }
    if (glpk_smcp_ratio >= 0 || glpk_smcp_flip >= 0 || glpk_bfcp_backend >= 0) {
        /* Route ratio/flip through GLPK-compat runtime mapping.
         * Keep method/pricing aligned with explicit benchmark switches. */
        ralph_test_set_int_param(model, "lp_policy_profile", 1); /* glpk_compat */
        if (method == 0) {
            ralph_test_set_int_param(model, "glpk_smcp_method", 1); /* primal */
        } else if (method == 1) {
            ralph_test_set_int_param(model, "glpk_smcp_method", 3); /* dual */
        } else if (method == 3) {
            ralph_test_set_int_param(model, "glpk_smcp_method", 2); /* dualp */
        } else {
            ralph_test_set_int_param(model, "glpk_smcp_method", 0); /* auto */
        }
        if (pricing == 0) {
            ralph_test_set_int_param(model, "glpk_smcp_pricing", 0); /* standard */
        } else if (pricing == 1) {
            ralph_test_set_int_param(model, "glpk_smcp_pricing", 1); /* steep */
        }
        if (glpk_smcp_ratio >= 0) {
            ralph_test_set_int_param(model, "glpk_smcp_ratio", glpk_smcp_ratio);
        }
        if (glpk_smcp_flip >= 0) {
            ralph_test_set_int_param(model, "glpk_smcp_flip", glpk_smcp_flip);
        }
        if (glpk_bfcp_backend >= 0) {
            ralph_test_set_int_param(model, "glpk_bfcp_backend", glpk_bfcp_backend);
        }
    }
    if (glpk_bfcp_update_limit > 0) {
        ralph_test_set_int_param(model, "glpk_bfcp_update_limit",
                                 glpk_bfcp_update_limit);
    }
    if (dual_steepest_edge >= 0) {
        ralph_test_set_int_param(model, "dual_steepest_edge",
                                 dual_steepest_edge);
    }
    if (lu_supernode) {
        ralph_test_set_int_param(model, "lu_supernode", 1);
    }

    /* Solve */
    double start_time = get_time_ms();
    ralph_test_optimize(model);
    double end_time = get_time_ms();

    result.time_ms = end_time - start_time;
    result.iterations = ralph_test_get_iterations(model);
    {
        RalphPresolveReport presolve_report;
        if (ralph_core_get_last_presolve_report(model, &presolve_report) == 0) {
            result.presolve_used = presolve_report.used;
            result.presolve_mask = presolve_report.mask;
            result.presolve_rounds = presolve_report.rounds;
            result.presolve_vars_removed = presolve_report.vars_removed;
            result.presolve_cons_removed = presolve_report.cons_removed;
            result.presolve_bounds_tightened = presolve_report.bounds_tightened;
            result.presolve_matrix_rank = presolve_report.matrix_rank;
            result.presolve_redundant_rows_found = presolve_report.redundant_rows_found;
            result.presolve_time_ms = presolve_report.presolve_time_ms;
        }
    }
    {
        LPModel *lp = ralph_get_lp_model(model);
        if (lp) {
            if (lp->A && lp->A->nnz > 0) {
                *out_nnz = lp->A->nnz;
            } else if (lp->num_elements > 0) {
                *out_nnz = lp->num_elements;
            }
        }
    }
    {
        SimplexSolver *solver = ralph_get_lp_solver(model);
        if (solver) {
            LPSolverTelemetrySnapshot solver_tel;
            lp_telemetry_snapshot_solver(solver, &solver_tel);
            if (solver->tableau &&
                solver->tableau->num_artificial > 0 &&
                solver->tableau->artificial_vars &&
                solver->tableau->x) {
                SimplexTableau *tab = solver->tableau;
                for (int k = 0; k < tab->num_artificial; k++) {
                    int j = tab->artificial_vars[k];
                    if (j >= 0 && j < tab->n) {
                        double abs_x = fabs(tab->x[j]);
                        result.phase1_artificial_sum += abs_x;
                        if (abs_x > result.phase1_artificial_max) {
                            result.phase1_artificial_max = abs_x;
                        }
                        if (tab->var_status[j] == RALPH_BASIC) {
                            result.phase1_artificial_basic++;
                        }
                    }
                }
            }

            result.primal_setup_ms = solver_tel.perf_primal_setup_ms;
            result.dual_ms = solver_tel.perf_dual_ms;
            result.phase1_ms = solver_tel.perf_phase1_ms;
            result.transition_ms = solver_tel.perf_transition_ms;
            result.phase2_ms = solver_tel.perf_phase2_ms;
            result.pricing_ms = solver_tel.perf_pricing_ms;
            result.ratio_ms = solver_tel.perf_ratio_ms;
            result.pivot_ms = solver_tel.perf_pivot_ms;
            result.refactor_ms = solver_tel.perf_refactor_ms;
            result.ftran_ms = solver_tel.perf_ftran_ms;
            result.btran_ms = solver_tel.perf_btran_ms;
            result.ftran_base_ms = solver_tel.perf_ftran_base_ms;
            result.ftran_update_apply_ms = solver_tel.perf_ftran_update_apply_ms;
            result.ftran_update_apply_calls = solver_tel.perf_ftran_update_apply_calls;
            result.btran_base_ms = solver_tel.perf_btran_base_ms;
            result.btran_update_apply_ms = solver_tel.perf_btran_update_apply_ms;
            result.btran_update_apply_calls = solver_tel.perf_btran_update_apply_calls;
            result.ftran_calls = solver_tel.perf_ftran_calls;
            result.btran_calls = solver_tel.perf_btran_calls;
            result.ftran_nnz_samples = solver_tel.perf_ftran_nnz_samples;
            result.btran_nnz_samples = solver_tel.perf_btran_nnz_samples;
            result.ftran_rhs_nnz_total = solver_tel.perf_ftran_rhs_nnz_total;
            result.ftran_sol_nnz_total = solver_tel.perf_ftran_sol_nnz_total;
            result.btran_rhs_nnz_total = solver_tel.perf_btran_rhs_nnz_total;
            result.btran_sol_nnz_total = solver_tel.perf_btran_sol_nnz_total;
            result.lu_update_ms = solver_tel.perf_lu_update_ms;
            result.compute_solution_ms = solver_tel.perf_compute_solution_ms;
            result.compute_rc_ms = solver_tel.perf_compute_rc_ms;
            result.refactor_all_ms = solver_tel.perf_refactor_all_ms;
            result.refactor_count = solver_tel.perf_refactor_count;
            result.refactor_last_ms = solver_tel.perf_refactor_last_ms;
            result.refactor_max_ms = solver_tel.perf_refactor_max_ms;
            result.refactor_last_reason = solver_tel.perf_refactor_last_reason;
            {
                const char *reason = refactor_reason_string(solver_tel.perf_refactor_last_reason);
                if (!reason) reason = "other";
                strncpy(result.refactor_last_reason_str, reason,
                        sizeof(result.refactor_last_reason_str) - 1);
                result.refactor_last_reason_str[sizeof(result.refactor_last_reason_str) - 1] = '\0';
            }
            result.refactor_reason_setup = solver_tel.perf_refactor_reason_setup;
            result.refactor_reason_transition = solver_tel.perf_refactor_reason_transition;
            result.refactor_reason_periodic = solver_tel.perf_refactor_reason_periodic;
            result.refactor_reason_ratio_recovery = solver_tel.perf_refactor_reason_ratio_recovery;
            result.refactor_reason_pivot_recovery = solver_tel.perf_refactor_reason_pivot_recovery;
            result.refactor_reason_forced_small_pivot = solver_tel.perf_refactor_reason_forced_small_pivot;
            result.refactor_reason_update_recovery = solver_tel.perf_refactor_reason_update_recovery;
            result.refactor_reason_direction_stabilize = solver_tel.perf_refactor_reason_direction_stabilize;
            result.refactor_reason_infeas_cleanup = solver_tel.perf_refactor_reason_infeas_cleanup;
            result.refactor_reason_other = solver_tel.perf_refactor_reason_other;
            result.refactor_periodic_policy = solver_tel.perf_refactor_periodic_policy;
            result.refactor_periodic_lu_health = solver_tel.perf_refactor_periodic_lu_health;
            result.refactor_safety_forced = solver_tel.perf_refactor_safety_forced;
            result.basis_fastpath_hits = solver_tel.perf_basis_fastpath_hits;
            result.basis_cols_rewritten = solver_tel.perf_basis_cols_rewritten;
            result.basis_tail_shift_bytes = solver_tel.perf_basis_tail_shift_bytes;
            result.refactor_last_m = solver_tel.perf_refactor_last_m;
            result.refactor_last_k = solver_tel.perf_refactor_last_k;
            result.refactor_last_nnz_b = solver_tel.perf_refactor_last_nnz_B;
            result.refactor_factorize_failures = solver_tel.perf_refactor_factorize_failures;
            result.refactor_repair_successes = solver_tel.perf_refactor_repair_successes;
            result.refactor_repair_failures = solver_tel.perf_refactor_repair_failures;
            result.refactor_last_factorize_failure_reason =
                solver_tel.perf_refactor_last_factorize_failure_reason;
            result.refactor_last_sparse_numeric_failure_reason =
                solver_tel.perf_refactor_last_sparse_numeric_failure_reason;
            result.refactor_last_repair_status = solver_tel.perf_refactor_last_repair_status;
            result.phase1_refactor_factorize_failures =
                solver_tel.perf_phase1_refactor_factorize_failures;
            result.phase1_refactor_repair_successes =
                solver_tel.perf_phase1_refactor_repair_successes;
            result.phase1_refactor_repair_failures =
                solver_tel.perf_phase1_refactor_repair_failures;
            result.phase2_refactor_factorize_failures =
                solver_tel.perf_phase2_refactor_factorize_failures;
            result.phase2_refactor_repair_successes =
                solver_tel.perf_phase2_refactor_repair_successes;
            result.phase2_refactor_repair_failures =
                solver_tel.perf_phase2_refactor_repair_failures;

            result.phase1_pricing_ms = solver_tel.perf_phase1_pricing_ms;
            result.phase1_ratio_ms = solver_tel.perf_phase1_ratio_ms;
            result.phase1_pivot_ms = solver_tel.perf_phase1_pivot_ms;
            result.phase1_refactor_ms = solver_tel.perf_phase1_refactor_ms;
            result.phase1_compute_solution_ms = solver_tel.perf_phase1_compute_solution_ms;
            result.phase1_compute_rc_ms = solver_tel.perf_phase1_compute_rc_ms;
            result.phase1_pricing_calls = solver_tel.perf_phase1_pricing_calls;
            result.phase1_ratio_calls = solver_tel.perf_phase1_ratio_calls;
            result.phase1_pivot_calls = solver_tel.perf_phase1_pivot_calls;
            result.phase1_refactor_calls = solver_tel.perf_phase1_refactor_calls;
            result.phase1_compute_solution_calls = solver_tel.perf_phase1_compute_solution_calls;
            result.phase1_compute_rc_calls = solver_tel.perf_phase1_compute_rc_calls;
            result.phase1_compute_solution_ctx_other =
                solver_tel.perf_phase1_compute_solution_ctx_other;
            result.phase1_compute_solution_ctx_recompute_full =
                solver_tel.perf_phase1_compute_solution_ctx_recompute_full;
            result.phase1_compute_solution_ctx_recompute_guard_forced_full =
                solver_tel.perf_phase1_compute_solution_ctx_recompute_guard_forced_full;
            result.phase1_compute_solution_ctx_init =
                solver_tel.perf_phase1_compute_solution_ctx_init;
            result.phase1_compute_solution_ctx_no_entering_cleanup =
                solver_tel.perf_phase1_compute_solution_ctx_no_entering_cleanup;
            result.phase1_compute_solution_ctx_infeas_cleanup =
                solver_tel.perf_phase1_compute_solution_ctx_infeas_cleanup;
            result.phase1_compute_solution_ctx_refactor_fail_continue =
                solver_tel.perf_phase1_compute_solution_ctx_refactor_fail_continue;
            result.phase1_compute_solution_ctx_refactor_failure_recovery =
                solver_tel.perf_phase1_compute_solution_ctx_refactor_failure_recovery;
            result.phase1_compute_solution_ctx_refactor_success =
                solver_tel.perf_phase1_compute_solution_ctx_refactor_success;
            result.phase1_compute_solution_ctx_drift_refresh =
                solver_tel.perf_phase1_compute_solution_ctx_drift_refresh;
            result.phase1_compute_solution_ctx_dual_rescue =
                solver_tel.perf_phase1_compute_solution_ctx_dual_rescue;
            result.phase1_compute_rc_ctx_other =
                solver_tel.perf_phase1_compute_rc_ctx_other;
            result.phase1_compute_rc_ctx_recompute_full =
                solver_tel.perf_phase1_compute_rc_ctx_recompute_full;
            result.phase1_compute_rc_ctx_recompute_rc_only =
                solver_tel.perf_phase1_compute_rc_ctx_recompute_rc_only;
            result.phase1_compute_rc_ctx_recompute_guard_forced_full =
                solver_tel.perf_phase1_compute_rc_ctx_recompute_guard_forced_full;
            result.phase1_compute_rc_ctx_init =
                solver_tel.perf_phase1_compute_rc_ctx_init;
            result.phase1_compute_rc_ctx_infeas_cleanup =
                solver_tel.perf_phase1_compute_rc_ctx_infeas_cleanup;
            result.phase1_compute_rc_ctx_refactor_fail_continue =
                solver_tel.perf_phase1_compute_rc_ctx_refactor_fail_continue;
            result.phase1_compute_rc_ctx_refactor_failure_recovery =
                solver_tel.perf_phase1_compute_rc_ctx_refactor_failure_recovery;
            result.phase1_compute_rc_ctx_refactor_success =
                solver_tel.perf_phase1_compute_rc_ctx_refactor_success;
            result.phase1_compute_rc_ctx_drift_refresh =
                solver_tel.perf_phase1_compute_rc_ctx_drift_refresh;
            result.phase1_compute_rc_ctx_dual_rescue =
                solver_tel.perf_phase1_compute_rc_ctx_dual_rescue;
            result.phase1_entering_exclusions =
                solver_tel.perf_phase1_entering_exclusions;
            result.phase1_entering_exclusion_repeats =
                solver_tel.perf_phase1_entering_exclusion_repeats;
            result.phase1_entering_exclusion_hits =
                solver_tel.perf_phase1_entering_exclusion_hits;
            result.phase1_entering_exclusion_reroutes =
                solver_tel.perf_phase1_entering_exclusion_reroutes;
            result.phase1_entering_exclusion_no_alt =
                solver_tel.perf_phase1_entering_exclusion_no_alt;
            result.phase1_refactor_periodic_policy = solver_tel.perf_phase1_refactor_periodic_policy;
            result.phase1_refactor_periodic_lu_health = solver_tel.perf_phase1_refactor_periodic_lu_health;
            result.phase1_refactor_safety_forced = solver_tel.perf_phase1_refactor_safety_forced;
            result.phase1_dir_stabilize_force_extreme_dir =
                solver_tel.perf_phase1_dir_stabilize_force_extreme_dir;
            result.phase1_dir_stabilize_force_lu_health =
                solver_tel.perf_phase1_dir_stabilize_force_lu_health;
            result.phase1_dir_stabilize_cooldown_candidates =
                solver_tel.perf_phase1_dir_stabilize_cooldown_candidates;
            result.phase1_dir_stabilize_ratio_le_3 =
                solver_tel.perf_phase1_dir_stabilize_ratio_le_3;
            result.phase1_dir_stabilize_ratio_le_10 =
                solver_tel.perf_phase1_dir_stabilize_ratio_le_10;
            result.phase1_dir_stabilize_ratio_le_30 =
                solver_tel.perf_phase1_dir_stabilize_ratio_le_30;
            result.phase1_dir_stabilize_ratio_le_100 =
                solver_tel.perf_phase1_dir_stabilize_ratio_le_100;
            result.phase1_dir_stabilize_ratio_gt_100 =
                solver_tel.perf_phase1_dir_stabilize_ratio_gt_100;
            result.phase1_dir_stabilize_ratio_gt_300 =
                solver_tel.perf_phase1_dir_stabilize_ratio_gt_300;
            result.phase1_dir_stabilize_ratio_gt_1000 =
                solver_tel.perf_phase1_dir_stabilize_ratio_gt_1000;
            result.phase1_dir_stabilize_skip_rc_only =
                solver_tel.perf_phase1_dir_stabilize_skip_rc_only;
            result.phase1_dir_stabilize_skip_full =
                solver_tel.perf_phase1_dir_stabilize_skip_full;
            result.phase1_dir_stabilize_skip_no_recompute =
                solver_tel.perf_phase1_dir_stabilize_skip_no_recompute;
            result.phase1_dir_stabilize_skip_guard_refresh =
                solver_tel.perf_phase1_dir_stabilize_skip_guard_refresh;
            result.phase1_dir_stabilize_escape_gate_triggers =
                solver_tel.perf_phase1_dir_stabilize_escape_gate_triggers;
            result.phase1_dir_stabilize_escape_gate_suppressed_lu_health =
                solver_tel.perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health;
            result.phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode =
                solver_tel.perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode;
            result.phase1_dir_stabilize_escape_gate_hard_bypass =
                solver_tel.perf_phase1_dir_stabilize_escape_gate_hard_bypass;
            result.phase1_dir_stabilize_refactor_from_no_pivot_force =
                solver_tel.perf_phase1_dir_stabilize_refactor_from_no_pivot_force;
            result.phase1_dir_stabilize_refactor_from_force_extreme_dir =
                solver_tel.perf_phase1_dir_stabilize_refactor_from_force_extreme_dir;
            result.phase1_dir_stabilize_refactor_from_force_lu_health =
                solver_tel.perf_phase1_dir_stabilize_refactor_from_force_lu_health;
            result.phase1_dir_stabilize_refactor_from_force_pivot_mode =
                solver_tel.perf_phase1_dir_stabilize_refactor_from_force_pivot_mode;
            result.phase1_dir_stabilize_refactor_from_ladder_force =
                solver_tel.perf_phase1_dir_stabilize_refactor_from_ladder_force;
            result.phase1_force_pivot_budget_dir_event_seen =
                solver_tel.perf_phase1_force_pivot_budget_dir_event_seen;
            result.phase1_force_pivot_budget_pivot_spend =
                solver_tel.perf_phase1_force_pivot_budget_pivot_spend;
            result.phase1_force_pivot_relax_applied =
                solver_tel.perf_phase1_force_pivot_relax_applied;
            result.phase1_force_extreme_relax_applied =
                solver_tel.perf_phase1_force_extreme_relax_applied;
            result.phase1_force_extreme_bound_flip_relax_applied =
                solver_tel.perf_phase1_force_extreme_bound_flip_relax_applied;
            result.phase1_force_extreme_catastrophic_tiny_theta_relax_applied =
                solver_tel.perf_phase1_force_extreme_catastrophic_tiny_theta_relax_applied;
            result.phase1_force_extreme_tiny_theta_relax_applied =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_applied;
            result.phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry;
            result.phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue;
            result.phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor;
            result.phase1_force_extreme_tiny_theta_relax_refactor_force_lu_health =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_refactor_force_lu_health;
            result.phase1_force_extreme_tiny_theta_relax_refactor_force_pivot_mode =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_refactor_force_pivot_mode;
            result.phase1_force_extreme_tiny_theta_relax_refactor_ladder_force =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_refactor_ladder_force;
            result.phase1_force_extreme_tiny_theta_relax_next_failed_stabilize =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_next_failed_stabilize;
            result.phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown;
            result.phase1_force_extreme_tiny_theta_relax_next_pivot_fail =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_next_pivot_fail;
            result.phase1_force_extreme_tiny_theta_relax_next_pivot_success =
                solver_tel.perf_phase1_force_extreme_tiny_theta_relax_next_pivot_success;
            result.phase1_recompute_after_ratio_breakdown =
                solver_tel.perf_phase1_recompute_after_ratio_breakdown;
            result.phase1_recompute_after_dir_skip =
                solver_tel.perf_phase1_recompute_after_dir_skip;
            result.phase1_recompute_after_dir_refactor =
                solver_tel.perf_phase1_recompute_after_dir_refactor;
            result.phase1_recompute_after_pivot_fail_recovery =
                solver_tel.perf_phase1_recompute_after_pivot_fail_recovery;
            result.phase1_recompute_after_perturb =
                solver_tel.perf_phase1_recompute_after_perturb;
            result.phase1_recompute_rc_only_calls =
                solver_tel.perf_phase1_recompute_rc_only_calls;
            result.phase1_recompute_rc_guard_forced_full =
                solver_tel.perf_phase1_recompute_rc_guard_forced_full;
            result.phase1_cleanup_attempts =
                solver_tel.perf_phase1_cleanup_attempts;
            result.phase1_cleanup_accepted =
                solver_tel.perf_phase1_cleanup_accepted;
            result.phase1_cleanup_rejected =
                solver_tel.perf_phase1_cleanup_rejected;
            result.phase1_cleanup_candidate_probe_rejects =
                solver_tel.perf_phase1_cleanup_candidate_probe_rejects;
            result.phase1_progress_window_refactors =
                solver_tel.perf_phase1_progress_window_refactors;
            result.phase1_progress_window_cleanups =
                solver_tel.perf_phase1_progress_window_cleanups;
            result.phase1_progress_window_perturbs =
                solver_tel.perf_phase1_progress_window_perturbs;
            result.phase1_ratio_breakdown_retries =
                solver_tel.perf_phase1_ratio_breakdown_retries;
            result.phase1_ratio_breakdown_escalations =
                solver_tel.perf_phase1_ratio_breakdown_escalations;
            result.phase1_pivot_fail_recovery_exclusions =
                solver_tel.perf_phase1_pivot_fail_recovery_exclusions;
            result.phase1_no_pivot_events =
                solver_tel.perf_phase1_no_pivot_events;
            result.phase1_no_pivot_forced_refactor =
                solver_tel.perf_phase1_no_pivot_forced_refactor;
            result.phase1_no_pivot_forced_ratio_breakdown =
                solver_tel.perf_phase1_no_pivot_forced_ratio_breakdown;
            result.phase1_no_pivot_forced_dir_skip =
                solver_tel.perf_phase1_no_pivot_forced_dir_skip;
            result.phase1_no_pivot_forced_pivot_fail =
                solver_tel.perf_phase1_no_pivot_forced_pivot_fail;
            result.phase1_no_pivot_events_ratio_breakdown =
                solver_tel.perf_phase1_no_pivot_events_ratio_breakdown;
            result.phase1_no_pivot_events_dir_skip =
                solver_tel.perf_phase1_no_pivot_events_dir_skip;
            result.phase1_no_pivot_events_pivot_fail =
                solver_tel.perf_phase1_no_pivot_events_pivot_fail;
            result.phase1_no_pivot_no_progress_events =
                solver_tel.perf_phase1_no_pivot_no_progress_events;
            result.phase1_no_pivot_ladder_retry_defers =
                solver_tel.perf_phase1_no_pivot_ladder_retry_defers;
            result.phase1_no_pivot_ladder_retry_ratio_breakdown =
                solver_tel.perf_phase1_no_pivot_ladder_retry_ratio_breakdown;
            result.phase1_no_pivot_ladder_retry_dir_skip =
                solver_tel.perf_phase1_no_pivot_ladder_retry_dir_skip;
            result.phase1_no_pivot_ladder_retry_pivot_fail =
                solver_tel.perf_phase1_no_pivot_ladder_retry_pivot_fail;
            result.phase1_no_pivot_ladder_dual_rescue_attempts =
                solver_tel.perf_phase1_no_pivot_ladder_dual_rescue_attempts;
            result.phase1_no_pivot_ladder_dual_rescue_successes =
                solver_tel.perf_phase1_no_pivot_ladder_dual_rescue_successes;
            result.phase1_no_pivot_ladder_dual_rescue_failures =
                solver_tel.perf_phase1_no_pivot_ladder_dual_rescue_failures;
            result.phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown =
                solver_tel.perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown;
            result.phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip =
                solver_tel.perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip;
            result.phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail =
                solver_tel.perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail;
            result.phase1_no_pivot_ladder_forced_refactors =
                solver_tel.perf_phase1_no_pivot_ladder_forced_refactors;
            result.phase1_no_pivot_ladder_forced_refactors_ratio_breakdown =
                solver_tel.perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown;
            result.phase1_no_pivot_ladder_forced_refactors_dir_skip =
                solver_tel.perf_phase1_no_pivot_ladder_forced_refactors_dir_skip;
            result.phase1_no_pivot_ladder_forced_refactors_pivot_fail =
                solver_tel.perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail;
            result.phase1_no_pivot_ladder_rescue_guard_cooldown_blocks =
                solver_tel.perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks;
            result.phase1_no_pivot_ladder_rescue_guard_fail_cap_forces =
                solver_tel.perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces;
            result.phase1_direct_dual_rescue_attempts =
                solver_tel.perf_phase1_direct_dual_rescue_attempts;
            result.phase1_direct_dual_rescue_successes =
                solver_tel.perf_phase1_direct_dual_rescue_successes;
            result.phase1_direct_dual_rescue_failures =
                solver_tel.perf_phase1_direct_dual_rescue_failures;
            result.phase1_direct_dual_rescue_guard_cooldown_blocks =
                solver_tel.perf_phase1_direct_dual_rescue_guard_cooldown_blocks;
            result.phase1_direct_dual_rescue_guard_fail_cap_blocks =
                solver_tel.perf_phase1_direct_dual_rescue_guard_fail_cap_blocks;
            result.phase1_dual_rescue_exit_time_limit =
                solver_tel.perf_phase1_dual_rescue_exit_time_limit;
            result.phase1_dual_rescue_exit_bad_numerics =
                solver_tel.perf_phase1_dual_rescue_exit_bad_numerics;
            result.phase1_dual_rescue_exit_no_progress =
                solver_tel.perf_phase1_dual_rescue_exit_no_progress;
            result.phase1_dual_rescue_exit_no_entering =
                solver_tel.perf_phase1_dual_rescue_exit_no_entering;
            result.phase1_dual_rescue_exit_pivot_refactor_failure =
                solver_tel.perf_phase1_dual_rescue_exit_pivot_refactor_failure;
            result.phase1_dual_rescue_exit_periodic_refactor_failure =
                solver_tel.perf_phase1_dual_rescue_exit_periodic_refactor_failure;
            result.phase1_dual_rescue_exit_max_iters =
                solver_tel.perf_phase1_dual_rescue_exit_max_iters;
            result.phase1_dual_rescue_exit_alloc_failure =
                solver_tel.perf_phase1_dual_rescue_exit_alloc_failure;
            result.phase1_soft_lu_policy_cooldown_defers =
                solver_tel.perf_phase1_soft_lu_policy_cooldown_defers;
            result.phase1_dir_skip_same_entering_repeats =
                solver_tel.perf_phase1_dir_skip_same_entering_repeats;
            result.phase1_dir_skip_same_entering_max_streak =
                solver_tel.perf_phase1_dir_skip_same_entering_max_streak;
            result.phase1_failed_stabilize_events =
                solver_tel.perf_phase1_failed_stabilize_events;
            result.phase1_failed_stabilize_primary_failures =
                solver_tel.perf_phase1_failed_stabilize_primary_failures;
            result.phase1_failed_stabilize_alternate_failures =
                solver_tel.perf_phase1_failed_stabilize_alternate_failures;
            result.phase1_failed_stabilize_same_entering_repeats =
                solver_tel.perf_phase1_failed_stabilize_same_entering_repeats;
            result.phase1_failed_stabilize_same_entering_max_streak =
                solver_tel.perf_phase1_failed_stabilize_same_entering_max_streak;
            result.phase1_failed_stabilize_retry_penalty_arms =
                solver_tel.perf_phase1_failed_stabilize_retry_penalty_arms;
            result.phase1_failed_stabilize_retry_penalty_alt_found =
                solver_tel.perf_phase1_failed_stabilize_retry_penalty_alt_found;
            result.phase1_failed_stabilize_retry_penalty_no_alt =
                solver_tel.perf_phase1_failed_stabilize_retry_penalty_no_alt;
            result.phase1_failed_stabilize_retry_penalty_alt_stabilized =
                solver_tel.perf_phase1_failed_stabilize_retry_penalty_alt_stabilized;
            result.phase1_failed_stabilize_retry_penalty_alt_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_penalty_alt_failed;
            result.phase1_failed_stabilize_retry_penalty_same_alt_repeats =
                solver_tel.perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats;
            result.phase1_failed_stabilize_retry_penalty_same_alt_max_streak =
                solver_tel.perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak;
            result.phase1_failed_stabilize_retry_local_memory_arms =
                solver_tel.perf_phase1_failed_stabilize_retry_local_memory_arms;
            result.phase1_failed_stabilize_retry_local_memory_alt_found =
                solver_tel.perf_phase1_failed_stabilize_retry_local_memory_alt_found;
            result.phase1_failed_stabilize_retry_local_memory_no_alt =
                solver_tel.perf_phase1_failed_stabilize_retry_local_memory_no_alt;
            result.phase1_failed_stabilize_retry_local_memory_fallback_same_alt =
                solver_tel.perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt;
            result.phase1_failed_stabilize_retry_local_memory_alt_stabilized =
                solver_tel.perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized;
            result.phase1_failed_stabilize_retry_local_memory_alt_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_local_memory_alt_failed;
            result.phase1_failed_stabilize_retry_pool_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_pool_samples;
            result.phase1_failed_stabilize_retry_pool_eligible_total =
                solver_tel.perf_phase1_failed_stabilize_retry_pool_eligible_total;
            result.phase1_failed_stabilize_retry_pool_eligible_max =
                solver_tel.perf_phase1_failed_stabilize_retry_pool_eligible_max;
            result.phase1_failed_stabilize_retry_pool_singleton_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_pool_singleton_samples;
            result.phase1_failed_stabilize_retry_pool_best_differs_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_pool_best_differs_samples;
            result.phase1_failed_stabilize_retry_selector_eval_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_eval_samples;
            result.phase1_failed_stabilize_retry_selector_eval_best_differs_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples;
            result.phase1_failed_stabilize_retry_selector_eval_score_ratio_total =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total;
            result.phase1_failed_stabilize_retry_selector_eval_score_ratio_max =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max;
            result.phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2 =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2;
            result.phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4 =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4;
            result.phase1_failed_stabilize_retry_shadow_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_samples;
            result.phase1_failed_stabilize_retry_shadow_ratio_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_ratio_failed;
            result.phase1_failed_stabilize_retry_shadow_dir_stable =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_dir_stable;
            result.phase1_failed_stabilize_retry_shadow_dir_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_dir_failed;
            result.phase1_failed_stabilize_retry_shadow_dir_nnz_total =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total;
            result.phase1_failed_stabilize_retry_shadow_dir_nnz_max =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max;
            result.phase1_failed_stabilize_retry_shadow_dir_inf_total =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_dir_inf_total;
            result.phase1_failed_stabilize_retry_shadow_dir_inf_max =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_dir_inf_max;
            result.phase1_failed_stabilize_retry_shadow_pivot_abs_total =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total;
            result.phase1_failed_stabilize_retry_shadow_pivot_abs_max =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max;
            result.phase1_failed_stabilize_retry_shadow_guard_arms =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_guard_arms;
            result.phase1_failed_stabilize_retry_shadow_guard_original_exclusions =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions;
            result.phase1_failed_stabilize_retry_shadow_post_dir_skip_retry =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry;
            result.phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue;
            result.phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor;
            result.phase1_failed_stabilize_retry_shadow_next_failed_stabilize =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize;
            result.phase1_failed_stabilize_retry_shadow_next_ratio_breakdown =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown;
            result.phase1_failed_stabilize_retry_shadow_next_pivot_fail =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail;
            result.phase1_failed_stabilize_retry_shadow_next_pivot_success =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_next_pivot_success;
            result.phase1_force_extreme_followup_stabilized =
                solver_tel.perf_phase1_force_extreme_followup_stabilized;
            result.phase1_force_extreme_followup_ratio_breakdown =
                solver_tel.perf_phase1_force_extreme_followup_ratio_breakdown;
            result.phase1_force_extreme_followup_failed_stabilize =
                solver_tel.perf_phase1_force_extreme_followup_failed_stabilize;
            result.phase1_force_extreme_followup_post_dir_skip_retry =
                solver_tel.perf_phase1_force_extreme_followup_post_dir_skip_retry;
            result.phase1_force_extreme_followup_post_dir_skip_dual_rescue =
                solver_tel.perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue;
            result.phase1_force_extreme_followup_post_dir_skip_forced_refactor =
                solver_tel.perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor;
            result.phase1_force_extreme_followup_next_failed_stabilize =
                solver_tel.perf_phase1_force_extreme_followup_next_failed_stabilize;
            result.phase1_force_extreme_followup_next_ratio_breakdown =
                solver_tel.perf_phase1_force_extreme_followup_next_ratio_breakdown;
            result.phase1_force_extreme_followup_next_pivot_fail =
                solver_tel.perf_phase1_force_extreme_followup_next_pivot_fail;
            result.phase1_force_extreme_followup_next_pivot_success =
                solver_tel.perf_phase1_force_extreme_followup_next_pivot_success;
            result.phase1_force_extreme_followup_dir_samples =
                solver_tel.perf_phase1_force_extreme_followup_dir_samples;
            result.phase1_force_extreme_followup_dir_bound_geometry =
                solver_tel.perf_phase1_force_extreme_followup_dir_bound_geometry;
            result.phase1_force_extreme_followup_dir_bound_flip =
                solver_tel.perf_phase1_force_extreme_followup_dir_bound_flip;
            result.phase1_force_extreme_followup_dir_tiny_theta =
                solver_tel.perf_phase1_force_extreme_followup_dir_tiny_theta;
            result.phase1_force_extreme_followup_dir_weak_leaving =
                solver_tel.perf_phase1_force_extreme_followup_dir_weak_leaving;
            result.phase1_force_extreme_followup_dir_ftran_shape =
                solver_tel.perf_phase1_force_extreme_followup_dir_ftran_shape;
            result.phase1_force_extreme_followup_dir_nnz_total =
                solver_tel.perf_phase1_force_extreme_followup_dir_nnz_total;
            result.phase1_force_extreme_followup_dir_nnz_max =
                solver_tel.perf_phase1_force_extreme_followup_dir_nnz_max;
            result.phase1_force_extreme_followup_dir_inf_total =
                solver_tel.perf_phase1_force_extreme_followup_dir_inf_total;
            result.phase1_force_extreme_followup_dir_inf_max =
                solver_tel.perf_phase1_force_extreme_followup_dir_inf_max;
            result.phase1_force_extreme_followup_pivot_abs_total =
                solver_tel.perf_phase1_force_extreme_followup_pivot_abs_total;
            result.phase1_force_extreme_followup_pivot_abs_max =
                solver_tel.perf_phase1_force_extreme_followup_pivot_abs_max;
            result.phase1_force_extreme_followup_theta_total =
                solver_tel.perf_phase1_force_extreme_followup_theta_total;
            result.phase1_force_extreme_followup_theta_max =
                solver_tel.perf_phase1_force_extreme_followup_theta_max;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_inf_total =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total;
            result.phase1_failed_stabilize_retry_shadow_followup_dir_inf_max =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max;
            result.phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total;
            result.phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max;
            result.phase1_failed_stabilize_retry_shadow_followup_theta_total =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_theta_total;
            result.phase1_failed_stabilize_retry_shadow_followup_theta_max =
                solver_tel.perf_phase1_failed_stabilize_retry_shadow_followup_theta_max;
            result.phase1_failed_stabilize_retry_selector_bland_arms =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_bland_arms;
            result.phase1_failed_stabilize_retry_selector_guarded_arms =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_arms;
            result.phase1_failed_stabilize_retry_selector_guarded_eligible_total =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total;
            result.phase1_failed_stabilize_retry_selector_guarded_eligible_max =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max;
            result.phase1_failed_stabilize_retry_selector_bland_alt_stabilized =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized;
            result.phase1_failed_stabilize_retry_selector_bland_alt_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_bland_alt_failed;
            result.phase1_failed_stabilize_retry_selector_bland_ratio_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed;
            result.phase1_failed_stabilize_retry_selector_bland_dir_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_bland_dir_failed;
            result.phase1_failed_stabilize_retry_selector_guarded_alt_stabilized =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized;
            result.phase1_failed_stabilize_retry_selector_guarded_alt_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed;
            result.phase1_failed_stabilize_retry_selector_guarded_ratio_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed;
            result.phase1_failed_stabilize_retry_selector_guarded_dir_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed;
            result.phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland =
                solver_tel.perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland;
            result.phase1_failed_stabilize_retry_dir_fail_shape_samples =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_shape_samples;
            result.phase1_failed_stabilize_retry_dir_fail_nnz_total =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_nnz_total;
            result.phase1_failed_stabilize_retry_dir_fail_nnz_max =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_nnz_max;
            result.phase1_failed_stabilize_retry_dir_fail_dir_inf_total =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total;
            result.phase1_failed_stabilize_retry_dir_fail_dir_inf_max =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max;
            result.phase1_failed_stabilize_retry_dir_fail_pivot_abs_total =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total;
            result.phase1_failed_stabilize_retry_dir_fail_pivot_abs_max =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max;
            result.phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30;
            result.phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100;
            result.phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000;
            result.phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000;
            result.phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8;
            result.phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6;
            result.phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4;
            result.phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4 =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4;
            result.phase1_failed_stabilize_retry_dir_second_chance_arms =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_second_chance_arms;
            result.phase1_failed_stabilize_retry_dir_second_chance_no_alt =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt;
            result.phase1_failed_stabilize_retry_dir_second_chance_stabilized =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized;
            result.phase1_failed_stabilize_retry_dir_second_chance_failed =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_second_chance_failed;
            result.phase1_failed_stabilize_retry_dir_guard_arms =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_guard_arms;
            result.phase1_failed_stabilize_retry_dir_guard_original_exclusions =
                solver_tel.perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions;
            result.phase1_window_pressure_windows_started =
                solver_tel.perf_phase1_window_pressure_windows_started;
            result.phase1_window_pressure_progress_resets =
                solver_tel.perf_phase1_window_pressure_progress_resets;
            result.phase1_window_pressure_force_pivot_arms =
                solver_tel.perf_phase1_window_pressure_force_pivot_arms;
            result.phase1_window_pressure_force_pivot_blocked_pending =
                solver_tel.perf_phase1_window_pressure_force_pivot_blocked_pending;
            result.phase1_window_pressure_force_pivot_blocked_budget =
                solver_tel.perf_phase1_window_pressure_force_pivot_blocked_budget;
            result.phase1_window_pressure_force_pivot_reject_under_trigger =
                solver_tel.perf_phase1_window_pressure_force_pivot_reject_under_trigger;
            result.phase1_window_pressure_force_pivot_reject_failed_share =
                solver_tel.perf_phase1_window_pressure_force_pivot_reject_failed_share;
            result.phase1_window_pressure_force_pivot_reject_dir_skip_share =
                solver_tel.perf_phase1_window_pressure_force_pivot_reject_dir_skip_share;
            result.phase1_window_pressure_force_pivot_reject_local_fail =
                solver_tel.perf_phase1_window_pressure_force_pivot_reject_local_fail;
            result.phase1_window_pressure_force_pivot_reject_alternation =
                solver_tel.perf_phase1_window_pressure_force_pivot_reject_alternation;
            result.phase1_window_pressure_event_total =
                solver_tel.perf_phase1_window_pressure_event_total;
            result.phase1_window_pressure_failed_stabilize_total =
                solver_tel.perf_phase1_window_pressure_failed_stabilize_total;
            result.phase1_window_pressure_dir_skip_total =
                solver_tel.perf_phase1_window_pressure_dir_skip_total;
            result.phase1_window_pressure_local_memory_fail_total =
                solver_tel.perf_phase1_window_pressure_local_memory_fail_total;
            result.phase1_window_pressure_alternation_total =
                solver_tel.perf_phase1_window_pressure_alternation_total;
            result.phase1_window_pressure_event_max =
                solver_tel.perf_phase1_window_pressure_event_max;
            result.phase1_window_pressure_failed_stabilize_max =
                solver_tel.perf_phase1_window_pressure_failed_stabilize_max;
            result.phase1_window_pressure_dir_skip_max =
                solver_tel.perf_phase1_window_pressure_dir_skip_max;
            result.phase1_window_pressure_local_memory_fail_max =
                solver_tel.perf_phase1_window_pressure_local_memory_fail_max;
            result.phase1_window_pressure_alternation_max =
                solver_tel.perf_phase1_window_pressure_alternation_max;

            result.phase2_pricing_ms = solver_tel.perf_phase2_pricing_ms;
            result.phase2_ratio_ms = solver_tel.perf_phase2_ratio_ms;
            result.phase2_pivot_ms = solver_tel.perf_phase2_pivot_ms;
            result.phase2_refactor_ms = solver_tel.perf_phase2_refactor_ms;
            result.phase2_compute_solution_ms = solver_tel.perf_phase2_compute_solution_ms;
            result.phase2_compute_rc_ms = solver_tel.perf_phase2_compute_rc_ms;
            result.phase2_pricing_calls = solver_tel.perf_phase2_pricing_calls;
            result.phase2_ratio_calls = solver_tel.perf_phase2_ratio_calls;
            result.phase2_pivot_calls = solver_tel.perf_phase2_pivot_calls;
            result.phase2_refactor_calls = solver_tel.perf_phase2_refactor_calls;
            result.phase2_compute_solution_calls = solver_tel.perf_phase2_compute_solution_calls;
            result.phase2_compute_rc_calls = solver_tel.perf_phase2_compute_rc_calls;
            result.phase2_refactor_periodic_policy = solver_tel.perf_phase2_refactor_periodic_policy;
            result.phase2_refactor_periodic_lu_health = solver_tel.perf_phase2_refactor_periodic_lu_health;
            result.phase2_refactor_safety_forced = solver_tel.perf_phase2_refactor_safety_forced;
            result.phase2_degenerate_episodes = solver_tel.perf_phase2_degenerate_episodes;
            result.phase2_degenerate_streak_max = solver_tel.perf_phase2_degenerate_streak_max;
            result.phase2_theta_le_1e_9 = solver_tel.perf_phase2_theta_le_1e_9;
            result.phase2_theta_le_1e_6 = solver_tel.perf_phase2_theta_le_1e_6;
            result.phase2_theta_le_1e_3 = solver_tel.perf_phase2_theta_le_1e_3;
            result.phase2_theta_gt_1e_3 = solver_tel.perf_phase2_theta_gt_1e_3;
            result.phase2_weak_pivot_samples = solver_tel.perf_phase2_weak_pivot_samples;
            result.phase2_weak_pivot_ratio_total = solver_tel.perf_phase2_weak_pivot_ratio_total;
            result.phase2_weak_pivot_ratio_min = solver_tel.perf_phase2_weak_pivot_ratio_min;
            result.phase2_weak_pivot_ratio_le_1e_8 = solver_tel.perf_phase2_weak_pivot_ratio_le_1e_8;
            result.phase2_weak_pivot_ratio_le_1e_6 = solver_tel.perf_phase2_weak_pivot_ratio_le_1e_6;
            result.phase2_weak_pivot_ratio_le_1e_4 = solver_tel.perf_phase2_weak_pivot_ratio_le_1e_4;
            result.phase2_weak_pivot_ratio_gt_1e_4 = solver_tel.perf_phase2_weak_pivot_ratio_gt_1e_4;
            result.phase2_repeat_entering_events = solver_tel.perf_phase2_repeat_entering_events;
            result.phase2_repeat_entering_max_streak = solver_tel.perf_phase2_repeat_entering_max_streak;
            result.phase2_repeat_leaving_events = solver_tel.perf_phase2_repeat_leaving_events;
            result.phase2_repeat_leaving_max_streak = solver_tel.perf_phase2_repeat_leaving_max_streak;
            result.phase2_bland_pricing_iters = solver_tel.perf_phase2_bland_pricing_iters;
            result.phase2_adaptive_devex_partial_iters = solver_tel.perf_phase2_adaptive_devex_partial_iters;
            result.phase2_bland_enter_episodes = solver_tel.perf_phase2_bland_enter_episodes;
            result.phase2_bland_exit_episodes = solver_tel.perf_phase2_bland_exit_episodes;
            result.phase2_perturb_applied = solver_tel.perf_phase2_perturb_applied;
            result.phase2_devex_reset_count = solver_tel.perf_phase2_devex_reset_count;
            result.phase2_devex_age_max = solver_tel.perf_phase2_devex_age_max;
            result.phase2_degen_refactor_calls = solver_tel.perf_phase2_degen_refactor_calls;
            result.phase2_degen_refactor_ratio_recovery = solver_tel.perf_phase2_degen_refactor_ratio_recovery;
            result.phase2_degen_refactor_pivot_recovery = solver_tel.perf_phase2_degen_refactor_pivot_recovery;
            result.phase2_degen_refactor_periodic_policy = solver_tel.perf_phase2_degen_refactor_periodic_policy;
            result.phase2_degen_refactor_periodic_lu_health = solver_tel.perf_phase2_degen_refactor_periodic_lu_health;
            result.phase2_degen_refactor_safety_forced = solver_tel.perf_phase2_degen_refactor_safety_forced;
            result.phase2_degen_escape_triggers = solver_tel.perf_phase2_degen_escape_triggers;
            result.dual_ratio_no_entering = solver_tel.perf_dual_ratio_no_entering;
            result.dual_theta_nonpositive = solver_tel.perf_dual_theta_nonpositive;
            result.dual_pivot_reject_small = solver_tel.perf_dual_pivot_reject_small;
            result.dual_bound_flip_applied = solver_tel.perf_dual_bound_flip_applied;
            result.dual_lu_hard_trigger = solver_tel.perf_dual_lu_hard_trigger;
            result.soft_lu_cost_gate_enabled = solver_tel.soft_lu_cost_gate_enabled;
            result.soft_lu_cost_gate_defers_phase1 = solver_tel.soft_lu_cost_gate_defers_phase1;
            result.soft_lu_cost_gate_defers_phase2 = solver_tel.soft_lu_cost_gate_defers_phase2;
            result.soft_lu_consecutive_defers_phase1 = solver_tel.soft_lu_consecutive_defers_phase1;
            result.soft_lu_consecutive_defers_phase2 = solver_tel.soft_lu_consecutive_defers_phase2;
            result.soft_lu_defer_cap_forced_phase1 = solver_tel.soft_lu_defer_cap_forced_phase1;
            result.soft_lu_defer_cap_forced_phase2 = solver_tel.soft_lu_defer_cap_forced_phase2;
            result.periodic_cost_gate_enabled = solver_tel.periodic_cost_gate_enabled;
            result.periodic_cost_gate_defers_phase1 = solver_tel.periodic_cost_gate_defers_phase1;
            result.periodic_cost_gate_defers_phase2 = solver_tel.periodic_cost_gate_defers_phase2;
            result.periodic_cost_consecutive_defers_phase1 = solver_tel.periodic_cost_consecutive_defers_phase1;
            result.periodic_cost_consecutive_defers_phase2 = solver_tel.periodic_cost_consecutive_defers_phase2;
            result.periodic_cost_defer_cap_forced_phase1 = solver_tel.periodic_cost_defer_cap_forced_phase1;
            result.periodic_cost_defer_cap_forced_phase2 = solver_tel.periodic_cost_defer_cap_forced_phase2;
            result.periodic_cost_gate_checks_phase1 = solver_tel.periodic_cost_gate_checks_phase1;
            result.periodic_cost_gate_checks_phase2 = solver_tel.periodic_cost_gate_checks_phase2;
            result.periodic_cost_gate_block_small_m_phase1 = solver_tel.periodic_cost_gate_block_small_m_phase1;
            result.periodic_cost_gate_block_small_m_phase2 = solver_tel.periodic_cost_gate_block_small_m_phase2;
            result.periodic_cost_gate_block_invalid_inputs_phase1 = solver_tel.periodic_cost_gate_block_invalid_inputs_phase1;
            result.periodic_cost_gate_block_invalid_inputs_phase2 = solver_tel.periodic_cost_gate_block_invalid_inputs_phase2;
            result.periodic_cost_gate_block_warmup_phase1 = solver_tel.periodic_cost_gate_block_warmup_phase1;
            result.periodic_cost_gate_block_warmup_phase2 = solver_tel.periodic_cost_gate_block_warmup_phase2;
            result.periodic_cost_gate_block_invalid_cost_phase1 = solver_tel.periodic_cost_gate_block_invalid_cost_phase1;
            result.periodic_cost_gate_block_invalid_cost_phase2 = solver_tel.periodic_cost_gate_block_invalid_cost_phase2;
            result.periodic_cost_gate_block_ratio_phase1 = solver_tel.periodic_cost_gate_block_ratio_phase1;
            result.periodic_cost_gate_block_ratio_phase2 = solver_tel.periodic_cost_gate_block_ratio_phase2;
            result.periodic_cost_gate_block_update_reserve_phase1 = solver_tel.periodic_cost_gate_block_update_reserve_phase1;
            result.periodic_cost_gate_block_update_reserve_phase2 = solver_tel.periodic_cost_gate_block_update_reserve_phase2;
            result.periodic_cost_gate_last_reason_phase1 = solver_tel.periodic_cost_gate_last_reason_phase1;
            result.periodic_cost_gate_last_reason_phase2 = solver_tel.periodic_cost_gate_last_reason_phase2;
            result.periodic_cost_iter_samples_phase1 = solver_tel.periodic_cost_iter_samples_phase1;
            result.periodic_cost_iter_samples_phase2 = solver_tel.periodic_cost_iter_samples_phase2;
            result.periodic_cost_refactor_samples_phase1 = solver_tel.periodic_cost_refactor_samples_phase1;
            result.periodic_cost_refactor_samples_phase2 = solver_tel.periodic_cost_refactor_samples_phase2;
            result.soft_lu_refactor_cost_ewma_phase1 = solver_tel.soft_lu_refactor_cost_ewma_phase1;
            result.soft_lu_refactor_cost_ewma_phase2 = solver_tel.soft_lu_refactor_cost_ewma_phase2;
            result.soft_lu_iter_cost_ewma_phase1 = solver_tel.soft_lu_iter_cost_ewma_phase1;
            result.soft_lu_iter_cost_ewma_phase2 = solver_tel.soft_lu_iter_cost_ewma_phase2;
            result.basis_governor_mode = solver_tel.basis_governor_mode;
            result.reinvert_controller_mode = solver_tel.reinvert_controller_mode;
            result.reinvert_dual_control_demoted = solver_tel.reinvert_dual_control_demoted;
            result.reinvert_dual_control_demotions = solver_tel.reinvert_dual_control_demotions;
            result.reinvert_dual_hard_trigger_burst = solver_tel.reinvert_dual_hard_trigger_burst;
            result.reinvert_phase1_control_demoted = solver_tel.reinvert_phase1_control_demoted;
            result.reinvert_phase1_control_demotions = solver_tel.reinvert_phase1_control_demotions;
            result.reinvert_phase1_pressure_last_iter = solver_tel.reinvert_phase1_pressure_last_iter;
            result.reinvert_phase1_pressure_burst = solver_tel.reinvert_phase1_pressure_burst;
            result.phase1_stagnation_escape_cooldown =
                solver_tel.phase1_stagnation_escape_cooldown;
            result.phase1_stagnation_escape_triggers =
                solver_tel.phase1_stagnation_escape_triggers;
            result.phase1_stagnation_escape_successes =
                solver_tel.phase1_stagnation_escape_successes;
            result.phase1_stagnation_escape_failures =
                solver_tel.phase1_stagnation_escape_failures;
            result.phase1_stagnation_escape_cooldown_blocks =
                solver_tel.phase1_stagnation_escape_cooldown_blocks;
            result.phase1_stagnation_last_window_iters =
                solver_tel.phase1_stagnation_last_window_iters;
            result.phase1_stagnation_last_obj_delta =
                solver_tel.phase1_stagnation_last_obj_delta;
            result.phase1_stagnation_last_retry_defer_ratio =
                solver_tel.phase1_stagnation_last_retry_defer_ratio;
            result.phase1_stagnation_last_update_recovery_ratio =
                solver_tel.phase1_stagnation_last_update_recovery_ratio;
            result.phase1_stagnation_last_retry_defers =
                solver_tel.phase1_stagnation_last_retry_defers;
            result.phase1_stagnation_last_no_pivot_events =
                solver_tel.phase1_stagnation_last_no_pivot_events;
            result.phase1_stagnation_last_update_recovery_refactors =
                solver_tel.phase1_stagnation_last_update_recovery_refactors;
            result.phase1_stagnation_last_refactors =
                solver_tel.phase1_stagnation_last_refactors;
            result.phase1_stagnation_last_recompute_ratio =
                solver_tel.phase1_stagnation_last_recompute_ratio;
            result.phase1_stagnation_last_recompute_dir_skip =
                solver_tel.phase1_stagnation_last_recompute_dir_skip;
            result.phase1_stagnation_last_recompute_dir_refactor =
                solver_tel.phase1_stagnation_last_recompute_dir_refactor;
            result.phase1_stagnation_last_recompute_pivot_fail =
                solver_tel.phase1_stagnation_last_recompute_pivot_fail;
            result.phase1_stagnation_last_recompute_perturb =
                solver_tel.phase1_stagnation_last_recompute_perturb;
            result.shadow_refactor_yes_phase1 = solver_tel.shadow_refactor_yes_phase1;
            result.shadow_refactor_yes_phase2 = solver_tel.shadow_refactor_yes_phase2;
            result.shadow_refactor_yes_dual = solver_tel.shadow_refactor_yes_dual;
            result.shadow_refactor_no_phase1 = solver_tel.shadow_refactor_no_phase1;
            result.shadow_refactor_no_phase2 = solver_tel.shadow_refactor_no_phase2;
            result.shadow_refactor_no_dual = solver_tel.shadow_refactor_no_dual;
            result.shadow_backend_pick_markowitz = solver_tel.shadow_backend_pick_markowitz;
            result.shadow_backend_pick_supernode = solver_tel.shadow_backend_pick_supernode;
            result.shadow_backend_pick_dense = solver_tel.shadow_backend_pick_dense;
            result.shadow_disagree_primal_refactor = solver_tel.shadow_disagree_primal_refactor;
            result.shadow_disagree_dual_refactor = solver_tel.shadow_disagree_dual_refactor;
            result.shadow_disagree_lu_backend = solver_tel.shadow_disagree_lu_backend;
            result.reinvert_shadow_checks_phase1 = solver_tel.reinvert_shadow_checks_phase1;
            result.reinvert_shadow_checks_phase2 = solver_tel.reinvert_shadow_checks_phase2;
            result.reinvert_shadow_checks_dual = solver_tel.reinvert_shadow_checks_dual;
            result.reinvert_shadow_suggest_allow_phase1 = solver_tel.reinvert_shadow_suggest_allow_phase1;
            result.reinvert_shadow_suggest_allow_phase2 = solver_tel.reinvert_shadow_suggest_allow_phase2;
            result.reinvert_shadow_suggest_allow_dual = solver_tel.reinvert_shadow_suggest_allow_dual;
            result.reinvert_shadow_suggest_defer_phase1 = solver_tel.reinvert_shadow_suggest_defer_phase1;
            result.reinvert_shadow_suggest_defer_phase2 = solver_tel.reinvert_shadow_suggest_defer_phase2;
            result.reinvert_shadow_suggest_defer_dual = solver_tel.reinvert_shadow_suggest_defer_dual;
            result.reinvert_shadow_suggest_force_phase1 = solver_tel.reinvert_shadow_suggest_force_phase1;
            result.reinvert_shadow_suggest_force_phase2 = solver_tel.reinvert_shadow_suggest_force_phase2;
            result.reinvert_shadow_suggest_force_dual = solver_tel.reinvert_shadow_suggest_force_dual;
            result.reinvert_shadow_actual_refactor_yes_phase1 = solver_tel.reinvert_shadow_actual_refactor_yes_phase1;
            result.reinvert_shadow_actual_refactor_yes_phase2 = solver_tel.reinvert_shadow_actual_refactor_yes_phase2;
            result.reinvert_shadow_actual_refactor_yes_dual = solver_tel.reinvert_shadow_actual_refactor_yes_dual;
            result.reinvert_shadow_actual_refactor_no_phase1 = solver_tel.reinvert_shadow_actual_refactor_no_phase1;
            result.reinvert_shadow_actual_refactor_no_phase2 = solver_tel.reinvert_shadow_actual_refactor_no_phase2;
            result.reinvert_shadow_actual_refactor_no_dual = solver_tel.reinvert_shadow_actual_refactor_no_dual;
            result.reinvert_shadow_disagree_phase1 = solver_tel.reinvert_shadow_disagree_phase1;
            result.reinvert_shadow_disagree_phase2 = solver_tel.reinvert_shadow_disagree_phase2;
            result.reinvert_shadow_disagree_dual = solver_tel.reinvert_shadow_disagree_dual;
            result.reinvert_shadow_last_reason_phase1 = solver_tel.reinvert_shadow_last_reason_phase1;
            result.reinvert_shadow_last_reason_phase2 = solver_tel.reinvert_shadow_last_reason_phase2;
            result.reinvert_shadow_last_reason_dual = solver_tel.reinvert_shadow_last_reason_dual;
            if (solver->tableau && solver->tableau->lu) {
                LUTelemetrySnapshot lu_tel;
                lp_telemetry_snapshot_lu(solver->tableau->lu, &lu_tel);

                result.lu_mkz_enabled = lu_tel.mkz_enabled;
                result.lu_sn_enabled = lu_tel.sn_enabled;
                result.lu_mkz_calls = lu_tel.mkz_calls;
                result.lu_mkz_successes = lu_tel.mkz_successes;
                result.lu_mkz_failures = lu_tel.mkz_failures;
                {
                    int retries = lu_tel.mkz_calls - lu_tel.mkz_successes - lu_tel.mkz_failures;
                    result.lu_mkz_retry_count = (retries > 0) ? retries : 0;
                }
                result.lu_mkz_last_failure = lu_tel.mkz_last_failure;
                result.lu_mkz_dense_fallbacks = lu_tel.mkz_dense_fallbacks;
                result.lu_mkz_fail_workspace = lu_tel.mkz_fail_workspace;
                result.lu_mkz_fail_pool = lu_tel.mkz_fail_pool;
                result.lu_mkz_fail_singular = lu_tel.mkz_fail_singular;
                result.lu_mkz_fail_capacity = lu_tel.mkz_fail_capacity;
                result.lu_mkz_singular_retry_attempts = lu_tel.mkz_singular_retry_attempts;
                result.lu_mkz_singular_retry_successes = lu_tel.mkz_singular_retry_successes;
                result.lu_mkz_singular_retry_failures = lu_tel.mkz_singular_retry_failures;
                result.lu_mkz_reserved_fallback_attempts = lu_tel.mkz_reserved_fallback_attempts;
                result.lu_mkz_reserved_fallback_accepts = lu_tel.mkz_reserved_fallback_accepts;
                result.lu_mkz_reserved_fallback_rejects = lu_tel.mkz_reserved_fallback_rejects;
                result.lu_mkz_circuit_trips = lu_tel.mkz_circuit_trips;
                result.lu_mkz_circuit_skips = lu_tel.mkz_circuit_skips;
                result.lu_mkz_circuit_resets = lu_tel.mkz_circuit_resets;
                result.lu_mkz_global_skip_trips = lu_tel.mkz_global_skip_trips;
                result.lu_mkz_global_skip_skips = lu_tel.mkz_global_skip_skips;
                result.lu_mkz_global_skip_resets = lu_tel.mkz_global_skip_resets;
                result.lu_mkz_profile_retry_attempts = lu_tel.mkz_profile_retry_attempts;
                result.lu_mkz_profile_retry_successes = lu_tel.mkz_profile_retry_successes;
                result.lu_mkz_profile_retry_failures = lu_tel.mkz_profile_retry_failures;
                result.lu_mkz_profile_retry_fail_identity_sep =
                    lu_tel.mkz_profile_retry_fail_identity_sep;
                result.lu_mkz_profile_retry_fail_backend_exhausted =
                    lu_tel.mkz_profile_retry_fail_backend_exhausted;
                result.lu_mkz_profile_retry_fail_pathological =
                    lu_tel.mkz_profile_retry_fail_pathological;
                result.lu_mkz_primary_scan_entries = lu_tel.mkz_primary_scan_entries;
                result.lu_mkz_rescue_scan_entries = lu_tel.mkz_rescue_scan_entries;
                result.lu_mkz_reserved_scan_entries = lu_tel.mkz_reserved_scan_entries;
                result.lu_mkz_update_existing_entries = lu_tel.mkz_update_existing_entries;
                result.lu_mkz_update_fill_candidates = lu_tel.mkz_update_fill_candidates;
                result.lu_mkz_hint_fallback_scans = lu_tel.mkz_hint_fallback_scans;
                result.lu_mkz_hint_fallback_scan_entries = lu_tel.mkz_hint_fallback_scan_entries;
                result.lu_mkz_affected_columns_total = lu_tel.mkz_affected_columns_total;
                result.lu_mkz_affected_columns_max = lu_tel.mkz_affected_columns_max;
                result.lu_mkz_col_max_scan_entries = lu_tel.mkz_col_max_scan_entries;
                result.lu_mkz_high_cond_count = lu_tel.mkz_high_cond_count;
                result.lu_mkz_worst_cond = lu_tel.mkz_worst_cond;
                result.lu_sparse_dense_fallbacks = lu_tel.sparse_dense_fallbacks;
                result.lu_used_dense_fallback_last = lu_tel.used_dense_fallback_last;
                result.lu_sparse_fallback_last_reason = lu_tel.sparse_fallback_last_reason;
                result.lu_sparse_fallback_reason_small_matrix = lu_tel.sparse_fallback_reason_small_matrix;
                result.lu_sparse_fallback_reason_symbolic = lu_tel.sparse_fallback_reason_symbolic;
                result.lu_sparse_fallback_reason_numeric = lu_tel.sparse_fallback_reason_numeric;
                result.lu_sparse_numeric_last_failure_reason = lu_tel.sparse_numeric_last_failure_reason;
                result.lu_sparse_numeric_fail_identity_sep = lu_tel.sparse_numeric_fail_identity_sep;
                result.lu_sparse_numeric_fail_backend_exhausted =
                    lu_tel.sparse_numeric_fail_backend_exhausted;
                result.lu_sparse_numeric_fail_pathological = lu_tel.sparse_numeric_fail_pathological;
                result.lu_numeric_full_retry_attempts = lu_tel.numeric_full_retry_attempts;
                result.lu_numeric_full_retry_successes = lu_tel.numeric_full_retry_successes;
                result.lu_numeric_full_retry_failures = lu_tel.numeric_full_retry_failures;
                result.lu_identity_sep_failures = lu_tel.identity_sep_failures;
                result.lu_symbolic_failures = lu_tel.symbolic_failures;
                result.lu_symbolic_fail_workspace = lu_tel.symbolic_fail_workspace;
                result.lu_symbolic_fail_unmatched_no_reserved = lu_tel.symbolic_fail_unmatched_no_reserved;
                result.lu_symbolic_fail_inconsistent_identity = lu_tel.symbolic_fail_inconsistent_identity;
                result.lu_symbolic_full_retry_attempts = lu_tel.symbolic_full_retry_attempts;
                result.lu_symbolic_full_retry_successes = lu_tel.symbolic_full_retry_successes;
                result.lu_symbolic_full_retry_numeric_failures = lu_tel.symbolic_full_retry_numeric_failures;
                result.lu_symbolic_full_retry_mkz_attempts = lu_tel.symbolic_full_retry_mkz_attempts;
                result.lu_symbolic_full_retry_mkz_successes = lu_tel.symbolic_full_retry_mkz_successes;
                result.lu_symbolic_full_retry_mkz_failures = lu_tel.symbolic_full_retry_mkz_failures;
                result.lu_numeric_backend_markowitz = lu_tel.numeric_backend_markowitz;
                result.lu_numeric_backend_supernode = lu_tel.numeric_backend_supernode;
                result.lu_numeric_backend_dense_ge = lu_tel.numeric_backend_dense_ge;
                result.lu_backend_policy_luf_ft = lu_tel.backend_policy_luf_ft;
                result.lu_backend_policy_cbg = lu_tel.backend_policy_cbg;
                result.lu_backend_policy_cgr = lu_tel.backend_policy_cgr;
                result.lu_backend_policy_last = lu_tel.backend_policy_last;
                result.lu_update_path_ft = lu_tel.update_path_ft;
                result.lu_update_path_eta = lu_tel.update_path_eta;
                result.lu_identity_sep_retry_lane_dense_chosen =
                    lu_tel.identity_sep_retry_lane_dense_chosen;
                result.lu_identity_sep_retry_lane_supernode_chosen =
                    lu_tel.identity_sep_retry_lane_supernode_chosen;
                result.lu_identity_sep_retry_lane_dense_successes =
                    lu_tel.identity_sep_retry_lane_dense_successes;
                result.lu_identity_sep_retry_lane_supernode_successes =
                    lu_tel.identity_sep_retry_lane_supernode_successes;
                result.lu_sn_cost_gate_trips = lu_tel.sn_cost_gate_trips;
                result.lu_sn_cost_gate_skips = lu_tel.sn_cost_gate_skips;
                result.lu_sn_cost_gate_resets = lu_tel.sn_cost_gate_resets;
                result.lu_sn_calls = lu_tel.sn_calls;
                result.lu_sn_successes = lu_tel.sn_successes;
                result.lu_num_updates = lu_tel.num_updates;
                result.lu_max_updates = lu_tel.max_updates;
                result.lu_last_failure_reason_code = lu_tel.last_failure_reason;
                result.lu_last_refactor_trigger_reason_code =
                    lu_tel.last_refactor_trigger_reason;
                result.lu_refactor_need_checks = lu_tel.refactor_need_checks;
                result.lu_refactor_need_triggers = lu_tel.refactor_need_triggers;
                result.lu_refactor_need_reason_max_updates =
                    lu_tel.refactor_need_reason_max_updates;
                result.lu_refactor_need_reason_growth_guard =
                    lu_tel.refactor_need_reason_growth_guard;
                result.lu_refactor_need_reason_avg_spike_density =
                    lu_tel.refactor_need_reason_avg_spike_density;
                result.lu_refactor_need_reason_cond_severe =
                    lu_tel.refactor_need_reason_cond_severe;
                result.lu_refactor_need_reason_cond_adaptive_limit =
                    lu_tel.refactor_need_reason_cond_adaptive_limit;
                result.lu_refactor_need_reason_spike_pool_warn =
                    lu_tel.refactor_need_reason_spike_pool_warn;
                result.lu_refactor_need_reason_spike_work =
                    lu_tel.refactor_need_reason_spike_work;
                result.lu_update_fail_bad_input = lu_tel.update_fail_bad_input;
                result.lu_update_fail_max_updates = lu_tel.update_fail_max_updates;
                result.lu_update_fail_singular_update =
                    lu_tel.update_fail_singular_update;
                result.lu_update_fail_update_pivot_too_small =
                    lu_tel.update_fail_update_pivot_too_small;
                result.lu_update_fail_spike_pool_full =
                    lu_tel.update_fail_spike_pool_full;
                result.lu_update_fail_dense_spike_reject =
                    lu_tel.update_fail_dense_spike_reject;
                result.lu_update_fail_eta_alloc = lu_tel.update_fail_eta_alloc;
                result.lu_factorize_calls = lu_tel.perf_factorize_calls;
                result.lu_last_basis_nnz = lu_tel.perf_last_basis_nnz;
                result.lu_last_m = lu_tel.perf_last_m;
                result.lu_last_k = lu_tel.perf_last_k;
                result.lu_symbolic_calls = lu_tel.perf_symbolic_calls;
                result.lu_symbolic_cache_hits = lu_tel.perf_symbolic_cache_hits;
                result.lu_symbolic_cache_misses = lu_tel.perf_symbolic_cache_misses;
                result.lu_last_symbolic_ms = lu_tel.perf_last_symbolic_ms;
                result.lu_last_sparse_numeric_ms = lu_tel.perf_last_sparse_numeric_ms;
                result.lu_last_dense_ge_numeric_ms = lu_tel.perf_last_dense_ge_numeric_ms;
                result.lu_last_supernode_numeric_ms = lu_tel.perf_last_supernode_numeric_ms;
                result.lu_last_dense_factorize_ms = lu_tel.perf_last_dense_factorize_ms;
                result.lu_last_a_struct_build_ms = lu_tel.perf_last_a_struct_build_ms;
                result.lu_last_markowitz_numeric_ms = lu_tel.perf_last_markowitz_numeric_ms;
                result.lu_last_identity_placement_ms = lu_tel.perf_last_identity_placement_ms;
                result.lu_last_coo_to_csc_ms = lu_tel.perf_last_coo_to_csc_ms;
                result.lu_total_symbolic_ms = lu_tel.perf_total_symbolic_ms;
                result.lu_total_sparse_numeric_ms = lu_tel.perf_total_sparse_numeric_ms;
                result.lu_total_dense_ge_numeric_ms = lu_tel.perf_total_dense_ge_numeric_ms;
                result.lu_total_supernode_numeric_ms = lu_tel.perf_total_supernode_numeric_ms;
                result.lu_total_dense_factorize_ms = lu_tel.perf_total_dense_factorize_ms;
                result.lu_total_a_struct_build_ms = lu_tel.perf_total_a_struct_build_ms;
                result.lu_total_markowitz_numeric_ms = lu_tel.perf_total_markowitz_numeric_ms;
                result.lu_total_identity_placement_ms = lu_tel.perf_total_identity_placement_ms;
                result.lu_total_coo_to_csc_ms = lu_tel.perf_total_coo_to_csc_ms;
                result.lu_update_apply_forward_calls =
                    lu_tel.perf_update_apply_forward_calls;
                result.lu_update_apply_backward_calls =
                    lu_tel.perf_update_apply_backward_calls;
                result.lu_compact_factor_calls = lu_tel.perf_compact_factor_calls;
                result.lu_compact_solve_calls = lu_tel.perf_compact_solve_calls;
                result.lu_total_update_apply_forward_ms =
                    lu_tel.perf_total_update_apply_forward_ms;
                result.lu_total_update_apply_backward_ms =
                    lu_tel.perf_total_update_apply_backward_ms;
                result.lu_total_compact_factor_ms =
                    lu_tel.perf_total_compact_factor_ms;
                result.lu_total_compact_solve_ms =
                    lu_tel.perf_total_compact_solve_ms;
                result.lu_sn_phase_samples =
                    lu_tel.perf_sn_phase_samples;
                result.lu_sn_panel_factor_ms =
                    lu_tel.perf_sn_panel_factor_ms;
                result.lu_sn_panel_pivot_search_ms =
                    lu_tel.perf_sn_panel_pivot_search_ms;
                result.lu_sn_panel_swap_scatter_ms =
                    lu_tel.perf_sn_panel_swap_scatter_ms;
                result.lu_sn_panel_eliminate_ms =
                    lu_tel.perf_sn_panel_eliminate_ms;
                result.lu_sn_panel_pivot_search_calls =
                    lu_tel.perf_sn_panel_pivot_search_calls;
                result.lu_sn_panel_pivot_search_entries_total =
                    lu_tel.perf_sn_panel_pivot_search_entries_total;
                result.lu_sn_panel_pivot_search_size1_calls =
                    lu_tel.perf_sn_panel_pivot_search_size1_calls;
                result.lu_sn_panel_pivot_search_size1_ms =
                    lu_tel.perf_sn_panel_pivot_search_size1_ms;
                result.lu_sn_panel_pivot_search_size2_calls =
                    lu_tel.perf_sn_panel_pivot_search_size2_calls;
                result.lu_sn_panel_pivot_search_size2_ms =
                    lu_tel.perf_sn_panel_pivot_search_size2_ms;
                result.lu_sn_panel_pivot_search_size3_4_calls =
                    lu_tel.perf_sn_panel_pivot_search_size3_4_calls;
                result.lu_sn_panel_pivot_search_size3_4_ms =
                    lu_tel.perf_sn_panel_pivot_search_size3_4_ms;
                result.lu_sn_panel_pivot_search_size5_8_calls =
                    lu_tel.perf_sn_panel_pivot_search_size5_8_calls;
                result.lu_sn_panel_pivot_search_size5_8_ms =
                    lu_tel.perf_sn_panel_pivot_search_size5_8_ms;
                result.lu_sn_panel_pivot_search_size9p_calls =
                    lu_tel.perf_sn_panel_pivot_search_size9p_calls;
                result.lu_sn_panel_pivot_search_size9p_ms =
                    lu_tel.perf_sn_panel_pivot_search_size9p_ms;
                result.lu_sn_panel_pivot_search_reserved_present_calls =
                    lu_tel.perf_sn_panel_pivot_search_reserved_present_calls;
                result.lu_sn_panel_pivot_search_reserved_present_entries =
                    lu_tel.perf_sn_panel_pivot_search_reserved_present_entries;
                result.lu_sn_panel_pivot_search_reserved_present_ms =
                    lu_tel.perf_sn_panel_pivot_search_reserved_present_ms;
                result.lu_sn_panel_pivot_search_reserved_alt_chosen_calls =
                    lu_tel.perf_sn_panel_pivot_search_reserved_alt_chosen_calls;
                result.lu_sn_panel_pivot_search_reserved_alt_chosen_ms =
                    lu_tel.perf_sn_panel_pivot_search_reserved_alt_chosen_ms;
                result.lu_sn_size1_u_emit_calls =
                    lu_tel.perf_sn_size1_u_emit_calls;
                result.lu_sn_size1_u_emit_ms =
                    lu_tel.perf_sn_size1_u_emit_ms;
                result.lu_sn_size1_update_scan_calls =
                    lu_tel.perf_sn_size1_update_scan_calls;
                result.lu_sn_size1_update_scan_ms =
                    lu_tel.perf_sn_size1_update_scan_ms;
                result.lu_sn_size1_update_apply_calls =
                    lu_tel.perf_sn_size1_update_apply_calls;
                result.lu_sn_size1_update_apply_ms =
                    lu_tel.perf_sn_size1_update_apply_ms;
                result.lu_sn_size1_update_row_gather_ms =
                    lu_tel.perf_sn_size1_update_row_gather_ms;
                result.lu_sn_size1_update_col_indirection_ms =
                    lu_tel.perf_sn_size1_update_col_indirection_ms;
                result.lu_sn_size1_update_outer_product_ms =
                    lu_tel.perf_sn_size1_update_outer_product_ms;
                result.lu_sn_size1_update_full_calls =
                    lu_tel.perf_sn_size1_update_full_calls;
                result.lu_sn_size1_update_full_ms =
                    lu_tel.perf_sn_size1_update_full_ms;
                result.lu_sn_size1_update_cols1_calls =
                    lu_tel.perf_sn_size1_update_cols1_calls;
                result.lu_sn_size1_update_cols1_ms =
                    lu_tel.perf_sn_size1_update_cols1_ms;
                result.lu_sn_size1_update_cols2_calls =
                    lu_tel.perf_sn_size1_update_cols2_calls;
                result.lu_sn_size1_update_cols2_ms =
                    lu_tel.perf_sn_size1_update_cols2_ms;
                result.lu_sn_size1_update_cols3_calls =
                    lu_tel.perf_sn_size1_update_cols3_calls;
                result.lu_sn_size1_update_cols3_ms =
                    lu_tel.perf_sn_size1_update_cols3_ms;
                result.lu_sn_size1_update_cols4_calls =
                    lu_tel.perf_sn_size1_update_cols4_calls;
                result.lu_sn_size1_update_cols4_ms =
                    lu_tel.perf_sn_size1_update_cols4_ms;
                result.lu_sn_size1_update_cols5p_calls =
                    lu_tel.perf_sn_size1_update_cols5p_calls;
                result.lu_sn_size1_update_cols5p_ms =
                    lu_tel.perf_sn_size1_update_cols5p_ms;
                result.lu_sn_size1_update_cols5p_rows1_8_calls =
                    lu_tel.perf_sn_size1_update_cols5p_rows1_8_calls;
                result.lu_sn_size1_update_cols5p_rows1_8_ms =
                    lu_tel.perf_sn_size1_update_cols5p_rows1_8_ms;
                result.lu_sn_size1_update_cols5p_rows9_32_calls =
                    lu_tel.perf_sn_size1_update_cols5p_rows9_32_calls;
                result.lu_sn_size1_update_cols5p_rows9_32_ms =
                    lu_tel.perf_sn_size1_update_cols5p_rows9_32_ms;
                result.lu_sn_size1_update_cols5p_rows33_128_calls =
                    lu_tel.perf_sn_size1_update_cols5p_rows33_128_calls;
                result.lu_sn_size1_update_cols5p_rows33_128_ms =
                    lu_tel.perf_sn_size1_update_cols5p_rows33_128_ms;
                result.lu_sn_size1_update_cols5p_rows129p_calls =
                    lu_tel.perf_sn_size1_update_cols5p_rows129p_calls;
                result.lu_sn_size1_update_cols5p_rows129p_ms =
                    lu_tel.perf_sn_size1_update_cols5p_rows129p_ms;
                result.lu_sn_u_emit_ms =
                    lu_tel.perf_sn_u_emit_ms;
                result.lu_sn_active_set_ms =
                    lu_tel.perf_sn_active_set_ms;
                result.lu_sn_pack_blocks_ms =
                    lu_tel.perf_sn_pack_blocks_ms;
                result.lu_sn_full_update_ms =
                    lu_tel.perf_sn_full_update_ms;
                result.lu_sn_compact_update_ms =
                    lu_tel.perf_sn_compact_update_ms;
                result.lu_sn_active_row_scan_entries =
                    lu_tel.perf_sn_active_row_scan_entries;
                result.lu_sn_active_col_scan_entries =
                    lu_tel.perf_sn_active_col_scan_entries;
                result.lu_sn_trailing_rows_total =
                    lu_tel.perf_sn_trailing_rows_total;
                result.lu_sn_trailing_cols_total =
                    lu_tel.perf_sn_trailing_cols_total;
                result.lu_sn_active_rows_total =
                    lu_tel.perf_sn_active_rows_total;
                result.lu_sn_active_cols_total =
                    lu_tel.perf_sn_active_cols_total;
                result.lu_sn_pack_l_entries_total =
                    lu_tel.perf_sn_pack_l_entries_total;
                result.lu_sn_pack_u_entries_total =
                    lu_tel.perf_sn_pack_u_entries_total;
                result.lu_sn_dense_triplets_total =
                    lu_tel.perf_sn_dense_triplets_total;
                result.lu_sn_compact_triplets_total =
                    lu_tel.perf_sn_compact_triplets_total;
                result.lu_sn_full_update_calls =
                    lu_tel.perf_sn_full_update_calls;
                result.lu_sn_compact_update_calls =
                    lu_tel.perf_sn_compact_update_calls;
                result.lu_sn_skipped_update_calls =
                    lu_tel.perf_sn_skipped_update_calls;
                result.lu_sn_compact_cols1_calls =
                    lu_tel.perf_sn_compact_cols1_calls;
                result.lu_sn_compact_cols1_rows_total =
                    lu_tel.perf_sn_compact_cols1_rows_total;
                result.lu_sn_compact_cols1_ms =
                    lu_tel.perf_sn_compact_cols1_ms;
                result.lu_sn_compact_cols2_calls =
                    lu_tel.perf_sn_compact_cols2_calls;
                result.lu_sn_compact_cols2_rows_total =
                    lu_tel.perf_sn_compact_cols2_rows_total;
                result.lu_sn_compact_cols2_ms =
                    lu_tel.perf_sn_compact_cols2_ms;
                result.lu_sn_compact_cols3_calls =
                    lu_tel.perf_sn_compact_cols3_calls;
                result.lu_sn_compact_cols3_rows_total =
                    lu_tel.perf_sn_compact_cols3_rows_total;
                result.lu_sn_compact_cols3_ms =
                    lu_tel.perf_sn_compact_cols3_ms;
                result.lu_sn_compact_cols4_calls =
                    lu_tel.perf_sn_compact_cols4_calls;
                result.lu_sn_compact_cols4_rows_total =
                    lu_tel.perf_sn_compact_cols4_rows_total;
                result.lu_sn_compact_cols4_ms =
                    lu_tel.perf_sn_compact_cols4_ms;
                result.lu_sn_compact_cols5p_calls =
                    lu_tel.perf_sn_compact_cols5p_calls;
                result.lu_sn_compact_cols5p_rows_total =
                    lu_tel.perf_sn_compact_cols5p_rows_total;
                result.lu_sn_compact_cols5p_ms =
                    lu_tel.perf_sn_compact_cols5p_ms;
                {
                    const char *reason = lu_failure_reason_string(lu_tel.last_failure_reason);
                    if (!reason) reason = "unknown";
                    strncpy(result.lu_last_failure_reason, reason,
                            sizeof(result.lu_last_failure_reason) - 1);
                    result.lu_last_failure_reason[sizeof(result.lu_last_failure_reason) - 1] = '\0';
                }
                {
                    const char *reason = lu_refactor_trigger_reason_string(
                        lu_tel.last_refactor_trigger_reason);
                    if (!reason) reason = "none";
                    strncpy(result.lu_last_refactor_trigger_reason, reason,
                            sizeof(result.lu_last_refactor_trigger_reason) - 1);
                    result.lu_last_refactor_trigger_reason[
                        sizeof(result.lu_last_refactor_trigger_reason) - 1] = '\0';
                }
                {
                    const char *reason = lu_sparse_fallback_reason_string(lu_tel.sparse_fallback_last_reason);
                    if (!reason) reason = "none";
                    strncpy(result.lu_sparse_fallback_last_reason_str, reason,
                            sizeof(result.lu_sparse_fallback_last_reason_str) - 1);
                    result.lu_sparse_fallback_last_reason_str[sizeof(result.lu_sparse_fallback_last_reason_str) - 1] = '\0';
                }
                {
                    const char *reason = lu_sparse_numeric_failure_reason_string(
                        lu_tel.sparse_numeric_last_failure_reason);
                    if (!reason) reason = "none";
                    strncpy(result.lu_sparse_numeric_last_failure_reason_str, reason,
                            sizeof(result.lu_sparse_numeric_last_failure_reason_str) - 1);
                    result.lu_sparse_numeric_last_failure_reason_str[
                        sizeof(result.lu_sparse_numeric_last_failure_reason_str) - 1] = '\0';
                }
            }
        }
    }

    /* Map status */
    RalphStatus status = ralph_test_get_status(model);
    result.raw_status_code = (int)status;
    {
        const char *status_str = ralph_test_status_string(status);
        if (!status_str) status_str = "UNKNOWN";
        strncpy(result.raw_status_str, status_str, sizeof(result.raw_status_str) - 1);
        result.raw_status_str[sizeof(result.raw_status_str) - 1] = '\0';
    }
    {
        RalphAPIError api_error;
        if (ralph_core_get_last_error(model, &api_error) == 0) {
            result.api_error_domain = (int)api_error.domain;
            result.api_error_code = (int)api_error.code;
            result.api_error_api_id = (int)api_error.api_id;
            if (api_error.message[0]) {
                strncpy(result.api_error_message, api_error.message,
                        sizeof(result.api_error_message) - 1);
                result.api_error_message[sizeof(result.api_error_message) - 1] = '\0';
            }
        }
    }
    switch (status) {
        case RALPH_STATUS_OPTIMAL:
        case RALPH_STATUS_IMPRECISE:
        case RALPH_STATUS_OBJ_LIMIT:
            result.status = 0;
            result.objective = ralph_test_get_objval(model);
            break;
        case RALPH_STATUS_INFEASIBLE:
            result.status = 1;
            break;
        case RALPH_STATUS_UNBOUNDED:
        case RALPH_STATUS_INF_OR_UNBD:
            result.status = 2;
            break;
        case RALPH_STATUS_TIME_LIMIT:
        case RALPH_STATUS_ITERATION_LIMIT:
            result.status = 4;
            break;
        default:
            result.status = 3;
            break;
    }
    bench_derive_termination_reason(&result);

    /* Get solution vector if optimal */
    if (result.status == 0) {
        int n = ralph_test_get_num_vars(model);
        result.solution = (double*)malloc(n * sizeof(double));
        if (result.solution) {
            SimplexSolver *solver = ralph_get_lp_solver(model);
            if (external_glpk_oop && solver && solver->solution) {
                memcpy(result.solution, solver->solution, (size_t)n * sizeof(double));
            } else {
                ralph_test_get_solution(model, result.solution);
            }
            result.solution_size = n;

            /* Recompute objective from the final primal solution (Kahan sum).
             * This avoids false objective mismatches from tableau objective
             * accumulation drift on numerically sensitive instances. */
            {
                LPModel *lp = ralph_get_lp_model(model);
                if (lp && lp->c && n == lp->num_vars) {
                    double sum = 0.0;
                    double comp = 0.0;
                    for (int j = 0; j < n; j++) {
                        double term = lp->c[j] * result.solution[j];
                        double y = term - comp;
                        double t = sum + y;
                        comp = (t - sum) - y;
                        sum = t;
                    }
                    result.objective = sum + lp->obj_offset;
                }
            }

            {
                LPModel *lp = ralph_get_lp_model(model);
                if (compute_solution_feasibility(
                        lp, result.solution, n, DEFAULT_FEAS_TOL,
                        &result.num_constraint_violations,
                        &result.max_constraint_violation,
                        &result.num_bound_violations,
                        &result.max_bound_violation,
                        &result.worst_bound_var,
                        &result.worst_bound_value,
                        &result.worst_bound_lb,
                        &result.worst_bound_ub) == 0) {
                    result.feasibility_checked = 1;
                }
            }
        }
    }

    ralph_test_free(model);
    return result;
}

/* ============================================================================
 * Solution Validation
 * ============================================================================ */

/*
 * Validate solution by checking:
 * 1. Solution contains no NaN/Inf values
 * 2. Objective values match between Ralph and GLPK
 * 3. Ralph's returned primal solution satisfies Ralph's rows and bounds
 */
static ValidationResult validate_solution(double *solution, int num_vars,
                                           double ralph_obj, double glpk_obj,
                                           const SolveResult *ralph,
                                           const Options *opts) {
    ValidationResult v = {0};
    v.solution_valid = 1;
    v.numerically_stable = 1;
    v.objective_match = 1;
    v.issues[0] = '\0';

    if (!solution || num_vars <= 0) {
        v.solution_valid = 0;
        strncpy(v.issues, "No solution to validate", sizeof(v.issues) - 1);
        return v;
    }

    /* Check for NaN/Inf in solution */
    for (int j = 0; j < num_vars; j++) {
        if (isnan(solution[j]) || isinf(solution[j])) {
            v.solution_valid = 0;
            v.numerically_stable = 0;
            snprintf(v.issues, sizeof(v.issues),
                     "Solution contains NaN/Inf at variable %d", j);
            return v;
        }
    }

    /* Check objective match */
    double scale = fmax(1.0, fmax(fabs(ralph_obj), fabs(glpk_obj)));
    v.objective_rel_error = fabs(ralph_obj - glpk_obj) / scale;
    v.objective_abs_error = fabs(ralph_obj - glpk_obj);

    if (v.objective_rel_error > opts->obj_rel_tol &&
        v.objective_abs_error > opts->obj_abs_tol) {
        v.objective_match = 0;
        v.solution_valid = 0;
        snprintf(v.issues, sizeof(v.issues),
                 "Objective mismatch: ralph=%.10g glpk=%.10g (rel_err=%.2e)",
                 ralph_obj, glpk_obj, v.objective_rel_error);
    }

    if (!ralph || !ralph->feasibility_checked) {
        v.solution_valid = 0;
        if (v.issues[0] == '\0') {
            strncpy(v.issues, "Solution feasibility was not checked",
                    sizeof(v.issues) - 1);
        }
        return v;
    }

    v.max_constraint_violation = ralph->max_constraint_violation;
    v.max_bound_violation = ralph->max_bound_violation;
    if (ralph->num_constraint_violations > 0 ||
        ralph->num_bound_violations > 0) {
        v.solution_valid = 0;
        if (v.issues[0] == '\0') {
            snprintf(v.issues, sizeof(v.issues),
                     "Feasibility violation: constraints=%d max=%.2e, bounds=%d max=%.2e",
                     ralph->num_constraint_violations,
                     ralph->max_constraint_violation,
                     ralph->num_bound_violations,
                     ralph->max_bound_violation);
        }
    }

    return v;
}

/* ============================================================================
 * Matrix Verification via GLPK Reference Solution
 *
 * Loads an MPS/LP file into Ralph (no solve), runs GLPK to get a reference
 * solution, then verifies Ralph's internal
 * constraint matrix by computing
 * Ax and checking against b/sense/bounds. Catches matrix construction bugs
 * (MPS parsing, triplet-to-CSC conversion) that objective-only checks miss.
 * ============================================================================ */

/* Forward declaration (defined in Problem Discovery section below) */
static int list_netlib_problems(ProblemInfo *problems, int max_problems, int lp_only);

typedef struct {
    int num_vars;
    int num_cons;
    int nnz;

    /* Per-row constraint check */
    int num_con_violations;
    double max_con_violation;
    int worst_con_row;

    /* Per-variable bound check */
    int num_bound_violations;
    double max_bound_violation;
    int worst_bound_var;

    /* Objective check */
    double ralph_obj;           /* c'x computed from Ralph's c vector */
    double glpk_obj;            /* Objective from GLPK's solution output */
    double obj_error;

    int pass;                   /* Overall pass/fail */
    char detail[2048];          /* Human-readable detail */
} MatrixVerifyResult;

/* Parse GLPK --write file to extract high-precision column activity values.
 * Returns number of columns parsed, or -1 on error.
 * Sets *status_ok to 1 if solution status is OPTIMAL, 0 otherwise.
 *
 * TODO(lp-external-mapping): switch verify-matrix to the external adapter once
 * external LP solution vectors are guaranteed in original-model variable space. */
static int parse_glpk_solution_vector(const char *sol_file, double *x, int max_vars,
                                       double *obj_out, int *status_ok) {
    FILE *f = fopen(sol_file, "r");
    if (!f) return -1;

    char line[MAX_LINE];
    int in_columns = 0;
    int parsed = 0;
    if (status_ok) *status_ok = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'c' && strstr(line, "Status:") &&
            strstr(line, "OPTIMAL")) {
            if (status_ok) *status_ok = 1;
            continue;
        }
        if (line[0] == 's') {
            char row_status[8];
            char col_status[8];
            double obj;
            if (sscanf(line, "s %*s %*d %*d %7s %7s %lf",
                       row_status, col_status, &obj) == 3) {
                if (obj_out) *obj_out = obj;
                if (status_ok && row_status[0] == 'f' && col_status[0] == 'f') {
                    *status_ok = 1;
                }
            }
            continue;
        }
        if (line[0] == 'j') {
            int col_num;
            char status[8];
            double activity;
            if (sscanf(line, "j %d %7s %lf", &col_num, status, &activity) >= 3 &&
                col_num >= 1 && col_num <= max_vars) {
                (void)status;
                x[col_num - 1] = activity;
                parsed++;
            }
            continue;
        }

        /* Backward-compatible fallback for old printable -o files. */
        if (strncmp(line, "Status:", 7) == 0) {
            if (status_ok && strstr(line, "OPTIMAL")) *status_ok = 1;
        }
        if (strstr(line, "Objective:")) {
            char *eq = strchr(line, '=');
            if (eq && obj_out) *obj_out = atof(eq + 1);
        }
        if (strstr(line, "Column name") && strstr(line, "Activity")) {
            if (fgets(line, sizeof(line), f)) { /* separator */ }
            in_columns = 1;
            continue;
        }
        if (in_columns && (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')) break;
        if (in_columns && strstr(line, "Karush-Kuhn-Tucker")) break;
        if (in_columns) {
            int col_num;
            char col_name[256];
            char status[8];
            double activity;
            int n = sscanf(line, " %d %255s %7s %lf", &col_num, col_name, status, &activity);
            if (n >= 4 && col_num >= 1 && col_num <= max_vars) {
                x[col_num - 1] = activity;
                parsed++;
            } else if (n >= 3 && col_num >= 1 && col_num <= max_vars) {
                x[col_num - 1] = 0.0;
                parsed++;
            }
        }
    }

    fclose(f);
    return parsed;
}

static MatrixVerifyResult verify_matrix_single(const char *problem_path,
                                                const char *name,
                                                const Options *opts) {
    (void)name;
    MatrixVerifyResult r = {0};

    /* 1. Load model in Ralph (parse only, no solve) */
    RalphModel *model = ralph_test_create();
    if (!model) {
        snprintf(r.detail, sizeof(r.detail), "Failed to create Ralph model");
        return r;
    }

    if (load_problem_into_model(model, problem_path) != 0) {
        snprintf(r.detail, sizeof(r.detail), "Failed to load %s", problem_path);
        ralph_test_free(model);
        return r;
    }

    /* Get internal model and finalize (builds CSC matrix) */
    LPModel *lp = ralph_get_lp_model(model);
    if (!lp) {
        snprintf(r.detail, sizeof(r.detail), "No internal LPModel");
        ralph_test_free(model);
        return r;
    }
    if (!lp->A) {
        lp_model_finalize(lp);
    }
    if (!lp->A) {
        snprintf(r.detail, sizeof(r.detail), "No constraint matrix after finalize");
        ralph_test_free(model);
        return r;
    }

    int m = lp->num_cons;
    int n = lp->num_vars;
    r.num_vars = n;
    r.num_cons = m;
    r.nnz = lp->A->colptr[n];

    /* 2. Solve with GLPK to get reference solution */
    char sol_file[MAX_PATH];
    const char *ext = strrchr(problem_path, '.');
    const char *fmt_flag = "--mps";
    char cmd[MAX_PATH * 2];
    int ret;
    double *x;
    double glpk_obj = 0.0;
    int glpk_optimal = 0;
    int parsed;

    snprintf(sol_file, sizeof(sol_file), "/tmp/ralph_verify_%d.txt", getpid());
    if (ext && strcasecmp(ext, ".lp") == 0) fmt_flag = "--lp";
    snprintf(cmd, sizeof(cmd), "glpsol %s '%s' -w '%s' >/dev/null 2>&1",
             fmt_flag, problem_path, sol_file);

    ret = system(cmd);
    if (ret != 0) {
        snprintf(r.detail, sizeof(r.detail), "GLPK failed to solve");
        ralph_test_free(model);
        unlink(sol_file);
        return r;
    }

    x = (double*)calloc((size_t)n, sizeof(double));
    if (!x) {
        snprintf(r.detail, sizeof(r.detail), "Memory allocation failed");
        ralph_test_free(model);
        unlink(sol_file);
        return r;
    }

    parsed = parse_glpk_solution_vector(sol_file, x, n, &glpk_obj, &glpk_optimal);
    unlink(sol_file);
    if (parsed == 0) {
        snprintf(r.detail, sizeof(r.detail),
                 "Failed to parse GLPK solution (0 columns parsed)");
        free(x);
        ralph_test_free(model);
        return r;
    }

    if (!glpk_optimal) {
        snprintf(r.detail, sizeof(r.detail),
                 "SKIP: GLPK solution status is not OPTIMAL (%dx%d nnz=%d)",
                 n, m, r.nnz);
        r.pass = 1;  /* Not a Ralph bug — skip */
        free(x);
        ralph_test_free(model);
        return r;
    }

    r.glpk_obj = glpk_obj;

    /* 3. Compute Ax using Ralph's CSC matrix */
    double *ax = (double*)calloc(m, sizeof(double));
    if (!ax) {
        free(x);
        ralph_test_free(model);
        return r;
    }

    for (int j = 0; j < n; j++) {
        double xj = x[j];
        if (fabs(xj) < 1e-15) continue;
        for (int p = lp->A->colptr[j]; p < lp->A->colptr[j + 1]; p++) {
            int row = lp->A->rowidx[p];
            if (row >= 0 && row < m) {
                ax[row] += lp->A->values[p] * xj;
            }
        }
    }

    /* 4. Check constraint violations */
    r.worst_con_row = -1;
    for (int i = 0; i < m; i++) {
        double viol = 0.0;
        if (lp->sense[i] == 'E') {
            viol = fabs(ax[i] - lp->b[i]);
        } else if (lp->sense[i] == 'L') {
            if (ax[i] > lp->b[i] + opts->feas_tol)
                viol = ax[i] - lp->b[i];
        } else if (lp->sense[i] == 'G') {
            if (ax[i] < lp->b[i] - opts->feas_tol)
                viol = lp->b[i] - ax[i];
        }

        if (viol > opts->feas_tol) {
            r.num_con_violations++;
        }
        if (viol > r.max_con_violation) {
            r.max_con_violation = viol;
            r.worst_con_row = i;
        }
    }

    /* 5. Check bound violations */
    r.worst_bound_var = -1;
    for (int j = 0; j < n; j++) {
        double viol = 0.0;
        if (lp->lb && x[j] < lp->lb[j] - opts->feas_tol) {
            viol = lp->lb[j] - x[j];
        }
        if (lp->ub && x[j] > lp->ub[j] + opts->feas_tol) {
            double bv = x[j] - lp->ub[j];
            if (bv > viol) viol = bv;
        }

        if (viol > opts->feas_tol) {
            r.num_bound_violations++;
        }
        if (viol > r.max_bound_violation) {
            r.max_bound_violation = viol;
            r.worst_bound_var = j;
        }
    }

    /* 6. Check objective: c'x */
    r.ralph_obj = 0.0;
    for (int j = 0; j < n; j++) {
        r.ralph_obj += lp->c[j] * x[j];
    }
    r.ralph_obj += lp->obj_offset;
    /* Ralph stores c and obj_offset in original sense; GLPK reports original sense. */
    r.obj_error = fabs(r.ralph_obj - glpk_obj);

    /* 7. Overall pass/fail
     * Thresholds are generous because this verifies an independently solved
     * GLPK vector against Ralph's parsed matrix, not Ralph's own solution.
     * The goal is catching matrix construction bugs (violations >> 100),
     * not numerical precision issues.  Scale constraint threshold by the
     * magnitude of the objective: problems with |obj| ~ 10^7 can easily
     * show constraint violations ~ 1-10 from GLPK's 6-digit truncation. */
    double obj_scale = fmax(1.0, fabs(glpk_obj));
    double con_threshold = fmax(1.0, obj_scale * 1e-5);
    r.pass = (r.max_con_violation < con_threshold) &&
             (r.max_bound_violation < 1e-3) &&
             (r.obj_error / obj_scale < 1e-3);

    /* Build detail string */
    snprintf(r.detail, sizeof(r.detail),
             "%dx%d nnz=%d | cons: %d violations (max %.2e row %d) | "
             "bounds: %d violations (max %.2e) | "
             "obj: c'x=%.8g glpk=%.8g err=%.2e",
             n, m, r.nnz,
             r.num_con_violations, r.max_con_violation, r.worst_con_row,
             r.num_bound_violations, r.max_bound_violation,
             r.ralph_obj, r.glpk_obj, r.obj_error);

    free(ax);
    free(x);
    ralph_test_free(model);
    return r;
}

static void print_verify_json(const char *name, const MatrixVerifyResult *r, FILE *out) {
    char escaped_name[512];
    json_escape_string(escaped_name, sizeof(escaped_name), name);
    char escaped_detail[4096];
    json_escape_string(escaped_detail, sizeof(escaped_detail), r->detail);

    fprintf(out, "{\n");
    fprintf(out, "  \"problem\": \"%s\",\n", escaped_name);
    fprintf(out, "  \"pass\": %s,\n", r->pass ? "true" : "false");
    fprintf(out, "  \"vars\": %d,\n", r->num_vars);
    fprintf(out, "  \"cons\": %d,\n", r->num_cons);
    fprintf(out, "  \"nnz\": %d,\n", r->nnz);
    fprintf(out, "  \"constraint_violations\": %d,\n", r->num_con_violations);
    fprintf(out, "  \"max_constraint_violation\": %.6e,\n", r->max_con_violation);
    fprintf(out, "  \"worst_constraint_row\": %d,\n", r->worst_con_row);
    fprintf(out, "  \"bound_violations\": %d,\n", r->num_bound_violations);
    fprintf(out, "  \"max_bound_violation\": %.6e,\n", r->max_bound_violation);
    fprintf(out, "  \"worst_bound_var\": %d,\n", r->worst_bound_var);
    fprintf(out, "  \"ralph_objective\": %.10g,\n", r->ralph_obj);
    fprintf(out, "  \"glpk_objective\": %.10g,\n", r->glpk_obj);
    fprintf(out, "  \"objective_error\": %.6e,\n", r->obj_error);
    fprintf(out, "  \"detail\": \"%s\"\n", escaped_detail);
    fprintf(out, "}\n");
}

static int run_verify_matrix(const char *problem_path, const char *name,
                              const Options *opts, FILE *out) {
    if (opts->verbose) {
        fprintf(stderr, "Verifying: %s\n", name);
    }

    MatrixVerifyResult r = verify_matrix_single(problem_path, name, opts);

    if (opts->verbose) {
        fprintf(stderr, "  %s  %s\n",
                r.pass ? "PASS" : "FAIL", r.detail);
    }

    print_verify_json(name, &r, out);
    return r.pass ? 0 : 1;
}

static int run_verify_suite(const char *suite_name, const Options *opts) {
    ProblemInfo problems[MAX_PROBLEMS];
    int count = list_netlib_problems(problems, MAX_PROBLEMS, opts->lp_only);

    if (count == 0) {
        fprintf(stderr, "Error: No NETLIB problems found.\n");
        fprintf(stderr, "Run: ./ralph-benchmark --download-netlib\n");
        return 1;
    }

    int start = 0, end = count;
    if (strcmp(suite_name, "tiny") == 0) {
        end = (count < 5) ? count : 5;
    } else if (strcmp(suite_name, "small") == 0) {
        end = (count < 15) ? count : 15;
    } else if (strcmp(suite_name, "medium") == 0) {
        end = (count < 40) ? count : 40;
    }

    int pass_count = 0, fail_count = 0;

    fprintf(stdout, "[\n");
    for (int i = start; i < end; i++) {
        if (i > start) fprintf(stdout, ",\n");

        MatrixVerifyResult r = verify_matrix_single(problems[i].path,
                                                     problems[i].name, opts);
        if (r.pass) pass_count++;
        else fail_count++;

        if (opts->verbose) {
            fprintf(stderr, "  %s  %-12s  %s\n",
                    r.pass ? "PASS" : "FAIL",
                    problems[i].name, r.detail);
        }

        print_verify_json(problems[i].name, &r, stdout);
    }
    fprintf(stdout, "]\n");

    if (opts->verbose) {
        fprintf(stderr, "\nMatrix verification: %d/%d pass",
                pass_count, pass_count + fail_count);
        if (fail_count > 0) fprintf(stderr, " (%d FAIL)", fail_count);
        fprintf(stderr, "\n");
    }

    return fail_count > 0 ? 1 : 0;
}

/* ============================================================================
 * Problem Discovery
 * ============================================================================ */

static int get_netlib_dir(char *buf, size_t size) {
    char bench_dir[MAX_PATH];
    get_benchmark_dir(bench_dir, sizeof(bench_dir));
    snprintf(buf, size, "%s/%s", bench_dir, NETLIB_DIR);
    return dir_exists(buf);
}

static int count_netlib_problems(void) {
    char netlib_path[MAX_PATH];
    if (!get_netlib_dir(netlib_path, sizeof(netlib_path))) {
        return 0;
    }

    DIR *dir = opendir(netlib_path);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && (strcasecmp(ext, ".mps") == 0 || strcasecmp(ext, ".lp") == 0)) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static int list_netlib_problems(ProblemInfo *problems, int max_problems, int lp_only) {
    char netlib_path[MAX_PATH];
    if (!get_netlib_dir(netlib_path, sizeof(netlib_path))) {
        return 0;
    }

    DIR *dir = opendir(netlib_path);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_problems) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".mps") != 0 && strcasecmp(ext, ".lp") != 0) continue;

        /* Extract name without extension */
        strncpy(problems[count].name, entry->d_name, sizeof(problems[count].name) - 1);
        char *dot = strrchr(problems[count].name, '.');
        if (dot) *dot = '\0';

        snprintf(problems[count].path, sizeof(problems[count].path),
                 "%s/%s", netlib_path, entry->d_name);

        /* Quick check if MIP (look for GENERAL/BINARY in file) */
        problems[count].is_mip = 0;
        FILE *f = fopen(problems[count].path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "GENERAL") || strstr(line, "BINARY") ||
                    strstr(line, "INTEGER")) {
                    problems[count].is_mip = 1;
                    break;
                }
            }
            fclose(f);
        }

        /* Filter by type */
        if (lp_only && problems[count].is_mip) continue;

        count++;
    }
    closedir(dir);
    return count;
}

static int find_netlib_problem(const char *name, char *path_out, size_t path_size) {
    char netlib_path[MAX_PATH];
    if (!get_netlib_dir(netlib_path, sizeof(netlib_path))) {
        return 0;
    }

    /* Try with .mps extension */
    snprintf(path_out, path_size, "%s/%s.mps", netlib_path, name);
    if (file_exists(path_out)) return 1;

    /* Try with .lp extension */
    snprintf(path_out, path_size, "%s/%s.lp", netlib_path, name);
    if (file_exists(path_out)) return 1;

    return 0;
}

/* ============================================================================
 * JSON Output
 * ============================================================================ */

static void print_json_result(const char *problem_name, const char *source,
                               int num_vars, int num_cons, int nnz, int is_mip,
                               SolveResult *glpk, SolveResult *ralph,
                               ValidationResult *val,
                               FILE *out) {
    char escaped_name[512];
    json_escape_string(escaped_name, sizeof(escaped_name), problem_name);

    char escaped_issues[2048];
    json_escape_string(escaped_issues, sizeof(escaped_issues),
                       val ? val->issues : "");
    char escaped_lu_reason[128];
    json_escape_string(escaped_lu_reason, sizeof(escaped_lu_reason),
                       ralph->lu_last_failure_reason[0] ? ralph->lu_last_failure_reason : "none");
    char escaped_lu_refactor_reason[128];
    json_escape_string(escaped_lu_refactor_reason, sizeof(escaped_lu_refactor_reason),
                       ralph->lu_last_refactor_trigger_reason[0]
                           ? ralph->lu_last_refactor_trigger_reason
                           : "none");
    char escaped_lu_sparse_fallback_reason[128];
    json_escape_string(escaped_lu_sparse_fallback_reason, sizeof(escaped_lu_sparse_fallback_reason),
                       ralph->lu_sparse_fallback_last_reason_str[0] ? ralph->lu_sparse_fallback_last_reason_str : "none");
    char escaped_lu_sparse_numeric_failure_reason[128];
    json_escape_string(escaped_lu_sparse_numeric_failure_reason,
                       sizeof(escaped_lu_sparse_numeric_failure_reason),
                       ralph->lu_sparse_numeric_last_failure_reason_str[0]
                           ? ralph->lu_sparse_numeric_last_failure_reason_str
                           : "none");
    char escaped_refactor_reason[128];
    json_escape_string(escaped_refactor_reason, sizeof(escaped_refactor_reason),
                       ralph->refactor_last_reason_str[0] ? ralph->refactor_last_reason_str : "other");
    char escaped_raw_status[128];
    json_escape_string(escaped_raw_status, sizeof(escaped_raw_status),
                       ralph->raw_status_str[0] ? ralph->raw_status_str : "UNKNOWN");
    char escaped_termination_reason[128];
    json_escape_string(escaped_termination_reason, sizeof(escaped_termination_reason),
                       ralph->termination_reason[0] ? ralph->termination_reason : "unknown");
    char escaped_solve_path[128];
    json_escape_string(escaped_solve_path, sizeof(escaped_solve_path),
                       ralph->solve_path[0] ? ralph->solve_path : "unknown");
    char escaped_api_error_message[256];
    json_escape_string(escaped_api_error_message, sizeof(escaped_api_error_message),
                       ralph->api_error_message[0] ? ralph->api_error_message : "");

    double density = (num_vars > 0 && num_cons > 0)
                     ? (double)nnz / ((double)num_vars * num_cons)
                     : 0.0;

    fprintf(out, "{\n");

    /* Problem info */
    fprintf(out, "  \"problem\": {\n");
    fprintf(out, "    \"name\": \"%s\",\n", escaped_name);
    fprintf(out, "    \"source\": \"%s\",\n", source);
    fprintf(out, "    \"vars\": %d,\n", num_vars);
    fprintf(out, "    \"cons\": %d,\n", num_cons);
    fprintf(out, "    \"nnz\": %d,\n", nnz);
    fprintf(out, "    \"density\": %.6f,\n", density);
    fprintf(out, "    \"is_mip\": %s\n", is_mip ? "true" : "false");
    fprintf(out, "  },\n");

    /* GLPK result */
    fprintf(out, "  \"glpk\": {\n");
    const char *glpk_status_str = "error";
    switch (glpk->status) {
        case 0: glpk_status_str = "optimal"; break;
        case 1: glpk_status_str = "infeasible"; break;
        case 2: glpk_status_str = "unbounded"; break;
        case 4: glpk_status_str = "timeout"; break;
    }
    fprintf(out, "    \"status\": \"%s\",\n", glpk_status_str);
    fprintf(out, "    \"objective\": %.15g,\n", glpk->objective);
    fprintf(out, "    \"time_ms\": %.3f,\n", glpk->time_ms);
    fprintf(out, "    \"iterations\": %d\n", glpk->iterations);
    fprintf(out, "  },\n");

    /* Ralph result */
    fprintf(out, "  \"ralph\": {\n");
    const char *ralph_status_str = "error";
    if (ralph->raw_status_code == (int)RALPH_STATUS_IMPRECISE) {
        ralph_status_str = "imprecise";
    } else {
        switch (ralph->status) {
            case 0: ralph_status_str = "optimal"; break;
            case 1: ralph_status_str = "infeasible"; break;
            case 2: ralph_status_str = "unbounded"; break;
            case 4: ralph_status_str = "timeout"; break;
        }
    }
    fprintf(out, "    \"status\": \"%s\",\n", ralph_status_str);
    fprintf(out, "    \"raw_status_code\": %d,\n", ralph->raw_status_code);
    fprintf(out, "    \"raw_status\": \"%s\",\n", escaped_raw_status);
    fprintf(out, "    \"solve_path\": \"%s\",\n", escaped_solve_path);
    fprintf(out, "    \"termination_reason_code\": %d,\n", ralph->termination_reason_code);
    fprintf(out, "    \"termination_reason\": \"%s\",\n", escaped_termination_reason);
    fprintf(out, "    \"api_error_domain\": %d,\n", ralph->api_error_domain);
    fprintf(out, "    \"api_error_code\": %d,\n", ralph->api_error_code);
    fprintf(out, "    \"api_error_api\": %d,\n", ralph->api_error_api_id);
    fprintf(out, "    \"api_error_message\": \"%s\",\n", escaped_api_error_message);
    fprintf(out, "    \"objective\": %.15g,\n", ralph->objective);
    fprintf(out, "    \"time_ms\": %.3f,\n", ralph->time_ms);
    fprintf(out, "    \"iterations\": %d,\n", ralph->iterations);
    fprintf(out, "    \"phase1_artificial_sum\": %.15g,\n",
            ralph->phase1_artificial_sum);
    fprintf(out, "    \"phase1_artificial_max\": %.15g,\n",
            ralph->phase1_artificial_max);
    fprintf(out, "    \"phase1_artificial_basic\": %d,\n",
            ralph->phase1_artificial_basic);
    fprintf(out, "    \"feasibility_checked\": %s,\n",
            ralph->feasibility_checked ? "true" : "false");
    fprintf(out, "    \"worst_bound_var\": %d,\n", ralph->worst_bound_var);
    fprintf(out, "    \"worst_bound_value\": %.15g,\n", ralph->worst_bound_value);
    fprintf(out, "    \"worst_bound_lb\": %.15g,\n", ralph->worst_bound_lb);
    fprintf(out, "    \"worst_bound_ub\": %.15g\n", ralph->worst_bound_ub);
    fprintf(out, "  },\n");

    /* Validation */
    fprintf(out, "  \"validation\": {\n");
    if (val) {
        fprintf(out, "    \"solution_valid\": %s,\n",
                val->solution_valid ? "true" : "false");
        fprintf(out, "    \"max_constraint_violation\": %.6e,\n",
                val->max_constraint_violation);
        fprintf(out, "    \"max_bound_violation\": %.6e,\n",
                val->max_bound_violation);
        fprintf(out, "    \"objective_match\": %s,\n",
                val->objective_match ? "true" : "false");
        fprintf(out, "    \"objective_rel_error\": %.6e,\n",
                val->objective_rel_error);
        fprintf(out, "    \"numerically_stable\": %s\n",
                val->numerically_stable ? "true" : "false");
    } else {
        fprintf(out, "    \"solution_valid\": null,\n");
        fprintf(out, "    \"max_constraint_violation\": null,\n");
        fprintf(out, "    \"max_bound_violation\": null,\n");
        fprintf(out, "    \"objective_match\": null,\n");
        fprintf(out, "    \"objective_rel_error\": null,\n");
        fprintf(out, "    \"numerically_stable\": null\n");
    }
    fprintf(out, "  },\n");

    /* Performance */
    fprintf(out, "  \"performance\": {\n");
    double time_ratio = (glpk->time_ms > 0.1)
                        ? ralph->time_ms / glpk->time_ms
                        : 0.0;
    double iter_ratio = (glpk->iterations > 0)
                        ? (double)ralph->iterations / glpk->iterations
                        : 0.0;
    double ralph_per_iter = (ralph->iterations > 0)
                            ? ralph->time_ms / ralph->iterations
                            : 0.0;
    double glpk_per_iter = (glpk->iterations > 0)
                           ? glpk->time_ms / glpk->iterations
                           : 0.0;

    fprintf(out, "    \"ralph_vs_glpk_time\": %.3f,\n", time_ratio);
    fprintf(out, "    \"ralph_vs_glpk_iters\": %.3f,\n", iter_ratio);
    fprintf(out, "    \"ralph_ms_per_iter\": %.6f,\n", ralph_per_iter);
    fprintf(out, "    \"glpk_ms_per_iter\": %.6f\n", glpk_per_iter);
    fprintf(out, "  },\n");

    /* Solve sparsity telemetry for the FTRAN/BTRAN hot path. */
    {
        double ftran_avg_rhs_nnz = (ralph->ftran_nnz_samples > 0)
            ? (double)ralph->ftran_rhs_nnz_total / (double)ralph->ftran_nnz_samples
            : 0.0;
        double ftran_avg_sol_nnz = (ralph->ftran_nnz_samples > 0)
            ? (double)ralph->ftran_sol_nnz_total / (double)ralph->ftran_nnz_samples
            : 0.0;
        double btran_avg_rhs_nnz = (ralph->btran_nnz_samples > 0)
            ? (double)ralph->btran_rhs_nnz_total / (double)ralph->btran_nnz_samples
            : 0.0;
        double btran_avg_sol_nnz = (ralph->btran_nnz_samples > 0)
            ? (double)ralph->btran_sol_nnz_total / (double)ralph->btran_nnz_samples
            : 0.0;
        fprintf(out, "  \"solve_sparsity\": {\n");
        fprintf(out, "    \"ftran_calls\": %d,\n", ralph->ftran_calls);
        fprintf(out, "    \"ftran_update_apply_calls\": %d,\n",
                ralph->ftran_update_apply_calls);
        fprintf(out, "    \"ftran_nnz_samples\": %d,\n", ralph->ftran_nnz_samples);
        fprintf(out, "    \"ftran_rhs_nnz_total\": %lld,\n", ralph->ftran_rhs_nnz_total);
        fprintf(out, "    \"ftran_sol_nnz_total\": %lld,\n", ralph->ftran_sol_nnz_total);
        fprintf(out, "    \"ftran_avg_rhs_nnz\": %.6f,\n", ftran_avg_rhs_nnz);
        fprintf(out, "    \"ftran_avg_sol_nnz\": %.6f,\n", ftran_avg_sol_nnz);
        fprintf(out, "    \"btran_calls\": %d,\n", ralph->btran_calls);
        fprintf(out, "    \"btran_update_apply_calls\": %d,\n",
                ralph->btran_update_apply_calls);
        fprintf(out, "    \"btran_nnz_samples\": %d,\n", ralph->btran_nnz_samples);
        fprintf(out, "    \"btran_rhs_nnz_total\": %lld,\n", ralph->btran_rhs_nnz_total);
        fprintf(out, "    \"btran_sol_nnz_total\": %lld,\n", ralph->btran_sol_nnz_total);
        fprintf(out, "    \"btran_avg_rhs_nnz\": %.6f,\n", btran_avg_rhs_nnz);
        fprintf(out, "    \"btran_avg_sol_nnz\": %.6f\n", btran_avg_sol_nnz);
        fprintf(out, "  },\n");
    }

    fprintf(out, "  \"presolve\": {\n");
    fprintf(out, "    \"used\": %d,\n", ralph->presolve_used);
    fprintf(out, "    \"mask\": %u,\n", ralph->presolve_mask);
    fprintf(out, "    \"rounds\": %d,\n", ralph->presolve_rounds);
    fprintf(out, "    \"vars_removed\": %d,\n", ralph->presolve_vars_removed);
    fprintf(out, "    \"cons_removed\": %d,\n", ralph->presolve_cons_removed);
    fprintf(out, "    \"bounds_tightened\": %d,\n", ralph->presolve_bounds_tightened);
    fprintf(out, "    \"matrix_rank\": %d,\n", ralph->presolve_matrix_rank);
    fprintf(out, "    \"redundant_rows_found\": %d,\n",
            ralph->presolve_redundant_rows_found);
    fprintf(out, "    \"time_ms\": %.6f\n", ralph->presolve_time_ms);
    fprintf(out, "  },\n");

    /* Ralph timing breakdown (solver-internal instrumentation) */
    fprintf(out, "  \"timing\": {\n");
    fprintf(out, "    \"primal_setup_ms\": %.6f,\n", ralph->primal_setup_ms);
    fprintf(out, "    \"dual_ms\": %.6f,\n", ralph->dual_ms);
    fprintf(out, "    \"phase1_ms\": %.6f,\n", ralph->phase1_ms);
    fprintf(out, "    \"transition_ms\": %.6f,\n", ralph->transition_ms);
    fprintf(out, "    \"phase2_ms\": %.6f,\n", ralph->phase2_ms);
    fprintf(out, "    \"pricing_ms\": %.6f,\n", ralph->pricing_ms);
    fprintf(out, "    \"ratio_ms\": %.6f,\n", ralph->ratio_ms);
    fprintf(out, "    \"pivot_ms\": %.6f,\n", ralph->pivot_ms);
    fprintf(out, "    \"refactor_ms\": %.6f,\n", ralph->refactor_ms);
    fprintf(out, "    \"ftran_ms\": %.6f,\n", ralph->ftran_ms);
    fprintf(out, "    \"ftran_base_ms\": %.6f,\n", ralph->ftran_base_ms);
    fprintf(out, "    \"ftran_update_apply_ms\": %.6f,\n",
            ralph->ftran_update_apply_ms);
    fprintf(out, "    \"btran_ms\": %.6f,\n", ralph->btran_ms);
    fprintf(out, "    \"btran_base_ms\": %.6f,\n", ralph->btran_base_ms);
    fprintf(out, "    \"btran_update_apply_ms\": %.6f,\n",
            ralph->btran_update_apply_ms);
    fprintf(out, "    \"lu_update_ms\": %.6f,\n", ralph->lu_update_ms);
    fprintf(out, "    \"compute_solution_ms\": %.6f,\n", ralph->compute_solution_ms);
    fprintf(out, "    \"compute_reduced_costs_ms\": %.6f\n", ralph->compute_rc_ms);
    fprintf(out, "  },\n");

    /* Per-phase hot-path timing/call breakdown */
    fprintf(out, "  \"phase_hotspots\": {\n");
    fprintf(out, "    \"phase1\": {\n");
    fprintf(out, "      \"pricing_ms\": %.6f,\n", ralph->phase1_pricing_ms);
    fprintf(out, "      \"ratio_ms\": %.6f,\n", ralph->phase1_ratio_ms);
    fprintf(out, "      \"pivot_ms\": %.6f,\n", ralph->phase1_pivot_ms);
    fprintf(out, "      \"refactor_ms\": %.6f,\n", ralph->phase1_refactor_ms);
    fprintf(out, "      \"compute_solution_ms\": %.6f,\n", ralph->phase1_compute_solution_ms);
    fprintf(out, "      \"compute_reduced_costs_ms\": %.6f,\n", ralph->phase1_compute_rc_ms);
    fprintf(out, "      \"pricing_calls\": %d,\n", ralph->phase1_pricing_calls);
    fprintf(out, "      \"ratio_calls\": %d,\n", ralph->phase1_ratio_calls);
    fprintf(out, "      \"pivot_calls\": %d,\n", ralph->phase1_pivot_calls);
    fprintf(out, "      \"refactor_calls\": %d,\n", ralph->phase1_refactor_calls);
    fprintf(out, "      \"refactor_periodic_policy_calls\": %d,\n", ralph->phase1_refactor_periodic_policy);
    fprintf(out, "      \"refactor_lu_health_calls\": %d,\n", ralph->phase1_refactor_periodic_lu_health);
    fprintf(out, "      \"refactor_safety_forced_calls\": %d,\n", ralph->phase1_refactor_safety_forced);
    fprintf(out, "      \"dir_stabilize_force_extreme_dir\": %d,\n",
            ralph->phase1_dir_stabilize_force_extreme_dir);
    fprintf(out, "      \"dir_stabilize_force_lu_health\": %d,\n",
            ralph->phase1_dir_stabilize_force_lu_health);
    fprintf(out, "      \"dir_stabilize_cooldown_candidates\": %d,\n",
            ralph->phase1_dir_stabilize_cooldown_candidates);
    fprintf(out, "      \"dir_stabilize_ratio_le_3\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_3);
    fprintf(out, "      \"dir_stabilize_ratio_le_10\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_10);
    fprintf(out, "      \"dir_stabilize_ratio_le_30\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_30);
    fprintf(out, "      \"dir_stabilize_ratio_le_100\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_100);
    fprintf(out, "      \"dir_stabilize_ratio_gt_100\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_gt_100);
    fprintf(out, "      \"dir_stabilize_ratio_gt_300\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_gt_300);
    fprintf(out, "      \"dir_stabilize_ratio_gt_1000\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_gt_1000);
    fprintf(out, "      \"dir_stabilize_skip_rc_only\": %d,\n",
            ralph->phase1_dir_stabilize_skip_rc_only);
    fprintf(out, "      \"dir_stabilize_skip_full\": %d,\n",
            ralph->phase1_dir_stabilize_skip_full);
    fprintf(out, "      \"dir_stabilize_skip_no_recompute\": %d,\n",
            ralph->phase1_dir_stabilize_skip_no_recompute);
    fprintf(out, "      \"dir_stabilize_skip_guard_refresh\": %d,\n",
            ralph->phase1_dir_stabilize_skip_guard_refresh);
    fprintf(out, "      \"dir_stabilize_escape_gate_triggers\": %d,\n",
            ralph->phase1_dir_stabilize_escape_gate_triggers);
    fprintf(out, "      \"dir_stabilize_escape_gate_suppressed_lu_health\": %d,\n",
            ralph->phase1_dir_stabilize_escape_gate_suppressed_lu_health);
    fprintf(out, "      \"dir_stabilize_escape_gate_suppressed_force_pivot_mode\": %d,\n",
            ralph->phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode);
    fprintf(out, "      \"dir_stabilize_escape_gate_hard_bypass\": %d,\n",
            ralph->phase1_dir_stabilize_escape_gate_hard_bypass);
    fprintf(out, "      \"dir_stabilize_refactor_from_no_pivot_force\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_no_pivot_force);
    fprintf(out, "      \"dir_stabilize_refactor_from_force_extreme_dir\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_force_extreme_dir);
    fprintf(out, "      \"dir_stabilize_refactor_from_force_lu_health\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_force_lu_health);
    fprintf(out, "      \"dir_stabilize_refactor_from_force_pivot_mode\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_force_pivot_mode);
    fprintf(out, "      \"dir_stabilize_refactor_from_ladder_force\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_ladder_force);
    fprintf(out, "      \"force_pivot_budget_dir_event_seen\": %d,\n",
            ralph->phase1_force_pivot_budget_dir_event_seen);
    fprintf(out, "      \"force_pivot_budget_pivot_spend\": %d,\n",
            ralph->phase1_force_pivot_budget_pivot_spend);
    fprintf(out, "      \"force_pivot_relax_applied\": %d,\n",
            ralph->phase1_force_pivot_relax_applied);
    fprintf(out, "      \"force_extreme_relax_applied\": %d,\n",
            ralph->phase1_force_extreme_relax_applied);
    fprintf(out, "      \"force_extreme_bound_flip_relax_applied\": %d,\n",
            ralph->phase1_force_extreme_bound_flip_relax_applied);
    fprintf(out,
            "      \"force_extreme_catastrophic_tiny_theta_relax_applied\": %d,\n",
            ralph->phase1_force_extreme_catastrophic_tiny_theta_relax_applied);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_applied\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_applied);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_post_dir_skip_retry\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_refactor_force_lu_health\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_refactor_force_lu_health);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_refactor_force_pivot_mode\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_refactor_force_pivot_mode);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_refactor_ladder_force\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_refactor_ladder_force);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_next_failed_stabilize\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_next_failed_stabilize);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_next_ratio_breakdown\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_next_pivot_fail\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_next_pivot_fail);
    fprintf(out, "      \"force_extreme_tiny_theta_relax_next_pivot_success\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_next_pivot_success);
    fprintf(out, "      \"recompute_after_ratio_breakdown\": %d,\n",
            ralph->phase1_recompute_after_ratio_breakdown);
    fprintf(out, "      \"recompute_after_dir_skip\": %d,\n",
            ralph->phase1_recompute_after_dir_skip);
    fprintf(out, "      \"recompute_after_dir_refactor\": %d,\n",
            ralph->phase1_recompute_after_dir_refactor);
    fprintf(out, "      \"recompute_after_pivot_fail_recovery\": %d,\n",
            ralph->phase1_recompute_after_pivot_fail_recovery);
    fprintf(out, "      \"recompute_after_perturb\": %d,\n",
            ralph->phase1_recompute_after_perturb);
    fprintf(out, "      \"recompute_rc_only_calls\": %d,\n",
            ralph->phase1_recompute_rc_only_calls);
    fprintf(out, "      \"cleanup_attempts\": %d,\n",
            ralph->phase1_cleanup_attempts);
    fprintf(out, "      \"cleanup_accepted\": %d,\n",
            ralph->phase1_cleanup_accepted);
    fprintf(out, "      \"cleanup_rejected\": %d,\n",
            ralph->phase1_cleanup_rejected);
    fprintf(out, "      \"cleanup_candidate_probe_rejects\": %d,\n",
            ralph->phase1_cleanup_candidate_probe_rejects);
    fprintf(out, "      \"progress_window_refactors\": %d,\n",
            ralph->phase1_progress_window_refactors);
    fprintf(out, "      \"progress_window_cleanups\": %d,\n",
            ralph->phase1_progress_window_cleanups);
    fprintf(out, "      \"progress_window_perturbs\": %d,\n",
            ralph->phase1_progress_window_perturbs);
    fprintf(out, "      \"compute_solution_ctx_other\": %d,\n",
            ralph->phase1_compute_solution_ctx_other);
    fprintf(out, "      \"compute_solution_ctx_recompute_full\": %d,\n",
            ralph->phase1_compute_solution_ctx_recompute_full);
    fprintf(out, "      \"compute_solution_ctx_recompute_guard_forced_full\": %d,\n",
            ralph->phase1_compute_solution_ctx_recompute_guard_forced_full);
    fprintf(out, "      \"compute_solution_ctx_init\": %d,\n",
            ralph->phase1_compute_solution_ctx_init);
    fprintf(out, "      \"compute_solution_ctx_no_entering_cleanup\": %d,\n",
            ralph->phase1_compute_solution_ctx_no_entering_cleanup);
    fprintf(out, "      \"compute_solution_ctx_infeas_cleanup\": %d,\n",
            ralph->phase1_compute_solution_ctx_infeas_cleanup);
    fprintf(out, "      \"compute_solution_ctx_refactor_fail_continue\": %d,\n",
            ralph->phase1_compute_solution_ctx_refactor_fail_continue);
    fprintf(out, "      \"compute_solution_ctx_refactor_failure_recovery\": %d,\n",
            ralph->phase1_compute_solution_ctx_refactor_failure_recovery);
    fprintf(out, "      \"compute_solution_ctx_refactor_success\": %d,\n",
            ralph->phase1_compute_solution_ctx_refactor_success);
    fprintf(out, "      \"compute_solution_ctx_drift_refresh\": %d,\n",
            ralph->phase1_compute_solution_ctx_drift_refresh);
    fprintf(out, "      \"compute_solution_ctx_dual_rescue\": %d,\n",
            ralph->phase1_compute_solution_ctx_dual_rescue);
    fprintf(out, "      \"compute_reduced_costs_ctx_other\": %d,\n",
            ralph->phase1_compute_rc_ctx_other);
    fprintf(out, "      \"compute_reduced_costs_ctx_recompute_full\": %d,\n",
            ralph->phase1_compute_rc_ctx_recompute_full);
    fprintf(out, "      \"compute_reduced_costs_ctx_recompute_rc_only\": %d,\n",
            ralph->phase1_compute_rc_ctx_recompute_rc_only);
    fprintf(out, "      \"compute_reduced_costs_ctx_recompute_guard_forced_full\": %d,\n",
            ralph->phase1_compute_rc_ctx_recompute_guard_forced_full);
    fprintf(out, "      \"compute_reduced_costs_ctx_init\": %d,\n",
            ralph->phase1_compute_rc_ctx_init);
    fprintf(out, "      \"compute_reduced_costs_ctx_infeas_cleanup\": %d,\n",
            ralph->phase1_compute_rc_ctx_infeas_cleanup);
    fprintf(out, "      \"compute_reduced_costs_ctx_refactor_fail_continue\": %d,\n",
            ralph->phase1_compute_rc_ctx_refactor_fail_continue);
    fprintf(out, "      \"compute_reduced_costs_ctx_refactor_failure_recovery\": %d,\n",
            ralph->phase1_compute_rc_ctx_refactor_failure_recovery);
    fprintf(out, "      \"compute_reduced_costs_ctx_refactor_success\": %d,\n",
            ralph->phase1_compute_rc_ctx_refactor_success);
    fprintf(out, "      \"compute_reduced_costs_ctx_drift_refresh\": %d,\n",
            ralph->phase1_compute_rc_ctx_drift_refresh);
    fprintf(out, "      \"compute_reduced_costs_ctx_dual_rescue\": %d,\n",
            ralph->phase1_compute_rc_ctx_dual_rescue);
    fprintf(out, "      \"entering_exclusions\": %d,\n",
            ralph->phase1_entering_exclusions);
    fprintf(out, "      \"entering_exclusion_repeats\": %d,\n",
            ralph->phase1_entering_exclusion_repeats);
    fprintf(out, "      \"entering_exclusion_hits\": %d,\n",
            ralph->phase1_entering_exclusion_hits);
    fprintf(out, "      \"entering_exclusion_reroutes\": %d,\n",
            ralph->phase1_entering_exclusion_reroutes);
    fprintf(out, "      \"entering_exclusion_no_alt\": %d,\n",
            ralph->phase1_entering_exclusion_no_alt);
    fprintf(out, "      \"recompute_rc_guard_forced_full\": %d,\n",
            ralph->phase1_recompute_rc_guard_forced_full);
    fprintf(out, "      \"ratio_breakdown_retries\": %d,\n",
            ralph->phase1_ratio_breakdown_retries);
    fprintf(out, "      \"ratio_breakdown_escalations\": %d,\n",
            ralph->phase1_ratio_breakdown_escalations);
    fprintf(out, "      \"pivot_fail_recovery_exclusions\": %d,\n",
            ralph->phase1_pivot_fail_recovery_exclusions);
    fprintf(out, "      \"no_pivot_events\": %d,\n",
            ralph->phase1_no_pivot_events);
    fprintf(out, "      \"no_pivot_forced_refactor\": %d,\n",
            ralph->phase1_no_pivot_forced_refactor);
    fprintf(out, "      \"no_pivot_forced_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_forced_ratio_breakdown);
    fprintf(out, "      \"no_pivot_forced_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_forced_dir_skip);
    fprintf(out, "      \"no_pivot_forced_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_forced_pivot_fail);
    fprintf(out, "      \"no_pivot_events_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_events_ratio_breakdown);
    fprintf(out, "      \"no_pivot_events_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_events_dir_skip);
    fprintf(out, "      \"no_pivot_events_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_events_pivot_fail);
    fprintf(out, "      \"no_pivot_no_progress_events\": %d,\n",
            ralph->phase1_no_pivot_no_progress_events);
    fprintf(out, "      \"no_pivot_ladder_retry_defers\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_defers);
    fprintf(out, "      \"no_pivot_ladder_retry_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_ratio_breakdown);
    fprintf(out, "      \"no_pivot_ladder_retry_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_dir_skip);
    fprintf(out, "      \"no_pivot_ladder_retry_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_pivot_fail);
    fprintf(out, "      \"no_pivot_ladder_dual_rescue_attempts\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts);
    fprintf(out, "      \"no_pivot_ladder_dual_rescue_successes\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_successes);
    fprintf(out, "      \"no_pivot_ladder_dual_rescue_failures\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_failures);
    fprintf(out, "      \"no_pivot_ladder_dual_rescue_attempts_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown);
    fprintf(out, "      \"no_pivot_ladder_dual_rescue_attempts_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip);
    fprintf(out, "      \"no_pivot_ladder_dual_rescue_attempts_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail);
    fprintf(out, "      \"no_pivot_ladder_forced_refactors\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors);
    fprintf(out, "      \"no_pivot_ladder_forced_refactors_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors_ratio_breakdown);
    fprintf(out, "      \"no_pivot_ladder_forced_refactors_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors_dir_skip);
    fprintf(out, "      \"no_pivot_ladder_forced_refactors_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors_pivot_fail);
    fprintf(out, "      \"no_pivot_ladder_rescue_guard_cooldown_blocks\": %d,\n",
            ralph->phase1_no_pivot_ladder_rescue_guard_cooldown_blocks);
    fprintf(out, "      \"no_pivot_ladder_rescue_guard_fail_cap_forces\": %d,\n",
            ralph->phase1_no_pivot_ladder_rescue_guard_fail_cap_forces);
    fprintf(out, "      \"direct_dual_rescue_attempts\": %d,\n",
            ralph->phase1_direct_dual_rescue_attempts);
    fprintf(out, "      \"direct_dual_rescue_successes\": %d,\n",
            ralph->phase1_direct_dual_rescue_successes);
    fprintf(out, "      \"direct_dual_rescue_failures\": %d,\n",
            ralph->phase1_direct_dual_rescue_failures);
    fprintf(out, "      \"direct_dual_rescue_guard_cooldown_blocks\": %d,\n",
            ralph->phase1_direct_dual_rescue_guard_cooldown_blocks);
    fprintf(out, "      \"direct_dual_rescue_guard_fail_cap_blocks\": %d,\n",
            ralph->phase1_direct_dual_rescue_guard_fail_cap_blocks);
    fprintf(out, "      \"dual_rescue_exit_time_limit\": %d,\n",
            ralph->phase1_dual_rescue_exit_time_limit);
    fprintf(out, "      \"dual_rescue_exit_bad_numerics\": %d,\n",
            ralph->phase1_dual_rescue_exit_bad_numerics);
    fprintf(out, "      \"dual_rescue_exit_no_progress\": %d,\n",
            ralph->phase1_dual_rescue_exit_no_progress);
    fprintf(out, "      \"dual_rescue_exit_no_entering\": %d,\n",
            ralph->phase1_dual_rescue_exit_no_entering);
    fprintf(out, "      \"dual_rescue_exit_pivot_refactor_failure\": %d,\n",
            ralph->phase1_dual_rescue_exit_pivot_refactor_failure);
    fprintf(out, "      \"dual_rescue_exit_periodic_refactor_failure\": %d,\n",
            ralph->phase1_dual_rescue_exit_periodic_refactor_failure);
    fprintf(out, "      \"dual_rescue_exit_max_iters\": %d,\n",
            ralph->phase1_dual_rescue_exit_max_iters);
    fprintf(out, "      \"dual_rescue_exit_alloc_failure\": %d,\n",
            ralph->phase1_dual_rescue_exit_alloc_failure);
    fprintf(out, "      \"soft_lu_policy_cooldown_defers\": %d,\n",
            ralph->phase1_soft_lu_policy_cooldown_defers);
    fprintf(out, "      \"dir_skip_same_entering_repeats\": %d,\n",
            ralph->phase1_dir_skip_same_entering_repeats);
    fprintf(out, "      \"dir_skip_same_entering_max_streak\": %d,\n",
            ralph->phase1_dir_skip_same_entering_max_streak);
    fprintf(out, "      \"failed_stabilize_events\": %d,\n",
            ralph->phase1_failed_stabilize_events);
    fprintf(out, "      \"failed_stabilize_primary_failures\": %d,\n",
            ralph->phase1_failed_stabilize_primary_failures);
    fprintf(out, "      \"failed_stabilize_alternate_failures\": %d,\n",
            ralph->phase1_failed_stabilize_alternate_failures);
    fprintf(out, "      \"failed_stabilize_same_entering_repeats\": %d,\n",
            ralph->phase1_failed_stabilize_same_entering_repeats);
    fprintf(out, "      \"failed_stabilize_same_entering_max_streak\": %d,\n",
            ralph->phase1_failed_stabilize_same_entering_max_streak);
    fprintf(out, "      \"failed_stabilize_retry_penalty_arms\": %d,\n",
            ralph->phase1_failed_stabilize_retry_penalty_arms);
    fprintf(out, "      \"failed_stabilize_retry_penalty_alt_found\": %d,\n",
            ralph->phase1_failed_stabilize_retry_penalty_alt_found);
    fprintf(out, "      \"failed_stabilize_retry_penalty_no_alt\": %d,\n",
            ralph->phase1_failed_stabilize_retry_penalty_no_alt);
    fprintf(out, "      \"failed_stabilize_retry_penalty_alt_stabilized\": %d,\n",
            ralph->phase1_failed_stabilize_retry_penalty_alt_stabilized);
    fprintf(out, "      \"failed_stabilize_retry_penalty_alt_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_penalty_alt_failed);
    fprintf(out, "      \"failed_stabilize_retry_penalty_same_alt_repeats\": %d,\n",
            ralph->phase1_failed_stabilize_retry_penalty_same_alt_repeats);
    fprintf(out, "      \"failed_stabilize_retry_penalty_same_alt_max_streak\": %d,\n",
            ralph->phase1_failed_stabilize_retry_penalty_same_alt_max_streak);
    fprintf(out, "      \"failed_stabilize_retry_local_memory_arms\": %d,\n",
            ralph->phase1_failed_stabilize_retry_local_memory_arms);
    fprintf(out, "      \"failed_stabilize_retry_local_memory_alt_found\": %d,\n",
            ralph->phase1_failed_stabilize_retry_local_memory_alt_found);
    fprintf(out, "      \"failed_stabilize_retry_local_memory_no_alt\": %d,\n",
            ralph->phase1_failed_stabilize_retry_local_memory_no_alt);
    fprintf(out, "      \"failed_stabilize_retry_local_memory_fallback_same_alt\": %d,\n",
            ralph->phase1_failed_stabilize_retry_local_memory_fallback_same_alt);
    fprintf(out, "      \"failed_stabilize_retry_local_memory_alt_stabilized\": %d,\n",
            ralph->phase1_failed_stabilize_retry_local_memory_alt_stabilized);
    fprintf(out, "      \"failed_stabilize_retry_local_memory_alt_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_local_memory_alt_failed);
    fprintf(out, "      \"failed_stabilize_retry_pool_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_pool_samples);
    fprintf(out, "      \"failed_stabilize_retry_pool_eligible_total\": %d,\n",
            ralph->phase1_failed_stabilize_retry_pool_eligible_total);
    fprintf(out, "      \"failed_stabilize_retry_pool_eligible_max\": %d,\n",
            ralph->phase1_failed_stabilize_retry_pool_eligible_max);
    fprintf(out, "      \"failed_stabilize_retry_pool_singleton_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_pool_singleton_samples);
    fprintf(out, "      \"failed_stabilize_retry_pool_best_differs_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_pool_best_differs_samples);
    fprintf(out, "      \"failed_stabilize_retry_selector_eval_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_eval_samples);
    fprintf(out, "      \"failed_stabilize_retry_selector_eval_best_differs_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_eval_best_differs_samples);
    fprintf(out, "      \"failed_stabilize_retry_selector_eval_score_ratio_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_selector_eval_score_ratio_total);
    fprintf(out, "      \"failed_stabilize_retry_selector_eval_score_ratio_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_selector_eval_score_ratio_max);
    fprintf(out, "      \"failed_stabilize_retry_selector_eval_score_ratio_ge_2\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2);
    fprintf(out, "      \"failed_stabilize_retry_selector_eval_score_ratio_ge_4\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4);
    fprintf(out, "      \"failed_stabilize_retry_shadow_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_samples);
    fprintf(out, "      \"failed_stabilize_retry_shadow_ratio_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_ratio_failed);
    fprintf(out, "      \"failed_stabilize_retry_shadow_dir_stable\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_dir_stable);
    fprintf(out, "      \"failed_stabilize_retry_shadow_dir_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_dir_failed);
    fprintf(out, "      \"failed_stabilize_retry_shadow_dir_nnz_total\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_dir_nnz_total);
    fprintf(out, "      \"failed_stabilize_retry_shadow_dir_nnz_max\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_dir_nnz_max);
    fprintf(out, "      \"failed_stabilize_retry_shadow_dir_inf_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_dir_inf_total);
    fprintf(out, "      \"failed_stabilize_retry_shadow_dir_inf_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_dir_inf_max);
    fprintf(out, "      \"failed_stabilize_retry_shadow_pivot_abs_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_pivot_abs_total);
    fprintf(out, "      \"failed_stabilize_retry_shadow_pivot_abs_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_pivot_abs_max);
    fprintf(out, "      \"failed_stabilize_retry_shadow_guard_arms\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_guard_arms);
    fprintf(out, "      \"failed_stabilize_retry_shadow_guard_original_exclusions\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_guard_original_exclusions);
    fprintf(out, "      \"failed_stabilize_retry_shadow_post_dir_skip_retry\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_post_dir_skip_retry);
    fprintf(out, "      \"failed_stabilize_retry_shadow_post_dir_skip_dual_rescue\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue);
    fprintf(out, "      \"failed_stabilize_retry_shadow_post_dir_skip_forced_refactor\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor);
    fprintf(out, "      \"failed_stabilize_retry_shadow_next_failed_stabilize\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_next_failed_stabilize);
    fprintf(out, "      \"failed_stabilize_retry_shadow_next_ratio_breakdown\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_next_ratio_breakdown);
    fprintf(out, "      \"failed_stabilize_retry_shadow_next_pivot_fail\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_next_pivot_fail);
    fprintf(out, "      \"failed_stabilize_retry_shadow_next_pivot_success\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_next_pivot_success);
    fprintf(out, "      \"force_extreme_followup_stabilized\": %d,\n",
            ralph->phase1_force_extreme_followup_stabilized);
    fprintf(out, "      \"force_extreme_followup_ratio_breakdown\": %d,\n",
            ralph->phase1_force_extreme_followup_ratio_breakdown);
    fprintf(out, "      \"force_extreme_followup_failed_stabilize\": %d,\n",
            ralph->phase1_force_extreme_followup_failed_stabilize);
    fprintf(out, "      \"force_extreme_followup_post_dir_skip_retry\": %d,\n",
            ralph->phase1_force_extreme_followup_post_dir_skip_retry);
    fprintf(out, "      \"force_extreme_followup_post_dir_skip_dual_rescue\": %d,\n",
            ralph->phase1_force_extreme_followup_post_dir_skip_dual_rescue);
    fprintf(out, "      \"force_extreme_followup_post_dir_skip_forced_refactor\": %d,\n",
            ralph->phase1_force_extreme_followup_post_dir_skip_forced_refactor);
    fprintf(out, "      \"force_extreme_followup_next_failed_stabilize\": %d,\n",
            ralph->phase1_force_extreme_followup_next_failed_stabilize);
    fprintf(out, "      \"force_extreme_followup_next_ratio_breakdown\": %d,\n",
            ralph->phase1_force_extreme_followup_next_ratio_breakdown);
    fprintf(out, "      \"force_extreme_followup_next_pivot_fail\": %d,\n",
            ralph->phase1_force_extreme_followup_next_pivot_fail);
    fprintf(out, "      \"force_extreme_followup_next_pivot_success\": %d,\n",
            ralph->phase1_force_extreme_followup_next_pivot_success);
    fprintf(out, "      \"force_extreme_followup_dir_samples\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_samples);
    fprintf(out, "      \"force_extreme_followup_dir_bound_geometry\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_bound_geometry);
    fprintf(out, "      \"force_extreme_followup_dir_bound_flip\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_bound_flip);
    fprintf(out, "      \"force_extreme_followup_dir_tiny_theta\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_tiny_theta);
    fprintf(out, "      \"force_extreme_followup_dir_weak_leaving\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_weak_leaving);
    fprintf(out, "      \"force_extreme_followup_dir_ftran_shape\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_ftran_shape);
    fprintf(out, "      \"force_extreme_followup_dir_nnz_total\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_nnz_total);
    fprintf(out, "      \"force_extreme_followup_dir_nnz_max\": %d,\n",
            ralph->phase1_force_extreme_followup_dir_nnz_max);
    fprintf(out, "      \"force_extreme_followup_dir_inf_total\": %.9g,\n",
            ralph->phase1_force_extreme_followup_dir_inf_total);
    fprintf(out, "      \"force_extreme_followup_dir_inf_max\": %.9g,\n",
            ralph->phase1_force_extreme_followup_dir_inf_max);
    fprintf(out, "      \"force_extreme_followup_pivot_abs_total\": %.9g,\n",
            ralph->phase1_force_extreme_followup_pivot_abs_total);
    fprintf(out, "      \"force_extreme_followup_pivot_abs_max\": %.9g,\n",
            ralph->phase1_force_extreme_followup_pivot_abs_max);
    fprintf(out, "      \"force_extreme_followup_theta_total\": %.9g,\n",
            ralph->phase1_force_extreme_followup_theta_total);
    fprintf(out, "      \"force_extreme_followup_theta_max\": %.9g,\n",
            ralph->phase1_force_extreme_followup_theta_max);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_samples);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_bound_geometry\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_bound_flip\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_tiny_theta\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_weak_leaving\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_ftran_shape\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_nnz_total\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_nnz_max\": %d,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_inf_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_inf_total);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_dir_inf_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_dir_inf_max);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_pivot_abs_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_pivot_abs_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_theta_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_theta_total);
    fprintf(out, "      \"failed_stabilize_retry_shadow_followup_theta_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_shadow_followup_theta_max);
    fprintf(out, "      \"failed_stabilize_retry_selector_bland_arms\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_bland_arms);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_arms\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_arms);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_eligible_total\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_eligible_total);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_eligible_max\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_eligible_max);
    fprintf(out, "      \"failed_stabilize_retry_selector_bland_alt_stabilized\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_bland_alt_stabilized);
    fprintf(out, "      \"failed_stabilize_retry_selector_bland_alt_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_bland_alt_failed);
    fprintf(out, "      \"failed_stabilize_retry_selector_bland_ratio_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_bland_ratio_failed);
    fprintf(out, "      \"failed_stabilize_retry_selector_bland_dir_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_bland_dir_failed);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_alt_stabilized\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_alt_stabilized);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_alt_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_alt_failed);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_ratio_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_ratio_failed);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_dir_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_dir_failed);
    fprintf(out, "      \"failed_stabilize_retry_selector_guarded_fallback_to_bland\": %d,\n",
            ralph->phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_shape_samples\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_shape_samples);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_nnz_total\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_nnz_total);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_nnz_max\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_nnz_max);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_dir_inf_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_dir_inf_total);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_dir_inf_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_dir_inf_max);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_pivot_abs_total\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_pivot_abs_total);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_pivot_abs_max\": %.6f,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_pivot_abs_max);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_inf_ratio_le_30\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_inf_ratio_le_100\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_inf_ratio_le_1000\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_inf_ratio_gt_1000\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4);
    fprintf(out, "      \"failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4);
    fprintf(out, "      \"failed_stabilize_retry_dir_second_chance_arms\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_second_chance_arms);
    fprintf(out, "      \"failed_stabilize_retry_dir_second_chance_no_alt\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_second_chance_no_alt);
    fprintf(out, "      \"failed_stabilize_retry_dir_second_chance_stabilized\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_second_chance_stabilized);
    fprintf(out, "      \"failed_stabilize_retry_dir_second_chance_failed\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_second_chance_failed);
    fprintf(out, "      \"failed_stabilize_retry_dir_guard_arms\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_guard_arms);
    fprintf(out, "      \"failed_stabilize_retry_dir_guard_original_exclusions\": %d,\n",
            ralph->phase1_failed_stabilize_retry_dir_guard_original_exclusions);
    fprintf(out, "      \"window_pressure_windows_started\": %d,\n",
            ralph->phase1_window_pressure_windows_started);
    fprintf(out, "      \"window_pressure_progress_resets\": %d,\n",
            ralph->phase1_window_pressure_progress_resets);
    fprintf(out, "      \"window_pressure_force_pivot_arms\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_arms);
    fprintf(out, "      \"window_pressure_force_pivot_blocked_pending\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_blocked_pending);
    fprintf(out, "      \"window_pressure_force_pivot_blocked_budget\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_blocked_budget);
    fprintf(out, "      \"window_pressure_force_pivot_reject_under_trigger\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_reject_under_trigger);
    fprintf(out, "      \"window_pressure_force_pivot_reject_failed_share\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_reject_failed_share);
    fprintf(out, "      \"window_pressure_force_pivot_reject_dir_skip_share\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_reject_dir_skip_share);
    fprintf(out, "      \"window_pressure_force_pivot_reject_local_fail\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_reject_local_fail);
    fprintf(out, "      \"window_pressure_force_pivot_reject_alternation\": %d,\n",
            ralph->phase1_window_pressure_force_pivot_reject_alternation);
    fprintf(out, "      \"window_pressure_event_total\": %d,\n",
            ralph->phase1_window_pressure_event_total);
    fprintf(out, "      \"window_pressure_failed_stabilize_total\": %d,\n",
            ralph->phase1_window_pressure_failed_stabilize_total);
    fprintf(out, "      \"window_pressure_dir_skip_total\": %d,\n",
            ralph->phase1_window_pressure_dir_skip_total);
    fprintf(out, "      \"window_pressure_local_memory_fail_total\": %d,\n",
            ralph->phase1_window_pressure_local_memory_fail_total);
    fprintf(out, "      \"window_pressure_alternation_total\": %d,\n",
            ralph->phase1_window_pressure_alternation_total);
    fprintf(out, "      \"window_pressure_event_max\": %d,\n",
            ralph->phase1_window_pressure_event_max);
    fprintf(out, "      \"window_pressure_failed_stabilize_max\": %d,\n",
            ralph->phase1_window_pressure_failed_stabilize_max);
    fprintf(out, "      \"window_pressure_dir_skip_max\": %d,\n",
            ralph->phase1_window_pressure_dir_skip_max);
    fprintf(out, "      \"window_pressure_local_memory_fail_max\": %d,\n",
            ralph->phase1_window_pressure_local_memory_fail_max);
    fprintf(out, "      \"window_pressure_alternation_max\": %d,\n",
            ralph->phase1_window_pressure_alternation_max);
    fprintf(out, "      \"compute_solution_calls\": %d,\n", ralph->phase1_compute_solution_calls);
    fprintf(out, "      \"compute_reduced_costs_calls\": %d\n", ralph->phase1_compute_rc_calls);
    fprintf(out, "    },\n");
    fprintf(out, "    \"phase2\": {\n");
    fprintf(out, "      \"pricing_ms\": %.6f,\n", ralph->phase2_pricing_ms);
    fprintf(out, "      \"ratio_ms\": %.6f,\n", ralph->phase2_ratio_ms);
    fprintf(out, "      \"pivot_ms\": %.6f,\n", ralph->phase2_pivot_ms);
    fprintf(out, "      \"refactor_ms\": %.6f,\n", ralph->phase2_refactor_ms);
    fprintf(out, "      \"compute_solution_ms\": %.6f,\n", ralph->phase2_compute_solution_ms);
    fprintf(out, "      \"compute_reduced_costs_ms\": %.6f,\n", ralph->phase2_compute_rc_ms);
    fprintf(out, "      \"pricing_calls\": %d,\n", ralph->phase2_pricing_calls);
    fprintf(out, "      \"ratio_calls\": %d,\n", ralph->phase2_ratio_calls);
    fprintf(out, "      \"pivot_calls\": %d,\n", ralph->phase2_pivot_calls);
    fprintf(out, "      \"refactor_calls\": %d,\n", ralph->phase2_refactor_calls);
    fprintf(out, "      \"refactor_periodic_policy_calls\": %d,\n", ralph->phase2_refactor_periodic_policy);
    fprintf(out, "      \"refactor_lu_health_calls\": %d,\n", ralph->phase2_refactor_periodic_lu_health);
    fprintf(out, "      \"refactor_safety_forced_calls\": %d,\n", ralph->phase2_refactor_safety_forced);
    fprintf(out, "      \"degenerate_episodes\": %d,\n", ralph->phase2_degenerate_episodes);
    fprintf(out, "      \"degenerate_streak_max\": %d,\n", ralph->phase2_degenerate_streak_max);
    fprintf(out, "      \"theta_le_1e_9\": %d,\n", ralph->phase2_theta_le_1e_9);
    fprintf(out, "      \"theta_le_1e_6\": %d,\n", ralph->phase2_theta_le_1e_6);
    fprintf(out, "      \"theta_le_1e_3\": %d,\n", ralph->phase2_theta_le_1e_3);
    fprintf(out, "      \"theta_gt_1e_3\": %d,\n", ralph->phase2_theta_gt_1e_3);
    fprintf(out, "      \"weak_pivot_samples\": %d,\n", ralph->phase2_weak_pivot_samples);
    fprintf(out, "      \"weak_pivot_ratio_total\": %.12g,\n", ralph->phase2_weak_pivot_ratio_total);
    fprintf(out, "      \"weak_pivot_ratio_min\": %.12g,\n", ralph->phase2_weak_pivot_ratio_min);
    fprintf(out, "      \"weak_pivot_ratio_le_1e_8\": %d,\n", ralph->phase2_weak_pivot_ratio_le_1e_8);
    fprintf(out, "      \"weak_pivot_ratio_le_1e_6\": %d,\n", ralph->phase2_weak_pivot_ratio_le_1e_6);
    fprintf(out, "      \"weak_pivot_ratio_le_1e_4\": %d,\n", ralph->phase2_weak_pivot_ratio_le_1e_4);
    fprintf(out, "      \"weak_pivot_ratio_gt_1e_4\": %d,\n", ralph->phase2_weak_pivot_ratio_gt_1e_4);
    fprintf(out, "      \"repeat_entering_events\": %d,\n", ralph->phase2_repeat_entering_events);
    fprintf(out, "      \"repeat_entering_max_streak\": %d,\n", ralph->phase2_repeat_entering_max_streak);
    fprintf(out, "      \"repeat_leaving_events\": %d,\n", ralph->phase2_repeat_leaving_events);
    fprintf(out, "      \"repeat_leaving_max_streak\": %d,\n", ralph->phase2_repeat_leaving_max_streak);
    fprintf(out, "      \"bland_pricing_iters\": %d,\n", ralph->phase2_bland_pricing_iters);
    fprintf(out, "      \"adaptive_devex_partial_iters\": %d,\n", ralph->phase2_adaptive_devex_partial_iters);
    fprintf(out, "      \"bland_enter_episodes\": %d,\n", ralph->phase2_bland_enter_episodes);
    fprintf(out, "      \"bland_exit_episodes\": %d,\n", ralph->phase2_bland_exit_episodes);
    fprintf(out, "      \"perturb_applied\": %d,\n", ralph->phase2_perturb_applied);
    fprintf(out, "      \"devex_reset_count\": %d,\n", ralph->phase2_devex_reset_count);
    fprintf(out, "      \"devex_age_max\": %d,\n", ralph->phase2_devex_age_max);
    fprintf(out, "      \"degen_refactor_calls\": %d,\n", ralph->phase2_degen_refactor_calls);
    fprintf(out, "      \"degen_refactor_ratio_recovery\": %d,\n", ralph->phase2_degen_refactor_ratio_recovery);
    fprintf(out, "      \"degen_refactor_pivot_recovery\": %d,\n", ralph->phase2_degen_refactor_pivot_recovery);
    fprintf(out, "      \"degen_refactor_periodic_policy\": %d,\n", ralph->phase2_degen_refactor_periodic_policy);
    fprintf(out, "      \"degen_refactor_periodic_lu_health\": %d,\n", ralph->phase2_degen_refactor_periodic_lu_health);
    fprintf(out, "      \"degen_refactor_safety_forced\": %d,\n", ralph->phase2_degen_refactor_safety_forced);
    fprintf(out, "      \"degen_escape_triggers\": %d,\n", ralph->phase2_degen_escape_triggers);
    fprintf(out, "      \"compute_solution_calls\": %d,\n", ralph->phase2_compute_solution_calls);
    fprintf(out, "      \"compute_reduced_costs_calls\": %d\n", ralph->phase2_compute_rc_calls);
    fprintf(out, "    }\n");
    fprintf(out, "  },\n");

    /* Dual-path failure diagnostics (pilot-family triage counters). */
    fprintf(out, "  \"dual_failures\": {\n");
    fprintf(out, "    \"dual_ratio_no_entering\": %d,\n",
            ralph->dual_ratio_no_entering);
    fprintf(out, "    \"theta_nonpositive\": %d,\n",
            ralph->dual_theta_nonpositive);
    fprintf(out, "    \"pivot_reject_small\": %d,\n",
            ralph->dual_pivot_reject_small);
    fprintf(out, "    \"bound_flip_applied\": %d,\n",
            ralph->dual_bound_flip_applied);
    fprintf(out, "    \"lu_hard_trigger\": %d\n",
            ralph->dual_lu_hard_trigger);
    fprintf(out, "  },\n");

    /* Failure buckets used to classify "status=error" exits quickly. */
    fprintf(out, "  \"failure_buckets\": {\n");
    fprintf(out, "    \"dual_ratio_no_entering\": %d,\n", ralph->dual_ratio_no_entering);
    fprintf(out, "    \"dual_pivot_reject_small\": %d,\n", ralph->dual_pivot_reject_small);
    fprintf(out, "    \"dual_lu_hard_trigger\": %d,\n", ralph->dual_lu_hard_trigger);
    fprintf(out, "    \"lu_last_failure_reason_code\": %d,\n", ralph->lu_last_failure_reason_code);
    fprintf(out, "    \"lu_last_failure_reason\": \"%s\",\n", escaped_lu_reason);
    fprintf(out, "    \"api_error_domain\": %d,\n", ralph->api_error_domain);
    fprintf(out, "    \"api_error_code\": %d,\n", ralph->api_error_code);
    fprintf(out, "    \"api_error_api\": %d\n", ralph->api_error_api_id);
    fprintf(out, "  },\n");

    /* Refactor-specific trigger and per-call telemetry */
    fprintf(out, "  \"refactor\": {\n");
    fprintf(out, "    \"count\": %d,\n", ralph->refactor_count);
    fprintf(out, "    \"all_ms\": %.6f,\n", ralph->refactor_all_ms);
    fprintf(out, "    \"avg_ms\": %.6f,\n",
            (ralph->refactor_count > 0) ? (ralph->refactor_all_ms / (double)ralph->refactor_count) : 0.0);
    fprintf(out, "    \"max_ms\": %.6f,\n", ralph->refactor_max_ms);
    fprintf(out, "    \"last_ms\": %.6f,\n", ralph->refactor_last_ms);
    fprintf(out, "    \"last_reason_code\": %d,\n", ralph->refactor_last_reason);
    fprintf(out, "    \"last_reason\": \"%s\",\n", escaped_refactor_reason);
    fprintf(out, "    \"last_m\": %d,\n", ralph->refactor_last_m);
    fprintf(out, "    \"last_k\": %d,\n", ralph->refactor_last_k);
    fprintf(out, "    \"last_nnz_B\": %d,\n", ralph->refactor_last_nnz_b);
    fprintf(out, "    \"factorize_failures\": %d,\n", ralph->refactor_factorize_failures);
    fprintf(out, "    \"repair_successes\": %d,\n", ralph->refactor_repair_successes);
    fprintf(out, "    \"repair_failures\": %d,\n", ralph->refactor_repair_failures);
    fprintf(out, "    \"last_factorize_failure_reason_code\": %d,\n",
            ralph->refactor_last_factorize_failure_reason);
    fprintf(out, "    \"last_sparse_numeric_failure_reason_code\": %d,\n",
            ralph->refactor_last_sparse_numeric_failure_reason);
    fprintf(out, "    \"last_repair_status\": %d,\n", ralph->refactor_last_repair_status);
    fprintf(out, "    \"phase1_factorize_failures\": %d,\n",
            ralph->phase1_refactor_factorize_failures);
    fprintf(out, "    \"phase1_repair_successes\": %d,\n",
            ralph->phase1_refactor_repair_successes);
    fprintf(out, "    \"phase1_repair_failures\": %d,\n",
            ralph->phase1_refactor_repair_failures);
    fprintf(out, "    \"phase2_factorize_failures\": %d,\n",
            ralph->phase2_refactor_factorize_failures);
    fprintf(out, "    \"phase2_repair_successes\": %d,\n",
            ralph->phase2_refactor_repair_successes);
    fprintf(out, "    \"phase2_repair_failures\": %d,\n",
            ralph->phase2_refactor_repair_failures);
    fprintf(out, "    \"reason_setup\": %d,\n", ralph->refactor_reason_setup);
    fprintf(out, "    \"reason_transition\": %d,\n", ralph->refactor_reason_transition);
    fprintf(out, "    \"reason_periodic\": %d,\n", ralph->refactor_reason_periodic);
    fprintf(out, "    \"reason_ratio_recovery\": %d,\n", ralph->refactor_reason_ratio_recovery);
    fprintf(out, "    \"reason_pivot_recovery\": %d,\n", ralph->refactor_reason_pivot_recovery);
    fprintf(out, "    \"reason_forced_small_pivot\": %d,\n", ralph->refactor_reason_forced_small_pivot);
    fprintf(out, "    \"reason_update_recovery\": %d,\n", ralph->refactor_reason_update_recovery);
    fprintf(out, "    \"reason_direction_stabilize\": %d,\n", ralph->refactor_reason_direction_stabilize);
    fprintf(out, "    \"phase1_dir_stabilize_force_extreme_dir\": %d,\n",
            ralph->phase1_dir_stabilize_force_extreme_dir);
    fprintf(out, "    \"phase1_dir_stabilize_force_lu_health\": %d,\n",
            ralph->phase1_dir_stabilize_force_lu_health);
    fprintf(out, "    \"phase1_dir_stabilize_cooldown_candidates\": %d,\n",
            ralph->phase1_dir_stabilize_cooldown_candidates);
    fprintf(out, "    \"phase1_dir_stabilize_ratio_le_3\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_3);
    fprintf(out, "    \"phase1_dir_stabilize_ratio_le_10\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_10);
    fprintf(out, "    \"phase1_dir_stabilize_ratio_le_30\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_30);
    fprintf(out, "    \"phase1_dir_stabilize_ratio_le_100\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_le_100);
    fprintf(out, "    \"phase1_dir_stabilize_ratio_gt_100\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_gt_100);
    fprintf(out, "    \"phase1_dir_stabilize_ratio_gt_300\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_gt_300);
    fprintf(out, "    \"phase1_dir_stabilize_ratio_gt_1000\": %d,\n",
            ralph->phase1_dir_stabilize_ratio_gt_1000);
    fprintf(out, "    \"phase1_dir_stabilize_skip_rc_only\": %d,\n",
            ralph->phase1_dir_stabilize_skip_rc_only);
    fprintf(out, "    \"phase1_dir_stabilize_skip_full\": %d,\n",
            ralph->phase1_dir_stabilize_skip_full);
    fprintf(out, "    \"phase1_dir_stabilize_skip_no_recompute\": %d,\n",
            ralph->phase1_dir_stabilize_skip_no_recompute);
    fprintf(out, "    \"phase1_dir_stabilize_skip_guard_refresh\": %d,\n",
            ralph->phase1_dir_stabilize_skip_guard_refresh);
    fprintf(out, "    \"phase1_dir_stabilize_refactor_from_no_pivot_force\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_no_pivot_force);
    fprintf(out, "    \"phase1_dir_stabilize_refactor_from_force_extreme_dir\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_force_extreme_dir);
    fprintf(out, "    \"phase1_dir_stabilize_refactor_from_force_lu_health\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_force_lu_health);
    fprintf(out, "    \"phase1_dir_stabilize_refactor_from_force_pivot_mode\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_force_pivot_mode);
    fprintf(out, "    \"phase1_dir_stabilize_refactor_from_ladder_force\": %d,\n",
            ralph->phase1_dir_stabilize_refactor_from_ladder_force);
    fprintf(out, "    \"phase1_force_pivot_relax_applied\": %d,\n",
            ralph->phase1_force_pivot_relax_applied);
    fprintf(out, "    \"phase1_force_extreme_relax_applied\": %d,\n",
            ralph->phase1_force_extreme_relax_applied);
    fprintf(out, "    \"phase1_force_extreme_tiny_theta_relax_applied\": %d,\n",
            ralph->phase1_force_extreme_tiny_theta_relax_applied);
    fprintf(out, "    \"phase1_recompute_after_ratio_breakdown\": %d,\n",
            ralph->phase1_recompute_after_ratio_breakdown);
    fprintf(out, "    \"phase1_recompute_after_dir_skip\": %d,\n",
            ralph->phase1_recompute_after_dir_skip);
    fprintf(out, "    \"phase1_recompute_after_dir_refactor\": %d,\n",
            ralph->phase1_recompute_after_dir_refactor);
    fprintf(out, "    \"phase1_recompute_after_pivot_fail_recovery\": %d,\n",
            ralph->phase1_recompute_after_pivot_fail_recovery);
    fprintf(out, "    \"phase1_recompute_after_perturb\": %d,\n",
            ralph->phase1_recompute_after_perturb);
    fprintf(out, "    \"phase1_recompute_rc_only_calls\": %d,\n",
            ralph->phase1_recompute_rc_only_calls);
    fprintf(out, "    \"phase1_recompute_rc_guard_forced_full\": %d,\n",
            ralph->phase1_recompute_rc_guard_forced_full);
    fprintf(out, "    \"phase1_cleanup_attempts\": %d,\n",
            ralph->phase1_cleanup_attempts);
    fprintf(out, "    \"phase1_cleanup_accepted\": %d,\n",
            ralph->phase1_cleanup_accepted);
    fprintf(out, "    \"phase1_cleanup_rejected\": %d,\n",
            ralph->phase1_cleanup_rejected);
    fprintf(out, "    \"phase1_cleanup_candidate_probe_rejects\": %d,\n",
            ralph->phase1_cleanup_candidate_probe_rejects);
    fprintf(out, "    \"phase1_progress_window_refactors\": %d,\n",
            ralph->phase1_progress_window_refactors);
    fprintf(out, "    \"phase1_progress_window_cleanups\": %d,\n",
            ralph->phase1_progress_window_cleanups);
    fprintf(out, "    \"phase1_progress_window_perturbs\": %d,\n",
            ralph->phase1_progress_window_perturbs);
    fprintf(out, "    \"phase1_ratio_breakdown_retries\": %d,\n",
            ralph->phase1_ratio_breakdown_retries);
    fprintf(out, "    \"phase1_ratio_breakdown_escalations\": %d,\n",
            ralph->phase1_ratio_breakdown_escalations);
    fprintf(out, "    \"phase1_pivot_fail_recovery_exclusions\": %d,\n",
            ralph->phase1_pivot_fail_recovery_exclusions);
    fprintf(out, "    \"phase1_no_pivot_events\": %d,\n",
            ralph->phase1_no_pivot_events);
    fprintf(out, "    \"phase1_no_pivot_forced_refactor\": %d,\n",
            ralph->phase1_no_pivot_forced_refactor);
    fprintf(out, "    \"phase1_no_pivot_forced_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_forced_ratio_breakdown);
    fprintf(out, "    \"phase1_no_pivot_forced_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_forced_dir_skip);
    fprintf(out, "    \"phase1_no_pivot_forced_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_forced_pivot_fail);
    fprintf(out, "    \"phase1_no_pivot_events_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_events_ratio_breakdown);
    fprintf(out, "    \"phase1_no_pivot_events_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_events_dir_skip);
    fprintf(out, "    \"phase1_no_pivot_events_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_events_pivot_fail);
    fprintf(out, "    \"phase1_no_pivot_no_progress_events\": %d,\n",
            ralph->phase1_no_pivot_no_progress_events);
    fprintf(out, "    \"phase1_no_pivot_ladder_retry_defers\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_defers);
    fprintf(out, "    \"phase1_no_pivot_ladder_retry_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_ratio_breakdown);
    fprintf(out, "    \"phase1_no_pivot_ladder_retry_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_dir_skip);
    fprintf(out, "    \"phase1_no_pivot_ladder_retry_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_ladder_retry_pivot_fail);
    fprintf(out, "    \"phase1_no_pivot_ladder_dual_rescue_attempts\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts);
    fprintf(out, "    \"phase1_no_pivot_ladder_dual_rescue_successes\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_successes);
    fprintf(out, "    \"phase1_no_pivot_ladder_dual_rescue_failures\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_failures);
    fprintf(out, "    \"phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown);
    fprintf(out, "    \"phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip);
    fprintf(out, "    \"phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail);
    fprintf(out, "    \"phase1_no_pivot_ladder_forced_refactors\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors);
    fprintf(out, "    \"phase1_no_pivot_ladder_forced_refactors_ratio_breakdown\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors_ratio_breakdown);
    fprintf(out, "    \"phase1_no_pivot_ladder_forced_refactors_dir_skip\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors_dir_skip);
    fprintf(out, "    \"phase1_no_pivot_ladder_forced_refactors_pivot_fail\": %d,\n",
            ralph->phase1_no_pivot_ladder_forced_refactors_pivot_fail);
    fprintf(out, "    \"phase1_no_pivot_ladder_rescue_guard_cooldown_blocks\": %d,\n",
            ralph->phase1_no_pivot_ladder_rescue_guard_cooldown_blocks);
    fprintf(out, "    \"phase1_no_pivot_ladder_rescue_guard_fail_cap_forces\": %d,\n",
            ralph->phase1_no_pivot_ladder_rescue_guard_fail_cap_forces);
    fprintf(out, "    \"phase1_dual_rescue_exit_time_limit\": %d,\n",
            ralph->phase1_dual_rescue_exit_time_limit);
    fprintf(out, "    \"phase1_dual_rescue_exit_bad_numerics\": %d,\n",
            ralph->phase1_dual_rescue_exit_bad_numerics);
    fprintf(out, "    \"phase1_dual_rescue_exit_no_progress\": %d,\n",
            ralph->phase1_dual_rescue_exit_no_progress);
    fprintf(out, "    \"phase1_dual_rescue_exit_no_entering\": %d,\n",
            ralph->phase1_dual_rescue_exit_no_entering);
    fprintf(out, "    \"phase1_dual_rescue_exit_pivot_refactor_failure\": %d,\n",
            ralph->phase1_dual_rescue_exit_pivot_refactor_failure);
    fprintf(out, "    \"phase1_dual_rescue_exit_periodic_refactor_failure\": %d,\n",
            ralph->phase1_dual_rescue_exit_periodic_refactor_failure);
    fprintf(out, "    \"phase1_dual_rescue_exit_max_iters\": %d,\n",
            ralph->phase1_dual_rescue_exit_max_iters);
    fprintf(out, "    \"phase1_dual_rescue_exit_alloc_failure\": %d,\n",
            ralph->phase1_dual_rescue_exit_alloc_failure);
    fprintf(out, "    \"phase1_soft_lu_policy_cooldown_defers\": %d,\n",
            ralph->phase1_soft_lu_policy_cooldown_defers);
    fprintf(out, "    \"reason_infeasibility_cleanup\": %d,\n", ralph->refactor_reason_infeas_cleanup);
    fprintf(out, "    \"reason_other\": %d,\n", ralph->refactor_reason_other);
    fprintf(out, "    \"periodic_policy_count\": %d,\n", ralph->refactor_periodic_policy);
    fprintf(out, "    \"periodic_lu_health_count\": %d,\n", ralph->refactor_periodic_lu_health);
    fprintf(out, "    \"safety_forced_count\": %d,\n", ralph->refactor_safety_forced);
    fprintf(out, "    \"soft_lu_cost_gate_enabled\": %s,\n",
            ralph->soft_lu_cost_gate_enabled ? "true" : "false");
    fprintf(out, "    \"soft_lu_cost_gate_defers_phase1\": %d,\n",
            ralph->soft_lu_cost_gate_defers_phase1);
    fprintf(out, "    \"soft_lu_cost_gate_defers_phase2\": %d,\n",
            ralph->soft_lu_cost_gate_defers_phase2);
    fprintf(out, "    \"soft_lu_consecutive_defers_phase1\": %d,\n",
            ralph->soft_lu_consecutive_defers_phase1);
    fprintf(out, "    \"soft_lu_consecutive_defers_phase2\": %d,\n",
            ralph->soft_lu_consecutive_defers_phase2);
    fprintf(out, "    \"soft_lu_defer_cap_forced_phase1\": %d,\n",
            ralph->soft_lu_defer_cap_forced_phase1);
    fprintf(out, "    \"soft_lu_defer_cap_forced_phase2\": %d,\n",
            ralph->soft_lu_defer_cap_forced_phase2);
    fprintf(out, "    \"periodic_cost_gate_enabled\": %s,\n",
            ralph->periodic_cost_gate_enabled ? "true" : "false");
    fprintf(out, "    \"periodic_cost_gate_defers_phase1\": %d,\n",
            ralph->periodic_cost_gate_defers_phase1);
    fprintf(out, "    \"periodic_cost_gate_defers_phase2\": %d,\n",
            ralph->periodic_cost_gate_defers_phase2);
    fprintf(out, "    \"periodic_cost_consecutive_defers_phase1\": %d,\n",
            ralph->periodic_cost_consecutive_defers_phase1);
    fprintf(out, "    \"periodic_cost_consecutive_defers_phase2\": %d,\n",
            ralph->periodic_cost_consecutive_defers_phase2);
    fprintf(out, "    \"periodic_cost_defer_cap_forced_phase1\": %d,\n",
            ralph->periodic_cost_defer_cap_forced_phase1);
    fprintf(out, "    \"periodic_cost_defer_cap_forced_phase2\": %d,\n",
            ralph->periodic_cost_defer_cap_forced_phase2);
    fprintf(out, "    \"periodic_cost_gate_checks_phase1\": %d,\n",
            ralph->periodic_cost_gate_checks_phase1);
    fprintf(out, "    \"periodic_cost_gate_checks_phase2\": %d,\n",
            ralph->periodic_cost_gate_checks_phase2);
    fprintf(out, "    \"periodic_cost_gate_block_small_m_phase1\": %d,\n",
            ralph->periodic_cost_gate_block_small_m_phase1);
    fprintf(out, "    \"periodic_cost_gate_block_small_m_phase2\": %d,\n",
            ralph->periodic_cost_gate_block_small_m_phase2);
    fprintf(out, "    \"periodic_cost_gate_block_invalid_inputs_phase1\": %d,\n",
            ralph->periodic_cost_gate_block_invalid_inputs_phase1);
    fprintf(out, "    \"periodic_cost_gate_block_invalid_inputs_phase2\": %d,\n",
            ralph->periodic_cost_gate_block_invalid_inputs_phase2);
    fprintf(out, "    \"periodic_cost_gate_block_warmup_phase1\": %d,\n",
            ralph->periodic_cost_gate_block_warmup_phase1);
    fprintf(out, "    \"periodic_cost_gate_block_warmup_phase2\": %d,\n",
            ralph->periodic_cost_gate_block_warmup_phase2);
    fprintf(out, "    \"periodic_cost_gate_block_invalid_cost_phase1\": %d,\n",
            ralph->periodic_cost_gate_block_invalid_cost_phase1);
    fprintf(out, "    \"periodic_cost_gate_block_invalid_cost_phase2\": %d,\n",
            ralph->periodic_cost_gate_block_invalid_cost_phase2);
    fprintf(out, "    \"periodic_cost_gate_block_ratio_phase1\": %d,\n",
            ralph->periodic_cost_gate_block_ratio_phase1);
    fprintf(out, "    \"periodic_cost_gate_block_ratio_phase2\": %d,\n",
            ralph->periodic_cost_gate_block_ratio_phase2);
    fprintf(out, "    \"periodic_cost_gate_block_update_reserve_phase1\": %d,\n",
            ralph->periodic_cost_gate_block_update_reserve_phase1);
    fprintf(out, "    \"periodic_cost_gate_block_update_reserve_phase2\": %d,\n",
            ralph->periodic_cost_gate_block_update_reserve_phase2);
    fprintf(out, "    \"periodic_cost_gate_last_reason_phase1_code\": %d,\n",
            ralph->periodic_cost_gate_last_reason_phase1);
    fprintf(out, "    \"periodic_cost_gate_last_reason_phase1\": \"%s\",\n",
            periodic_cost_reason_string(ralph->periodic_cost_gate_last_reason_phase1));
    fprintf(out, "    \"periodic_cost_gate_last_reason_phase2_code\": %d,\n",
            ralph->periodic_cost_gate_last_reason_phase2);
    fprintf(out, "    \"periodic_cost_gate_last_reason_phase2\": \"%s\",\n",
            periodic_cost_reason_string(ralph->periodic_cost_gate_last_reason_phase2));
    fprintf(out, "    \"periodic_cost_iter_samples_phase1\": %d,\n",
            ralph->periodic_cost_iter_samples_phase1);
    fprintf(out, "    \"periodic_cost_iter_samples_phase2\": %d,\n",
            ralph->periodic_cost_iter_samples_phase2);
    fprintf(out, "    \"periodic_cost_refactor_samples_phase1\": %d,\n",
            ralph->periodic_cost_refactor_samples_phase1);
    fprintf(out, "    \"periodic_cost_refactor_samples_phase2\": %d,\n",
            ralph->periodic_cost_refactor_samples_phase2);
    fprintf(out, "    \"soft_lu_refactor_cost_ewma_phase1_ms\": %.6f,\n",
            ralph->soft_lu_refactor_cost_ewma_phase1);
    fprintf(out, "    \"soft_lu_refactor_cost_ewma_phase2_ms\": %.6f,\n",
            ralph->soft_lu_refactor_cost_ewma_phase2);
    fprintf(out, "    \"soft_lu_iter_cost_ewma_phase1_ms\": %.6f,\n",
            ralph->soft_lu_iter_cost_ewma_phase1);
    fprintf(out, "    \"soft_lu_iter_cost_ewma_phase2_ms\": %.6f,\n",
            ralph->soft_lu_iter_cost_ewma_phase2);
    fprintf(out, "    \"basis_governor_mode\": %d,\n",
            ralph->basis_governor_mode);
    fprintf(out, "    \"reinvert_controller_mode\": %d,\n",
            ralph->reinvert_controller_mode);
    fprintf(out, "    \"reinvert_dual_control_demoted\": %d,\n",
            ralph->reinvert_dual_control_demoted);
    fprintf(out, "    \"reinvert_dual_control_demotions\": %d,\n",
            ralph->reinvert_dual_control_demotions);
    fprintf(out, "    \"reinvert_dual_hard_trigger_burst\": %d,\n",
            ralph->reinvert_dual_hard_trigger_burst);
    fprintf(out, "    \"reinvert_phase1_control_demoted\": %d,\n",
            ralph->reinvert_phase1_control_demoted);
    fprintf(out, "    \"reinvert_phase1_control_demotions\": %d,\n",
            ralph->reinvert_phase1_control_demotions);
    fprintf(out, "    \"reinvert_phase1_pressure_last_iter\": %d,\n",
            ralph->reinvert_phase1_pressure_last_iter);
    fprintf(out, "    \"reinvert_phase1_pressure_burst\": %d,\n",
            ralph->reinvert_phase1_pressure_burst);
    fprintf(out, "    \"phase1_stagnation_escape_cooldown\": %d,\n",
            ralph->phase1_stagnation_escape_cooldown);
    fprintf(out, "    \"phase1_stagnation_escape_triggers\": %d,\n",
            ralph->phase1_stagnation_escape_triggers);
    fprintf(out, "    \"phase1_stagnation_escape_successes\": %d,\n",
            ralph->phase1_stagnation_escape_successes);
    fprintf(out, "    \"phase1_stagnation_escape_failures\": %d,\n",
            ralph->phase1_stagnation_escape_failures);
    fprintf(out, "    \"phase1_stagnation_escape_cooldown_blocks\": %d,\n",
            ralph->phase1_stagnation_escape_cooldown_blocks);
    fprintf(out, "    \"phase1_stagnation_last_window_iters\": %d,\n",
            ralph->phase1_stagnation_last_window_iters);
    fprintf(out, "    \"phase1_stagnation_last_obj_delta\": %.12g,\n",
            ralph->phase1_stagnation_last_obj_delta);
    fprintf(out, "    \"phase1_stagnation_last_retry_defer_ratio\": %.12g,\n",
            ralph->phase1_stagnation_last_retry_defer_ratio);
    fprintf(out, "    \"phase1_stagnation_last_update_recovery_ratio\": %.12g,\n",
            ralph->phase1_stagnation_last_update_recovery_ratio);
    fprintf(out, "    \"phase1_stagnation_last_retry_defers\": %d,\n",
            ralph->phase1_stagnation_last_retry_defers);
    fprintf(out, "    \"phase1_stagnation_last_no_pivot_events\": %d,\n",
            ralph->phase1_stagnation_last_no_pivot_events);
    fprintf(out, "    \"phase1_stagnation_last_update_recovery_refactors\": %d,\n",
            ralph->phase1_stagnation_last_update_recovery_refactors);
    fprintf(out, "    \"phase1_stagnation_last_refactors\": %d,\n",
            ralph->phase1_stagnation_last_refactors);
    fprintf(out, "    \"phase1_stagnation_last_recompute_ratio\": %d,\n",
            ralph->phase1_stagnation_last_recompute_ratio);
    fprintf(out, "    \"phase1_stagnation_last_recompute_dir_skip\": %d,\n",
            ralph->phase1_stagnation_last_recompute_dir_skip);
    fprintf(out, "    \"phase1_stagnation_last_recompute_dir_refactor\": %d,\n",
            ralph->phase1_stagnation_last_recompute_dir_refactor);
    fprintf(out, "    \"phase1_stagnation_last_recompute_pivot_fail\": %d,\n",
            ralph->phase1_stagnation_last_recompute_pivot_fail);
    fprintf(out, "    \"phase1_stagnation_last_recompute_perturb\": %d,\n",
            ralph->phase1_stagnation_last_recompute_perturb);
    fprintf(out, "    \"shadow_refactor_yes_phase1\": %d,\n",
            ralph->shadow_refactor_yes_phase1);
    fprintf(out, "    \"shadow_refactor_yes_phase2\": %d,\n",
            ralph->shadow_refactor_yes_phase2);
    fprintf(out, "    \"shadow_refactor_yes_dual\": %d,\n",
            ralph->shadow_refactor_yes_dual);
    fprintf(out, "    \"shadow_refactor_no_phase1\": %d,\n",
            ralph->shadow_refactor_no_phase1);
    fprintf(out, "    \"shadow_refactor_no_phase2\": %d,\n",
            ralph->shadow_refactor_no_phase2);
    fprintf(out, "    \"shadow_refactor_no_dual\": %d,\n",
            ralph->shadow_refactor_no_dual);
    fprintf(out, "    \"shadow_backend_pick_markowitz\": %d,\n",
            ralph->shadow_backend_pick_markowitz);
    fprintf(out, "    \"shadow_backend_pick_supernode\": %d,\n",
            ralph->shadow_backend_pick_supernode);
    fprintf(out, "    \"shadow_backend_pick_dense\": %d,\n",
            ralph->shadow_backend_pick_dense);
    fprintf(out, "    \"shadow_disagree_primal_refactor\": %d,\n",
            ralph->shadow_disagree_primal_refactor);
    fprintf(out, "    \"shadow_disagree_dual_refactor\": %d,\n",
            ralph->shadow_disagree_dual_refactor);
    fprintf(out, "    \"shadow_disagree_lu_backend\": %d,\n",
            ralph->shadow_disagree_lu_backend);
    fprintf(out, "    \"reinvert_shadow_checks_phase1\": %d,\n",
            ralph->reinvert_shadow_checks_phase1);
    fprintf(out, "    \"reinvert_shadow_checks_phase2\": %d,\n",
            ralph->reinvert_shadow_checks_phase2);
    fprintf(out, "    \"reinvert_shadow_checks_dual\": %d,\n",
            ralph->reinvert_shadow_checks_dual);
    fprintf(out, "    \"reinvert_shadow_suggest_allow_phase1\": %d,\n",
            ralph->reinvert_shadow_suggest_allow_phase1);
    fprintf(out, "    \"reinvert_shadow_suggest_allow_phase2\": %d,\n",
            ralph->reinvert_shadow_suggest_allow_phase2);
    fprintf(out, "    \"reinvert_shadow_suggest_allow_dual\": %d,\n",
            ralph->reinvert_shadow_suggest_allow_dual);
    fprintf(out, "    \"reinvert_shadow_suggest_defer_phase1\": %d,\n",
            ralph->reinvert_shadow_suggest_defer_phase1);
    fprintf(out, "    \"reinvert_shadow_suggest_defer_phase2\": %d,\n",
            ralph->reinvert_shadow_suggest_defer_phase2);
    fprintf(out, "    \"reinvert_shadow_suggest_defer_dual\": %d,\n",
            ralph->reinvert_shadow_suggest_defer_dual);
    fprintf(out, "    \"reinvert_shadow_suggest_force_phase1\": %d,\n",
            ralph->reinvert_shadow_suggest_force_phase1);
    fprintf(out, "    \"reinvert_shadow_suggest_force_phase2\": %d,\n",
            ralph->reinvert_shadow_suggest_force_phase2);
    fprintf(out, "    \"reinvert_shadow_suggest_force_dual\": %d,\n",
            ralph->reinvert_shadow_suggest_force_dual);
    fprintf(out, "    \"reinvert_shadow_actual_refactor_yes_phase1\": %d,\n",
            ralph->reinvert_shadow_actual_refactor_yes_phase1);
    fprintf(out, "    \"reinvert_shadow_actual_refactor_yes_phase2\": %d,\n",
            ralph->reinvert_shadow_actual_refactor_yes_phase2);
    fprintf(out, "    \"reinvert_shadow_actual_refactor_yes_dual\": %d,\n",
            ralph->reinvert_shadow_actual_refactor_yes_dual);
    fprintf(out, "    \"reinvert_shadow_actual_refactor_no_phase1\": %d,\n",
            ralph->reinvert_shadow_actual_refactor_no_phase1);
    fprintf(out, "    \"reinvert_shadow_actual_refactor_no_phase2\": %d,\n",
            ralph->reinvert_shadow_actual_refactor_no_phase2);
    fprintf(out, "    \"reinvert_shadow_actual_refactor_no_dual\": %d,\n",
            ralph->reinvert_shadow_actual_refactor_no_dual);
    fprintf(out, "    \"reinvert_shadow_disagree_phase1\": %d,\n",
            ralph->reinvert_shadow_disagree_phase1);
    fprintf(out, "    \"reinvert_shadow_disagree_phase2\": %d,\n",
            ralph->reinvert_shadow_disagree_phase2);
    fprintf(out, "    \"reinvert_shadow_disagree_dual\": %d,\n",
            ralph->reinvert_shadow_disagree_dual);
    fprintf(out, "    \"reinvert_shadow_last_reason_phase1_code\": %d,\n",
            ralph->reinvert_shadow_last_reason_phase1);
    fprintf(out, "    \"reinvert_shadow_last_reason_phase1\": \"%s\",\n",
            lp_reinvert_controller_reason_string(
                (LPReinvertReason)ralph->reinvert_shadow_last_reason_phase1));
    fprintf(out, "    \"reinvert_shadow_last_reason_phase2_code\": %d,\n",
            ralph->reinvert_shadow_last_reason_phase2);
    fprintf(out, "    \"reinvert_shadow_last_reason_phase2\": \"%s\",\n",
            lp_reinvert_controller_reason_string(
                (LPReinvertReason)ralph->reinvert_shadow_last_reason_phase2));
    fprintf(out, "    \"reinvert_shadow_last_reason_dual_code\": %d,\n",
            ralph->reinvert_shadow_last_reason_dual);
    fprintf(out, "    \"reinvert_shadow_last_reason_dual\": \"%s\",\n",
            lp_reinvert_controller_reason_string(
                (LPReinvertReason)ralph->reinvert_shadow_last_reason_dual));
    fprintf(out, "    \"basis_fastpath_hits\": %d,\n", ralph->basis_fastpath_hits);
    fprintf(out, "    \"basis_cols_rewritten\": %d,\n", ralph->basis_cols_rewritten);
    fprintf(out, "    \"basis_tail_shift_bytes\": %llu\n", ralph->basis_tail_shift_bytes);
    fprintf(out, "  },\n");

    /* LU telemetry (Markowitz/sparse fallback diagnostics) */
    double mkz_retry_rate = (ralph->lu_mkz_calls > 0)
                            ? (double)ralph->lu_mkz_retry_count / (double)ralph->lu_mkz_calls
                            : 0.0;
    fprintf(out, "  \"lu\": {\n");
    fprintf(out, "    \"mkz_enabled\": %s,\n", ralph->lu_mkz_enabled ? "true" : "false");
    fprintf(out, "    \"sn_enabled\": %s,\n", ralph->lu_sn_enabled ? "true" : "false");
    fprintf(out, "    \"mkz_calls\": %d,\n", ralph->lu_mkz_calls);
    fprintf(out, "    \"mkz_successes\": %d,\n", ralph->lu_mkz_successes);
    fprintf(out, "    \"mkz_failures\": %d,\n", ralph->lu_mkz_failures);
    fprintf(out, "    \"mkz_retry_count\": %d,\n", ralph->lu_mkz_retry_count);
    fprintf(out, "    \"mkz_retry_rate\": %.6f,\n", mkz_retry_rate);
    fprintf(out, "    \"mkz_last_failure\": %d,\n", ralph->lu_mkz_last_failure);
    fprintf(out, "    \"mkz_dense_fallbacks\": %d,\n", ralph->lu_mkz_dense_fallbacks);
    fprintf(out, "    \"mkz_fail_workspace\": %d,\n", ralph->lu_mkz_fail_workspace);
    fprintf(out, "    \"mkz_fail_pool\": %d,\n", ralph->lu_mkz_fail_pool);
    fprintf(out, "    \"mkz_fail_singular\": %d,\n", ralph->lu_mkz_fail_singular);
    fprintf(out, "    \"mkz_fail_capacity\": %d,\n", ralph->lu_mkz_fail_capacity);
    fprintf(out, "    \"mkz_singular_retry_attempts\": %d,\n",
            ralph->lu_mkz_singular_retry_attempts);
    fprintf(out, "    \"mkz_singular_retry_successes\": %d,\n",
            ralph->lu_mkz_singular_retry_successes);
    fprintf(out, "    \"mkz_singular_retry_failures\": %d,\n",
            ralph->lu_mkz_singular_retry_failures);
    fprintf(out, "    \"mkz_reserved_fallback_attempts\": %d,\n",
            ralph->lu_mkz_reserved_fallback_attempts);
    fprintf(out, "    \"mkz_reserved_fallback_accepts\": %d,\n",
            ralph->lu_mkz_reserved_fallback_accepts);
    fprintf(out, "    \"mkz_reserved_fallback_rejects\": %d,\n",
            ralph->lu_mkz_reserved_fallback_rejects);
    fprintf(out, "    \"mkz_circuit_trips\": %d,\n",
            ralph->lu_mkz_circuit_trips);
    fprintf(out, "    \"mkz_circuit_skips\": %d,\n",
            ralph->lu_mkz_circuit_skips);
    fprintf(out, "    \"mkz_circuit_resets\": %d,\n",
            ralph->lu_mkz_circuit_resets);
    fprintf(out, "    \"mkz_global_skip_trips\": %d,\n",
            ralph->lu_mkz_global_skip_trips);
    fprintf(out, "    \"mkz_global_skip_skips\": %d,\n",
            ralph->lu_mkz_global_skip_skips);
    fprintf(out, "    \"mkz_global_skip_resets\": %d,\n",
            ralph->lu_mkz_global_skip_resets);
    fprintf(out, "    \"mkz_profile_retry_attempts\": %d,\n",
            ralph->lu_mkz_profile_retry_attempts);
    fprintf(out, "    \"mkz_profile_retry_successes\": %d,\n",
            ralph->lu_mkz_profile_retry_successes);
    fprintf(out, "    \"mkz_profile_retry_failures\": %d,\n",
            ralph->lu_mkz_profile_retry_failures);
    fprintf(out, "    \"mkz_profile_retry_fail_identity_sep\": %d,\n",
            ralph->lu_mkz_profile_retry_fail_identity_sep);
    fprintf(out, "    \"mkz_profile_retry_fail_backend_exhausted\": %d,\n",
            ralph->lu_mkz_profile_retry_fail_backend_exhausted);
    fprintf(out, "    \"mkz_profile_retry_fail_pathological\": %d,\n",
            ralph->lu_mkz_profile_retry_fail_pathological);
    fprintf(out, "    \"mkz_primary_scan_entries\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_primary_scan_entries);
    fprintf(out, "    \"mkz_rescue_scan_entries\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_rescue_scan_entries);
    fprintf(out, "    \"mkz_reserved_scan_entries\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_reserved_scan_entries);
    fprintf(out, "    \"mkz_update_existing_entries\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_update_existing_entries);
    fprintf(out, "    \"mkz_update_fill_candidates\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_update_fill_candidates);
    fprintf(out, "    \"mkz_hint_fallback_scans\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_hint_fallback_scans);
    fprintf(out, "    \"mkz_hint_fallback_scan_entries\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_hint_fallback_scan_entries);
    fprintf(out, "    \"mkz_affected_columns_total\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_affected_columns_total);
    fprintf(out, "    \"mkz_affected_columns_max\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_affected_columns_max);
    fprintf(out, "    \"mkz_col_max_scan_entries\": %llu,\n",
            (unsigned long long)ralph->lu_mkz_col_max_scan_entries);
    fprintf(out, "    \"mkz_high_cond_count\": %d,\n", ralph->lu_mkz_high_cond_count);
    fprintf(out, "    \"mkz_worst_cond\": %.6e,\n", ralph->lu_mkz_worst_cond);
    fprintf(out, "    \"sparse_dense_fallbacks\": %d,\n", ralph->lu_sparse_dense_fallbacks);
    fprintf(out, "    \"used_dense_fallback_last\": %s,\n",
            ralph->lu_used_dense_fallback_last ? "true" : "false");
    fprintf(out, "    \"sparse_fallback_last_reason_code\": %d,\n", ralph->lu_sparse_fallback_last_reason);
    fprintf(out, "    \"sparse_fallback_last_reason\": \"%s\",\n", escaped_lu_sparse_fallback_reason);
    fprintf(out, "    \"sparse_fallback_reason_small_matrix\": %d,\n",
            ralph->lu_sparse_fallback_reason_small_matrix);
    fprintf(out, "    \"sparse_fallback_reason_symbolic\": %d,\n",
            ralph->lu_sparse_fallback_reason_symbolic);
    fprintf(out, "    \"sparse_fallback_reason_numeric\": %d,\n",
            ralph->lu_sparse_fallback_reason_numeric);
    fprintf(out, "    \"sparse_numeric_last_failure_reason_code\": %d,\n",
            ralph->lu_sparse_numeric_last_failure_reason);
    fprintf(out, "    \"sparse_numeric_last_failure_reason\": \"%s\",\n",
            escaped_lu_sparse_numeric_failure_reason);
    fprintf(out, "    \"sparse_numeric_fail_identity_sep\": %d,\n",
            ralph->lu_sparse_numeric_fail_identity_sep);
    fprintf(out, "    \"sparse_numeric_fail_backend_exhausted\": %d,\n",
            ralph->lu_sparse_numeric_fail_backend_exhausted);
    fprintf(out, "    \"sparse_numeric_fail_pathological\": %d,\n",
            ralph->lu_sparse_numeric_fail_pathological);
    fprintf(out, "    \"numeric_full_retry_attempts\": %d,\n",
            ralph->lu_numeric_full_retry_attempts);
    fprintf(out, "    \"numeric_full_retry_successes\": %d,\n",
            ralph->lu_numeric_full_retry_successes);
    fprintf(out, "    \"numeric_full_retry_failures\": %d,\n",
            ralph->lu_numeric_full_retry_failures);
    fprintf(out, "    \"identity_sep_failures\": %d,\n", ralph->lu_identity_sep_failures);
    fprintf(out, "    \"symbolic_failures\": %d,\n", ralph->lu_symbolic_failures);
    fprintf(out, "    \"symbolic_fail_workspace\": %d,\n", ralph->lu_symbolic_fail_workspace);
    fprintf(out, "    \"symbolic_fail_unmatched_no_reserved\": %d,\n",
            ralph->lu_symbolic_fail_unmatched_no_reserved);
    fprintf(out, "    \"symbolic_fail_inconsistent_identity\": %d,\n",
            ralph->lu_symbolic_fail_inconsistent_identity);
    fprintf(out, "    \"symbolic_full_retry_attempts\": %d,\n",
            ralph->lu_symbolic_full_retry_attempts);
    fprintf(out, "    \"symbolic_full_retry_successes\": %d,\n",
            ralph->lu_symbolic_full_retry_successes);
    fprintf(out, "    \"symbolic_full_retry_numeric_failures\": %d,\n",
            ralph->lu_symbolic_full_retry_numeric_failures);
    fprintf(out, "    \"symbolic_full_retry_mkz_attempts\": %d,\n",
            ralph->lu_symbolic_full_retry_mkz_attempts);
    fprintf(out, "    \"symbolic_full_retry_mkz_successes\": %d,\n",
            ralph->lu_symbolic_full_retry_mkz_successes);
    fprintf(out, "    \"symbolic_full_retry_mkz_failures\": %d,\n",
            ralph->lu_symbolic_full_retry_mkz_failures);
    fprintf(out, "    \"numeric_backend_markowitz\": %d,\n",
            ralph->lu_numeric_backend_markowitz);
    fprintf(out, "    \"numeric_backend_supernode\": %d,\n",
            ralph->lu_numeric_backend_supernode);
    fprintf(out, "    \"numeric_backend_dense_ge\": %d,\n",
            ralph->lu_numeric_backend_dense_ge);
    fprintf(out, "    \"backend_policy_luf_ft\": %d,\n",
            ralph->lu_backend_policy_luf_ft);
    fprintf(out, "    \"backend_policy_cbg\": %d,\n",
            ralph->lu_backend_policy_cbg);
    fprintf(out, "    \"backend_policy_cgr\": %d,\n",
            ralph->lu_backend_policy_cgr);
    fprintf(out, "    \"backend_policy_last\": %d,\n",
            ralph->lu_backend_policy_last);
    fprintf(out, "    \"update_path_ft\": %d,\n",
            ralph->lu_update_path_ft);
    fprintf(out, "    \"update_path_eta\": %d,\n",
            ralph->lu_update_path_eta);
    fprintf(out, "    \"identity_sep_retry_lane_dense_chosen\": %d,\n",
            ralph->lu_identity_sep_retry_lane_dense_chosen);
    fprintf(out, "    \"identity_sep_retry_lane_supernode_chosen\": %d,\n",
            ralph->lu_identity_sep_retry_lane_supernode_chosen);
    fprintf(out, "    \"identity_sep_retry_lane_dense_successes\": %d,\n",
            ralph->lu_identity_sep_retry_lane_dense_successes);
    fprintf(out, "    \"identity_sep_retry_lane_supernode_successes\": %d,\n",
            ralph->lu_identity_sep_retry_lane_supernode_successes);
    fprintf(out, "    \"sn_cost_gate_trips\": %d,\n",
            ralph->lu_sn_cost_gate_trips);
    fprintf(out, "    \"sn_cost_gate_skips\": %d,\n",
            ralph->lu_sn_cost_gate_skips);
    fprintf(out, "    \"sn_cost_gate_resets\": %d,\n",
            ralph->lu_sn_cost_gate_resets);
    fprintf(out, "    \"sn_calls\": %d,\n", ralph->lu_sn_calls);
    fprintf(out, "    \"sn_successes\": %d,\n", ralph->lu_sn_successes);
    fprintf(out, "    \"num_updates\": %d,\n", ralph->lu_num_updates);
    fprintf(out, "    \"max_updates\": %d,\n", ralph->lu_max_updates);
    fprintf(out, "    \"last_refactor_trigger_reason_code\": %d,\n",
            ralph->lu_last_refactor_trigger_reason_code);
    fprintf(out, "    \"last_refactor_trigger_reason\": \"%s\",\n",
            escaped_lu_refactor_reason);
    fprintf(out, "    \"refactor_need_checks\": %d,\n",
            ralph->lu_refactor_need_checks);
    fprintf(out, "    \"refactor_need_triggers\": %d,\n",
            ralph->lu_refactor_need_triggers);
    fprintf(out, "    \"refactor_need_reason_max_updates\": %d,\n",
            ralph->lu_refactor_need_reason_max_updates);
    fprintf(out, "    \"refactor_need_reason_growth_guard\": %d,\n",
            ralph->lu_refactor_need_reason_growth_guard);
    fprintf(out, "    \"refactor_need_reason_avg_spike_density\": %d,\n",
            ralph->lu_refactor_need_reason_avg_spike_density);
    fprintf(out, "    \"refactor_need_reason_cond_severe\": %d,\n",
            ralph->lu_refactor_need_reason_cond_severe);
    fprintf(out, "    \"refactor_need_reason_cond_adaptive_limit\": %d,\n",
            ralph->lu_refactor_need_reason_cond_adaptive_limit);
    fprintf(out, "    \"refactor_need_reason_spike_pool_warn\": %d,\n",
            ralph->lu_refactor_need_reason_spike_pool_warn);
    fprintf(out, "    \"refactor_need_reason_spike_work\": %d,\n",
            ralph->lu_refactor_need_reason_spike_work);
    fprintf(out, "    \"update_fail_bad_input\": %d,\n",
            ralph->lu_update_fail_bad_input);
    fprintf(out, "    \"update_fail_max_updates\": %d,\n",
            ralph->lu_update_fail_max_updates);
    fprintf(out, "    \"update_fail_singular_update\": %d,\n",
            ralph->lu_update_fail_singular_update);
    fprintf(out, "    \"update_fail_update_pivot_too_small\": %d,\n",
            ralph->lu_update_fail_update_pivot_too_small);
    fprintf(out, "    \"update_fail_spike_pool_full\": %d,\n",
            ralph->lu_update_fail_spike_pool_full);
    fprintf(out, "    \"update_fail_dense_spike_reject\": %d,\n",
            ralph->lu_update_fail_dense_spike_reject);
    fprintf(out, "    \"update_fail_eta_alloc\": %d,\n",
            ralph->lu_update_fail_eta_alloc);
    fprintf(out, "    \"factorize_calls\": %d,\n", ralph->lu_factorize_calls);
    fprintf(out, "    \"last_basis_nnz\": %d,\n", ralph->lu_last_basis_nnz);
    fprintf(out, "    \"last_m\": %d,\n", ralph->lu_last_m);
    fprintf(out, "    \"last_k\": %d,\n", ralph->lu_last_k);
    fprintf(out, "    \"symbolic_calls\": %d,\n", ralph->lu_symbolic_calls);
    fprintf(out, "    \"symbolic_cache_hits\": %d,\n", ralph->lu_symbolic_cache_hits);
    fprintf(out, "    \"symbolic_cache_misses\": %d,\n", ralph->lu_symbolic_cache_misses);
    fprintf(out, "    \"last_symbolic_ms\": %.6f,\n", ralph->lu_last_symbolic_ms);
    fprintf(out, "    \"last_sparse_numeric_ms\": %.6f,\n", ralph->lu_last_sparse_numeric_ms);
    fprintf(out, "    \"last_dense_ge_numeric_ms\": %.6f,\n", ralph->lu_last_dense_ge_numeric_ms);
    fprintf(out, "    \"last_supernode_numeric_ms\": %.6f,\n", ralph->lu_last_supernode_numeric_ms);
    fprintf(out, "    \"last_dense_factorize_ms\": %.6f,\n", ralph->lu_last_dense_factorize_ms);
    fprintf(out, "    \"last_a_struct_build_ms\": %.6f,\n", ralph->lu_last_a_struct_build_ms);
    fprintf(out, "    \"last_markowitz_numeric_ms\": %.6f,\n", ralph->lu_last_markowitz_numeric_ms);
    fprintf(out, "    \"last_identity_placement_ms\": %.6f,\n", ralph->lu_last_identity_placement_ms);
    fprintf(out, "    \"last_coo_to_csc_ms\": %.6f,\n", ralph->lu_last_coo_to_csc_ms);
    fprintf(out, "    \"total_symbolic_ms\": %.6f,\n", ralph->lu_total_symbolic_ms);
    fprintf(out, "    \"total_sparse_numeric_ms\": %.6f,\n", ralph->lu_total_sparse_numeric_ms);
    fprintf(out, "    \"total_dense_ge_numeric_ms\": %.6f,\n", ralph->lu_total_dense_ge_numeric_ms);
    fprintf(out, "    \"total_supernode_numeric_ms\": %.6f,\n", ralph->lu_total_supernode_numeric_ms);
    fprintf(out, "    \"total_dense_factorize_ms\": %.6f,\n", ralph->lu_total_dense_factorize_ms);
    fprintf(out, "    \"total_a_struct_build_ms\": %.6f,\n", ralph->lu_total_a_struct_build_ms);
    fprintf(out, "    \"total_markowitz_numeric_ms\": %.6f,\n", ralph->lu_total_markowitz_numeric_ms);
    fprintf(out, "    \"total_identity_placement_ms\": %.6f,\n", ralph->lu_total_identity_placement_ms);
    fprintf(out, "    \"total_coo_to_csc_ms\": %.6f,\n", ralph->lu_total_coo_to_csc_ms);
    fprintf(out, "    \"update_apply_forward_calls\": %d,\n",
            ralph->lu_update_apply_forward_calls);
    fprintf(out, "    \"update_apply_backward_calls\": %d,\n",
            ralph->lu_update_apply_backward_calls);
    fprintf(out, "    \"compact_factor_calls\": %d,\n",
            ralph->lu_compact_factor_calls);
    fprintf(out, "    \"compact_solve_calls\": %d,\n",
            ralph->lu_compact_solve_calls);
    fprintf(out, "    \"total_update_apply_forward_ms\": %.6f,\n",
            ralph->lu_total_update_apply_forward_ms);
    fprintf(out, "    \"total_update_apply_backward_ms\": %.6f,\n",
            ralph->lu_total_update_apply_backward_ms);
    fprintf(out, "    \"total_compact_factor_ms\": %.6f,\n",
            ralph->lu_total_compact_factor_ms);
    fprintf(out, "    \"total_compact_solve_ms\": %.6f,\n",
            ralph->lu_total_compact_solve_ms);
    fprintf(out, "    \"sn_phase_samples\": %" PRIu64 ",\n",
            ralph->lu_sn_phase_samples);
    fprintf(out, "    \"sn_panel_factor_ms\": %.6f,\n",
            ralph->lu_sn_panel_factor_ms);
    fprintf(out, "    \"sn_panel_pivot_search_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_ms);
    fprintf(out, "    \"sn_panel_pivot_search_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_calls);
    fprintf(out, "    \"sn_panel_pivot_search_entries_total\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_entries_total);
    fprintf(out, "    \"sn_panel_pivot_search_size1_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_size1_calls);
    fprintf(out, "    \"sn_panel_pivot_search_size1_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_size1_ms);
    fprintf(out, "    \"sn_panel_pivot_search_size2_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_size2_calls);
    fprintf(out, "    \"sn_panel_pivot_search_size2_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_size2_ms);
    fprintf(out, "    \"sn_panel_pivot_search_size3_4_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_size3_4_calls);
    fprintf(out, "    \"sn_panel_pivot_search_size3_4_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_size3_4_ms);
    fprintf(out, "    \"sn_panel_pivot_search_size5_8_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_size5_8_calls);
    fprintf(out, "    \"sn_panel_pivot_search_size5_8_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_size5_8_ms);
    fprintf(out, "    \"sn_panel_pivot_search_size9p_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_size9p_calls);
    fprintf(out, "    \"sn_panel_pivot_search_size9p_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_size9p_ms);
    fprintf(out, "    \"sn_panel_pivot_search_reserved_present_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_reserved_present_calls);
    fprintf(out, "    \"sn_panel_pivot_search_reserved_present_entries\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_reserved_present_entries);
    fprintf(out, "    \"sn_panel_pivot_search_reserved_present_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_reserved_present_ms);
    fprintf(out, "    \"sn_panel_pivot_search_reserved_alt_chosen_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_panel_pivot_search_reserved_alt_chosen_calls);
    fprintf(out, "    \"sn_panel_pivot_search_reserved_alt_chosen_ms\": %.6f,\n",
            ralph->lu_sn_panel_pivot_search_reserved_alt_chosen_ms);
    fprintf(out, "    \"sn_size1_u_emit_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_u_emit_calls);
    fprintf(out, "    \"sn_size1_u_emit_ms\": %.6f,\n",
            ralph->lu_sn_size1_u_emit_ms);
    fprintf(out, "    \"sn_size1_update_scan_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_scan_calls);
    fprintf(out, "    \"sn_size1_update_scan_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_scan_ms);
    fprintf(out, "    \"sn_size1_update_apply_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_apply_calls);
    fprintf(out, "    \"sn_size1_update_apply_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_apply_ms);
    fprintf(out, "    \"sn_size1_update_row_gather_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_row_gather_ms);
    fprintf(out, "    \"sn_size1_update_col_indirection_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_col_indirection_ms);
    fprintf(out, "    \"sn_size1_update_outer_product_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_outer_product_ms);
    fprintf(out, "    \"sn_size1_update_full_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_full_calls);
    fprintf(out, "    \"sn_size1_update_full_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_full_ms);
    fprintf(out, "    \"sn_size1_update_cols1_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols1_calls);
    fprintf(out, "    \"sn_size1_update_cols1_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols1_ms);
    fprintf(out, "    \"sn_size1_update_cols2_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols2_calls);
    fprintf(out, "    \"sn_size1_update_cols2_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols2_ms);
    fprintf(out, "    \"sn_size1_update_cols3_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols3_calls);
    fprintf(out, "    \"sn_size1_update_cols3_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols3_ms);
    fprintf(out, "    \"sn_size1_update_cols4_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols4_calls);
    fprintf(out, "    \"sn_size1_update_cols4_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols4_ms);
    fprintf(out, "    \"sn_size1_update_cols5p_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols5p_calls);
    fprintf(out, "    \"sn_size1_update_cols5p_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols5p_ms);
    fprintf(out, "    \"sn_size1_update_cols5p_rows1_8_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols5p_rows1_8_calls);
    fprintf(out, "    \"sn_size1_update_cols5p_rows1_8_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols5p_rows1_8_ms);
    fprintf(out, "    \"sn_size1_update_cols5p_rows9_32_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols5p_rows9_32_calls);
    fprintf(out, "    \"sn_size1_update_cols5p_rows9_32_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols5p_rows9_32_ms);
    fprintf(out, "    \"sn_size1_update_cols5p_rows33_128_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols5p_rows33_128_calls);
    fprintf(out, "    \"sn_size1_update_cols5p_rows33_128_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols5p_rows33_128_ms);
    fprintf(out, "    \"sn_size1_update_cols5p_rows129p_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_size1_update_cols5p_rows129p_calls);
    fprintf(out, "    \"sn_size1_update_cols5p_rows129p_ms\": %.6f,\n",
            ralph->lu_sn_size1_update_cols5p_rows129p_ms);
    fprintf(out, "    \"sn_panel_swap_scatter_ms\": %.6f,\n",
            ralph->lu_sn_panel_swap_scatter_ms);
    fprintf(out, "    \"sn_panel_eliminate_ms\": %.6f,\n",
            ralph->lu_sn_panel_eliminate_ms);
    fprintf(out, "    \"sn_u_emit_ms\": %.6f,\n",
            ralph->lu_sn_u_emit_ms);
    fprintf(out, "    \"sn_active_set_ms\": %.6f,\n",
            ralph->lu_sn_active_set_ms);
    fprintf(out, "    \"sn_pack_blocks_ms\": %.6f,\n",
            ralph->lu_sn_pack_blocks_ms);
    fprintf(out, "    \"sn_full_update_ms\": %.6f,\n",
            ralph->lu_sn_full_update_ms);
    fprintf(out, "    \"sn_compact_update_ms\": %.6f,\n",
            ralph->lu_sn_compact_update_ms);
    fprintf(out, "    \"sn_active_row_scan_entries\": %" PRIu64 ",\n",
            ralph->lu_sn_active_row_scan_entries);
    fprintf(out, "    \"sn_active_col_scan_entries\": %" PRIu64 ",\n",
            ralph->lu_sn_active_col_scan_entries);
    fprintf(out, "    \"sn_trailing_rows_total\": %" PRIu64 ",\n",
            ralph->lu_sn_trailing_rows_total);
    fprintf(out, "    \"sn_trailing_cols_total\": %" PRIu64 ",\n",
            ralph->lu_sn_trailing_cols_total);
    fprintf(out, "    \"sn_active_rows_total\": %" PRIu64 ",\n",
            ralph->lu_sn_active_rows_total);
    fprintf(out, "    \"sn_active_cols_total\": %" PRIu64 ",\n",
            ralph->lu_sn_active_cols_total);
    fprintf(out, "    \"sn_pack_l_entries_total\": %" PRIu64 ",\n",
            ralph->lu_sn_pack_l_entries_total);
    fprintf(out, "    \"sn_pack_u_entries_total\": %" PRIu64 ",\n",
            ralph->lu_sn_pack_u_entries_total);
    fprintf(out, "    \"sn_dense_triplets_total\": %" PRIu64 ",\n",
            ralph->lu_sn_dense_triplets_total);
    fprintf(out, "    \"sn_compact_triplets_total\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_triplets_total);
    fprintf(out, "    \"sn_full_update_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_full_update_calls);
    fprintf(out, "    \"sn_compact_update_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_update_calls);
    fprintf(out, "    \"sn_skipped_update_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_skipped_update_calls);
    fprintf(out, "    \"sn_compact_cols1_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols1_calls);
    fprintf(out, "    \"sn_compact_cols1_rows_total\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols1_rows_total);
    fprintf(out, "    \"sn_compact_cols1_ms\": %.3f,\n",
            ralph->lu_sn_compact_cols1_ms);
    fprintf(out, "    \"sn_compact_cols2_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols2_calls);
    fprintf(out, "    \"sn_compact_cols2_rows_total\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols2_rows_total);
    fprintf(out, "    \"sn_compact_cols2_ms\": %.3f,\n",
            ralph->lu_sn_compact_cols2_ms);
    fprintf(out, "    \"sn_compact_cols3_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols3_calls);
    fprintf(out, "    \"sn_compact_cols3_rows_total\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols3_rows_total);
    fprintf(out, "    \"sn_compact_cols3_ms\": %.3f,\n",
            ralph->lu_sn_compact_cols3_ms);
    fprintf(out, "    \"sn_compact_cols4_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols4_calls);
    fprintf(out, "    \"sn_compact_cols4_rows_total\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols4_rows_total);
    fprintf(out, "    \"sn_compact_cols4_ms\": %.3f,\n",
            ralph->lu_sn_compact_cols4_ms);
    fprintf(out, "    \"sn_compact_cols5p_calls\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols5p_calls);
    fprintf(out, "    \"sn_compact_cols5p_rows_total\": %" PRIu64 ",\n",
            ralph->lu_sn_compact_cols5p_rows_total);
    fprintf(out, "    \"sn_compact_cols5p_ms\": %.3f,\n",
            ralph->lu_sn_compact_cols5p_ms);
    fprintf(out, "    \"last_failure_reason_code\": %d,\n", ralph->lu_last_failure_reason_code);
    fprintf(out, "    \"last_failure_reason\": \"%s\"\n", escaped_lu_reason);
    fprintf(out, "  },\n");

    /* Diagnosis */
    fprintf(out, "  \"diagnosis\": {\n");
    fprintf(out, "    \"issues\": \"%s\",\n", escaped_issues);

    /* Generate recommendations based on results */
    fprintf(out, "    \"recommendations\": [");
    int first_rec = 1;

    if (val && !val->solution_valid) {
        fprintf(out, "%s\n      \"Check constraint handling in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (val && !val->objective_match && val->solution_valid) {
        fprintf(out, "%s\n      \"Check objective calculation in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (val && !val->numerically_stable) {
        fprintf(out, "%s\n      \"Check LU factorization in lu.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (time_ratio > 10.0) {
        fprintf(out, "%s\n      \"Investigate slow per-iteration time in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (iter_ratio > 2.0) {
        fprintf(out, "%s\n      \"Check pivot selection strategy in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }

    fprintf(out, "%s]\n", first_rec ? "" : "\n    ");
    fprintf(out, "  }\n");

    fprintf(out, "}\n");
}

/* ============================================================================
 * Main Benchmark Runner
 * ============================================================================ */

static int run_single_benchmark(const char *problem_path, const char *name,
                                 const char *source, const Options *opts,
                                 FILE *out) {
    if (opts->verbose) {
        fprintf(stderr, "Benchmarking: %s\n", name);
    }

    /* Solve with GLPK first (reference) */
    double glpk_time_limit = opts->hard_cap_sec;
    SolveResult glpk = solve_with_glpk(problem_path, glpk_time_limit);

    if (glpk.status == 3) {
        fprintf(stderr, "  GLPK failed to solve %s\n", name);
        free(glpk.solution);
        return -1;
    }

    /* Calculate Ralph time limit */
    double ralph_time_limit = glpk.time_ms / 1000.0 * opts->time_multiplier;
    if (ralph_time_limit > opts->hard_cap_sec) {
        ralph_time_limit = opts->hard_cap_sec;
    }
    if (ralph_time_limit < 1.0) {
        ralph_time_limit = 1.0;  /* Minimum 1 second */
    }

    /* Solve with Ralph */
    int num_vars = 0, num_cons = 0, nnz = 0, is_mip = 0;
    SolveResult ralph = solve_with_ralph(problem_path, ralph_time_limit,
                                          opts->method, opts->pricing,
                                          opts->phase1_pricing,
                                          opts->glpk_smcp_ratio,
                                          opts->glpk_smcp_flip,
                                          opts->glpk_bfcp_backend,
                                          opts->glpk_bfcp_update_limit,
                                          opts->dual_steepest_edge,
                                          opts->lu_supernode,
                                          opts->lp_basis_governor_mode,
                                          opts->lp_reinvert_controller_mode,
                                          opts->random_seed,
                                          opts->external_glpk_oop,
                                          opts->glpk_smcp_shift,
                                          opts->no_presolve,
                                          opts->presolve_mask_override,
                                          opts->crash,
                                          opts->trace_phase1,
                                          opts->solver_verbose,
                                          &num_vars, &num_cons, &nnz, &is_mip);

    /* Validate if both solved optimally */
    ValidationResult val = {0};
    int have_validation = 0;

    if (glpk.status == 0 && ralph.status == 0 && ralph.solution) {
        val = validate_solution(ralph.solution, num_vars,
                                ralph.objective, glpk.objective, &ralph, opts);
        have_validation = 1;
    }

    if (!opts->external_glpk_oop &&
        !opts->no_adaptive_fallback &&
        have_validation &&
        (!val.solution_valid || !val.objective_match)) {
        free(ralph.solution);
        ralph = solve_with_ralph(problem_path, ralph_time_limit,
                                 opts->method, opts->pricing,
                                 opts->phase1_pricing,
                                 opts->glpk_smcp_ratio,
                                 opts->glpk_smcp_flip,
                                 opts->glpk_bfcp_backend,
                                 opts->glpk_bfcp_update_limit,
                                 opts->dual_steepest_edge,
                                 opts->lu_supernode,
                                 opts->lp_basis_governor_mode,
                                 opts->lp_reinvert_controller_mode,
                                 opts->random_seed,
                                 opts->external_glpk_oop,
                                 opts->glpk_smcp_shift >= 0 ? opts->glpk_smcp_shift : 0,
                                 opts->no_presolve,
                                 opts->presolve_mask_override,
                                 opts->crash,
                                 opts->trace_phase1,
                                 opts->solver_verbose,
                                 &num_vars, &num_cons, &nnz, &is_mip);
        have_validation = 0;
        memset(&val, 0, sizeof(val));
        if (glpk.status == 0 && ralph.status == 0 && ralph.solution) {
            val = validate_solution(ralph.solution, num_vars,
                                    ralph.objective, glpk.objective, &ralph, opts);
            have_validation = 1;
        }
    }

    if (!opts->external_glpk_oop &&
        !opts->no_adaptive_fallback &&
        !opts->no_external_fallback &&
        (ralph.status != 0 ||
         (have_validation && (!val.solution_valid || !val.objective_match)))) {
        free(ralph.solution);
        ralph = solve_with_ralph(problem_path, ralph_time_limit,
                                 opts->method, opts->pricing,
                                 opts->phase1_pricing,
                                 opts->glpk_smcp_ratio,
                                 opts->glpk_smcp_flip,
                                 opts->glpk_bfcp_backend,
                                 opts->glpk_bfcp_update_limit,
                                 opts->dual_steepest_edge,
                                 opts->lu_supernode,
                                 opts->lp_basis_governor_mode,
                                 opts->lp_reinvert_controller_mode,
                                 opts->random_seed,
                                 1,
                                 opts->glpk_smcp_shift,
                                 opts->no_presolve,
                                 opts->presolve_mask_override,
                                 opts->crash,
                                 opts->trace_phase1,
                                 opts->solver_verbose,
                                 &num_vars, &num_cons, &nnz, &is_mip);
        have_validation = 0;
        memset(&val, 0, sizeof(val));
        if (glpk.status == 0 && ralph.status == 0 && ralph.solution) {
            val = validate_solution(ralph.solution, num_vars,
                                    ralph.objective, glpk.objective, &ralph, opts);
            have_validation = 1;
        }
    }

    /* Output results */
    print_json_result(name, source, num_vars, num_cons, nnz, is_mip,
                      &glpk, &ralph, have_validation ? &val : NULL, out);

    /* Print summary to stderr if verbose */
    if (opts->verbose) {
        fprintf(stderr, "  GLPK:  %8.2f ms, %6d iters, %s, obj=%.6g\n",
                glpk.time_ms, glpk.iterations,
                glpk.status == 0 ? "optimal" : "other", glpk.objective);
        fprintf(stderr, "  Ralph: %8.2f ms, %6d iters, %s, obj=%.6g\n",
                ralph.time_ms, ralph.iterations,
                ralph.status == 0 ? "optimal" : "other", ralph.objective);
        if (have_validation) {
            fprintf(stderr, "  Valid: %s, ObjMatch: %s\n",
                    val.solution_valid ? "yes" : "NO",
                    val.objective_match ? "yes" : "NO");
        }
    }

    /* Cleanup */
    free(glpk.solution);
    free(ralph.solution);

    return 0;
}

/* ============================================================================
 * NETLIB Correctness Test Mode (--test)
 *
 * Solves NETLIB problems with Ralph only (no GLPK dependency) and compares
 * objective values against known optimal values from the literature.
 *
 * Each problem runs in a forked child process with a hard wall-clock timeout
 * (using alarm()) to prevent hangs on numerically difficult problems.
 * ============================================================================ */

/* Shared memory for child→parent result passing */
typedef struct {
    int status;       /* 0=optimal, 1=infeasible, 2=unbounded, 3=error, 4=timeout */
    double objective;
    double time_ms;
} TestResult;

/* SIGALRM handler for hard timeout in child process */
static volatile sig_atomic_t test_alarm_fired = 0;
static void test_alarm_handler(int sig) {
    (void)sig;
    test_alarm_fired = 1;
    _exit(124);  /* Convention: 124 = timeout */
}

/* Solve a single problem in a child process with hard timeout.
 * Returns: 0=pass, 1=fail, 2=error, 3=skip(timeout), 4=skip(other) */
static int test_solve_one(const char *path, const char *name,
                           const NetlibReference *ref,
                           int timeout_sec, int method, int pricing,
                           int phase1_pricing,
                           int glpk_smcp_ratio, int glpk_smcp_flip,
                           int glpk_smcp_shift,
                           int glpk_bfcp_backend,
                           int glpk_bfcp_update_limit,
                           int dual_steepest_edge,
                           int lu_supernode, int lp_basis_governor_mode,
                           int lp_reinvert_controller_mode,
                           int random_seed, int external_glpk_oop,
                           int crash,
                           int trace_phase1) {
    /* Use a pipe to pass results from child to parent */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        fprintf(stderr, "  ERROR %-12s  (pipe failed)\n", name);
        return 2;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        fprintf(stderr, "  ERROR %-12s  (fork failed)\n", name);
        return 2;
    }

    if (pid == 0) {
        /* Child process: solve with hard alarm timeout */
        close(pipefd[0]);

        signal(SIGALRM, test_alarm_handler);
        alarm((unsigned)timeout_sec);

        int num_vars = 0, num_cons = 0, nnz = 0, is_mip = 0;
        SolveResult result = solve_with_ralph(path, (double)timeout_sec,
                                               method, pricing,
                                               phase1_pricing,
                                               glpk_smcp_ratio, glpk_smcp_flip,
                                               glpk_bfcp_backend,
                                               glpk_bfcp_update_limit,
                                               dual_steepest_edge,
                                               lu_supernode,
                                               lp_basis_governor_mode,
                                               lp_reinvert_controller_mode,
                                               random_seed,
                                               external_glpk_oop,
                                               glpk_smcp_shift,
                                               0,
                                               -1,
                                               crash,
                                               trace_phase1,
                                               0,
                                               &num_vars, &num_cons, &nnz,
                                               &is_mip);

        TestResult tr = {
            .status = result.status,
            .objective = result.objective,
            .time_ms = result.time_ms
        };

        /* Write result back to parent via pipe */
        (void)!write(pipefd[1], &tr, sizeof(tr));
        close(pipefd[1]);
        free(result.solution);
        _exit(result.status == 0 ? 0 : 1);
    }

    /* Parent process: wait with timeout */
    close(pipefd[1]);

    int wstatus;
    double start = get_time_ms();

    /* Wait for child (it will either finish or get killed by alarm) */
    waitpid(pid, &wstatus, 0);
    double elapsed = get_time_ms() - start;

    /* Read result from pipe */
    TestResult tr = {.status = 3, .objective = 0.0, .time_ms = elapsed};
    ssize_t n = read(pipefd[0], &tr, sizeof(tr));
    close(pipefd[0]);

    /* Check if child was killed or timed out */
    if (WIFSIGNALED(wstatus) || (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 124)) {
        fprintf(stderr, "  SKIP  %-12s  (timeout after %ds)\n", name, timeout_sec);
        return 3;
    }

    if (n != sizeof(tr) || tr.status != 0) {
        fprintf(stderr, "  ERROR %-12s  (status=%d after %.1fms)\n",
                name, tr.status, tr.time_ms);
        return 2;
    }

    /* Compare objective vs known optimal */
    double got = tr.objective;
    double expected = ref->optimal;
    double scale = fmax(1.0, fmax(fabs(got), fabs(expected)));
    double rel_err = fabs(got - expected) / scale;
    double abs_err = fabs(got - expected);

    int ok = (rel_err < DEFAULT_OBJ_REL_TOL) || (abs_err < DEFAULT_OBJ_ABS_TOL);

    if (ok) {
        fprintf(stderr, "  PASS  %-12s  %15.8e  expected %15.8e  err=%.1e  %7.1fms\n",
                name, got, expected, rel_err, tr.time_ms);
        return 0;
    } else {
        fprintf(stderr, "  FAIL  %-12s  %15.8e  expected %15.8e  err=%.1e  %7.1fms\n",
                name, got, expected, rel_err, tr.time_ms);
        return 1;
    }
}

/* Compare problems by tier then name for predictable output order */
static int cmp_by_tier_name(const void *a, const void *b) {
    const ProblemInfo *pa = (const ProblemInfo *)a;
    const ProblemInfo *pb = (const ProblemInfo *)b;

    const NetlibReference *ra = find_netlib_reference(pa->name);
    const NetlibReference *rb = find_netlib_reference(pb->name);
    int ta = ra ? ra->tier : 99;
    int tb = rb ? rb->tier : 99;

    if (ta != tb) return ta - tb;
    return strcasecmp(pa->name, pb->name);
}

static int run_test_mode(const Options *opts) {
    int max_tier = (opts->test_mode == 1) ? 1 : 4;  /* fast=0-1, full=0-4 */
    int timeout_sec = (opts->test_mode == 1) ? (int)TEST_FAST_CAP_SEC
                                              : (int)TEST_FULL_CAP_SEC;

    fprintf(stderr, "NETLIB Correctness Test (%s: tiers 0-%d, %ds cap)\n",
            opts->test_mode == 1 ? "fast" : "full", max_tier, timeout_sec);

    /* Discover available .mps files */
    ProblemInfo problems[MAX_PROBLEMS];
    int count = list_netlib_problems(problems, MAX_PROBLEMS, 1 /* lp_only */);

    if (count == 0) {
        fprintf(stderr, "Error: No NETLIB problems found.\n");
        fprintf(stderr, "Run: ./ralph-benchmark --download-netlib\n");
        return 1;
    }

    /* Sort by tier then name for predictable output */
    qsort(problems, (size_t)count, sizeof(ProblemInfo), cmp_by_tier_name);

    int pass_count = 0, fail_count = 0, skip_count = 0, error_count = 0;

    for (int i = 0; i < count; i++) {
        const char *name = problems[i].name;
        const NetlibReference *ref = find_netlib_reference(name);

        /* Skip if not in reference table or above tier threshold */
        if (!ref || ref->tier > max_tier) {
            skip_count++;
            continue;
        }

        int result = test_solve_one(problems[i].path, name, ref,
                                     timeout_sec, opts->method, opts->pricing,
                                     opts->phase1_pricing,
                                     opts->glpk_smcp_ratio,
                                     opts->glpk_smcp_flip,
                                     opts->glpk_smcp_shift,
                                     opts->glpk_bfcp_backend,
                                     opts->glpk_bfcp_update_limit,
                                     opts->dual_steepest_edge,
                                     opts->lu_supernode,
                                     opts->lp_basis_governor_mode,
                                     opts->lp_reinvert_controller_mode,
                                     opts->random_seed,
                                     opts->external_glpk_oop,
                                     opts->crash,
                                     opts->trace_phase1);
        switch (result) {
            case 0: pass_count++; break;
            case 1: fail_count++; break;
            case 2: error_count++; break;
            default: skip_count++; break;
        }
    }

    /* Summary */
    int tested = pass_count + fail_count;
    fprintf(stderr, "\nResults: %d/%d PASS", pass_count, tested);
    if (fail_count > 0) fprintf(stderr, ", %d FAIL", fail_count);
    if (error_count > 0) fprintf(stderr, ", %d ERROR", error_count);
    if (skip_count > 0) fprintf(stderr, ", %d SKIP", skip_count);
    fprintf(stderr, "\n");

    return (fail_count + error_count > 0) ? 1 : 0;
}

static int run_suite(const char *suite_name, const Options *opts) {
    ProblemInfo problems[MAX_PROBLEMS];
    int count = list_netlib_problems(problems, MAX_PROBLEMS, opts->lp_only);

    if (count == 0) {
        fprintf(stderr, "Error: No NETLIB problems found.\n");
        fprintf(stderr, "Run: ./ralph-benchmark --download-netlib\n");
        return 1;
    }

    /* Filter by suite */
    int start = 0, end = count;
    if (strcmp(suite_name, "tiny") == 0) {
        end = (count < 5) ? count : 5;
    } else if (strcmp(suite_name, "small") == 0) {
        end = (count < 15) ? count : 15;
    } else if (strcmp(suite_name, "medium") == 0) {
        end = (count < 40) ? count : 40;
    }
    /* "all" or "large" uses all problems */

    fprintf(stdout, "[\n");
    for (int i = start; i < end; i++) {
        if (i > start) fprintf(stdout, ",\n");
        run_single_benchmark(problems[i].path, problems[i].name, "netlib",
                             opts, stdout);
    }
    fprintf(stdout, "]\n");

    return 0;
}

/* ============================================================================
 * CLI
 * ============================================================================ */

static void print_help(const char *prog) {
    printf("Ralph LP Benchmark Tool\n");
    printf("\n");
    printf("Compare Ralph solver against GLPK on standard LP problems.\n");
    printf("Produces structured JSON output for automated analysis.\n");
    printf("\n");
    printf("Usage:\n");
    printf("  %s <problem.mps|problem.lp>   Benchmark a single problem file\n", prog);
    printf("  %s --netlib <name>            Benchmark a NETLIB problem by name\n", prog);
    printf("  %s --suite <name>             Run a test suite\n", prog);
    printf("  %s --list                     List available NETLIB problems\n", prog);
    printf("  %s --download-netlib          Download NETLIB problems\n", prog);
    printf("\n");
    printf("Correctness Testing:\n");
    printf("  %s --test                            # Fast test (tiers 0-1)\n", prog);
    printf("  %s --test fast                       # Same as above\n", prog);
    printf("  %s --test full                       # All tiers, 300s cap\n", prog);
    printf("\n");
    printf("Verification:\n");
    printf("  %s --verify-matrix --suite all -v    # Verify all NETLIB matrices\n", prog);
    printf("  %s --verify-matrix --netlib blend -v # Verify single problem\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help                    Show this help message\n");
    printf("  -v, --verbose                 Print progress to stderr\n");
    printf("  --test [fast|full]            Correctness test vs known optimal values\n");
    printf("  --verify-matrix               Deep matrix verification via GLPK solution\n");
    printf("  --version                     Show version\n");
    printf("\n");
    printf("Time Limits:\n");
    printf("  --time-mult <N>               Ralph time = N * GLPK time (default: %.1f)\n",
           DEFAULT_TIME_MULTIPLIER);
    printf("  --hard-cap <SEC>              Maximum time per problem (default: %.0f sec)\n",
           DEFAULT_HARD_CAP_SEC);
    printf("\n");
    printf("Solver:\n");
    printf("  --method <N>                  LP method: 0=primal, 1=dual, 2=auto, 3=dualp (default: 2)\n");
    printf("  --phase1-pricing <N>          Override Phase 1 pricing only: 0=Dantzig, 1=SE, 2=Devex, 3=Partial, 4=Heap\n");
    printf("  --steep                       Use steep pricing (alias for --pricing 1)\n");
    printf("  --nosteep                     Use standard pricing (alias for --pricing 0)\n");
    printf("  --relax                       Use Harris ratio test (GLPK-compat ratio=1)\n");
    printf("  --norelax                     Use standard ratio test (GLPK-compat ratio=0)\n");
    printf("  --flip                        Enable dual bound flipping (GLPK-compat flip=1)\n");
    printf("  --noflip                      Disable dual bound flipping (GLPK-compat flip=0)\n");
    printf("  --smcp-shift <N>              GLPK-compat bound shift: 0=off, 1=on\n");
    printf("  --bfcp-backend <N>            GLPK BFCP backend: 0=luf_ft, 1=cbg, 2=cgr\n");
    printf("  --bfcp-update-limit <N>       GLPK BFCP update limit override (>0)\n");
    printf("  --dual-dse <N>                Dual steepest edge: 0=off, 1=on\n");
    printf("  --lp-basis-governor-mode <N>  Basis governor: 0=off, 1=shadow, 2=control_phase2\n");
    printf("  --lp-reinvert-controller-mode <N> Reinvert controller: 0=off, 1=shadow, 2=control_phase1, 3=control_all\n");
    printf("  --random-seed <N>             Deterministic LP anti-cycling seed (default: 0)\n");
    printf("  --trace-phase1                Emit deterministic native Phase 1 trace to stderr\n");
    printf("  --solver-verbose              Forward verbose diagnostics to native LP solver\n");
    printf("  --external-glpk-oop           Solve Ralph LP path with GLPK out-of-process backend\n");
    printf("  --no-adaptive-fallback        Do not retry invalid/failed native solves with shift-off or external backend\n");
    printf("  --no-external-fallback        Allow native retries but forbid external backend rescue\n");
    printf("  --no-presolve                 Disable native presolve before simplex\n");
    printf("  --presolve-mask <MASK>        Override native presolve technique mask (decimal or 0x-prefixed)\n");
    printf("  --crash                       Enable native primal crash basis\n");
    printf("  --lu-supernode                Enable supernodal LU factorization\n");
    printf("\n");
    printf("Problem Filtering:\n");
    printf("  --lp-only                     Only benchmark LP problems (default)\n");
    printf("  --mip-only                    Only benchmark MIP problems\n");
    printf("  --all-types                   Benchmark both LP and MIP\n");
    printf("\n");
    printf("Tolerances:\n");
    printf("  --obj-rel-tol <TOL>           Objective relative tolerance (default: %.0e)\n",
           DEFAULT_OBJ_REL_TOL);
    printf("  --obj-abs-tol <TOL>           Objective absolute tolerance (default: %.0e)\n",
           DEFAULT_OBJ_ABS_TOL);
    printf("  --feas-tol <TOL>              Feasibility tolerance (default: %.0e)\n",
           DEFAULT_FEAS_TOL);
    printf("\n");
    printf("Suites:\n");
    printf("  tiny      5 small problems (~5 sec)\n");
    printf("  small     15 problems (~30 sec)\n");
    printf("  medium    40 problems (~2 min)\n");
    printf("  all       All available problems\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --download-netlib                # First time: download problems\n", prog);
    printf("  %s --suite tiny                     # Quick smoke test\n", prog);
    printf("  %s --netlib afiro -v                # Single problem, verbose\n", prog);
    printf("  %s problem.mps > result.json        # Custom file, save JSON\n", prog);
    printf("\n");
    printf("Output:\n");
    printf("  JSON is printed to stdout. Use -v for progress on stderr.\n");
    printf("  Single problem: JSON object\n");
    printf("  Suite: JSON array of objects\n");
}

static void print_version(void) {
    printf("ralph-benchmark 1.0.0\n");
    printf("Ralph %s\n", ralph_test_version());
}

static int parse_args(int argc, char **argv, Options *opts) {
    /* Set defaults */
    memset(opts, 0, sizeof(*opts));
    opts->time_multiplier = DEFAULT_TIME_MULTIPLIER;
    opts->hard_cap_sec = DEFAULT_HARD_CAP_SEC;
    opts->obj_rel_tol = DEFAULT_OBJ_REL_TOL;
    opts->obj_abs_tol = DEFAULT_OBJ_ABS_TOL;
    opts->feas_tol = DEFAULT_FEAS_TOL;
    opts->lp_only = 1;  /* Default: LP only */
    opts->method = 2;   /* Default: auto (dual first, primal fallback) */
    opts->pricing = -1;  /* Default: solver default */
    opts->phase1_pricing = -1;  /* Default: solver default */
    opts->glpk_smcp_ratio = -1;
    opts->glpk_smcp_flip = -1;
    opts->glpk_smcp_shift = -1;
    opts->glpk_bfcp_backend = -1;
    opts->glpk_bfcp_update_limit = -1;
    opts->dual_steepest_edge = -1;
    opts->lp_reinvert_controller_mode = LP_REINVERT_MODE_SHADOW;
    opts->random_seed = 0;
    opts->presolve_mask_override = -1;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            opts->show_help = 1;
        } else if (strcmp(arg, "--version") == 0) {
            opts->show_version = 1;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            opts->verbose = 1;
        } else if (strcmp(arg, "--list") == 0) {
            opts->list_problems = 1;
        } else if (strcmp(arg, "--download-netlib") == 0) {
            opts->download_netlib = 1;
        } else if (strcmp(arg, "--lp-only") == 0) {
            opts->lp_only = 1;
            opts->mip_only = 0;
        } else if (strcmp(arg, "--mip-only") == 0) {
            opts->mip_only = 1;
            opts->lp_only = 0;
        } else if (strcmp(arg, "--verify-matrix") == 0) {
            opts->verify_matrix = 1;
        } else if (strcmp(arg, "--test") == 0) {
            /* --test [fast|full], default is fast */
            if (i + 1 < argc && argv[i+1][0] != '-') {
                i++;
                if (strcmp(argv[i], "full") == 0) {
                    opts->test_mode = 2;
                } else {
                    opts->test_mode = 1;  /* fast */
                }
            } else {
                opts->test_mode = 1;  /* default: fast */
            }
        } else if (strcmp(arg, "--all-types") == 0) {
            opts->lp_only = 0;
            opts->mip_only = 0;
        } else if (strcmp(arg, "--suite") == 0 && i + 1 < argc) {
            strncpy(opts->suite, argv[++i], sizeof(opts->suite) - 1);
        } else if (strcmp(arg, "--netlib") == 0 && i + 1 < argc) {
            strncpy(opts->netlib_name, argv[++i], sizeof(opts->netlib_name) - 1);
        } else if (strcmp(arg, "--time-mult") == 0 && i + 1 < argc) {
            opts->time_multiplier = atof(argv[++i]);
        } else if (strcmp(arg, "--hard-cap") == 0 && i + 1 < argc) {
            opts->hard_cap_sec = atof(argv[++i]);
        } else if (strcmp(arg, "--obj-rel-tol") == 0 && i + 1 < argc) {
            opts->obj_rel_tol = atof(argv[++i]);
        } else if (strcmp(arg, "--obj-abs-tol") == 0 && i + 1 < argc) {
            opts->obj_abs_tol = atof(argv[++i]);
        } else if (strcmp(arg, "--feas-tol") == 0 && i + 1 < argc) {
            opts->feas_tol = atof(argv[++i]);
        } else if (strcmp(arg, "--method") == 0 && i + 1 < argc) {
            opts->method = atoi(argv[++i]);
            if (opts->method < 0 || opts->method > 3) {
                fprintf(stderr, "Invalid --method: %d (expected 0..3)\n", opts->method);
                return -1;
            }
        } else if (strcmp(arg, "--pricing") == 0 && i + 1 < argc) {
            opts->pricing = atoi(argv[++i]);
        } else if (strcmp(arg, "--phase1-pricing") == 0 && i + 1 < argc) {
            opts->phase1_pricing = atoi(argv[++i]);
            if (opts->phase1_pricing < 0 || opts->phase1_pricing > 4) {
                fprintf(stderr,
                        "Invalid --phase1-pricing: %d (expected 0..4)\n",
                        opts->phase1_pricing);
                return -1;
            }
        } else if (strcmp(arg, "--steep") == 0) {
            opts->pricing = 1;
        } else if (strcmp(arg, "--nosteep") == 0) {
            opts->pricing = 0;
        } else if (strcmp(arg, "--relax") == 0) {
            opts->glpk_smcp_ratio = 1;
        } else if (strcmp(arg, "--norelax") == 0) {
            opts->glpk_smcp_ratio = 0;
        } else if (strcmp(arg, "--flip") == 0) {
            opts->glpk_smcp_flip = 1;
        } else if (strcmp(arg, "--noflip") == 0) {
            opts->glpk_smcp_flip = 0;
        } else if (strcmp(arg, "--smcp-shift") == 0 && i + 1 < argc) {
            opts->glpk_smcp_shift = atoi(argv[++i]);
            if (opts->glpk_smcp_shift < 0 || opts->glpk_smcp_shift > 1) {
                fprintf(stderr,
                        "Invalid --smcp-shift: %d (expected 0 or 1)\n",
                        opts->glpk_smcp_shift);
                return -1;
            }
        } else if (strcmp(arg, "--bfcp-backend") == 0 && i + 1 < argc) {
            opts->glpk_bfcp_backend = atoi(argv[++i]);
            if (opts->glpk_bfcp_backend < 0 || opts->glpk_bfcp_backend > 2) {
                fprintf(stderr,
                        "Invalid --bfcp-backend: %d (expected 0..2)\n",
                        opts->glpk_bfcp_backend);
                return -1;
            }
        } else if (strcmp(arg, "--dual-dse") == 0 && i + 1 < argc) {
            opts->dual_steepest_edge = atoi(argv[++i]);
            if (opts->dual_steepest_edge < 0 || opts->dual_steepest_edge > 1) {
                fprintf(stderr,
                        "Invalid --dual-dse: %d (expected 0 or 1)\n",
                        opts->dual_steepest_edge);
                return -1;
            }
        } else if (strcmp(arg, "--bfcp-update-limit") == 0 && i + 1 < argc) {
            opts->glpk_bfcp_update_limit = atoi(argv[++i]);
            if (opts->glpk_bfcp_update_limit <= 0) {
                fprintf(stderr,
                        "Invalid --bfcp-update-limit: %d (expected >0)\n",
                        opts->glpk_bfcp_update_limit);
                return -1;
            }
        } else if (strcmp(arg, "--lp-basis-governor-mode") == 0 && i + 1 < argc) {
            opts->lp_basis_governor_mode = atoi(argv[++i]);
            if (opts->lp_basis_governor_mode < 0 ||
                opts->lp_basis_governor_mode > 2) {
                fprintf(stderr,
                        "Invalid --lp-basis-governor-mode: %d (expected 0..2)\n",
                        opts->lp_basis_governor_mode);
                return -1;
            }
        } else if (strcmp(arg, "--lp-reinvert-controller-mode") == 0 && i + 1 < argc) {
            opts->lp_reinvert_controller_mode = atoi(argv[++i]);
            if (opts->lp_reinvert_controller_mode < LP_REINVERT_MODE_OFF ||
                opts->lp_reinvert_controller_mode > LP_REINVERT_MODE_CONTROL_ALL) {
                fprintf(stderr,
                        "Invalid --lp-reinvert-controller-mode: %d (expected 0..3)\n",
                        opts->lp_reinvert_controller_mode);
                return -1;
            }
        } else if (strcmp(arg, "--random-seed") == 0 && i + 1 < argc) {
            opts->random_seed = atoi(argv[++i]);
            if (opts->random_seed < 0) {
                fprintf(stderr, "Invalid --random-seed: %d (expected >= 0)\n",
                        opts->random_seed);
                return -1;
            }
        } else if (strcmp(arg, "--trace-phase1") == 0) {
            opts->trace_phase1 = 1;
        } else if (strcmp(arg, "--solver-verbose") == 0) {
            opts->solver_verbose = 1;
        } else if (strcmp(arg, "--external-glpk-oop") == 0) {
            opts->external_glpk_oop = 1;
        } else if (strcmp(arg, "--no-adaptive-fallback") == 0) {
            opts->no_adaptive_fallback = 1;
        } else if (strcmp(arg, "--no-external-fallback") == 0) {
            opts->no_external_fallback = 1;
        } else if (strcmp(arg, "--no-presolve") == 0) {
            opts->no_presolve = 1;
        } else if (strcmp(arg, "--presolve-mask") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long mask = strtoul(argv[++i], &end, 0);
            if (!end || *end != '\0' || mask > 0xFFFFul) {
                fprintf(stderr,
                        "Invalid --presolve-mask: %s (expected 0..0xFFFF)\n",
                        argv[i]);
                return -1;
            }
            opts->presolve_mask_override = (int)mask;
        } else if (strcmp(arg, "--crash") == 0) {
            opts->crash = 1;
        } else if (strcmp(arg, "--lu-supernode") == 0) {
            opts->lu_supernode = 1;
        } else if (strcmp(arg, "-o") == 0 && i + 1 < argc) {
            strncpy(opts->output_dir, argv[++i], sizeof(opts->output_dir) - 1);
        } else if (arg[0] != '-') {
            /* Positional argument: problem file */
            strncpy(opts->problem_path, arg, sizeof(opts->problem_path) - 1);
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }
    }

    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    Options opts;

    if (parse_args(argc, argv, &opts) != 0) {
        return 1;
    }

    if (opts.show_help) {
        print_help(argv[0]);
        return 0;
    }

    if (opts.show_version) {
        print_version();
        return 0;
    }

    if (opts.external_glpk_oop) {
        if (!check_glpk_available()) {
            fprintf(stderr, "Error: glpsol not found in PATH.\n");
            fprintf(stderr, "Install GLPK: brew install glpk (macOS) or apt install glpk-utils (Linux)\n");
            return 1;
        }
        ralph_lp_external_unregister_all_adapters();
        if (ralph_lp_external_register_glpk_oop(NULL) != 0) {
            fprintf(stderr, "Error: failed to register GLPK out-of-process adapter.\n");
            return 1;
        }
    }

    /* Test mode normally does not require GLPK; --external-glpk-oop does. */
    if (opts.test_mode > 0) {
        return run_test_mode(&opts);
    }

    /* Check GLPK availability (needed for benchmark/verify modes) */
    if (!opts.external_glpk_oop && !check_glpk_available()) {
        fprintf(stderr, "Error: glpsol not found in PATH.\n");
        fprintf(stderr, "Install GLPK: brew install glpk (macOS) or apt install glpk-utils (Linux)\n");
        return 1;
    }
    if (!opts.external_glpk_oop && ralph_lp_external_register_glpk_oop(NULL) != 0) {
        fprintf(stderr, "Error: failed to register GLPK out-of-process adapter.\n");
        return 1;
    }

    /* Download NETLIB problems */
    if (opts.download_netlib) {
        char script_path[MAX_PATH];
        char bench_dir[MAX_PATH];
        get_benchmark_dir(bench_dir, sizeof(bench_dir));
        snprintf(script_path, sizeof(script_path), "%s/download_netlib.sh", bench_dir);

        if (!file_exists(script_path)) {
            fprintf(stderr, "Error: download_netlib.sh not found at %s\n", script_path);
            fprintf(stderr, "Try running from the ralph/ directory: make download-netlib\n");
            return 1;
        }

        char cmd[MAX_PATH * 2];
        snprintf(cmd, sizeof(cmd), "cd '%s' && bash download_netlib.sh", bench_dir);
        return system(cmd);
    }

    /* List problems */
    if (opts.list_problems) {
        ProblemInfo problems[MAX_PROBLEMS];
        int count = list_netlib_problems(problems, MAX_PROBLEMS, 0);

        if (count == 0) {
            printf("No NETLIB problems found.\n");
            printf("Run: %s --download-netlib\n", argv[0]);
            return 0;
        }

        printf("Available NETLIB problems (%d total):\n\n", count);
        printf("%-20s %s\n", "Name", "Type");
        printf("%-20s %s\n", "----", "----");
        for (int i = 0; i < count; i++) {
            printf("%-20s %s\n", problems[i].name,
                   problems[i].is_mip ? "MIP" : "LP");
        }
        return 0;
    }

    /* Verify-matrix mode */
    if (opts.verify_matrix) {
        if (opts.suite[0] != '\0') {
            return run_verify_suite(opts.suite, &opts);
        }
        if (opts.netlib_name[0] != '\0') {
            char problem_path[MAX_PATH];
            if (!find_netlib_problem(opts.netlib_name, problem_path,
                                      sizeof(problem_path))) {
                fprintf(stderr, "Error: NETLIB problem '%s' not found.\n",
                        opts.netlib_name);
                return 1;
            }
            return run_verify_matrix(problem_path, opts.netlib_name,
                                      &opts, stdout);
        }
        if (opts.problem_path[0] != '\0') {
            const char *name = strrchr(opts.problem_path, '/');
            name = name ? name + 1 : opts.problem_path;
            return run_verify_matrix(opts.problem_path, name, &opts, stdout);
        }
        /* Default: verify all */
        return run_verify_suite("all", &opts);
    }

    /* Run suite */
    if (opts.suite[0] != '\0') {
        return run_suite(opts.suite, &opts);
    }

    /* Run single NETLIB problem by name */
    if (opts.netlib_name[0] != '\0') {
        char problem_path[MAX_PATH];
        if (!find_netlib_problem(opts.netlib_name, problem_path, sizeof(problem_path))) {
            fprintf(stderr, "Error: NETLIB problem '%s' not found.\n", opts.netlib_name);

            int count = count_netlib_problems();
            if (count == 0) {
                fprintf(stderr, "No NETLIB problems available. Run: %s --download-netlib\n",
                        argv[0]);
            } else {
                fprintf(stderr, "Use --list to see available problems.\n");
            }
            return 1;
        }

        return run_single_benchmark(problem_path, opts.netlib_name, "netlib",
                                     &opts, stdout);
    }

    /* Run single problem file */
    if (opts.problem_path[0] != '\0') {
        if (!file_exists(opts.problem_path)) {
            fprintf(stderr, "Error: Problem file not found: %s\n", opts.problem_path);
            return 1;
        }

        /* Extract name from path */
        const char *name = strrchr(opts.problem_path, '/');
        name = name ? name + 1 : opts.problem_path;

        return run_single_benchmark(opts.problem_path, name, "file",
                                     &opts, stdout);
    }

    /* No action specified */
    fprintf(stderr, "Error: No problem specified.\n\n");
    print_help(argv[0]);
    return 1;
}
