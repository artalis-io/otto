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
#include "simplex_phase1_decision.h"
#include "simplex_phase1_trace.h"
#include "simplex_scaling.h"
#include "simplex_crash.h"
#include "simplex_refactor_schedule.h"

/* Variable eligibility check (accounts for GLPK-compat exclusion rules) */
int simplex_smcp_excl_skip_var(const SimplexTableau *tab, int j);

/* Dual computation */
int tableau_compute_duals(SimplexTableau *tab);

/* Lazy single reduced cost computation */
double tableau_get_rc(SimplexTableau *tab, int j);

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

/* Phase 2 optimality confirmation (refactorize + Dantzig recheck) */
int phase2_confirm_optimality(SimplexSolver *solver, int iter, int *entering_out);

/* Unbounded ray extraction */
void extract_unbounded_ray(SimplexSolver *solver, int entering, double dir);

/* No-pivot ladder, force relax plans, escape gate, soft LU cooldown
 * now in simplex_phase1_decision.h */

/* ── Phase 2 degen-escape constants (shared with simplex_phase2_zones.c) ── */

#define PHASE2_DEGEN_ESCAPE_MIN_M 1200
#define PHASE2_DEGEN_ESCAPE_DEGEN_TRIGGER 120
#define PHASE2_DEGEN_ESCAPE_POLICY_TRIGGER 200
#define PHASE2_DEGEN_ESCAPE_MAX_ATTEMPTS 2
#define PHASE2_DEGEN_ESCAPE_BLAND_HOLD_ITERS 16

#endif /* SIMPLEX_INTERNAL_H */
