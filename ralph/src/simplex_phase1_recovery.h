/*
 * simplex_phase1_recovery.h - Phase 1 crisis/recovery state for the simplex method.
 *
 * Extracted from simplex.c - groups ~79 Phase 1 local variables into 4 orthogonal
 * failure categories plus shared state. This makes the crisis state machine visible
 * and testable without changing any behavior.
 *
 * Categories:
 *   Cycling    - Bland's rule, perturbation, pricing switch
 *   Numerical  - Direction guard, stabilization, shadow/extreme followup
 *   BasisRepair - Entering exclusion, failed stabilize retry, pivot failure
 *   Progress   - No-pivot ladder, window pressure, force pivot
 */

#ifndef SIMPLEX_PHASE1_RECOVERY_H
#define SIMPLEX_PHASE1_RECOVERY_H

#include "lp.h"

/* Phase-1 pivot-failure reasons (shared between simplex.c and recovery module) */
enum {
    PHASE1_PIVOT_FAIL_NONE = 0,
    PHASE1_PIVOT_FAIL_SMALL_PIVOT = 1,
    PHASE1_PIVOT_FAIL_INVALID_COLUMN = 2,
    PHASE1_PIVOT_FAIL_LU_MAX_UPDATES = 3,
    PHASE1_PIVOT_FAIL_LU_SPIKE_POOL_FULL = 4,
    PHASE1_PIVOT_FAIL_LU_UPDATE_PIVOT_SMALL = 5,
    PHASE1_PIVOT_FAIL_LU_SINGULAR_UPDATE = 6,
    PHASE1_PIVOT_FAIL_FACTOR_SINGULAR = 7,
    PHASE1_PIVOT_FAIL_REFACTOR_FORCED_OTHER = 8,
    PHASE1_PIVOT_FAIL_REFACTOR_AFTER_UPDATE_OTHER = 9
};

/* Window pressure event kinds */
#define PHASE1_WINDOW_PRESSURE_EVENT_NONE 0
#define PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE 1
#define PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP 2

/* ── Cycling: Bland's rule, perturbation, pricing switch ──────────── */
typedef struct {
    int degenerate_count;
    int degen_threshold;          /* const after init */
    int use_bland;
    double last_obj;
    int stall_count;
    int stall_threshold;          /* const after init */
    int perturb_attempts;
    int max_perturb_attempts;     /* const after init */
    int pricing_strategy;
    int auto_dantzig_enabled;
} P1CyclingState;

/* ── Numerical: direction guard, stabilization, shadow/extreme followup ── */
typedef struct {
    /* Direction stabilize */
    int dir_stabilize_cooldown;
    int dir_stabilize_repeat_count;
    int dir_stabilize_moderate_defer_pending;

    /* RC-only / direction skip tracking */
    int rc_only_streak;
    int dir_skip_event_streak;
    int dir_skip_no_recompute_streak;
    int dir_force_refactor_streak;
    int last_dir_skip_entering;
    int dir_skip_same_entering_streak;

    /* Shadow guard followup */
    int shadow_guard_followup_pending;
    int shadow_guard_followup_direction_pending;

    /* Force extreme followup */
    int force_extreme_followup_pending;
    int force_extreme_followup_direction_pending;
    int force_extreme_followup_bound_flip_streak;
    int force_extreme_followup_tiny_theta_streak;
    int force_extreme_tiny_theta_relax_branch_pending;
    int force_extreme_tiny_theta_relax_next_pending;
} P1NumericalState;

/* ── Basis repair: entering exclusion, failed stabilize retry, pivot failure ── */
typedef struct {
    /* Pivot failure tracking */
    int fail_entering;
    int fail_leaving_pos;
    int fail_reason;
    int fail_repeat_count;

    /* Ratio test breakdown */
    int ratio_breakdown_count;
    int ratio_breakdown_last_entering;
    int ratio_breakdown_same_entering_streak;

    /* Entering exclusion (2 slots) */
    int excluded_entering_a;
    int excluded_entering_ttl_a;
    int excluded_entering_b;
    int excluded_entering_ttl_b;

    /* Failed stabilize retry */
    int last_failed_stabilize_entering;
    int failed_stabilize_same_entering_streak;
    int last_failed_stabilize_retry_alt;
    int failed_stabilize_retry_alt_streak;
    int failed_stabilize_retry_alt_ratio_fail_streak;
    int failed_stabilize_retry_pool_sample_counter;
} P1BasisRepairState;

/* ── Progress: no-pivot ladder, window pressure, force pivot ──────── */
typedef struct {
    /* No-pivot streak & progress window */
    int no_pivot_streak;
    int no_pivot_no_progress_streak;
    double no_pivot_prev_art_sum;
    double no_pivot_anchor_art_sum;
    int no_pivot_progress_window_steps;

    /* No-pivot force decision */
    int no_pivot_force_pending;
    LPPhase1NoPivotForceReason no_pivot_force_reason;
    int no_pivot_force_cooldown;

    /* No-pivot ladder rescue */
    int no_pivot_ladder_rescue_cooldown;
    int no_pivot_ladder_rescue_fail_streak;

    /* Window pressure tracking */
    int window_pressure_events;
    int window_pressure_failed_stabilize;
    int window_pressure_dir_skip;
    int window_pressure_local_memory_fail;
    int window_pressure_alternations;
    int window_pressure_last_event_kind;
    int window_pressure_force_pivot_armed;
    int dir_escape_cooldown;
    int force_pivot_attempt_budget;

    /* Post-optimal cleanup */
    int no_entering_cleanup_streak;
} P1ProgressState;

/* ── Shared: LU periodic policy, pressure decay ──────────────────── */
typedef struct {
    int recompute_interval;       /* const after init */
    int periodic_policy_cooldown;
    double periodic_policy_pressure_decay;
    int lu_soft_health_streak;
} P1SharedState;

/* ── Top-level recovery state (replaces ~79 locals) ──────────────── */
typedef struct {
    P1CyclingState     cycling;
    P1NumericalState   numerical;
    P1BasisRepairState basis;
    P1ProgressState    progress;
    P1SharedState      shared;
} P1RecoveryState;

/* Initialize all recovery state to defaults.
 * tab is needed for size-dependent thresholds; solver for pricing config. */
void p1_recovery_init(P1RecoveryState *rs,
                      const SimplexSolver *solver,
                      const SimplexTableau *tab);

#endif /* SIMPLEX_PHASE1_RECOVERY_H */
