/*
 * simplex_internal.h - Internal declarations shared between simplex sub-modules.
 *
 * These functions are defined in simplex.c but needed by extracted sub-modules
 * (simplex_pricing.c, simplex_ratio.c, simplex_perturb.c).
 *
 * Functions already declared in lp.h (tableau_compute_solution,
 * tableau_compute_reduced_costs) are not repeated here.
 */

#ifndef SIMPLEX_INTERNAL_H
#define SIMPLEX_INTERNAL_H

#include "lp.h"
#include "lp_refactor_policy.h"
#include "lp_reinvert_controller.h"
#include "simplex_phase1_stabilize.h"
#include "simplex_refactor_schedule.h"

/* Variable eligibility check (accounts for GLPK-compat exclusion rules) */
int simplex_smcp_excl_skip_var(const SimplexTableau *tab, int j);

/* Dual computation */
int tableau_compute_duals(SimplexTableau *tab);

/* Lazy single reduced cost computation */
double tableau_get_rc(SimplexTableau *tab, int j);

/* Phase 1 helpers needed by simplex_phase1_recovery.c */
double phase1_artificial_abs_sum(const SimplexTableau *tab);
void phase1_trace_emit_summary(SimplexSolver *solver, RalphStatus phase1_status);

/* Direct dual rescue guard (needed by p1_progress_attempt_direct_rescue) */
int phase1_direct_dual_rescue_guard_plan(SimplexSolver *solver,
                                         int rescue_cooldown_iters,
                                         int rescue_fail_streak);

/* LPReinvertShadowEval now defined in simplex_refactor_schedule.h */

/* ── Constants shared between simplex.c and simplex_phase1_zones.c ── */

/* Auto-Dantzig pricing switch thresholds */
#define PHASE1_AUTO_DANTZIG_MIN_M 700
#define PHASE1_AUTO_DANTZIG_MAX_M 1200
#define PHASE1_AUTO_DANTZIG_DEGEN_TRIGGER 20

/* Direction refactor telemetry trigger codes */
#define PHASE1_DIR_REFACTOR_TELEM_NO_PIVOT_FORCE 1
#define PHASE1_DIR_REFACTOR_TELEM_FORCE_EXTREME_DIR 2
#define PHASE1_DIR_REFACTOR_TELEM_FORCE_LU_HEALTH 3
#define PHASE1_DIR_REFACTOR_TELEM_FORCE_PIVOT_MODE 4
#define PHASE1_DIR_REFACTOR_TELEM_LADDER_FORCE 5

/* Dir-escape telemetry codes */
#define PHASE1_DIR_ESCAPE_TELEM_TRIGGER 1
#define PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_LU_HEALTH 2
#define PHASE1_DIR_ESCAPE_TELEM_HARD_BYPASS 3
#define PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_FORCE_PIVOT_MODE 4

/* Stagnation escape (currently compile-time disabled) */
#define PHASE1_STAGNATION_ESCAPE_RUNTIME 0
#define PHASE1_STAGNATION_MIN_M 700
#define PHASE1_STAGNATION_ESCAPE_COOLDOWN_ITERS 128
#define PHASE1_STAGNATION_ESCAPE_FAIL_COOLDOWN_ITERS 32

/* No-entering cleanup */
#define PHASE1_NO_ENTERING_CLEANUP_MAX_ITERS 128

/* RC-only streak guard */
#define PHASE1_RC_ONLY_STREAK_GUARD 6

/* No-pivot ladder rescue */
#define PHASE1_NO_PIVOT_LADDER_RESCUE_COOLDOWN_ITERS 16
#define PHASE1_NO_PIVOT_LADDER_RESCUE_FAIL_CAP 3

/* Failed-stabilize retry thresholds */
#define PHASE1_FAILED_STABILIZE_RETRY_PENALTY_TRIGGER 3
#define PHASE1_FAILED_STABILIZE_RETRY_LOCAL_MEMORY_TRIGGER 2
#define PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_RATIO_FAIL_TRIGGER 2
#define PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_TRIGGER 4
#define PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_MIN_ELIGIBLE 16
#define PHASE1_FAILED_STABILIZE_RETRY_GUARDED_SELECTOR_SCORE_RATIO 2.0
#define PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_STREAK_TRIGGER 2
#define PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_MIN_NNZ 64
#define PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_MIN_DIR_INF_RATIO 10.0
#define PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_MAX_PIVOT_DIR_RATIO 1e-8
#define PHASE1_FAILED_STABILIZE_RETRY_DIR_GUARD_EXCLUDE_ITERS 4
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_STREAK_TRIGGER 2
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_NNZ 64
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_DIR_INF_RATIO 10.0
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_SHADOW_DIR_MULT 100.0
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_SHADOW_DIR_RATIO 1e3
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_ACTUAL_PIVOT_DIR_RATIO 1e-8
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MAX_ACTUAL_PIVOT_DIR_RATIO 1e-4
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MAX_SHADOW_PIVOT_DIR_RATIO 1e-6
#define PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_EXCLUDE_ITERS 4

