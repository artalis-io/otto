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
#include "simplex_phase1_engine.h"

/* Recovery profile: controls which crisis mechanisms are active */
typedef enum {
    P1_RECOVERY_PROFILE_AGGRESSIVE = 0,  /* All mechanisms active (legacy default) */
    P1_RECOVERY_PROFILE_STANDARD = 1     /* Skip dead mechanisms */
} P1RecoveryProfile;

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

/* Pivot-fail recovery exclusion thresholds */
#define PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER 2
#define PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_ITERS RALPH_PHASE1_ENTERING_EXCLUDE_ITERS
#define PHASE1_ENTERING_EXCLUSION_POOL_SIZE 16

/* No-pivot ladder step decisions */
enum {
    PHASE1_NO_PIVOT_LADDER_STEP_RETRY = 0,
    PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE = 1,
    PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR = 2
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
    int auto_partial_enabled;
    int auto_partial_abandoned;
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

    /* Entering exclusion.
     *
     * The first two slots are retained for existing telemetry/logging; the
     * pool is the authoritative Phase 1 numerical tabu set. */
    int excluded_entering_a;
    int excluded_entering_ttl_a;
    int excluded_entering_b;
    int excluded_entering_ttl_b;
    int excluded_entering_pool[PHASE1_ENTERING_EXCLUSION_POOL_SIZE];
    int excluded_entering_pool_ttl[PHASE1_ENTERING_EXCLUSION_POOL_SIZE];

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
    /* Feasibility-engine progress window */
    P1ProgressWindow feasibility_window;

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
    P1RecoveryProfile  profile;
} P1RecoveryState;

/* Initialize all recovery state to defaults.
 * tab is needed for size-dependent thresholds; solver for pricing config. */
void p1_recovery_init(P1RecoveryState *rs,
                      const SimplexSolver *solver,
                      const SimplexTableau *tab);

/* ── Progress operations ─────────────────────────────────────────── */

/* Reset the 4 progress-window fields (no_progress_streak, prev/anchor art_sum,
 * window_steps).  Does NOT reset no_pivot_streak or other progress state. */
void p1_progress_reset(P1ProgressState *ps);

/* Update progress tracking based on current artificial variable sum. */
void p1_progress_update(SimplexSolver *solver,
                        const SimplexTableau *tab,
                        P1ProgressState *ps);

/* Note a no-pivot event, increment streak, and decide whether to force.
 * Returns 1 if a forced refactor should be triggered. */
int p1_progress_note_no_pivot(SimplexSolver *solver,
                              int m,
                              int degenerate_count,
                              LPPhase1NoPivotForceReason reason,
                              P1ProgressState *ps);

/* Record a window-pressure event (failed stabilize or dir skip). */
void p1_window_pressure_note(SimplexSolver *solver,
                             int event_kind,
                             int local_memory_fail,
                             P1ProgressState *ps);

/* Reset window-pressure counters after a successful pivot. */
void p1_window_pressure_reset(SimplexSolver *solver,
                              P1ProgressState *ps);

/* ── Cross-category helpers ──────────────────────────────────────── */

/* Attempt dual rescue via the no-pivot ladder.  Returns 1 on success,
 * 0 on failure (retry), -1 on time limit (caller must return). */
int p1_progress_attempt_ladder_rescue(
    SimplexSolver *solver,
    SimplexTableau *tab,
    int iter,
    LPPhase1NoPivotForceReason reason,
    LPPhase1RecomputeReason recomp_reason,
    P1ProgressState *ps,
    int *rc_only_streak_io);

/* Activate force-pivot mode if the streak/budget thresholds are met.
 * Returns 1 if activated.  When force_pending_out / force_reason_out
 * are non-NULL and activation occurs, outputs are written. */
int p1_progress_activate_force_pivot(
    SimplexSolver *solver,
    int m,
    int degenerate_count,
    int queue_force_pending,
    int *streak_io,
    P1ProgressState *ps,
    int *force_pending_out,
    LPPhase1NoPivotForceReason *force_reason_out);

/* ── Numerical followup consumption ────────────────────────────────── */

/* Event types for followup consumption dispatch */
typedef enum {
    P1_FOLLOWUP_EVENT_FAILED_STABILIZE = 0,
    P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN,
    P1_FOLLOWUP_EVENT_PIVOT_FAIL,
    P1_FOLLOWUP_EVENT_PIVOT_SUCCESS
} P1FollowupEvent;

/* Consume pending flags for all 3 numerical followup subsystems
 * (shadow guard, force extreme, force extreme tiny theta relax).
 * Clears each pending flag and fires the corresponding telemetry. */
void p1_numerical_consume_followup(SimplexSolver *solver,
                                   P1FollowupEvent event,
                                   P1NumericalState *ns);

/* ── Streak / entering trackers ───────────────────────────────────── */

void p1_numerical_note_dir_skip_entering(SimplexSolver *solver,
                                         int entering,
                                         P1NumericalState *ns);

void p1_basis_note_failed_stabilize_entering(SimplexSolver *solver,
                                             int entering,
                                             P1BasisRepairState *bs);

void p1_basis_note_failed_stabilize_retry_alt(SimplexSolver *solver,
                                              int entering,
                                              P1BasisRepairState *bs);

/* ── Direction recording ──────────────────────────────────────────── */

void p1_numerical_record_shadow_direction(SimplexSolver *solver,
                                          const SimplexTableau *tab,
                                          int leaving, double theta);

void p1_numerical_record_extreme_direction(SimplexSolver *solver,
                                           const SimplexTableau *tab,
                                           int leaving, double theta);

/* ── Basis repair: entering exclusion ─────────────────────────────── */

/* Exclude an entering variable from pricing for ttl iterations. */
void p1_basis_exclude_entering(SimplexSolver *solver,
                               int var, int ttl,
                               P1BasisRepairState *bs);
int p1_basis_is_entering_excluded(const P1BasisRepairState *bs, int var);
void p1_basis_tick_entering_exclusions(P1BasisRepairState *bs);
void p1_basis_clear_entering_exclusions(P1BasisRepairState *bs);

/* Conditionally exclude entering after repeated pivot failures. */
void p1_basis_pivot_fail_maybe_exclude(SimplexSolver *solver,
                                       int fail_repeat_count,
                                       int entering,
                                       P1BasisRepairState *bs);

/* ── Direct dual rescue ───────────────────────────────────────────── */

/* Attempt direct dual rescue (without ladder logic).  Returns 1 on success,
 * 0 on failure, -1 on time limit (caller must return). */
int p1_progress_attempt_direct_rescue(SimplexSolver *solver,
                                      SimplexTableau *tab,
                                      int iter,
                                      P1ProgressState *ps);

#endif /* SIMPLEX_PHASE1_RECOVERY_H */