/* Force-extreme tiny theta relax refactor telemetry */
#define PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_REFACTOR_LU_HEALTH 1
#define PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_REFACTOR_FORCE_PIVOT 2
#define PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_REFACTOR_LADDER 3

/* ── Functions shared between simplex.c and zone handlers ─────────── */

/* Refactorize with telemetry reason code */
static inline int tableau_refactorize_with_reason(SimplexTableau *tab, int reason) {
    lp_telemetry_set_refactor_next_reason(tab ? tab->owner : NULL, reason);
    return tableau_refactorize(tab);
}

/* Phase 1 trace recording */
void phase1_trace_record_no_entering(SimplexSolver *solver, int iter, int status_code);
void phase1_trace_record_pivot_failure(SimplexSolver *solver,
                                       const SimplexTableau *tab,
                                       int iter, int repeat_count);

/* Stagnation escape (conditional on PHASE1_STAGNATION_ESCAPE_RUNTIME) */
void phase1_stagnation_window_begin(SimplexSolver *solver,
                                    const SimplexTableau *tab,
                                    int iter);
int phase1_stagnation_escape_should_trigger(SimplexSolver *solver,
                                            const SimplexTableau *tab,
                                            int iter);

/* Vector utility */
double vec_abs_max(const double *x, int n);

/* Farkas ray extraction */
void extract_farkas_ray(SimplexSolver *solver);

/* Basis repair */
int repair_singular_basis(SimplexTableau *tab);

/* Core pivot operation */
int simplex_pivot(SimplexTableau *tab, int entering, int leaving_pos,
                  double theta, int repeat_pattern_count);

/* Heap rebuild for pricing strategy 4 */
void heap_build(SimplexTableau *tab);

/* No-pivot ladder decision */
int phase1_no_pivot_ladder_step(SimplexSolver *solver, int m,
                                int degenerate_count,
                                LPPhase1NoPivotForceReason reason,
                                int no_pivot_streak,
                                int no_progress_streak,
                                int force_pivot_mode_active,
                                int *refactor_threshold_out);
int phase1_no_pivot_ladder_apply_rescue_guard(SimplexSolver *solver,
                                              int ladder_step,
                                              int rescue_cooldown_iters,
                                              int rescue_fail_streak);

/* Direction stabilize escape gate */
int phase1_dir_stabilize_escape_gate_plan(int m, int degenerate_count,
    int dir_skip_event_streak, int no_progress_streak, int escape_cooldown,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int *next_escape_cooldown_out, int *triggered_out, int *hard_bypass_out);

/* Force relax plans */
int phase1_force_pivot_refactor_relax_plan(int m, int degenerate_count,
    int no_progress_streak, int force_pivot_mode_active,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int dual_rescue_attempts, int dual_rescue_successes,
    int dual_rescue_fail_streak);
int phase1_force_extreme_refactor_relax_plan(int m, int degenerate_count,
    int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int dual_rescue_attempts, int dual_rescue_successes,
    int dual_rescue_fail_streak);
int phase1_force_extreme_bound_flip_relax_plan(int m, int degenerate_count,
    int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int bound_flip_followup_streak);
int phase1_force_extreme_tiny_theta_relax_plan(int m, int degenerate_count,
    int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int tiny_theta_followup_streak);
int phase1_force_extreme_catastrophic_tiny_theta_relax_plan(int m,
    int degenerate_count, int no_progress_streak, double dir_inf_ratio,
    int force_extreme_dir, int force_lu_health, int lu_hard_trigger,
    int tiny_theta_followup_streak, double pivot_ratio);

/* Soft LU / periodic policy / reinvert helpers now in simplex_refactor_schedule.h */
int phase1_soft_lu_policy_cooldown_updates(int m, int degenerate_count,
                                           int periodic_interval);

#endif /* SIMPLEX_INTERNAL_H */
