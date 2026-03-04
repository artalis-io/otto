/*
 * Ralph - Revised Simplex Method Implementation
 *
 * Implements the primal revised simplex algorithm with:
 * - Steepest edge / Devex pricing
 * - Harris ratio test
 * - Bound flipping
 * - Anti-cycling via perturbation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include "lp.h"
#include "lp_refactor_policy.h"
#include "lp_policy_glpk_compat.h"
#include "lp_log.h"
#include "ralph_lp.h"

/* Forward declarations */
int lp_model_finalize(LPModel *model);
static int lp_run_user_callbacks(SimplexSolver *solver,
                                 const SimplexTableau *tab,
                                 RalphLPProgressPhase phase,
                                 int iter,
                                 int force_emit,
                                 int honor_progress_cancel);
static int lp_time_limit_exceeded(SimplexSolver *solver, int iter);
static void phase1_trace_emit_summary(SimplexSolver *solver, RalphStatus phase1_status);
static void phase1_recompute_full_with_reason(SimplexSolver *solver,
                                              SimplexTableau *tab,
                                              int *rc_only_streak,
                                              LPPhase1RecomputeReason reason);
static void primal_remove_perturbation(SimplexTableau *tab);

/* Phase-1 pivot-failure reasons used by deterministic tracing. */
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

static const char* phase1_pivot_fail_reason_str(int reason) {
    switch (reason) {
        case PHASE1_PIVOT_FAIL_SMALL_PIVOT: return "small_pivot";
        case PHASE1_PIVOT_FAIL_INVALID_COLUMN: return "invalid_entering_column";
        case PHASE1_PIVOT_FAIL_LU_MAX_UPDATES: return "lu_max_updates";
        case PHASE1_PIVOT_FAIL_LU_SPIKE_POOL_FULL: return "lu_spike_pool_full";
        case PHASE1_PIVOT_FAIL_LU_UPDATE_PIVOT_SMALL: return "lu_update_pivot_too_small";
        case PHASE1_PIVOT_FAIL_LU_SINGULAR_UPDATE: return "lu_singular_update";
        case PHASE1_PIVOT_FAIL_FACTOR_SINGULAR: return "factor_singular";
        case PHASE1_PIVOT_FAIL_REFACTOR_FORCED_OTHER: return "refactor_after_forced_pivot_other";
        case PHASE1_PIVOT_FAIL_REFACTOR_AFTER_UPDATE_OTHER: return "refactor_after_update_fail_other";
        default: return "unknown";
    }
}

static int phase1_trace_reason_from_lu_failure(int lu_reason, int forced_refactor_path) {
    switch ((LUFailureReason)lu_reason) {
        case LU_FAIL_MAX_UPDATES:
            return PHASE1_PIVOT_FAIL_LU_MAX_UPDATES;
        case LU_FAIL_SPIKE_POOL_FULL:
            return PHASE1_PIVOT_FAIL_LU_SPIKE_POOL_FULL;
        case LU_FAIL_UPDATE_PIVOT_TOO_SMALL:
            return PHASE1_PIVOT_FAIL_LU_UPDATE_PIVOT_SMALL;
        case LU_FAIL_SINGULAR_UPDATE:
            return PHASE1_PIVOT_FAIL_LU_SINGULAR_UPDATE;
        case LU_FAIL_FACTOR_SINGULAR:
            return PHASE1_PIVOT_FAIL_FACTOR_SINGULAR;
        default:
            return forced_refactor_path
                ? PHASE1_PIVOT_FAIL_REFACTOR_FORCED_OTHER
                : PHASE1_PIVOT_FAIL_REFACTOR_AFTER_UPDATE_OTHER;
    }
}

typedef enum {
    BASIS_ACTION_UPDATE = 0,
    BASIS_ACTION_REFACTOR = 1,
    BASIS_ACTION_REPAIR = 2,
    BASIS_ACTION_ABORT = 3
} BasisAction;

#define PHASE1_PERIODIC_REFACTOR_MIN_INTERVAL 24
#define PHASE1_PERIODIC_REFACTOR_MAX_INTERVAL 96
#define PHASE2_PERIODIC_REFACTOR_MIN_INTERVAL 10
#define PHASE2_PERIODIC_REFACTOR_MAX_INTERVAL 80
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_M 1200
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_COUNT 20
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MIN_INTERVAL 24
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_SPIKE_PCT 30
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_COND 1e6
#define PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_GROWTH 1e4
#define PHASE2_DEGEN_ESCAPE_MIN_M 1200
#define PHASE2_DEGEN_ESCAPE_DEGEN_TRIGGER 120
#define PHASE2_DEGEN_ESCAPE_POLICY_TRIGGER 200
#define PHASE2_DEGEN_ESCAPE_MAX_ATTEMPTS 2
#define PHASE2_DEGEN_ESCAPE_BLAND_HOLD_ITERS 16
#define PERIODIC_REFACTOR_MIN_UPDATE_AGE 8
#define PERIODIC_REFACTOR_PRESSURE_TRIGGER 0.40
#define PERIODIC_REFACTOR_SIZE_START_M 350
#define PERIODIC_REFACTOR_SIZE_FULL_M 500
#define PERIODIC_REFACTOR_RELAX_NUM 1
#define PERIODIC_REFACTOR_RELAX_DEN 3
#define PERIODIC_FEEDBACK_BIAS_LIMIT 0.25
#define PERIODIC_FEEDBACK_DECAY 0.85
#define PERIODIC_FEEDBACK_RELAX_STEP 0.06
#define PERIODIC_FEEDBACK_TIGHTEN_STEP 0.08
#define PERIODIC_FEEDBACK_LOW_PRESSURE 0.55
#define PERIODIC_FEEDBACK_HIGH_PRESSURE 0.85
#define PERIODIC_FEEDBACK_EARLY_RECOVERY_NUM 2
#define PERIODIC_FEEDBACK_EARLY_RECOVERY_DEN 3
#define PHASE2_POLICY_COOLDOWN_MIN_M 450
#define PHASE2_POLICY_COOLDOWN_DEGEN_TRIGGER 40
#define PHASE2_POLICY_COOLDOWN_MIN_UPDATES 16
#define PHASE2_POLICY_COOLDOWN_MAX_UPDATES 96
#define PHASE1_POLICY_PRESSURE_DECAY_STEP 0.06
#define PHASE1_POLICY_PRESSURE_DECAY_MAX 0.24
#define PHASE1_POLICY_PRESSURE_RECOVERY_STEP 0.01
#define PHASE2_POLICY_PRESSURE_DECAY_STEP 0.06
#define PHASE2_POLICY_PRESSURE_DECAY_MAX 0.24
#define PHASE2_POLICY_PRESSURE_RECOVERY_STEP 0.01
#define PHASE1_DEGEN_THRESHOLD_DEFAULT 50
#define PHASE1_DEGEN_THRESHOLD_LARGE_M 700
#define PHASE1_DEGEN_THRESHOLD_LARGE 20
#define PHASE1_STALL_THRESHOLD_DEFAULT 50
#define PHASE1_STALL_THRESHOLD_LARGE 30
#define PHASE1_NO_ENTERING_CLEANUP_MAX_ITERS 128
#define PHASE1_RC_ONLY_STREAK_GUARD 6
#define PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_THRESHOLD 3
#define PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_DIVISOR 3
#define PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER 2
#define PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_ITERS RALPH_PHASE1_ENTERING_EXCLUDE_ITERS
#define PHASE1_AUTO_DANTZIG_MIN_M 700
#define PHASE1_AUTO_DANTZIG_MAX_M 1200
#define PHASE1_AUTO_DANTZIG_DEGEN_TRIGGER 20
#define PHASE1_NO_PIVOT_FORCE_MIN_M 700
#define PHASE1_NO_PIVOT_FORCE_BASE_TRIGGER 48
#define PHASE1_NO_PIVOT_FORCE_MIN_TRIGGER 24
#define PHASE1_NO_PIVOT_FORCE_COOLDOWN_UPDATES 24
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_RATIO 8
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_DIR_SKIP 8
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_PIVOT_FAIL 4
#define PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN 3
#define PHASE1_NO_PIVOT_LADDER_RESCUE_START 3
#define PHASE1_NO_PIVOT_LADDER_RESCUE_PERIOD 4
#define PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_START 8
#define PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_PERIOD 4
#define PHASE1_NO_PIVOT_PROGRESS_WINDOW 6
#define PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN 1e-4
#define PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN 1e-8
#define PHASE1_NO_PIVOT_LADDER_RESCUE_COOLDOWN_ITERS 16
#define PHASE1_NO_PIVOT_LADDER_RESCUE_FAIL_CAP 3
#define PHASE1_DIR_SKIP_LADDER_RESCUE_START 16
#define PHASE1_DIR_SKIP_LADDER_RESCUE_PERIOD 8
#define PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_TRIGGER 64
#define PHASE1_DIR_SKIP_FORCE_PIVOT_MIN_TRIGGER 24
#define PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_BUDGET 12
#define PHASE1_DIR_SKIP_FORCE_PIVOT_MAX_BUDGET 32
#define PHASE1_FORCE_SMALL_PIVOT_MIN_UPDATE_AGE 6
#define PHASE1_DIR_ESCAPE_MIN_M 200
#define PHASE1_DIR_ESCAPE_BASE_TRIGGER 48
#define PHASE1_DIR_ESCAPE_MIN_TRIGGER 20
#define PHASE1_DIR_ESCAPE_BASE_NO_PROGRESS_TRIGGER 10
#define PHASE1_DIR_ESCAPE_MIN_NO_PROGRESS_TRIGGER 4
#define PHASE1_DIR_ESCAPE_BASE_COOLDOWN_UPDATES 48
#define PHASE1_DIR_ESCAPE_MAX_COOLDOWN_UPDATES 160
#define PHASE1_DIR_ESCAPE_TELEM_TRIGGER 1
#define PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_LU_HEALTH 2
#define PHASE1_DIR_ESCAPE_TELEM_HARD_BYPASS 3
#define PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_FORCE_PIVOT_MODE 4
#define PHASE1_DIR_REFACTOR_TELEM_NO_PIVOT_FORCE 1
#define PHASE1_DIR_REFACTOR_TELEM_FORCE_EXTREME_DIR 2
#define PHASE1_DIR_REFACTOR_TELEM_FORCE_LU_HEALTH 3
#define PHASE1_DIR_REFACTOR_TELEM_FORCE_PIVOT_MODE 4
#define PHASE1_DIR_REFACTOR_TELEM_LADDER_FORCE 5
#define PHASE1_FORCE_PIVOT_RELAX_MIN_M 700
#define PHASE1_FORCE_PIVOT_RELAX_DEGEN_TRIGGER 80
#define PHASE1_FORCE_PIVOT_RELAX_MAX_NO_PROGRESS 10
#define PHASE1_FORCE_PIVOT_RELAX_RESCUE_MIN_ATTEMPTS 8
#define PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_NUM 4
#define PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_DEN 5
#define PHASE1_FORCE_EXTREME_RELAX_MIN_M 700
#define PHASE1_FORCE_EXTREME_RELAX_DEGEN_TRIGGER 80
#define PHASE1_FORCE_EXTREME_RELAX_MAX_NO_PROGRESS 10
#define PHASE1_FORCE_EXTREME_RELAX_MAX_RATIO 300.0
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_M 700
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_DEGEN_TRIGGER 20
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_UPDATES 12
#define PHASE1_SOFT_LU_POLICY_COOLDOWN_MAX_UPDATES 48
#define SOFT_LU_COST_EWMA_ALPHA 0.20
#define SOFT_LU_MAX_CONSEC_DEFER_PHASE1 6
#define SOFT_LU_MAX_CONSEC_DEFER_PHASE2 4
#define PERIODIC_COST_MAX_CONSEC_DEFER_PHASE1 2
#define PERIODIC_COST_MAX_CONSEC_DEFER_PHASE2 3

static double clamp_unit_interval(double x) {
    if (!(x > 0.0)) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

static double clamp_feedback_bias(double x) {
    if (!isfinite(x)) return 0.0;
    if (x > PERIODIC_FEEDBACK_BIAS_LIMIT) return PERIODIC_FEEDBACK_BIAS_LIMIT;
    if (x < -PERIODIC_FEEDBACK_BIAS_LIMIT) return -PERIODIC_FEEDBACK_BIAS_LIMIT;
    return x;
}

static double ewma_update_ms(double prev_ms, double sample_ms) {
    if (!isfinite(sample_ms) || sample_ms <= 0.0) return prev_ms;
    if (!isfinite(prev_ms) || prev_ms <= 0.0) return sample_ms;
    return prev_ms + SOFT_LU_COST_EWMA_ALPHA * (sample_ms - prev_ms);
}

static double phase_hotpath_ms(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) {
        return owner->telemetry.perf_phase1_pricing_ms +
               owner->telemetry.perf_phase1_ratio_ms +
               owner->telemetry.perf_phase1_pivot_ms +
               owner->telemetry.perf_phase1_compute_solution_ms +
               owner->telemetry.perf_phase1_compute_rc_ms;
    }
    if (phase == 2) {
        return owner->telemetry.perf_phase2_pricing_ms +
               owner->telemetry.perf_phase2_ratio_ms +
               owner->telemetry.perf_phase2_pivot_ms +
               owner->telemetry.perf_phase2_compute_solution_ms +
               owner->telemetry.perf_phase2_compute_rc_ms;
    }
    return 0.0;
}

static double* soft_lu_iter_cost_ewma_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_iter_cost_ewma_phase1;
    if (phase == 2) return &owner->policy.soft_lu_iter_cost_ewma_phase2;
    return NULL;
}

static double* soft_lu_refactor_cost_ewma_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_refactor_cost_ewma_phase1;
    if (phase == 2) return &owner->policy.soft_lu_refactor_cost_ewma_phase2;
    return NULL;
}

int simplex_smcp_working_excl_should_skip_for_test(int smcp_excl,
                                                   int smcp_shift,
                                                   int var_status,
                                                   double lb,
                                                   double ub,
                                                   double tol_bnd) {
    return lp_policy_glpk_working_exclude_nonbasic(smcp_excl,
                                                   smcp_shift,
                                                   var_status,
                                                   lb,
                                                   ub,
                                                   tol_bnd);
}

int simplex_smcp_excl_should_skip_for_test(int smcp_excl,
                                           int var_status,
                                           double lb,
                                           double ub,
                                           double tol_bnd) {
    return simplex_smcp_working_excl_should_skip_for_test(smcp_excl,
                                                          LP_GLPK_SMCP_SHIFT_OFF,
                                                          var_status,
                                                          lb,
                                                          ub,
                                                          tol_bnd);
}

static inline int simplex_smcp_excl_skip_var(const SimplexTableau *tab, int j) {
    int smcp_excl = 1;
    int smcp_shift = 1;
    double tol_bnd = 1e-7;
    if (!tab || j < 0 || j >= tab->n) return 0;
    if (tab->owner) {
        smcp_excl = tab->owner->smcp_excl;
        smcp_shift = tab->owner->smcp_shift;
        tol_bnd = tab->owner->smcp_tol_bnd;
    }
    return simplex_smcp_working_excl_should_skip_for_test(smcp_excl,
                                                          smcp_shift,
                                                          (int)tab->var_status[j],
                                                          tab->lb_ext[j],
                                                          tab->ub_ext[j],
                                                          tol_bnd);
}

int simplex_smcp_shift_allows_perturb_for_test(int smcp_shift) {
    return smcp_shift != 0;
}

static inline int simplex_smcp_shift_allows_perturb(const SimplexTableau *tab) {
    if (!tab || !tab->owner) return 1;
    return simplex_smcp_shift_allows_perturb_for_test(tab->owner->smcp_shift);
}

static int* periodic_cost_iter_samples_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_iter_samples_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_iter_samples_phase2;
    return NULL;
}

static int* periodic_cost_refactor_samples_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_refactor_samples_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_refactor_samples_phase2;
    return NULL;
}

static int periodic_cost_iter_samples(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.periodic_cost_iter_samples_phase1;
    if (phase == 2) return owner->policy.periodic_cost_iter_samples_phase2;
    return 0;
}

static int periodic_cost_refactor_samples(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.periodic_cost_refactor_samples_phase1;
    if (phase == 2) return owner->policy.periodic_cost_refactor_samples_phase2;
    return 0;
}

static void soft_lu_record_iter_cost(SimplexSolver *owner, int phase, double iter_ms) {
    int *samples_ptr = periodic_cost_iter_samples_ptr(owner, phase);
    double *ewma_ptr = soft_lu_iter_cost_ewma_ptr(owner, phase);
    if (!ewma_ptr) return;
    if (!isfinite(iter_ms) || iter_ms <= 0.0) return;
    *ewma_ptr = ewma_update_ms(*ewma_ptr, iter_ms);
    if (samples_ptr) (*samples_ptr)++;
}

static void soft_lu_record_refactor_cost(SimplexSolver *owner, int phase, double refactor_ms) {
    int *samples_ptr = periodic_cost_refactor_samples_ptr(owner, phase);
    double *ewma_ptr = soft_lu_refactor_cost_ewma_ptr(owner, phase);
    if (!ewma_ptr) return;
    if (!isfinite(refactor_ms) || refactor_ms <= 0.0) return;
    *ewma_ptr = ewma_update_ms(*ewma_ptr, refactor_ms);
    if (samples_ptr) (*samples_ptr)++;
}

static double soft_lu_iter_cost_ewma(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) return owner->policy.soft_lu_iter_cost_ewma_phase1;
    if (phase == 2) return owner->policy.soft_lu_iter_cost_ewma_phase2;
    return 0.0;
}

static double soft_lu_refactor_cost_ewma(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) return owner->policy.soft_lu_refactor_cost_ewma_phase1;
    if (phase == 2) return owner->policy.soft_lu_refactor_cost_ewma_phase2;
    return 0.0;
}

static void soft_lu_record_defer(SimplexSolver *owner, int phase) {
    if (!owner) return;
    if (phase == 1) owner->policy.soft_lu_cost_gate_defers_phase1++;
    else if (phase == 2) owner->policy.soft_lu_cost_gate_defers_phase2++;
}

static int soft_lu_defer_cap_for_phase(int phase) {
    if (phase == 1) return SOFT_LU_MAX_CONSEC_DEFER_PHASE1;
    if (phase == 2) return SOFT_LU_MAX_CONSEC_DEFER_PHASE2;
    return 0;
}

static int* soft_lu_consecutive_defers_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_consecutive_defers_phase1;
    if (phase == 2) return &owner->policy.soft_lu_consecutive_defers_phase2;
    return NULL;
}

static int* soft_lu_cap_forced_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.soft_lu_defer_cap_forced_phase1;
    if (phase == 2) return &owner->policy.soft_lu_defer_cap_forced_phase2;
    return NULL;
}

static int soft_lu_consecutive_defers(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.soft_lu_consecutive_defers_phase1;
    if (phase == 2) return owner->policy.soft_lu_consecutive_defers_phase2;
    return 0;
}

static void soft_lu_set_consecutive_defers(SimplexSolver *owner, int phase, int value) {
    int *ptr = soft_lu_consecutive_defers_ptr(owner, phase);
    if (!ptr) return;
    if (value < 0) value = 0;
    *ptr = value;
}

static void soft_lu_reset_defer_streak(SimplexSolver *owner, int phase) {
    soft_lu_set_consecutive_defers(owner, phase, 0);
}

static void soft_lu_record_cap_forced(SimplexSolver *owner, int phase) {
    int *ptr = soft_lu_cap_forced_ptr(owner, phase);
    if (!ptr) return;
    (*ptr)++;
}

static void periodic_cost_record_defer(SimplexSolver *owner, int phase) {
    if (!owner) return;
    if (phase == 1) owner->policy.periodic_cost_gate_defers_phase1++;
    else if (phase == 2) owner->policy.periodic_cost_gate_defers_phase2++;
}

static int periodic_cost_defer_cap_for_phase(int phase) {
    if (phase == 1) return PERIODIC_COST_MAX_CONSEC_DEFER_PHASE1;
    if (phase == 2) return PERIODIC_COST_MAX_CONSEC_DEFER_PHASE2;
    return 0;
}

static int* periodic_cost_consecutive_defers_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_consecutive_defers_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_consecutive_defers_phase2;
    return NULL;
}

static int* periodic_cost_cap_forced_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_defer_cap_forced_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_defer_cap_forced_phase2;
    return NULL;
}

static int* periodic_cost_checks_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_checks_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_checks_phase2;
    return NULL;
}

static int* periodic_cost_block_small_m_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_block_small_m_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_block_small_m_phase2;
    return NULL;
}

static int* periodic_cost_block_invalid_inputs_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_block_invalid_inputs_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_block_invalid_inputs_phase2;
    return NULL;
}

static int* periodic_cost_block_warmup_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_block_warmup_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_block_warmup_phase2;
    return NULL;
}

static int* periodic_cost_block_invalid_cost_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_block_invalid_cost_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_block_invalid_cost_phase2;
    return NULL;
}

static int* periodic_cost_block_ratio_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_block_ratio_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_block_ratio_phase2;
    return NULL;
}

static int* periodic_cost_block_update_reserve_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_block_update_reserve_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_block_update_reserve_phase2;
    return NULL;
}

static int* periodic_cost_last_reason_ptr(SimplexSolver *owner, int phase) {
    if (!owner) return NULL;
    if (phase == 1) return &owner->policy.periodic_cost_gate_last_reason_phase1;
    if (phase == 2) return &owner->policy.periodic_cost_gate_last_reason_phase2;
    return NULL;
}

static int periodic_cost_consecutive_defers(const SimplexSolver *owner, int phase) {
    if (!owner) return 0;
    if (phase == 1) return owner->policy.periodic_cost_consecutive_defers_phase1;
    if (phase == 2) return owner->policy.periodic_cost_consecutive_defers_phase2;
    return 0;
}

static void periodic_cost_set_consecutive_defers(SimplexSolver *owner, int phase, int value) {
    int *ptr = periodic_cost_consecutive_defers_ptr(owner, phase);
    if (!ptr) return;
    if (value < 0) value = 0;
    *ptr = value;
}

static void periodic_cost_reset_defer_streak(SimplexSolver *owner, int phase) {
    periodic_cost_set_consecutive_defers(owner, phase, 0);
}

static void periodic_cost_record_cap_forced(SimplexSolver *owner, int phase) {
    int *ptr = periodic_cost_cap_forced_ptr(owner, phase);
    if (!ptr) return;
    (*ptr)++;
}

static void periodic_cost_record_gate_reason(SimplexSolver *owner,
                                             int phase,
                                             LPPeriodicCostDampenReason reason) {
    int *checks_ptr = periodic_cost_checks_ptr(owner, phase);
    int *last_reason_ptr = periodic_cost_last_reason_ptr(owner, phase);
    int *block_ptr = NULL;

    if (!owner) return;
    if (checks_ptr) (*checks_ptr)++;
    if (last_reason_ptr) *last_reason_ptr = (int)reason;

    switch (reason) {
        case LP_PERIODIC_COST_DAMPEN_BLOCK_SMALL_M:
            block_ptr = periodic_cost_block_small_m_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_INPUTS:
            block_ptr = periodic_cost_block_invalid_inputs_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_WARMUP:
            block_ptr = periodic_cost_block_warmup_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_COST:
            block_ptr = periodic_cost_block_invalid_cost_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO:
            block_ptr = periodic_cost_block_ratio_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_BLOCK_UPDATE_RESERVE:
            block_ptr = periodic_cost_block_update_reserve_ptr(owner, phase);
            break;
        case LP_PERIODIC_COST_DAMPEN_DEFER:
        case LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE:
        default:
            break;
    }

    if (block_ptr) (*block_ptr)++;
}

static double periodic_feedback_bias_for_phase(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) return owner->policy.periodic_feedback_bias_phase1;
    if (phase == 2) return owner->policy.periodic_feedback_bias_phase2;
    return 0.0;
}

static void periodic_feedback_set_hint(SimplexSolver *owner,
                                       int phase,
                                       int interval,
                                       double run_pressure) {
    if (!owner) return;
    if (interval < 0) interval = 0;
    run_pressure = clamp_unit_interval(run_pressure);
    if (phase == 1) {
        owner->policy.periodic_feedback_hint_interval_phase1 = interval;
        owner->policy.periodic_feedback_hint_pressure_phase1 = run_pressure;
    } else if (phase == 2) {
        owner->policy.periodic_feedback_hint_interval_phase2 = interval;
        owner->policy.periodic_feedback_hint_pressure_phase2 = run_pressure;
    }
}

static void periodic_feedback_record_refactor(SimplexSolver *owner,
                                              int phase,
                                              int reason,
                                              int updates_before,
                                              int status) {
    double *bias_ptr = NULL;
    int *last_reason_ptr = NULL;
    int *last_interval_ptr = NULL;
    int *hint_interval_ptr = NULL;
    double *hint_pressure_ptr = NULL;
    double bias;
    int hint_interval;
    double hint_pressure;

    if (!owner || (phase != 1 && phase != 2)) return;

    if (phase == 1) {
        bias_ptr = &owner->policy.periodic_feedback_bias_phase1;
        last_reason_ptr = &owner->policy.periodic_feedback_last_reason_phase1;
        last_interval_ptr = &owner->policy.periodic_feedback_last_interval_phase1;
        hint_interval_ptr = &owner->policy.periodic_feedback_hint_interval_phase1;
        hint_pressure_ptr = &owner->policy.periodic_feedback_hint_pressure_phase1;
    } else {
        bias_ptr = &owner->policy.periodic_feedback_bias_phase2;
        last_reason_ptr = &owner->policy.periodic_feedback_last_reason_phase2;
        last_interval_ptr = &owner->policy.periodic_feedback_last_interval_phase2;
        hint_interval_ptr = &owner->policy.periodic_feedback_hint_interval_phase2;
        hint_pressure_ptr = &owner->policy.periodic_feedback_hint_pressure_phase2;
    }

    bias = (*bias_ptr) * PERIODIC_FEEDBACK_DECAY;
    hint_interval = *hint_interval_ptr;
    hint_pressure = *hint_pressure_ptr;

    if (status != 0) {
        if (reason == RALPH_REFACTOR_REASON_PERIODIC) {
            bias += PERIODIC_FEEDBACK_TIGHTEN_STEP;
        }
        *bias_ptr = clamp_feedback_bias(bias);
        *last_reason_ptr = reason;
        if (reason == RALPH_REFACTOR_REASON_PERIODIC && hint_interval > 0) {
            *last_interval_ptr = hint_interval;
        }
        *hint_interval_ptr = 0;
        *hint_pressure_ptr = 0.0;
        return;
    }

    if (reason == RALPH_REFACTOR_REASON_PERIODIC) {
        double update_ratio = 1.0;
        if (hint_interval > 0 && updates_before > 0) {
            update_ratio = (double)updates_before / (double)hint_interval;
        }

        if (hint_pressure < PERIODIC_FEEDBACK_LOW_PRESSURE && update_ratio <= 1.05) {
            bias -= PERIODIC_FEEDBACK_RELAX_STEP;
        } else if (hint_pressure >= PERIODIC_FEEDBACK_HIGH_PRESSURE) {
            bias += PERIODIC_FEEDBACK_TIGHTEN_STEP;
        }

        if (hint_interval > 0) {
            *last_interval_ptr = hint_interval;
        } else if (updates_before > 0) {
            *last_interval_ptr = updates_before;
        }
    } else if (*last_reason_ptr == RALPH_REFACTOR_REASON_PERIODIC &&
               *last_interval_ptr > 0 &&
               updates_before > 0) {
        int early_threshold =
            (*last_interval_ptr * PERIODIC_FEEDBACK_EARLY_RECOVERY_NUM) /
            PERIODIC_FEEDBACK_EARLY_RECOVERY_DEN;
        if (early_threshold < PERIODIC_REFACTOR_MIN_UPDATE_AGE) {
            early_threshold = PERIODIC_REFACTOR_MIN_UPDATE_AGE;
        }

        if ((reason == RALPH_REFACTOR_REASON_UPDATE_RECOVERY ||
             reason == RALPH_REFACTOR_REASON_RATIO_RECOVERY ||
             reason == RALPH_REFACTOR_REASON_PIVOT_RECOVERY ||
             reason == RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) &&
            updates_before <= early_threshold) {
            bias += PERIODIC_FEEDBACK_TIGHTEN_STEP;
        }
    }

    *bias_ptr = clamp_feedback_bias(bias);
    *last_reason_ptr = reason;
    *hint_interval_ptr = 0;
    *hint_pressure_ptr = 0.0;
}

static LPPeriodicRefactorPolicy compute_periodic_refactor_policy(const SimplexTableau *tab,
                                                                 int phase,
                                                                 int use_bland,
                                                                 int degenerate_count) {
    LPPeriodicRefactorPolicy policy = {0, 0, 0.0, 0.0};
    double feedback_bias = 0.0;

    if (!tab || !tab->lu || !tab->use_two_phase) return policy;
    feedback_bias = periodic_feedback_bias_for_phase(tab->owner, phase);
    return lp_refactor_policy_build_from_metrics(phase,
                                                 tab->m,
                                                 tab->lu->max_updates,
                                                 tab->lu->num_updates,
                                                 tab->lu->spike_pool_used,
                                                 tab->lu->spike_pool_capacity,
                                                 tab->lu->cond_estimate,
                                                 tab->lu->growth_factor,
                                                 use_bland,
                                                 degenerate_count,
                                                 feedback_bias);
}

static int should_run_periodic_refactor(const SimplexTableau *tab,
                                        int iter,
                                        const LPPeriodicRefactorPolicy *policy,
                                        int use_bland,
                                        int degenerate_count) {
    if (!tab || !tab->lu) return 0;
    return lp_refactor_policy_should_run_metrics(iter,
                                                 tab->lu->num_updates,
                                                 policy,
                                                 use_bland,
                                                 degenerate_count);
}

static LPReinvertControllerState *reinvert_state_for_phase(SimplexSolver *solver,
                                                           int phase) {
    if (!solver) return NULL;
    if (phase == 1) return &solver->policy.reinvert_state_phase1;
    if (phase == 2) return &solver->policy.reinvert_state_phase2;
    return NULL;
}

static double average_solve_density(long long sol_nnz_total, int samples, int m) {
    double density;
    double denom;
    if (samples <= 0 || m <= 0) return 0.0;
    denom = (double)samples * (double)m;
    if (!(denom > 0.0)) return 0.0;
    density = (double)sol_nnz_total / denom;
    if (!(density > 0.0)) return 0.0;
    if (density > 1.0) return 1.0;
    return density;
}

#define PHASE1_REINVERT_PRESSURE_WINDOW_ITERS 96
#define PHASE1_REINVERT_PRESSURE_DEMOTE_COUNT 4
#define PHASE1_REINVERT_DEMOTE_COOLDOWN_ITERS 32
#define PHASE1_REINVERT_PRESSURE_NO_PIVOT_THRESHOLD 8
#define PHASE1_REINVERT_PRESSURE_NO_PROGRESS_THRESHOLD 24
#define PHASE1_REINVERT_PRESSURE_RATIO_BREAKDOWN_THRESHOLD 4
#define PHASE1_REINVERT_PRESSURE_DIR_SKIP_THRESHOLD 24

static int reinvert_phase1_pressure_event(int no_pivot_streak,
                                          int no_progress_streak,
                                          int ratio_breakdown_count,
                                          int dir_skip_no_recompute_streak) {
    if (no_pivot_streak >= PHASE1_REINVERT_PRESSURE_NO_PIVOT_THRESHOLD &&
        no_progress_streak >= (PHASE1_REINVERT_PRESSURE_NO_PROGRESS_THRESHOLD / 2)) {
        return 1;
    }
    if (no_progress_streak >= PHASE1_REINVERT_PRESSURE_NO_PROGRESS_THRESHOLD) return 1;
    if (dir_skip_no_recompute_streak >= PHASE1_REINVERT_PRESSURE_DIR_SKIP_THRESHOLD) return 1;
    if (ratio_breakdown_count >= PHASE1_REINVERT_PRESSURE_RATIO_BREAKDOWN_THRESHOLD) return 1;
    return 0;
}

static void reinvert_phase1_pressure_safety_step_core(
    int iter,
    int no_pivot_streak,
    int no_progress_streak,
    int ratio_breakdown_count,
    int dir_skip_no_recompute_streak,
    int hard_lu_trigger,
    int *last_iter_io,
    int *burst_io,
    int *demoted_io,
    int *demotions_io) {
    int last_iter;
    int burst;
    int demoted;
    int demotions;
    int pressure_event = 0;

    if (!last_iter_io || !burst_io || !demoted_io || !demotions_io) return;

    if (iter < 0) iter = 0;
    if (no_pivot_streak < 0) no_pivot_streak = 0;
    if (no_progress_streak < 0) no_progress_streak = 0;
    if (ratio_breakdown_count < 0) ratio_breakdown_count = 0;
    if (dir_skip_no_recompute_streak < 0) dir_skip_no_recompute_streak = 0;

    last_iter = *last_iter_io;
    burst = *burst_io;
    demoted = *demoted_io ? 1 : 0;
    demotions = *demotions_io;
    if (burst < 0) burst = 0;
    if (last_iter < -1) last_iter = -1;
    if (demotions < 0) demotions = 0;

    if (demoted) {
        if (last_iter < 0) {
            last_iter = iter;
        } else if ((iter - last_iter) >= PHASE1_REINVERT_DEMOTE_COOLDOWN_ITERS) {
            demoted = 0;
            burst = 0;
            last_iter = -1;
        }
        *last_iter_io = last_iter;
        *burst_io = burst;
        *demoted_io = demoted;
        *demotions_io = demotions;
        return;
    }

    if (hard_lu_trigger) {
        if (last_iter >= 0 && (iter - last_iter) > PHASE1_REINVERT_PRESSURE_WINDOW_ITERS) {
            burst = 0;
        }
        *last_iter_io = last_iter;
        *burst_io = burst;
        *demoted_io = demoted;
        *demotions_io = demotions;
        return;
    }

    pressure_event = reinvert_phase1_pressure_event(no_pivot_streak,
                                                    no_progress_streak,
                                                    ratio_breakdown_count,
                                                    dir_skip_no_recompute_streak);
    if (pressure_event) {
        if (last_iter >= 0 && (iter - last_iter) <= PHASE1_REINVERT_PRESSURE_WINDOW_ITERS) {
            burst++;
        } else {
            burst = 1;
        }
        last_iter = iter;
    } else if (last_iter >= 0 && (iter - last_iter) > PHASE1_REINVERT_PRESSURE_WINDOW_ITERS) {
        burst = 0;
    }

    if (burst >= PHASE1_REINVERT_PRESSURE_DEMOTE_COUNT) {
        demoted = 1;
        demotions++;
        burst = 0;
        last_iter = iter;
    }

    *last_iter_io = last_iter;
    *burst_io = burst;
    *demoted_io = demoted;
    *demotions_io = demotions;
}

void simplex_reinvert_phase1_pressure_safety_step_for_test(
    int iter,
    int no_pivot_streak,
    int no_progress_streak,
    int ratio_breakdown_count,
    int dir_skip_no_recompute_streak,
    int hard_lu_trigger,
    int *last_iter_io,
    int *burst_io,
    int *demoted_io,
    int *demotions_io) {
    reinvert_phase1_pressure_safety_step_core(iter,
                                              no_pivot_streak,
                                              no_progress_streak,
                                              ratio_breakdown_count,
                                              dir_skip_no_recompute_streak,
                                              hard_lu_trigger,
                                              last_iter_io,
                                              burst_io,
                                              demoted_io,
                                              demotions_io);
}

static int reinvert_controller_mode_get(const SimplexSolver *solver) {
    int mode;
    if (!solver) return LP_REINVERT_MODE_SHADOW;
    mode = solver->policy.reinvert_controller_mode;
    if (!lp_reinvert_controller_mode_is_valid(mode)) {
        return LP_REINVERT_MODE_SHADOW;
    }
    return mode;
}

static int reinvert_controller_collect_shadow(const SimplexSolver *solver) {
    return reinvert_controller_mode_get(solver) != LP_REINVERT_MODE_OFF;
}

static void reinvert_phase1_pressure_safety_update(SimplexSolver *solver,
                                                   int iter,
                                                   int no_pivot_streak,
                                                   int no_progress_streak,
                                                   int ratio_breakdown_count,
                                                   int dir_skip_no_recompute_streak,
                                                   int hard_lu_trigger) {
    if (!solver) return;
    if (reinvert_controller_mode_get(solver) != LP_REINVERT_MODE_CONTROL_ALL) return;
    reinvert_phase1_pressure_safety_step_core(
        iter,
        no_pivot_streak,
        no_progress_streak,
        ratio_breakdown_count,
        dir_skip_no_recompute_streak,
        hard_lu_trigger,
        &solver->policy.reinvert_phase1_pressure_last_iter,
        &solver->policy.reinvert_phase1_pressure_burst,
        &solver->policy.reinvert_phase1_control_demoted,
        &solver->policy.reinvert_phase1_control_demotions);
}

static int reinvert_controller_controls_periodic_phase_effective(int mode,
                                                                 int phase,
                                                                 int phase1_demoted) {
    if (mode == LP_REINVERT_MODE_CONTROL_ALL) {
        if (phase == 1 && phase1_demoted) return 0;
        return phase == 1 || phase == 2;
    }
    return (mode == LP_REINVERT_MODE_CONTROL_PHASE1 && phase == 1);
}

static int reinvert_controller_controls_periodic_phase(const SimplexSolver *solver,
                                                       int phase) {
    int mode = reinvert_controller_mode_get(solver);
    int phase1_demoted = 0;
    if (solver) {
        phase1_demoted = solver->policy.reinvert_phase1_control_demoted ? 1 : 0;
    }
    return reinvert_controller_controls_periodic_phase_effective(mode, phase, phase1_demoted);
}

static int reinvert_periodic_apply_decision(int phase,
                                            int hard_lu_trigger,
                                            int periodic_due,
                                            int decision,
                                            int control_enabled) {
    (void)phase;
    if (hard_lu_trigger) return periodic_due ? 1 : 0;
    if (!control_enabled) return periodic_due ? 1 : 0;
    switch ((LPReinvertDecision)decision) {
        case LP_REINVERT_DECISION_FORCE:
            return 1;
        case LP_REINVERT_DECISION_DEFER:
            return 0;
        case LP_REINVERT_DECISION_ALLOW:
        default:
            return periodic_due ? 1 : 0;
    }
}

int simplex_reinvert_periodic_control_for_test(int mode,
                                               int phase,
                                               int hard_lu_trigger,
                                               int periodic_due,
                                               int decision) {
    int control_enabled =
        reinvert_controller_controls_periodic_phase_effective(mode, phase, 0);
    return reinvert_periodic_apply_decision(phase,
                                            hard_lu_trigger,
                                            periodic_due,
                                            decision,
                                            control_enabled);
}

int simplex_reinvert_periodic_control_with_phase1_demotion_for_test(
    int mode,
    int phase,
    int phase1_demoted,
    int hard_lu_trigger,
    int periodic_due,
    int decision) {
    int control_enabled = reinvert_controller_controls_periodic_phase_effective(
        mode,
        phase,
        phase1_demoted);
    return reinvert_periodic_apply_decision(phase,
                                            hard_lu_trigger,
                                            periodic_due,
                                            decision,
                                            control_enabled);
}

typedef struct {
    int active;
    int suggested_refactor;
    LPReinvertControllerDecision decision;
} LPReinvertShadowEval;

static void reinvert_shadow_eval_reset(LPReinvertShadowEval *eval) {
    if (!eval) return;
    memset(eval, 0, sizeof(*eval));
}

static void reinvert_shadow_prepare_phase(SimplexSolver *solver,
                                          SimplexTableau *tab,
                                          int phase,
                                          int iter,
                                          const LPLUHealthRefactorDecision *lu_health_decision,
                                          int periodic_due,
                                          int min_update_age,
                                          int cooldown_updates,
                                          int allow_control,
                                          int *periodic_due_io,
                                          LPReinvertShadowEval *eval) {
    LPReinvertControllerState *state;
    LPReinvertControllerSignals signals;
    int control_enabled;
    double ftran_density;
    double btran_density;

    if (!eval) return;
    reinvert_shadow_eval_reset(eval);
    if (!solver || !tab || !tab->lu || !lu_health_decision) return;
    if (!reinvert_controller_collect_shadow(solver)) return;

    state = reinvert_state_for_phase(solver, phase);
    if (!state) return;

    lp_reinvert_controller_state_record_update_age_ratio(state,
                                                         tab->lu->num_updates,
                                                         tab->lu->max_updates);
    ftran_density = average_solve_density(solver->telemetry.perf_ftran_sol_nnz_total,
                                          solver->telemetry.perf_ftran_nnz_samples,
                                          tab->m);
    btran_density = average_solve_density(solver->telemetry.perf_btran_sol_nnz_total,
                                          solver->telemetry.perf_btran_nnz_samples,
                                          tab->m);
    lp_reinvert_controller_state_record_solve_density(state,
                                                      ftran_density,
                                                      btran_density);
    lp_reinvert_controller_state_set_cooldown(state, cooldown_updates);

    signals.phase = phase;
    signals.iter = iter;
    signals.m = tab->m;
    signals.num_updates = tab->lu->num_updates;
    signals.max_updates = tab->lu->max_updates;
    signals.periodic_due = periodic_due ? 1 : 0;
    signals.min_update_age = (min_update_age > 0) ? min_update_age : 0;
    signals.cooldown_updates = (cooldown_updates > 0) ? cooldown_updates : 0;
    signals.hard_lu_trigger = lu_health_decision->hard_trigger ? 1 : 0;
    signals.soft_lu_trigger = lu_health_decision->soft_trigger ? 1 : 0;
    signals.ftran_density = ftran_density;
    signals.btran_density = btran_density;

    eval->decision = lp_reinvert_controller_decide(state, &signals);
    eval->active = 1;
    eval->suggested_refactor =
        (eval->decision.decision == LP_REINVERT_DECISION_FORCE) ||
        (eval->decision.decision == LP_REINVERT_DECISION_ALLOW && signals.periodic_due);

    control_enabled = allow_control &&
                      reinvert_controller_controls_periodic_phase(solver, phase);
    if (periodic_due_io) {
        *periodic_due_io = reinvert_periodic_apply_decision(phase,
                                                            lu_health_decision->hard_trigger,
                                                            periodic_due,
                                                            eval->decision.decision,
                                                            control_enabled);
    }
}

static void reinvert_shadow_finalize_phase(SimplexSolver *solver,
                                           int phase,
                                           const LPReinvertShadowEval *eval,
                                           int actual_refactor) {
    LPReinvertControllerState *state;
    if (!solver || !eval || !eval->active) return;
    lp_telemetry_record_reinvert_shadow(solver,
                                        phase,
                                        eval->decision.decision,
                                        eval->decision.reason,
                                        eval->suggested_refactor,
                                        actual_refactor);
    state = reinvert_state_for_phase(solver, phase);
    lp_reinvert_controller_state_apply_decision(state, &eval->decision);
}

static int solution_refine_iteration_budget(double max_residual, double feas_tol) {
    if (!isfinite(max_residual) || !isfinite(feas_tol) || feas_tol <= 0.0) return 0;
    if (max_residual <= feas_tol) return 0;
    if (max_residual <= 10.0 * feas_tol) return 1;
    if (max_residual <= 100.0 * feas_tol) return 2;
    return 5;
}

int simplex_solution_refine_limit_for_test(double max_residual, double feas_tol) {
    return solution_refine_iteration_budget(max_residual, feas_tol);
}

/*
 * Centralized basis-update policy used by simplex_pivot().
 * lu_update_status convention:
 *   0  = no LU update attempt yet
 *  -1  = LU update failed
 *  -2  = refactorization failed
 *  -3  = repair failed
 */
static int phase1_small_pivot_refactor_allowed(int force_refactor,
                                               int repeat_pattern,
                                               int lu_num_updates) {
    if (!force_refactor) return 0;
    if (lu_num_updates >= 0 &&
        lu_num_updates < PHASE1_FORCE_SMALL_PIVOT_MIN_UPDATE_AGE &&
        repeat_pattern < RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER) {
        return 0;
    }
    return 1;
}

static BasisAction choose_basis_action(double pivot,
                                       int force_refactor,
                                       int lu_update_status,
                                       int lu_reason,
                                       int repeat_pattern,
                                       int lu_num_updates,
                                       double growth_factor,
                                       double growth_threshold) {
    (void)lu_reason;
    const int force_refactor_allowed = phase1_small_pivot_refactor_allowed(
        force_refactor,
        repeat_pattern,
        lu_num_updates);

    if (!isfinite(pivot) || fabs(pivot) < RALPH_PIVOT_TOL) {
        return BASIS_ACTION_ABORT;
    }
    if (lu_update_status <= -3) {
        return BASIS_ACTION_ABORT;
    }
    if (lu_update_status == -2) {
        return BASIS_ACTION_REPAIR;
    }
    if (lu_update_status == -1) {
        return BASIS_ACTION_REFACTOR;
    }
    if (force_refactor_allowed ||
        repeat_pattern >= RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER ||
        growth_factor > growth_threshold) {
        return BASIS_ACTION_REFACTOR;
    }
    return BASIS_ACTION_UPDATE;
}

int simplex_choose_basis_action_for_test(double pivot,
                                         int force_refactor,
                                         int lu_update_status,
                                         int lu_reason,
                                         int repeat_pattern,
                                         int lu_num_updates,
                                         double growth_factor) {
    return (int)choose_basis_action(pivot,
                                    force_refactor,
                                    lu_update_status,
                                    lu_reason,
                                    repeat_pattern,
                                    lu_num_updates,
                                    growth_factor,
                                    RALPH_LU_GROWTH_REFACTOR_THRESHOLD);
}

int simplex_periodic_refactor_plan_for_test(int phase,
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
                                            int periodic_policy_cooldown,
                                            double periodic_policy_pressure_decay,
                                            int *interval_out,
                                            double *pressure_out) {
    LPPeriodicRefactorPolicy policy = lp_refactor_policy_build_from_metrics(phase,
                                                                            m,
                                                                            max_updates,
                                                                            num_updates,
                                                                            spike_pool_used,
                                                                            spike_pool_capacity,
                                                                            cond_estimate,
                                                                            growth_factor,
                                                                            use_bland,
                                                                            degenerate_count,
                                                                            feedback_bias);
    int cooldown_eligible = 0;
    LPPeriodicRefactorPolicy effective_policy = policy;
    int should_run;

    if (phase == 1) {
        cooldown_eligible = lp_refactor_policy_phase1_cooldown_eligible(m,
                                                                        degenerate_count,
                                                                        0,
                                                                        spike_pool_used,
                                                                        spike_pool_capacity,
                                                                        cond_estimate,
                                                                        growth_factor);
    } else if (phase == 2) {
        cooldown_eligible = lp_refactor_policy_phase2_cooldown_eligible(m,
                                                                        degenerate_count,
                                                                        spike_pool_used,
                                                                        spike_pool_capacity,
                                                                        cond_estimate,
                                                                        growth_factor);
    }
    if (cooldown_eligible && periodic_policy_pressure_decay > 0.0) {
        effective_policy.run_pressure = clamp_unit_interval(
            effective_policy.run_pressure - periodic_policy_pressure_decay);
    }

    if (interval_out) *interval_out = policy.interval;
    if (pressure_out) *pressure_out = effective_policy.run_pressure;
    should_run = lp_refactor_policy_should_run_metrics(iter,
                                                       num_updates,
                                                       &effective_policy,
                                                       use_bland,
                                                       degenerate_count);
    if (should_run &&
        cooldown_eligible &&
        periodic_policy_cooldown > 0) {
        should_run = 0;
    }
    return should_run;
}

int simplex_lu_health_refactor_plan_for_test(int m,
                                             int use_ft_updates,
                                             int num_updates,
                                             int max_updates,
                                             int spike_pool_used,
                                             int spike_pool_capacity,
                                             double cond_estimate,
                                             double growth_factor,
                                             int soft_breach_streak,
                                             int *hard_trigger_out,
                                             int *soft_trigger_out,
                                             int *next_streak_out,
                                             int *soft_threshold_out,
                                             int *soft_min_update_age_out) {
    LPLUHealthRefactorDecision decision =
        lp_refactor_policy_lu_health_refactor_decision(m,
                                                       use_ft_updates,
                                                       num_updates,
                                                       max_updates,
                                                       spike_pool_used,
                                                       spike_pool_capacity,
                                                       cond_estimate,
                                                       growth_factor,
                                                       soft_breach_streak);
    if (hard_trigger_out) *hard_trigger_out = decision.hard_trigger;
    if (soft_trigger_out) *soft_trigger_out = decision.soft_trigger;
    if (next_streak_out) *next_streak_out = decision.soft_breach_streak_next;
    if (soft_threshold_out) *soft_threshold_out = decision.soft_breach_threshold;
    if (soft_min_update_age_out) *soft_min_update_age_out = decision.soft_min_update_age;
    return decision.refactor_now;
}

int simplex_soft_lu_defer_plan_for_test(int phase,
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
                                        int consecutive_defers,
                                        int *cap_out,
                                        int *cap_blocked_out,
                                        int *next_consecutive_defers_out) {
    int cap = soft_lu_defer_cap_for_phase(phase);
    int should_defer = 0;
    int cap_blocked = 0;
    int next_consecutive = 0;

    if (consecutive_defers < 0) consecutive_defers = 0;
    if (lp_refactor_policy_soft_lu_cost_gate_should_defer(phase,
                                                           m,
                                                           use_bland,
                                                           degenerate_count,
                                                           num_updates,
                                                           max_updates,
                                                           spike_pool_used,
                                                           spike_pool_capacity,
                                                           cond_estimate,
                                                           growth_factor,
                                                           refactor_cost_ewma_ms,
                                                           iter_cost_ewma_ms)) {
        if (cap > 0 && consecutive_defers >= cap) {
            cap_blocked = 1;
        } else {
            should_defer = 1;
            next_consecutive = consecutive_defers + 1;
        }
    }

    if (cap_out) *cap_out = cap;
    if (cap_blocked_out) *cap_blocked_out = cap_blocked;
    if (next_consecutive_defers_out) *next_consecutive_defers_out = next_consecutive;
    return should_defer;
}

int simplex_periodic_cost_defer_plan_for_test(int phase,
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
                                              int iter_cost_samples,
                                              int consecutive_defers,
                                              int *reason_out,
                                              int *cap_out,
                                              int *cap_blocked_out,
                                              int *next_consecutive_defers_out) {
    LPPeriodicCostDampenReason decision;
    int cap = periodic_cost_defer_cap_for_phase(phase);
    int should_defer = 0;
    int cap_blocked = 0;
    int next_consecutive = 0;

    if (consecutive_defers < 0) consecutive_defers = 0;
    decision = lp_refactor_policy_periodic_cost_dampen_decision(
        phase,
        m,
        use_bland,
        degenerate_count,
        num_updates,
        max_updates,
        spike_pool_used,
        spike_pool_capacity,
        cond_estimate,
        growth_factor,
        refactor_cost_ewma_ms,
        iter_cost_ewma_ms,
        refactor_cost_samples,
        iter_cost_samples);
    if (decision == LP_PERIODIC_COST_DAMPEN_DEFER) {
        if (cap > 0 && consecutive_defers >= cap) {
            cap_blocked = 1;
        } else {
            should_defer = 1;
            next_consecutive = consecutive_defers + 1;
        }
    }

    if (reason_out) *reason_out = (int)decision;
    if (cap_out) *cap_out = cap;
    if (cap_blocked_out) *cap_blocked_out = cap_blocked;
    if (next_consecutive_defers_out) *next_consecutive_defers_out = next_consecutive;
    return should_defer;
}

static const char* phase1_no_pivot_force_reason_string(
    LPPhase1NoPivotForceReason reason) {
    switch (reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
            return "ratio_breakdown";
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            return "dir_skip";
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            return "pivot_fail";
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            return "unknown";
    }
}

static int phase1_no_pivot_force_threshold(int m, int degenerate_count) {
    int threshold = PHASE1_NO_PIVOT_FORCE_BASE_TRIGGER;
    if (m >= 1200) threshold -= 8;
    if (degenerate_count >= 80) threshold -= 8;
    if (degenerate_count >= 160) threshold -= 8;
    if (threshold < PHASE1_NO_PIVOT_FORCE_MIN_TRIGGER) {
        threshold = PHASE1_NO_PIVOT_FORCE_MIN_TRIGGER;
    }
    return threshold;
}

enum {
    PHASE1_NO_PIVOT_LADDER_STEP_RETRY = 0,
    PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE = 1,
    PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR = 2
};

static double phase1_artificial_abs_sum(const SimplexTableau *tab) {
    double art_sum = 0.0;
    if (!tab || tab->num_artificial <= 0 || !tab->artificial_vars || !tab->x) {
        return 0.0;
    }
    for (int k = 0; k < tab->num_artificial; k++) {
        int j = tab->artificial_vars[k];
        if (j >= 0 && j < tab->n) {
            art_sum += fabs(tab->x[j]);
        }
    }
    return art_sum;
}

static void phase1_no_pivot_progress_reset(int *no_progress_streak_io,
                                           double *last_art_sum_io,
                                           double *anchor_art_sum_io,
                                           int *progress_window_steps_io) {
    if (no_progress_streak_io) *no_progress_streak_io = 0;
    if (last_art_sum_io) *last_art_sum_io = RALPH_INFINITY;
    if (anchor_art_sum_io) *anchor_art_sum_io = RALPH_INFINITY;
    if (progress_window_steps_io) *progress_window_steps_io = 0;
}

static void phase1_no_pivot_progress_update(SimplexSolver *solver,
                                            const SimplexTableau *tab,
                                            double *last_art_sum_io,
                                            double *anchor_art_sum_io,
                                            int *progress_window_steps_io,
                                            int *no_progress_streak_io) {
    double art_sum;
    double prev_art_sum;
    double anchor_art_sum;
    double abs_improve_anchor;
    double rel_improve_anchor;
    double abs_improve_prev;
    double rel_improve_prev;
    double abs_target_anchor;
    double abs_target_prev;
    double window_rel_target;
    int window_steps;
    int improved = 0;

    if (!tab || !last_art_sum_io || !anchor_art_sum_io ||
        !progress_window_steps_io || !no_progress_streak_io) {
        return;
    }

    art_sum = phase1_artificial_abs_sum(tab);
    prev_art_sum = *last_art_sum_io;
    anchor_art_sum = *anchor_art_sum_io;
    window_steps = *progress_window_steps_io;

    if (!isfinite(prev_art_sum) || !isfinite(anchor_art_sum) || window_steps < 0) {
        *last_art_sum_io = art_sum;
        *anchor_art_sum_io = art_sum;
        *progress_window_steps_io = 0;
        return;
    }

    if (window_steps < INT_MAX) {
        window_steps++;
    }

    abs_improve_anchor = anchor_art_sum - art_sum;
    rel_improve_anchor = abs_improve_anchor / (1.0 + fabs(anchor_art_sum));
    abs_improve_prev = prev_art_sum - art_sum;
    rel_improve_prev = abs_improve_prev / (1.0 + fabs(prev_art_sum));
    abs_target_anchor = fmax(
        PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN,
        PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN * (1.0 + fabs(anchor_art_sum)));
    abs_target_prev = fmax(
        PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN,
        0.5 * PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN * (1.0 + fabs(prev_art_sum)));
    window_rel_target = 0.5 * PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN;

    if (abs_improve_anchor >= abs_target_anchor) {
        improved = 1;
    } else if (rel_improve_prev >= PHASE1_NO_PIVOT_PROGRESS_REL_IMPROVE_MIN &&
               abs_improve_prev >= abs_target_prev) {
        improved = 1;
    } else if (window_steps >= PHASE1_NO_PIVOT_PROGRESS_WINDOW &&
               rel_improve_anchor >= window_rel_target &&
               abs_improve_anchor >= PHASE1_NO_PIVOT_PROGRESS_ABS_IMPROVE_MIN) {
        improved = 1;
    }

    if (improved) {
        *no_progress_streak_io = 0;
        *anchor_art_sum_io = art_sum;
        window_steps = 0;
    } else {
        if (*no_progress_streak_io < INT_MAX) {
            (*no_progress_streak_io)++;
        }
        lp_telemetry_record_phase1_no_pivot_no_progress(solver);
        if (window_steps >= PHASE1_NO_PIVOT_PROGRESS_WINDOW) {
            *anchor_art_sum_io = art_sum;
            window_steps = 0;
        }
    }
    *last_art_sum_io = art_sum;
    *progress_window_steps_io = window_steps;
}

static int phase1_no_pivot_ladder_refactor_threshold(
    int m,
    int degenerate_count,
    LPPhase1NoPivotForceReason reason,
    int force_pivot_mode_active) {
    int threshold;
    switch (reason) {
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP:
            threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_DIR_SKIP;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL:
            threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_PIVOT_FAIL;
            break;
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN:
        case LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN:
        default:
            threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_BASE_RATIO;
            break;
    }

    if (m >= 2500) threshold += 4;
    else if (m >= 1200) threshold += 2;
    if (degenerate_count >= 160) threshold -= 2;
    else if (degenerate_count >= 80) threshold -= 1;
    if (force_pivot_mode_active && threshold > PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN) {
        threshold -= 2;
    }
    if (reason == LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL && threshold > 6) {
        threshold = 6;
    }
    if (threshold < PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN) {
        threshold = PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN;
    }
    return threshold;
}

static int phase1_no_pivot_ladder_step(
    int m,
    int degenerate_count,
    LPPhase1NoPivotForceReason reason,
    int no_pivot_streak,
    int no_progress_streak,
    int force_pivot_mode_active,
    int *refactor_threshold_out) {
    int refactor_threshold = phase1_no_pivot_ladder_refactor_threshold(
        m, degenerate_count, reason, force_pivot_mode_active);
    int rescue_start = PHASE1_NO_PIVOT_LADDER_RESCUE_START;
    int rescue_period = PHASE1_NO_PIVOT_LADDER_RESCUE_PERIOD;
    int streak_rescue_start = PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_START;
    int streak_rescue_period = PHASE1_NO_PIVOT_LADDER_STREAK_RESCUE_PERIOD;
    int fast_escalation = 0;

    if (m >= 1200 && degenerate_count >= 80 &&
        reason != LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL) {
        fast_escalation = 1;
        if (refactor_threshold > PHASE1_NO_PIVOT_LADDER_REFACTOR_MIN + 1) {
            refactor_threshold -= 1;
        }
        rescue_start = 2;
        rescue_period = 3;
        streak_rescue_start = 6;
        streak_rescue_period = 3;
    }
    if (reason == LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL) {
        rescue_start = 2;
        rescue_period = 2;
        streak_rescue_start = 4;
        streak_rescue_period = 2;
    }
    if (refactor_threshold_out) *refactor_threshold_out = refactor_threshold;

    if (no_progress_streak >= refactor_threshold ||
        no_pivot_streak >= refactor_threshold * 2 ||
        (fast_escalation &&
         no_progress_streak >= rescue_start + 2 &&
         no_pivot_streak >= refactor_threshold)) {
        return PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR;
    }

    if (no_progress_streak >= rescue_start &&
        rescue_period > 0 &&
        (no_progress_streak % rescue_period) == 0) {
        return PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE;
    }
    if (no_pivot_streak >= streak_rescue_start &&
        streak_rescue_period > 0 &&
        (no_pivot_streak % streak_rescue_period) == 0) {
        return PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE;
    }

    return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
}

static int phase1_no_pivot_ladder_apply_rescue_guard(
    SimplexSolver *solver,
    int ladder_step,
    int rescue_cooldown_iters,
    int rescue_fail_streak) {
    if (ladder_step != PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
        return ladder_step;
    }
    if (rescue_fail_streak >= PHASE1_NO_PIVOT_LADDER_RESCUE_FAIL_CAP) {
        lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(solver, 1);
        return PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR;
    }
    if (rescue_cooldown_iters > 0) {
        lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(solver, 0);
        return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
    }
    return PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE;
}

static int phase1_dir_skip_ladder_rescue_due(int dir_skip_event_streak) {
    if (dir_skip_event_streak < PHASE1_DIR_SKIP_LADDER_RESCUE_START) return 0;
    if (PHASE1_DIR_SKIP_LADDER_RESCUE_PERIOD <= 0) return 0;
    return (dir_skip_event_streak % PHASE1_DIR_SKIP_LADDER_RESCUE_PERIOD) == 0;
}

static int phase1_attempt_ladder_dual_rescue(
    SimplexSolver *solver,
    SimplexTableau *tab,
    int iter,
    LPPhase1NoPivotForceReason reason,
    LPPhase1RecomputeReason recompute_reason,
    int *rescue_cooldown_io,
    int *rescue_fail_streak_io,
    int *phase1_rc_only_streak_io,
    int *no_pivot_no_progress_streak_io,
    double *no_pivot_prev_art_sum_io,
    double *no_pivot_anchor_art_sum_io,
    int *no_pivot_progress_window_steps_io) {
    int rescue_status = dual_simplex_phase1_rescue(
        solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
    if (rescue_cooldown_io) {
        *rescue_cooldown_io = PHASE1_NO_PIVOT_LADDER_RESCUE_COOLDOWN_ITERS;
    }
    if (rescue_status == 0) {
        if (rescue_fail_streak_io) *rescue_fail_streak_io = 0;
        lp_telemetry_record_phase1_no_pivot_ladder_dual_rescue(
            solver, reason, 1);
        phase1_recompute_full_with_reason(
            solver,
            tab,
            phase1_rc_only_streak_io,
            recompute_reason);
        phase1_no_pivot_progress_reset(
            no_pivot_no_progress_streak_io,
            no_pivot_prev_art_sum_io,
            no_pivot_anchor_art_sum_io,
            no_pivot_progress_window_steps_io);
        return 1;
    }
    if (solver->status == RALPH_STATUS_TIME_LIMIT) {
        primal_remove_perturbation(tab);
        solver->iterations = iter;
        phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
        return -1;
    }
    if (rescue_fail_streak_io && *rescue_fail_streak_io < INT_MAX) {
        (*rescue_fail_streak_io)++;
    }
    lp_telemetry_record_phase1_no_pivot_ladder_dual_rescue(
        solver, reason, 0);
    return 0;
}

static int phase1_dir_skip_force_pivot_threshold(int m, int degenerate_count) {
    int threshold = PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_TRIGGER;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) threshold -= 16;
    if (m >= 1200) threshold -= 8;
    if (degenerate_count >= 80) threshold -= 8;
    if (degenerate_count >= 160) threshold -= 8;
    if (threshold < PHASE1_DIR_SKIP_FORCE_PIVOT_MIN_TRIGGER) {
        threshold = PHASE1_DIR_SKIP_FORCE_PIVOT_MIN_TRIGGER;
    }
    return threshold;
}

static int phase1_dir_skip_force_pivot_budget(int m, int degenerate_count) {
    int budget = PHASE1_DIR_SKIP_FORCE_PIVOT_BASE_BUDGET;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) budget += 4;
    if (m >= 1200) budget += 4;
    if (degenerate_count >= 160) budget += 8;
    if (budget > PHASE1_DIR_SKIP_FORCE_PIVOT_MAX_BUDGET) {
        budget = PHASE1_DIR_SKIP_FORCE_PIVOT_MAX_BUDGET;
    }
    return budget;
}

static int phase1_activate_force_pivot_mode(
    int m,
    int degenerate_count,
    int *dir_skip_event_streak_io,
    int *force_pivot_attempt_budget_io,
    int *force_pending_io,
    LPPhase1NoPivotForceReason *force_reason_io) {
    int streak = 0;
    int budget = 0;
    int threshold;

    if (dir_skip_event_streak_io) {
        streak = *dir_skip_event_streak_io;
    }
    if (force_pivot_attempt_budget_io) {
        budget = *force_pivot_attempt_budget_io;
    }
    if (budget > 0) return 0;

    threshold = phase1_dir_skip_force_pivot_threshold(m, degenerate_count);
    if (streak < threshold) return 0;

    budget = phase1_dir_skip_force_pivot_budget(m, degenerate_count);
    if (force_pivot_attempt_budget_io) {
        *force_pivot_attempt_budget_io = budget;
    }
    if (dir_skip_event_streak_io) {
        *dir_skip_event_streak_io = 0;
    }
    if (force_pending_io && force_reason_io) {
        *force_pending_io = 1;
        *force_reason_io = LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
    }
    return 1;
}

static int phase1_dir_escape_trigger_streak(int m, int degenerate_count) {
    int trigger = PHASE1_DIR_ESCAPE_BASE_TRIGGER;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) trigger -= 8;
    if (degenerate_count >= 80) trigger -= 8;
    if (degenerate_count >= 160) trigger -= 8;
    if (trigger < PHASE1_DIR_ESCAPE_MIN_TRIGGER) {
        trigger = PHASE1_DIR_ESCAPE_MIN_TRIGGER;
    }
    return trigger;
}

static int phase1_dir_escape_no_progress_trigger(int m, int degenerate_count) {
    int trigger = PHASE1_DIR_ESCAPE_BASE_NO_PROGRESS_TRIGGER;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) trigger -= 2;
    if (degenerate_count >= 80) trigger -= 2;
    if (degenerate_count >= 160) trigger -= 2;
    if (trigger < PHASE1_DIR_ESCAPE_MIN_NO_PROGRESS_TRIGGER) {
        trigger = PHASE1_DIR_ESCAPE_MIN_NO_PROGRESS_TRIGGER;
    }
    return trigger;
}

static int phase1_dir_escape_cooldown_updates(int m, int degenerate_count) {
    int cooldown = PHASE1_DIR_ESCAPE_BASE_COOLDOWN_UPDATES;
    if (m >= PHASE1_DEGEN_THRESHOLD_LARGE_M) cooldown += 16;
    if (degenerate_count >= 80) cooldown += 16;
    if (degenerate_count >= 160) cooldown += 32;
    if (cooldown > PHASE1_DIR_ESCAPE_MAX_COOLDOWN_UPDATES) {
        cooldown = PHASE1_DIR_ESCAPE_MAX_COOLDOWN_UPDATES;
    }
    return cooldown;
}

static int phase1_dir_stabilize_lu_health_hard(const LUFactorization *lu) {
    if (!lu) return 0;
    if (lu->max_updates > 0 && lu->num_updates >= lu->max_updates) return 1;
    {
        double growth_threshold = (lu->growth_refactor_threshold > 0.0)
            ? lu->growth_refactor_threshold
            : RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
        if (isfinite(lu->growth_factor) &&
            lu->growth_factor > growth_threshold * 3.0) {
            return 1;
        }
    }
    if (isfinite(lu->cond_estimate) && lu->cond_estimate > 1e10) return 1;
    if (lu->use_ft_updates && lu->spike_pool_capacity > 0) {
        if (lu->spike_pool_used >= (lu->spike_pool_capacity * 95) / 100) {
            return 1;
        }
    }
    return 0;
}

static int phase1_dir_stabilize_escape_gate_plan(int m,
                                                 int degenerate_count,
                                                 int dir_skip_event_streak,
                                                 int no_progress_streak,
                                                 int escape_cooldown,
                                                 int force_extreme_dir,
                                                 int force_lu_health,
                                                 int lu_hard_trigger,
                                                 int *next_escape_cooldown_out,
                                                 int *triggered_out,
                                                 int *hard_bypass_out) {
    int next_escape_cooldown = escape_cooldown;
    int triggered = 0;
    int hard_bypass = 0;
    int suppress = 0;
    int streak_trigger;
    int no_progress_trigger;
    int chronic_treadmill = 0;

    if (next_escape_cooldown < 0) next_escape_cooldown = 0;
    if (force_lu_health && !force_extreme_dir && m >= PHASE1_DIR_ESCAPE_MIN_M) {
        streak_trigger = phase1_dir_escape_trigger_streak(m, degenerate_count);
        no_progress_trigger = phase1_dir_escape_no_progress_trigger(m, degenerate_count);
        chronic_treadmill =
            ((dir_skip_event_streak >= streak_trigger &&
              no_progress_streak >= no_progress_trigger) ||
             (no_progress_streak >= no_progress_trigger * 3));
        if (lu_hard_trigger) {
            if (next_escape_cooldown > 0 ||
                chronic_treadmill) {
                hard_bypass = 1;
            }
        } else if (next_escape_cooldown > 0) {
            suppress = 1;
        } else if (chronic_treadmill) {
            next_escape_cooldown = phase1_dir_escape_cooldown_updates(
                m, degenerate_count);
            triggered = 1;
            suppress = 1;
        }
    }

    if (next_escape_cooldown_out) *next_escape_cooldown_out = next_escape_cooldown;
    if (triggered_out) *triggered_out = triggered;
    if (hard_bypass_out) *hard_bypass_out = hard_bypass;
    return suppress;
}

static int phase1_force_pivot_refactor_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    int force_pivot_mode_active,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    if (!force_pivot_mode_active) return 0;
    if (force_extreme_dir || force_lu_health || lu_hard_trigger) return 0;
    if (m < PHASE1_FORCE_PIVOT_RELAX_MIN_M) return 0;
    if (degenerate_count < PHASE1_FORCE_PIVOT_RELAX_DEGEN_TRIGGER) return 0;
    if (no_progress_streak > PHASE1_FORCE_PIVOT_RELAX_MAX_NO_PROGRESS) return 0;
    if (dual_rescue_fail_streak > 0) return 0;
    if (dual_rescue_attempts < PHASE1_FORCE_PIVOT_RELAX_RESCUE_MIN_ATTEMPTS) return 0;
    if (dual_rescue_successes < 0) return 0;
    if (dual_rescue_successes > dual_rescue_attempts) return 0;
    return (dual_rescue_successes * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_DEN) >=
           (dual_rescue_attempts * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_NUM);
}

static int phase1_force_extreme_refactor_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    if (!force_extreme_dir) return 0;
    if (force_lu_health || lu_hard_trigger) return 0;
    if (!(dir_inf_ratio > 0.0)) return 0;
    if (dir_inf_ratio > PHASE1_FORCE_EXTREME_RELAX_MAX_RATIO) return 0;
    if (m < PHASE1_FORCE_EXTREME_RELAX_MIN_M) return 0;
    if (degenerate_count < PHASE1_FORCE_EXTREME_RELAX_DEGEN_TRIGGER) return 0;
    if (no_progress_streak > PHASE1_FORCE_EXTREME_RELAX_MAX_NO_PROGRESS) return 0;
    if (dual_rescue_fail_streak > 0) return 0;
    if (dual_rescue_attempts < PHASE1_FORCE_PIVOT_RELAX_RESCUE_MIN_ATTEMPTS) return 0;
    if (dual_rescue_successes < 0) return 0;
    if (dual_rescue_successes > dual_rescue_attempts) return 0;
    return (dual_rescue_successes * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_DEN) >=
           (dual_rescue_attempts * PHASE1_FORCE_PIVOT_RELAX_RESCUE_SUCCESS_NUM);
}

static int phase1_note_no_pivot_and_maybe_force(SimplexSolver *solver,
                                                int m,
                                                int degenerate_count,
                                                LPPhase1NoPivotForceReason reason,
                                                int *streak_io,
                                                int *cooldown_io) {
    int threshold = 0;
    if (!streak_io || !cooldown_io) return 0;
    if (*streak_io < INT_MAX) (*streak_io)++;
    lp_telemetry_record_phase1_no_pivot_event(solver, reason);
    if (*cooldown_io > 0) return 0;
    if (m < PHASE1_NO_PIVOT_FORCE_MIN_M) return 0;
    threshold = phase1_no_pivot_force_threshold(m, degenerate_count);
    if (*streak_io < threshold) return 0;
    *streak_io = 0;
    *cooldown_io = PHASE1_NO_PIVOT_FORCE_COOLDOWN_UPDATES;
    lp_telemetry_record_phase1_no_pivot_force(solver, reason);
    return 1;
}

static int phase1_soft_lu_policy_cooldown_updates(int m,
                                                  int degenerate_count,
                                                  int periodic_interval) {
    int cooldown = PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_UPDATES;
    if (m < PHASE1_SOFT_LU_POLICY_COOLDOWN_MIN_M) return 0;
    if (degenerate_count < PHASE1_SOFT_LU_POLICY_COOLDOWN_DEGEN_TRIGGER) return 0;
    if (periodic_interval > 0) {
        int half = periodic_interval / 2;
        if (half > cooldown) cooldown = half;
    }
    if (cooldown > PHASE1_SOFT_LU_POLICY_COOLDOWN_MAX_UPDATES) {
        cooldown = PHASE1_SOFT_LU_POLICY_COOLDOWN_MAX_UPDATES;
    }
    return cooldown;
}

int simplex_phase1_no_pivot_force_plan_for_test(int m,
                                                 int degenerate_count,
                                                 int streak,
                                                 int cooldown,
                                                 int reason,
                                                 int *next_streak_out,
                                                 int *next_cooldown_out) {
    int should_force = phase1_note_no_pivot_and_maybe_force(NULL,
                                                             m,
                                                             degenerate_count,
                                                             (LPPhase1NoPivotForceReason)reason,
                                                             &streak,
                                                             &cooldown);
    if (next_streak_out) *next_streak_out = streak;
    if (next_cooldown_out) *next_cooldown_out = cooldown;
    return should_force;
}

int simplex_phase1_no_pivot_ladder_plan_for_test(int m,
                                                  int degenerate_count,
                                                  int reason,
                                                  int no_pivot_streak,
                                                  int no_progress_streak,
                                                  int force_pivot_mode_active,
                                                  int *refactor_threshold_out) {
    return phase1_no_pivot_ladder_step(
        m,
        degenerate_count,
        (LPPhase1NoPivotForceReason)reason,
        no_pivot_streak,
        no_progress_streak,
        force_pivot_mode_active,
        refactor_threshold_out);
}

int simplex_phase1_no_pivot_ladder_rescue_guard_plan_for_test(
    int ladder_step,
    int rescue_cooldown_iters,
    int rescue_fail_streak) {
    return phase1_no_pivot_ladder_apply_rescue_guard(NULL,
                                                     ladder_step,
                                                     rescue_cooldown_iters,
                                                     rescue_fail_streak);
}

int simplex_phase1_dir_skip_rescue_cadence_plan_for_test(
    int dir_skip_event_streak) {
    return phase1_dir_skip_ladder_rescue_due(dir_skip_event_streak);
}

int simplex_phase1_force_pivot_mode_plan_for_test(int m,
                                                   int degenerate_count,
                                                   int dir_skip_event_streak,
                                                   int active_budget,
                                                   int *next_streak_out,
                                                   int *next_budget_out) {
    int streak = dir_skip_event_streak;
    int budget = active_budget;
    int pending = 0;
    LPPhase1NoPivotForceReason force_reason =
        LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
    int activated = phase1_activate_force_pivot_mode(
        m,
        degenerate_count,
        &streak,
        &budget,
        &pending,
        &force_reason);
    if (next_streak_out) *next_streak_out = streak;
    if (next_budget_out) *next_budget_out = budget;
    return activated;
}

int simplex_phase1_dir_stabilize_escape_gate_plan_for_test(
    int m,
    int degenerate_count,
    int dir_skip_event_streak,
    int no_progress_streak,
    int escape_cooldown,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int *next_escape_cooldown_out,
    int *triggered_out,
    int *hard_bypass_out) {
    return phase1_dir_stabilize_escape_gate_plan(
        m,
        degenerate_count,
        dir_skip_event_streak,
        no_progress_streak,
        escape_cooldown,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        next_escape_cooldown_out,
        triggered_out,
        hard_bypass_out);
}

int simplex_phase1_force_pivot_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    int force_pivot_mode_active,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    return phase1_force_pivot_refactor_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        force_pivot_mode_active,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        dual_rescue_attempts,
        dual_rescue_successes,
        dual_rescue_fail_streak);
}

int simplex_phase1_force_extreme_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int dual_rescue_attempts,
    int dual_rescue_successes,
    int dual_rescue_fail_streak) {
    return phase1_force_extreme_refactor_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        dual_rescue_attempts,
        dual_rescue_successes,
        dual_rescue_fail_streak);
}

int simplex_phase1_soft_lu_policy_cooldown_plan_for_test(
    int m,
    int degenerate_count,
    int periodic_interval,
    int lu_soft_cost_deferred,
    int periodic_policy_cooldown,
    int *next_cooldown_out) {
    int next_cooldown = periodic_policy_cooldown;
    if (lu_soft_cost_deferred) {
        int soft_cooldown = phase1_soft_lu_policy_cooldown_updates(m,
                                                                    degenerate_count,
                                                                    periodic_interval);
        if (soft_cooldown > next_cooldown) {
            next_cooldown = soft_cooldown;
        }
    }
    if (next_cooldown_out) *next_cooldown_out = next_cooldown;
    return next_cooldown > periodic_policy_cooldown;
}

/* FNV-1a style mixer for deterministic trace signatures. */
static unsigned long long phase1_trace_mix(unsigned long long sig, unsigned long long word) {
    sig ^= word;
    sig *= 1099511628211ULL;
    return sig;
}

static void phase1_trace_record_no_entering(SimplexSolver *solver, int iter, int status_code) {
    if (!solver || !solver->trace_phase1) return;
    solver->trace_phase1_no_entering_events++;
    solver->trace_phase1_signature = phase1_trace_mix(
        solver->trace_phase1_signature,
        ((unsigned long long)0x2u << 60) ^
        ((unsigned long long)(iter & 0xFFFFF) << 20) ^
        (unsigned long long)(status_code & 0xFFFFF));

    LP_LOG_STDERR("[phase1_trace] event=no_entering iter=%d code=%d\n",
            iter, status_code);
}

static void phase1_trace_record_pivot_failure(SimplexSolver *solver,
                                              const SimplexTableau *tab,
                                              int iter,
                                              int repeat_count) {
    if (!solver || !tab || !solver->trace_phase1) return;

    int reason = tab->trace_last_fail_reason;
    solver->trace_phase1_pivot_failures++;
    if (solver->trace_phase1_first_fail_iter < 0) {
        solver->trace_phase1_first_fail_iter = iter;
    }
    solver->trace_phase1_last_fail_iter = iter;

    if (reason == PHASE1_PIVOT_FAIL_SMALL_PIVOT) {
        solver->trace_phase1_fail_small_pivot++;
    } else if (reason == PHASE1_PIVOT_FAIL_INVALID_COLUMN) {
        solver->trace_phase1_fail_invalid_column++;
    } else if (reason == PHASE1_PIVOT_FAIL_LU_MAX_UPDATES) {
        solver->trace_phase1_fail_lu_max_updates++;
    } else if (reason == PHASE1_PIVOT_FAIL_LU_SPIKE_POOL_FULL) {
        solver->trace_phase1_fail_lu_spike_pool_full++;
    } else if (reason == PHASE1_PIVOT_FAIL_LU_UPDATE_PIVOT_SMALL) {
        solver->trace_phase1_fail_lu_update_pivot_small++;
    } else if (reason == PHASE1_PIVOT_FAIL_LU_SINGULAR_UPDATE) {
        solver->trace_phase1_fail_lu_singular_update++;
    } else if (reason == PHASE1_PIVOT_FAIL_FACTOR_SINGULAR) {
        solver->trace_phase1_fail_factor_singular++;
    } else if (reason == PHASE1_PIVOT_FAIL_REFACTOR_FORCED_OTHER) {
        solver->trace_phase1_fail_refactor_forced_other++;
    } else if (reason == PHASE1_PIVOT_FAIL_REFACTOR_AFTER_UPDATE_OTHER) {
        solver->trace_phase1_fail_refactor_after_update_other++;
    }

    solver->trace_phase1_signature = phase1_trace_mix(
        solver->trace_phase1_signature,
        ((unsigned long long)0x1u << 60) ^
        ((unsigned long long)(iter & 0xFFFFF) << 40) ^
        ((unsigned long long)(tab->trace_last_entering & 0xFFFFF) << 20) ^
        (unsigned long long)(tab->trace_last_leaving_pos & 0xFFFFF));
    solver->trace_phase1_signature = phase1_trace_mix(
        solver->trace_phase1_signature,
        (unsigned long long)(reason & 0xFFFF));

    LP_LOG_STDERR("[phase1_trace] event=pivot_fail iter=%d repeat=%d entering=%d leaving=%d theta=%.12e reason=%s pivot=%.12e dir_inf=%.12e\n",
            iter,
            repeat_count,
            tab->trace_last_entering,
            tab->trace_last_leaving_pos,
            tab->trace_last_theta,
            phase1_pivot_fail_reason_str(reason),
            tab->trace_last_pivot,
            tab->trace_last_dir_inf);
}

static void phase1_trace_emit_summary(SimplexSolver *solver, RalphStatus phase1_status) {
    if (!solver || !solver->trace_phase1) return;

    LP_LOG_STDERR("[phase1_trace] summary status=%s piv_fail=%d small_pivot=%d invalid_col=%d lu_max_updates=%d lu_spike_pool_full=%d lu_update_pivot_small=%d lu_singular_update=%d factor_singular=%d refactor_forced_other=%d refactor_after_update_other=%d no_entering=%d first_iter=%d last_iter=%d sig=0x%016llx\n",
            ralph_lp_status_string((RalphLPStatus)phase1_status),
            solver->trace_phase1_pivot_failures,
            solver->trace_phase1_fail_small_pivot,
            solver->trace_phase1_fail_invalid_column,
            solver->trace_phase1_fail_lu_max_updates,
            solver->trace_phase1_fail_lu_spike_pool_full,
            solver->trace_phase1_fail_lu_update_pivot_small,
            solver->trace_phase1_fail_lu_singular_update,
            solver->trace_phase1_fail_factor_singular,
            solver->trace_phase1_fail_refactor_forced_other,
            solver->trace_phase1_fail_refactor_after_update_other,
            solver->trace_phase1_no_entering_events,
            solver->trace_phase1_first_fail_iter,
            solver->trace_phase1_last_fail_iter,
            solver->trace_phase1_signature);
}

static double vec_abs_max(const double *x, int n) {
    double max_abs = 0.0;
    if (!x || n <= 0) return max_abs;
    for (int i = 0; i < n; i++) {
        double absval = fabs(x[i]);
        if (absval > max_abs) {
            max_abs = absval;
        }
    }
    return max_abs;
}

/* ============================================================================
 * Geometric Mean Scaling
 * ============================================================================ */

/*
 * Apply geometric mean scaling to the LP model.
 * This scales rows and columns to improve numerical stability.
 *
 * Row scaling: R[i] such that scaled row has max element ~1
 * Col scaling: C[j] such that scaled col has max element ~1
 *
 * Scaled problem: (R*A*C) * (C^-1 * x) = R*b with objective (C*c)' * (C^-1 * x)
 */
static int apply_scaling(SimplexSolver *solver) {
    LPModel *model = solver->model;
    if (!model || !model->A) return -1;

    int m = model->num_cons;
    int n = model->num_vars;
    SparseMatrix *A = model->A;

    int geo_rounds = solver->scaling;    /* N geometric mean rounds */
    int eq_rounds = (geo_rounds > 1) ? 20 : 0;  /* equilibrium only for multi-round */

    /* Allocate scaling factors */
    solver->row_scale = (double*)calloc(m, sizeof(double));
    solver->col_scale = (double*)calloc(n, sizeof(double));
    if (!solver->row_scale || !solver->col_scale) {
        free(solver->row_scale);
        free(solver->col_scale);
        solver->row_scale = NULL;
        solver->col_scale = NULL;
        return -1;
    }

    /* Initialize scaling factors to 1 */
    for (int i = 0; i < m; i++) solver->row_scale[i] = 1.0;
    for (int j = 0; j < n; j++) solver->col_scale[j] = 1.0;

    /* Workspace for row/column extremes */
    double *row_max = (double*)calloc(m, sizeof(double));
    double *col_max = (double*)calloc(n, sizeof(double));
    if (!row_max || !col_max) {
        free(row_max);
        free(col_max);
        return -1;
    }

    /* Geometric mean scaling rounds: R[i] *= 1/sqrt(max), C[j] *= 1/sqrt(max)
     * Each round shrinks the ratio between largest and smallest elements.
     * Uses virtual scaling: A is not modified, just R[] and C[] accumulate. */
    for (int round = 0; round < geo_rounds; round++) {
        /* Compute row maxes of |R[i] * A[i,j] * C[j]| */
        for (int i = 0; i < m; i++) row_max[i] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > row_max[i]) row_max[i] = scaled;
            }
        }

        /* Update row factors */
        for (int i = 0; i < m; i++) {
            if (row_max[i] > RALPH_ZERO_TOL) {
                solver->row_scale[i] *= 1.0 / sqrt(row_max[i]);
            }
        }

        /* Compute col maxes of |R[i] * A[i,j] * C[j]| with updated R */
        for (int j = 0; j < n; j++) col_max[j] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > col_max[j]) col_max[j] = scaled;
            }
        }

        /* Update column factors */
        for (int j = 0; j < n; j++) {
            if (col_max[j] > RALPH_ZERO_TOL) {
                solver->col_scale[j] *= 1.0 / sqrt(col_max[j]);
            }
        }
    }

    /* Equilibrium scaling rounds: R[i] *= 1/max, C[j] *= 1/max
     * Drives every row and column max toward 1.0.
     * Converges quickly — typically 3-5 rounds sufficient. */
    for (int round = 0; round < eq_rounds; round++) {
        /* Compute row maxes */
        for (int i = 0; i < m; i++) row_max[i] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > row_max[i]) row_max[i] = scaled;
            }
        }

        /* Update row factors (equilibrium: scale max to 1.0) */
        for (int i = 0; i < m; i++) {
            if (row_max[i] > RALPH_ZERO_TOL) {
                solver->row_scale[i] *= 1.0 / row_max[i];
            }
        }

        /* Compute col maxes with updated R */
        double max_deviation = 0.0;
        for (int j = 0; j < n; j++) col_max[j] = 0.0;
        for (int j = 0; j < n; j++) {
            double cj = solver->col_scale[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                int i = A->rowidx[p];
                double scaled = fabs(A->values[p]) * solver->row_scale[i] * cj;
                if (scaled > col_max[j]) col_max[j] = scaled;
            }
        }

        /* Update column factors and check convergence */
        for (int j = 0; j < n; j++) {
            if (col_max[j] > RALPH_ZERO_TOL) {
                double dev = fabs(col_max[j] - 1.0);
                if (dev > max_deviation) max_deviation = dev;
                solver->col_scale[j] *= 1.0 / col_max[j];
            }
        }

        /* Converged: all column maxes within 10% of 1.0 */
        if (max_deviation < 0.1) break;
    }

    free(row_max);
    free(col_max);

    /* Apply accumulated scaling to matrix A: A_scaled[i,j] = R[i] * A[i,j] * C[j] */
    for (int j = 0; j < n; j++) {
        double cj = solver->col_scale[j];
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int i = A->rowidx[p];
            A->values[p] *= solver->row_scale[i] * cj;
        }
    }

    /* Scale RHS: b_scaled[i] = R[i] * b[i] */
    for (int i = 0; i < m; i++) {
        model->b[i] *= solver->row_scale[i];
    }

    /* Scale objective: c_scaled[j] = C[j] * c[j] */
    for (int j = 0; j < n; j++) {
        model->c[j] *= solver->col_scale[j];
    }

    /* Scale variable bounds: x = C * x_scaled, so lb/C <= x_scaled <= ub/C */
    for (int j = 0; j < n; j++) {
        if (model->lb[j] > -RALPH_INFINITY/2) {
            model->lb[j] /= solver->col_scale[j];
        }
        if (model->ub[j] < RALPH_INFINITY/2) {
            model->ub[j] /= solver->col_scale[j];
        }
    }

    solver->is_scaled = 1;
    return 0;
}

/*
 * Post-solve verification (T2.3 + T3.6).
 * Read-only: inspects solution, dual, reduced costs, model A/b/c.
 * Checks:
 *   1. Primal feasibility:  ||Ax - b||_inf for constraint satisfaction
 *   2. Bound feasibility:   lb <= x <= ub for all vars
 *   3. Dual feasibility:    rc[j] >= -tol for nonbasics at lower bound
 *   4. Complementary slackness: |x_j - lb_j| * |rc_j| ~ 0
 *   5. Objective accuracy:  Kahan summation recomputation
 *   6. Basis conditioning:  LU condition estimate (T3.6)
 * Downgrades OPTIMAL → IMPRECISE if any check exceeds threshold.
 */
static void verify_solution(SimplexSolver *solver) {
    LPModel *model = solver->model;
    int m = model->num_cons;
    int n = model->num_vars;
    double *x = solver->solution;
    double *rc = solver->reduced_costs;
    SparseMatrix *A = model->A;

    if (!x || !A || !model->b || !model->lb || !model->ub) return;

    /* W2: Use runtime tolerances from model (default to compile-time constants) */
    double feas_tol = model->feas_tol;
    double opt_tol = model->opt_tol;

    double max_primal_infeas = 0.0;
    double max_bound_infeas = 0.0;
    double max_dual_infeas = 0.0;
    double max_comp_slack = 0.0;

    /* 1. Primal feasibility: compute Ax via column-wise sparse matvec (O(nnz)),
     * then check against b with sense.
     * (B6 fix: was O(n*m) triple-nested loop, now O(nnz)) */
    double *ax = (double*)calloc(m, sizeof(double));
    if (ax) {
        for (int j = 0; j < n; j++) {
            double xj = x[j];
            if (fabs(xj) < 1e-15) continue;
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                ax[A->rowidx[p]] += A->values[p] * xj;
            }
        }
        for (int i = 0; i < m; i++) {
            double violation = 0.0;
            char sense = model->sense[i];
            if (sense == 'L') {
                violation = ax[i] - model->b[i];
                if (violation < 0.0) violation = 0.0;
            } else if (sense == 'G') {
                violation = model->b[i] - ax[i];
                if (violation < 0.0) violation = 0.0;
            } else {
                violation = fabs(ax[i] - model->b[i]);
            }
            if (violation > max_primal_infeas) max_primal_infeas = violation;
        }
        free(ax);
    }

    /* 2. Bound feasibility */
    for (int j = 0; j < n; j++) {
        double lb_viol = model->lb[j] - x[j];
        if (lb_viol > max_bound_infeas) max_bound_infeas = lb_viol;
        double ub_viol = x[j] - model->ub[j];
        if (ub_viol > max_bound_infeas) max_bound_infeas = ub_viol;
    }

    /* 3. Dual feasibility: for minimization, nonbasics at lb should have rc >= 0,
     * at ub should have rc <= 0. Solution is in original space (obj_sense applied). */
    if (rc) {
        for (int j = 0; j < n; j++) {
            double xj = x[j];
            double rcj = rc[j];  /* Already in user space (obj_sense applied) */
            int at_lb = fabs(xj - model->lb[j]) < feas_tol;
            int at_ub = fabs(xj - model->ub[j]) < feas_tol;

            /* In user space: minimize → rc >= 0 at lb, rc <= 0 at ub
             *                maximize → rc <= 0 at lb, rc >= 0 at ub
             * Equivalently: obj_sense * rc >= 0 at lb (in internal space) */
            double viol = 0.0;
            if (at_lb && !at_ub) {
                /* At lower bound: rc should push away from lb.
                 * For min: rc >= 0. For max: rc <= 0. */
                viol = (model->obj_sense == 1) ? -rcj : rcj;
            } else if (at_ub && !at_lb) {
                /* At upper bound: rc should push away from ub. */
                viol = (model->obj_sense == 1) ? rcj : -rcj;
            }
            if (viol > max_dual_infeas) max_dual_infeas = viol;
        }
    }

    /* 4. Complementary slackness: for non-fixed vars, |x - lb| * |rc| should be ~ 0 */
    if (rc) {
        for (int j = 0; j < n; j++) {
            if (fabs(model->ub[j] - model->lb[j]) < feas_tol) continue;
            double dist_lb = fabs(x[j] - model->lb[j]);
            double dist_ub = fabs(x[j] - model->ub[j]);
            double min_dist = (dist_lb < dist_ub) ? dist_lb : dist_ub;
            double cs = min_dist * fabs(rc[j]);
            if (cs > max_comp_slack) max_comp_slack = cs;
        }
    }

    /* 5. Objective accuracy with Kahan summation */
    double obj_kahan = 0.0;
    double kahan_comp = 0.0;
    for (int j = 0; j < n; j++) {
        double term = model->c[j] * x[j] - kahan_comp;
        double temp = obj_kahan + term;
        kahan_comp = (temp - obj_kahan) - term;
        obj_kahan = temp;
    }
    obj_kahan = obj_kahan + model->obj_offset;
    double obj_denom = fabs(solver->obj_value) > 1.0 ? fabs(solver->obj_value) : 1.0;
    double obj_rel_error = fabs(obj_kahan - solver->obj_value) / obj_denom;

    /* 6. Basis conditioning (T3.6) */
    double cond = 1.0;
    if (solver->tableau && solver->tableau->lu) {
        cond = solver->tableau->lu->cond_estimate;
    }

    /* Store metrics */
    solver->verify_primal_infeas = max_primal_infeas;
    solver->verify_bound_infeas = max_bound_infeas;
    solver->verify_dual_infeas = max_dual_infeas;
    solver->verify_comp_slack = max_comp_slack;
    solver->verify_obj_error = obj_rel_error;
    solver->verify_cond_estimate = cond;

    /* W4: Dual iterative refinement — if dual infeasibility exceeds threshold,
     * recompute reduced costs from dual variables: rc[j] = c[j] - A^T y[j].
     * This catches RC drift from accumulated LU update errors without
     * requiring refactorization. Only fires when verification detects a problem. */
    if (max_dual_infeas > opt_tol && rc && solver->dual_solution && A) {
        /* Recompute reduced costs from duals in user space */
        for (int j = 0; j < n; j++) {
            double rc_refined = model->c[j];
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                rc_refined -= A->values[p] * solver->dual_solution[A->rowidx[p]];
            }
            solver->reduced_costs[j] = rc_refined;
        }

        /* Re-check dual feasibility with refined reduced costs */
        max_dual_infeas = 0.0;
        max_comp_slack = 0.0;
        for (int j = 0; j < n; j++) {
            double xj = x[j];
            double rcj = solver->reduced_costs[j];
            int at_lb = fabs(xj - model->lb[j]) < feas_tol;
            int at_ub = fabs(xj - model->ub[j]) < feas_tol;

            double viol = 0.0;
            if (at_lb && !at_ub) {
                viol = (model->obj_sense == 1) ? -rcj : rcj;
            } else if (at_ub && !at_lb) {
                viol = (model->obj_sense == 1) ? rcj : -rcj;
            }
            if (viol > max_dual_infeas) max_dual_infeas = viol;

            if (fabs(model->ub[j] - model->lb[j]) >= feas_tol) {
                double dist_lb = fabs(xj - model->lb[j]);
                double dist_ub = fabs(xj - model->ub[j]);
                double min_dist = (dist_lb < dist_ub) ? dist_lb : dist_ub;
                double cs = min_dist * fabs(rcj);
                if (cs > max_comp_slack) max_comp_slack = cs;
            }
        }

        /* Update stored metrics */
        solver->verify_dual_infeas = max_dual_infeas;
        solver->verify_comp_slack = max_comp_slack;
    }

    /* Downgrade to IMPRECISE if any metric exceeds threshold (W2: runtime tols) */
    int imprecise = 0;
    if (max_primal_infeas > feas_tol) imprecise = 1;
    if (max_bound_infeas > feas_tol) imprecise = 1;
    if (max_dual_infeas > opt_tol) imprecise = 1;
    if (obj_rel_error > opt_tol) imprecise = 1;

    if (imprecise) {
        solver->status = RALPH_STATUS_IMPRECISE;
        if (solver->verbose) {
            LP_LOG_STDOUT("[verify] IMPRECISE: primal=%.2e bound=%.2e dual=%.2e cs=%.2e obj=%.2e cond=%.2e\n",
                   max_primal_infeas, max_bound_infeas, max_dual_infeas,
                   max_comp_slack, obj_rel_error, cond);
        }
    } else if (solver->verbose) {
        LP_LOG_STDOUT("[verify] OK: primal=%.2e bound=%.2e dual=%.2e cs=%.2e obj=%.2e cond=%.2e\n",
               max_primal_infeas, max_bound_infeas, max_dual_infeas,
               max_comp_slack, obj_rel_error, cond);
    }
}

/*
 * Unscale the solution after solving.
 * x_original = C * x_scaled
 * y_original = R * y_scaled
 * rc_original = C^-1 * rc_scaled
 */
static void unscale_solution(SimplexSolver *solver) {
    if (!solver->is_scaled) return;

    int n = solver->model->num_vars;
    int m = solver->model->num_cons;

    /* Unscale primal solution: x = C * x_scaled */
    if (solver->solution) {
        for (int j = 0; j < n; j++) {
            solver->solution[j] *= solver->col_scale[j];
        }
    }

    /* Unscale dual solution: y = R * y_scaled */
    if (solver->dual_solution) {
        for (int i = 0; i < m; i++) {
            solver->dual_solution[i] *= solver->row_scale[i];
        }
    }

    /* Unscale reduced costs: rc = rc_scaled / C */
    if (solver->reduced_costs) {
        for (int j = 0; j < n; j++) {
            solver->reduced_costs[j] /= solver->col_scale[j];
        }
    }
}

/*
 * Restore the model to its original (unscaled) state.
 * This reverses the transformations applied by apply_scaling().
 */
static void restore_model(SimplexSolver *solver) {
    if (!solver->is_scaled) return;

    LPModel *model = solver->model;
    if (!model || !model->A) return;

    int m = model->num_cons;
    int n = model->num_vars;
    SparseMatrix *A = model->A;

    /* Unscale matrix A: A_original = A_scaled / (R[i] * C[j]) */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int i = A->rowidx[p];
            A->values[p] /= (solver->row_scale[i] * solver->col_scale[j]);
        }
    }

    /* Unscale RHS: b_original = b_scaled / R[i] */
    for (int i = 0; i < m; i++) {
        model->b[i] /= solver->row_scale[i];
    }

    /* Unscale objective: c_original = c_scaled / C[j] */
    for (int j = 0; j < n; j++) {
        model->c[j] /= solver->col_scale[j];
    }

    /* Unscale variable bounds: lb/ub_original = lb/ub_scaled * C[j] */
    for (int j = 0; j < n; j++) {
        if (model->lb[j] > -RALPH_INFINITY/2) {
            model->lb[j] *= solver->col_scale[j];
        }
        if (model->ub[j] < RALPH_INFINITY/2) {
            model->ub[j] *= solver->col_scale[j];
        }
    }

    solver->is_scaled = 0;
}

/* ============================================================================
 * Simplex Tableau Creation
 * ============================================================================ */

/* Allocate all tableau arrays using arena allocator.
 * Returns 0 on success, -1 on failure.
 * Caller is responsible for calling tableau_free on failure. */
static int tableau_alloc_arrays(SimplexTableau *tab, int num_aux_vars, int num_artificial) {
    int n = tab->n;
    int m = tab->m;

    /* Calculate total memory needed for arena (with 8-byte alignment padding).
     * Each allocation rounds up to 8 bytes, so add ~7 bytes padding per alloc.
     * We have 24 arrays, so add 24*8 = 192 bytes padding margin. */
    size_t arena_size =
        /* double arrays: c_ext, lb_ext, ub_ext (n each) */
        3 * (size_t)n * sizeof(double) +
        /* double arrays: x, rc, se_weights, work3 (n each) */
        4 * (size_t)n * sizeof(double) +
        /* double arrays: y, work1, work2, work4, rhs, row_sign, pivot_row, tau_work (m each) */
        8 * (size_t)m * sizeof(double) +
        /* double arrays: cb_sparse_val, aux_coef */
        (size_t)m * sizeof(double) + (size_t)num_aux_vars * sizeof(double) +
        /* double array: c_original for two-phase (n) */
        (size_t)n * sizeof(double) +
        /* int arrays: basis, basis_pos (m and n), basis cache cols/nnz (m each) */
        (size_t)m * sizeof(int) + (size_t)n * sizeof(int) +
        2 * (size_t)m * sizeof(int) +
        /* int arrays: nonbasis, var_status (n-m and n) */
        (size_t)(n - m) * sizeof(int) + (size_t)n * sizeof(VarStatus) +
        /* int arrays: cb_sparse_idx, aux_row, partial_candidates, dual_candidates */
        (size_t)m * sizeof(int) + (size_t)num_aux_vars * sizeof(int) + 100 * sizeof(int) + 200 * sizeof(int) +
        /* int array: artificial_vars for two-phase */
        (size_t)num_artificial * sizeof(int) +
        /* int array: redundant_rows for two-phase (m) */
        (size_t)m * sizeof(int) +
        /* double array: dse_weights for dual steepest edge (m) */
        (size_t)m * sizeof(double) +
        /* int array: flip_list for bound flipping (n) */
        (size_t)n * sizeof(int) +
        /* int arrays: heap, heap_pos for heap pricing (n each) */
        2 * (size_t)n * sizeof(int) +
        /* dual pivot backup: x(n dbl), rc(n dbl), basis(m int), basis_pos(n int), status(n VarStatus) */
        2 * (size_t)n * sizeof(double) + (size_t)m * sizeof(int) + (size_t)n * sizeof(int) + (size_t)n * sizeof(VarStatus) +
        /* Alignment padding (37 allocations * 8 bytes) */
        296;

    /* Create arena */
    tab->arena = sh_arena_create(arena_size);
    if (!tab->arena) {
        return -1;
    }

    /* Allocate all arrays from arena (calloc zeros memory) */
    tab->c_ext = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->lb_ext = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->ub_ext = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));

    tab->basis = (int*)sh_arena_alloc(tab->arena, m * sizeof(int));
    tab->nonbasis = (int*)sh_arena_alloc(tab->arena, (n - m) * sizeof(int));
    tab->var_status = (VarStatus*)sh_arena_alloc(tab->arena, n * sizeof(VarStatus));
    tab->basis_pos = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    tab->basis_col_cache = (int*)sh_arena_alloc(tab->arena, m * sizeof(int));
    tab->basis_col_nnz_cache = (int*)sh_arena_alloc(tab->arena, m * sizeof(int));

    tab->x = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->y = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->rc = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));

    tab->work1 = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->work2 = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->work3 = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->work4 = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->rhs = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->row_sign = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->pivot_row = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->tau_work = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));

    tab->se_weights = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));

    /* Pre-allocated sparse workspace for reduced cost computation */
    tab->cb_sparse_idx = (int*)sh_arena_alloc(tab->arena, m * sizeof(int));
    tab->cb_sparse_val = (double*)sh_arena_alloc(tab->arena, m * sizeof(double));

    /* Auxiliary variable mapping for cut generation */
    tab->aux_row = (int*)sh_arena_alloc(tab->arena, num_aux_vars * sizeof(int));
    tab->aux_coef = (double*)sh_arena_alloc(tab->arena, num_aux_vars * sizeof(double));

    /* Partial pricing candidate list (hot set) */
    tab->partial_cand_capacity = 100;
    tab->partial_candidates = (int*)sh_arena_alloc(tab->arena, tab->partial_cand_capacity * sizeof(int));
    tab->partial_cand_count = 0;

    /* Dual candidate list for ratio test (T2.2) */
    tab->dual_cand_capacity = 200;
    tab->dual_candidates = (int*)sh_arena_alloc(tab->arena, 200 * sizeof(int));
    tab->dual_cand_count = 0;
    tab->dual_cand_valid = 0;

    /* Two-phase simplex arrays */
    tab->c_original = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->num_artificial = num_artificial;
    if (num_artificial > 0) {
        tab->artificial_vars = (int*)sh_arena_alloc(tab->arena, num_artificial * sizeof(int));
    } else {
        tab->artificial_vars = NULL;
    }

    /* Redundant row tracking (for handling singular basis from stuck artificials) */
    tab->redundant_rows = (int*)sh_arena_calloc(tab->arena, m, sizeof(int));
    tab->num_redundant = 0;
    tab->redundant_rows_zeroed = 0;

    /* Dual steepest edge weights (P6) */
    tab->dse_weights = (double*)sh_arena_calloc(tab->arena, m, sizeof(double));
    tab->dse_initialized = 0;

    /* Bound flipping scratch (P5) */
    tab->flip_list = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    tab->flip_count = 0;

    /* Heap pricing (T2.2) */
    tab->heap = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    tab->heap_pos = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    tab->heap_size = 0;
    if (tab->heap_pos) memset(tab->heap_pos, -1, n * sizeof(int));

    /* Pre-allocated backup arrays for dual_simplex_pivot rollback (B2 fix) */
    tab->dual_x_backup = (double*)sh_arena_alloc(tab->arena, n * sizeof(double));
    tab->dual_rc_backup = (double*)sh_arena_alloc(tab->arena, n * sizeof(double));
    tab->dual_basis_backup = (int*)sh_arena_alloc(tab->arena, m * sizeof(int));
    tab->dual_basis_pos_backup = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    tab->dual_status_backup = (VarStatus*)sh_arena_alloc(tab->arena, n * sizeof(VarStatus));

    /* Single check for all allocations */
    if (!tab->c_ext || !tab->lb_ext || !tab->ub_ext ||
        !tab->basis || !tab->nonbasis || !tab->var_status || !tab->basis_pos ||
        !tab->basis_col_cache || !tab->basis_col_nnz_cache ||
        !tab->x || !tab->y || !tab->rc ||
        !tab->work1 || !tab->work2 || !tab->work3 || !tab->work4 || !tab->rhs || !tab->row_sign ||
        !tab->pivot_row || !tab->tau_work || !tab->se_weights ||
        !tab->cb_sparse_idx || !tab->cb_sparse_val ||
        !tab->aux_row || !tab->aux_coef || !tab->partial_candidates || !tab->dual_candidates ||
        !tab->c_original || (num_artificial > 0 && !tab->artificial_vars) ||
        !tab->redundant_rows || !tab->dse_weights || !tab->flip_list ||
        !tab->heap || !tab->heap_pos ||
        !tab->dual_x_backup || !tab->dual_rc_backup ||
        !tab->dual_basis_backup || !tab->dual_basis_pos_backup || !tab->dual_status_backup) {
        return -1;
    }
    return 0;
}

/* Initialize steepest edge / Devex weights.
 * For initial basis (typically slack identity), B^{-1} = I, so:
 *   gamma_j = ||B^{-1} * a_j||^2 = ||a_j||^2 */
static void tableau_init_weights(SimplexTableau *tab) {
    for (int j = 0; j < tab->n; j++) {
        double col_norm_sq = 0.0;
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            col_norm_sq += tab->A_ext->values[p] * tab->A_ext->values[p];
        }
        tab->se_weights[j] = (col_norm_sq > 1.0) ? col_norm_sq : 1.0;
    }
    tab->use_steepest_edge = 1;
    tab->pricing_strategy = 2;  /* Default to Devex */
    tab->devex_refcount = 0;

    /* Initialize lazy reduced cost computation flags */
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
}

/* Internal: create tableau with explicit two-phase control.
 * dual_mode: if set, creates auxiliaries without artificials:
 *   <= : slack (+1, cost 0, [0,inf))
 *   >= : surplus (-1, cost 0, [0,inf))  — no artificial
 *   =  : fixed slack (+1, cost 0, [0,0]) — dual drives to zero */
static SimplexTableau* tableau_create_ex(LPModel *model, int force_two_phase, int dual_mode) {
    if (!model) return NULL;

    /* Finalize model if not done */
    if (!model->A) {
        if (lp_model_finalize(model) != 0) return NULL;
    }

    SimplexTableau *tab = (SimplexTableau*)calloc(1, sizeof(SimplexTableau));
    if (!tab) return NULL;

    tab->model = model;
    tab->m = model->num_cons;

    /* Normalize constraint senses and RHS signs for each row:
     * - If RHS < 0, multiply entire row by -1 and flip sense (L<->G, E stays E)
     * Then count variables needed:
     * - <= : 1 slack (basic)
     * - >= : 1 surplus + 1 artificial (artificial basic)
     * - =  : 1 artificial (artificial basic)
     */
    char *norm_sense = (char*)calloc(model->num_cons, sizeof(char));
    double *norm_sign = (double*)calloc(model->num_cons, sizeof(double));
    if (!norm_sense || !norm_sign) {
        free(norm_sense);
        free(norm_sign);
        tableau_free(tab);
        return NULL;
    }

    int num_aux_vars = 0;  /* Count slack + surplus + artificial */
    int num_artificial = 0;  /* Count artificial variables only */
    int num_equalities = 0;  /* Count equality constraints */
    for (int i = 0; i < model->num_cons; i++) {
        /* Normalize so RHS >= 0 */
        if (model->b[i] < 0) {
            norm_sign[i] = -1.0;
            if (model->sense[i] == 'L') {
                norm_sense[i] = 'G';  /* <= with negative RHS becomes >= */
            } else if (model->sense[i] == 'G') {
                norm_sense[i] = 'L';  /* >= with negative RHS becomes <= */
            } else {
                norm_sense[i] = 'E';  /* = stays = */
            }
        } else {
            norm_sign[i] = 1.0;
            norm_sense[i] = model->sense[i];
        }

        /* Count auxiliary variables needed */
        if (dual_mode) {
            /* Dual mode: one auxiliary per constraint, no artificials */
            num_aux_vars += 1;
        } else if (norm_sense[i] == 'L') {
            num_aux_vars += 1;  /* slack only */
        } else if (norm_sense[i] == 'G') {
            num_aux_vars += 2;  /* surplus + artificial */
            num_artificial += 1;  /* artificial for >= */
        } else {
            num_aux_vars += 1;  /* artificial only */
            num_artificial += 1;  /* artificial for = */
            num_equalities += 1;
        }
    }

    tab->n = model->num_vars + num_aux_vars;
    tab->num_aux = num_aux_vars;
    tab->num_equalities = num_equalities;

    /* Use two-phase simplex for ALL problems with artificial variables.
     * Phase 1 minimizes sum of artificials (cost=1.0) to find a feasible basis.
     * Phase 2 optimizes the original objective.
     * This eliminates Big-M method entirely, avoiding objective contamination
     * for problems where M isn't large enough relative to optimal coefficients.
     */
    int use_two_phase = !dual_mode && (force_two_phase || num_artificial > 0);
    tab->use_two_phase = use_two_phase;

    /* Allocate all tableau arrays */
    if (tableau_alloc_arrays(tab, num_aux_vars, num_artificial) != 0) {
        free(norm_sense);
        free(norm_sign);
        tableau_free(tab);
        return NULL;
    }

    /* Copy structural variable data */
    for (int j = 0; j < model->num_vars; j++) {
        double orig_cost = model->c[j] * model->obj_sense;  /* Convert to minimization */
        tab->c_original[j] = orig_cost;
        if (use_two_phase) {
            /* Phase 1 objective: structural variables have zero cost */
            tab->c_ext[j] = 0.0;
        } else {
            tab->c_ext[j] = orig_cost;
        }
        tab->lb_ext[j] = model->lb[j];
        tab->ub_ext[j] = model->ub[j];
    }

    /* Build extended constraint matrix with slacks/surplus/artificial */
    SparseTriplets *trips = triplets_create(tab->m, tab->n,
                                            model->A->nnz + num_aux_vars);
    if (!trips) {
        free(norm_sense);
        free(norm_sign);
        tableau_free(tab);
        return NULL;
    }

    /* Copy original matrix (with row sign normalization) */
    for (int j = 0; j < model->num_vars; j++) {
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int row = model->A->rowidx[p];
            double val = model->A->values[p] * norm_sign[row];
            triplets_add(trips, row, j, val);
        }
    }

    /* Track which auxiliary variable is basic for each row */
    int *basic_var_for_row = (int*)calloc(model->num_cons, sizeof(int));
    if (!basic_var_for_row) {
        free(norm_sense);
        free(norm_sign);
        triplets_free(trips);
        tableau_free(tab);
        return NULL;
    }

    /* Compute initial Ax values (x at lower bounds) for each row to decide
     * whether surplus or artificial should be basic for >= constraints.
     * Not needed in dual_mode (no artificials to choose between). */
    double *ax_initial = NULL;
    if (!dual_mode) {
        ax_initial = (double*)calloc(model->num_cons, sizeof(double));
        if (!ax_initial) {
            free(norm_sense);
            free(norm_sign);
            triplets_free(trips);
            tableau_free(tab);
            return NULL;
        }
        for (int j = 0; j < model->num_vars; j++) {
            for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
                int row = model->A->rowidx[p];
                double val = model->A->values[p] * norm_sign[row];
                ax_initial[row] += val * model->lb[j];
            }
        }
    }

    /* Add auxiliary variables and record their mapping to constraints.
     * Artificial variable costs are 1.0 (Phase 1 objective).
     * For dual_mode: no artificials — one aux per constraint with zero cost. */
    double artificial_cost = 1.0;
    int aux_idx = model->num_vars;
    int aux_map_idx = 0;  /* Index into aux_row/aux_coef arrays */
    int art_idx = 0;  /* Index into artificial_vars array */
    for (int i = 0; i < model->num_cons; i++) {
        if (dual_mode) {
            /* Dual mode: one auxiliary per constraint, no artificials.
             * <= : slack (+1, [0,inf))
             * >= : surplus (-1, [0,inf))
             * =  : fixed slack (+1, [0,0]) — dual simplex drives to zero */
            double coef = (norm_sense[i] == 'G') ? -1.0 : 1.0;
            triplets_add(trips, i, aux_idx, coef);
            tab->c_ext[aux_idx] = 0.0;
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = (norm_sense[i] == 'E') ? 0.0 : RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;

            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = coef;
            aux_map_idx++;
            aux_idx++;
        } else if (norm_sense[i] == 'L') {
            /* <= : add slack with coef +1, slack is basic */
            triplets_add(trips, i, aux_idx, 1.0);
            tab->c_ext[aux_idx] = 0.0;
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;

            /* Record mapping: slack for row i with coefficient +1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = 1.0;
            aux_map_idx++;

            aux_idx++;
        } else if (norm_sense[i] == 'G') {
            /* >= : add surplus with coef -1, then artificial with coef +1 */
            double rhs = fabs(model->b[i]);

            /* Check if constraint is already satisfied at initial point (x at lb) */
            int surplus_idx = aux_idx;
            int artificial_idx = aux_idx + 1;

            /* Surplus variable */
            triplets_add(trips, i, surplus_idx, -1.0);
            tab->c_ext[surplus_idx] = 0.0;
            tab->lb_ext[surplus_idx] = 0.0;
            tab->ub_ext[surplus_idx] = RALPH_INFINITY;

            /* Record mapping: surplus for row i with coefficient -1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = -1.0;
            aux_map_idx++;

            /* Artificial variable */
            triplets_add(trips, i, artificial_idx, 1.0);
            tab->c_ext[artificial_idx] = artificial_cost;
            tab->lb_ext[artificial_idx] = 0.0;
            tab->ub_ext[artificial_idx] = RALPH_INFINITY;
            tab->artificial_vars[art_idx++] = artificial_idx;

            /* Record mapping: artificial for row i with coefficient +1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = 1.0;
            aux_map_idx++;

            /* Decide which is basic: if ax_initial >= rhs, surplus can be basic.
             * Otherwise we need the artificial variable. */
            if (ax_initial[i] >= rhs - RALPH_FEAS_TOL) {
                /* Constraint already satisfied, surplus is basic */
                basic_var_for_row[i] = surplus_idx;
            } else {
                /* Need artificial variable to be basic */
                basic_var_for_row[i] = artificial_idx;
            }

            aux_idx += 2;
        } else {
            /* = : add artificial with coef +1 (basic) */
            triplets_add(trips, i, aux_idx, 1.0);
            tab->c_ext[aux_idx] = artificial_cost;
            tab->lb_ext[aux_idx] = 0.0;
            tab->ub_ext[aux_idx] = RALPH_INFINITY;
            basic_var_for_row[i] = aux_idx;
            tab->artificial_vars[art_idx++] = aux_idx;

            /* Record mapping: artificial for row i with coefficient +1 */
            tab->aux_row[aux_map_idx] = i;
            tab->aux_coef[aux_map_idx] = 1.0;
            aux_map_idx++;

            aux_idx++;
        }
    }
    free(ax_initial);

    /* Store original costs for auxiliary variables (for Phase 2 transition).
     * Slack/surplus have zero cost, artificials have cost 1.0 (Phase 1 objective). */
    for (int j = model->num_vars; j < tab->n; j++) {
        /* Check if this is an artificial variable */
        int is_artificial = 0;
        for (int k = 0; k < tab->num_artificial; k++) {
            if (tab->artificial_vars[k] == j) {
                is_artificial = 1;
                break;
            }
        }
        if (is_artificial) {
            /* Artificial variables should have zero cost in Phase 2
             * (they should be driven to zero and removed from basis) */
            tab->c_original[j] = 0.0;
        } else {
            /* Slack/surplus variables have zero cost */
            tab->c_original[j] = 0.0;
        }
    }

    /* Initialize phase */
    tab->phase = use_two_phase ? 1 : 2;
    tab->perturb_scale = 1.0;
    tab->solution_last_residual_iter = -1;
    tab->solution_last_residual_factorize_calls = -1;
    tab->solution_last_residual_num_updates = -1;

    tab->A_ext = triplets_to_csc(trips);
    triplets_free(trips);

    if (!tab->A_ext) {
        free(norm_sense);
        free(norm_sign);
        free(basic_var_for_row);
        tableau_free(tab);
        return NULL;
    }

    /* Build CSR (row-form) transpose of A_ext for row-scatter RC update.
     * CSC→CSR transpose: count row nnz, prefix-sum, scatter entries. O(nnz). */
    {
        int csr_m = tab->m, csr_n = tab->n;
        int csr_nnz = tab->A_ext->colptr[csr_n];
        tab->csr_rowptr = (int*)calloc(csr_m + 1, sizeof(int));
        tab->csr_colidx = (int*)malloc(csr_nnz * sizeof(int));
        tab->csr_values = (double*)malloc(csr_nnz * sizeof(double));
        tab->csr_alpha = (double*)calloc(csr_n, sizeof(double));
        if (!tab->csr_rowptr || !tab->csr_colidx || !tab->csr_values || !tab->csr_alpha) {
            free(norm_sense); free(norm_sign); free(basic_var_for_row);
            tableau_free(tab);
            return NULL;
        }
        /* Count nnz per row */
        for (int j = 0; j < csr_n; j++) {
            for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j+1]; p++) {
                tab->csr_rowptr[tab->A_ext->rowidx[p] + 1]++;
            }
        }
        /* Prefix sum */
        for (int i = 0; i < csr_m; i++) {
            tab->csr_rowptr[i+1] += tab->csr_rowptr[i];
        }
        /* Scatter entries (use csr_alpha as temp position counter — it's zeroed) */
        int *pos = (int*)tab->csr_alpha;  /* Reuse scratch as int (same size, temp) */
        memset(pos, 0, csr_m * sizeof(int));
        for (int j = 0; j < csr_n; j++) {
            for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j+1]; p++) {
                int row = tab->A_ext->rowidx[p];
                int dest = tab->csr_rowptr[row] + pos[row];
                tab->csr_colidx[dest] = j;
                tab->csr_values[dest] = tab->A_ext->values[p];
                pos[row]++;
            }
        }
        /* Re-zero csr_alpha scratch for use in RC update */
        memset(tab->csr_alpha, 0, csr_n * sizeof(double));

        /* Enable row-scatter only for sparse matrices where it beats vectorized column-scan.
         * At density > ~2%, the column-scan benefits from auto-vectorization (SIMD) while
         * the row-scatter's irregular access patterns prevent it. */
        double density = (double)csr_nnz / ((double)csr_m * (double)csr_n);
        tab->csr_use_scatter = (density < 0.02);
    }

    /* Set up RHS (with sign normalization) and store row signs for Farkas mapping */
    for (int i = 0; i < model->num_cons; i++) {
        tab->rhs[i] = fabs(model->b[i]);  /* Already normalized to be non-negative */
        tab->row_sign[i] = norm_sign[i];  /* +1 or -1 for coordinate mapping */
    }

    /* Initialize basis using basic_var_for_row */
    /* First set all variables as nonbasic at their lower bounds */
    for (int j = 0; j < tab->n; j++) {
        tab->basis_pos[j] = -1;
        tab->var_status[j] = RALPH_NONBASIC_LOWER;
        tab->x[j] = tab->lb_ext[j];
    }

    /* Mark basic variables */
    for (int i = 0; i < model->num_cons; i++) {
        int bv = basic_var_for_row[i];
        tab->basis[i] = bv;
        tab->basis_pos[bv] = i;
        tab->var_status[bv] = RALPH_BASIC;
    }

    /* Compute initial basic variable values: x_B = B^{-1} * (b - N * x_N)
     * For slack/artificial (coeff +1): B^{-1} = 1, so x_B = rhs - sum(A_N * x_N)
     * For surplus (coeff -1): B^{-1} = -1, so x_B = -(rhs - sum(A_N * x_N))
     * General formula: x_B = (rhs - sum(A_N * x_N)) / col_coeff
     */
    for (int i = 0; i < model->num_cons; i++) {
        int bv = basic_var_for_row[i];
        double val = tab->rhs[i];

        /* Subtract A[i,j] * x[j] for nonbasic structural variables j */
        for (int j = 0; j < model->num_vars; j++) {
            if (tab->var_status[j] != RALPH_BASIC && fabs(tab->x[j]) > RALPH_ZERO_TOL) {
                /* Get A[i,j] from sparse matrix */
                for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                    if (tab->A_ext->rowidx[p] == i) {
                        val -= tab->A_ext->values[p] * tab->x[j];
                        break;
                    }
                }
            }
        }

        /* Get the diagonal coefficient of the basic variable (column of bv in row i) */
        double col_coeff = 0.0;
        if (bv >= 0 && bv < tab->A_ext->ncols) {
            for (int p = tab->A_ext->colptr[bv]; p < tab->A_ext->colptr[bv + 1]; p++) {
                if (tab->A_ext->rowidx[p] == i) {
                    col_coeff = tab->A_ext->values[p];
                    break;
                }
            }
        }

        /* Divide by column coefficient to get correct basic variable value */
        if (fabs(col_coeff) > RALPH_ZERO_TOL) {
            tab->x[bv] = val / col_coeff;
        } else {
            tab->x[bv] = val;  /* Fallback (shouldn't happen) */
        }
    }

    /* Initialize steepest edge / Devex weights and pricing flags */
    tableau_init_weights(tab);

    /* Create LU factorization */
    tab->lu = lu_create(tab->m);
    if (!tab->lu) {
        free(norm_sense);
        free(norm_sign);
        free(basic_var_for_row);
        tableau_free(tab);
        return NULL;
    }

    free(norm_sense);
    free(norm_sign);
    free(basic_var_for_row);
    return tab;
}

/* Public wrapper: create tableau with automatic two-phase decision */
SimplexTableau* tableau_create(LPModel *model) {
    return tableau_create_ex(model, 0, 0);
}

/* Dual mode wrapper: no artificials, one auxiliary per constraint */
SimplexTableau* tableau_create_dual(LPModel *model) {
    return tableau_create_ex(model, 0, 1);
}

/* Apply a saved basis/status snapshot into an existing tableau.
 * This updates basis, basis_pos, var_status, and nonbasic x-values.
 * Basic x-values are recomputed by tableau_compute_solution after refactorization. */
int tableau_apply_warm_basis(SimplexTableau *tab, int m, int n,
                             const int *basis, const VarStatus *var_status) {
    if (!tab || !basis || !var_status) return -1;
    if (m != tab->m || n != tab->n) return -1;

    unsigned char *seen = (unsigned char*)calloc((size_t)n, sizeof(unsigned char));
    if (!seen) return -1;

    /* Validate status values first. */
    for (int j = 0; j < n; j++) {
        int st = (int)var_status[j];
        if (st < (int)RALPH_BASIC || st > (int)RALPH_FIXED) {
            free(seen);
            return -1;
        }
    }

    /* Validate basis indices and uniqueness. */
    for (int i = 0; i < m; i++) {
        int bj = basis[i];
        if (bj < 0 || bj >= n || seen[bj]) {
            free(seen);
            return -1;
        }
        seen[bj] = 1;
    }

    /* Apply variable statuses and initialize nonbasic values accordingly. */
    for (int j = 0; j < n; j++) {
        VarStatus st = var_status[j];
        tab->var_status[j] = st;
        switch (st) {
            case RALPH_NONBASIC_UPPER:
                tab->x[j] = tab->ub_ext[j];
                break;
            case RALPH_NONBASIC_FREE:
                if (tab->lb_ext[j] > -RALPH_INFINITY/2 && tab->lb_ext[j] > 0.0) {
                    tab->x[j] = tab->lb_ext[j];
                } else if (tab->ub_ext[j] < RALPH_INFINITY/2 && tab->ub_ext[j] < 0.0) {
                    tab->x[j] = tab->ub_ext[j];
                } else {
                    tab->x[j] = 0.0;
                }
                break;
            case RALPH_FIXED:
            case RALPH_NONBASIC_LOWER:
                tab->x[j] = tab->lb_ext[j];
                break;
            case RALPH_BASIC:
            default:
                tab->x[j] = 0.0;
                break;
        }
        tab->basis_pos[j] = -1;
    }

    /* Apply basis and force listed basics to BASIC status. */
    for (int i = 0; i < m; i++) {
        int bj = basis[i];
        tab->basis[i] = bj;
        tab->basis_pos[bj] = i;
        tab->var_status[bj] = RALPH_BASIC;
    }

    /* Repair invalid status/basis mismatches in saved state. */
    for (int j = 0; j < n; j++) {
        if (tab->basis_pos[j] < 0 && tab->var_status[j] == RALPH_BASIC) {
            tab->var_status[j] = RALPH_NONBASIC_LOWER;
            tab->x[j] = tab->lb_ext[j];
        }
    }

    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    tab->basis_cache_valid = 0;
    tab->basis_cache_total_nnz = 0;

    free(seen);
    return 0;
}

int tableau_apply_structural_bounds(SimplexTableau *tab, int num_struct_vars,
                                    const double *lb, const double *ub) {
    if (!tab) return -1;
    if (num_struct_vars < 0 || num_struct_vars > tab->n) return -1;
    if (num_struct_vars > 0 && (!lb || !ub)) return -1;

    for (int j = 0; j < num_struct_vars; j++) {
        tab->lb_ext[j] = lb[j];
        tab->ub_ext[j] = ub[j];
    }

    /* Keep non-basics pinned to their status-implied bounds after bound changes. */
    for (int j = 0; j < tab->n; j++) {
        switch (tab->var_status[j]) {
            case RALPH_NONBASIC_LOWER:
            case RALPH_FIXED:
                tab->x[j] = tab->lb_ext[j];
                break;
            case RALPH_NONBASIC_UPPER:
                tab->x[j] = tab->ub_ext[j];
                break;
            case RALPH_NONBASIC_FREE:
                if (tab->lb_ext[j] > -RALPH_INFINITY / 2 && tab->lb_ext[j] > 0.0) {
                    tab->x[j] = tab->lb_ext[j];
                } else if (tab->ub_ext[j] < RALPH_INFINITY / 2 && tab->ub_ext[j] < 0.0) {
                    tab->x[j] = tab->ub_ext[j];
                } else {
                    tab->x[j] = 0.0;
                }
                break;
            case RALPH_BASIC:
            default:
                break;
        }
    }

    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    return 0;
}

void tableau_free(SimplexTableau *tab) {
    if (!tab) return;

    /* Free sparse matrix (not in arena) */
    sparse_free(tab->A_ext);
    tab->A_ext = NULL;
    sparse_free(tab->basis_work);
    tab->basis_work = NULL;

    /* Free CSR arrays (not in arena) */
    SAFE_FREE(tab->csr_rowptr);
    SAFE_FREE(tab->csr_colidx);
    SAFE_FREE(tab->csr_values);
    SAFE_FREE(tab->csr_alpha);

    /* Free arena (frees all workspace arrays in one call) */
    sh_arena_free(tab->arena);
    tab->arena = NULL;

    /* NULL out arena-allocated pointers (already freed, just for safety) */
    tab->c_ext = NULL;
    tab->lb_ext = NULL;
    tab->ub_ext = NULL;
    tab->basis = NULL;
    tab->nonbasis = NULL;
    tab->var_status = NULL;
    tab->basis_pos = NULL;
    tab->basis_col_cache = NULL;
    tab->basis_col_nnz_cache = NULL;
    tab->basis_cache_valid = 0;
    tab->basis_cache_total_nnz = 0;
    tab->x = NULL;
    tab->y = NULL;
    tab->rc = NULL;
    tab->work1 = NULL;
    tab->work2 = NULL;
    tab->work3 = NULL;
    tab->work4 = NULL;
    tab->rhs = NULL;
    tab->row_sign = NULL;
    tab->pivot_row = NULL;
    tab->tau_work = NULL;
    tab->se_weights = NULL;
    tab->cb_sparse_idx = NULL;
    tab->cb_sparse_val = NULL;
    tab->aux_row = NULL;
    tab->aux_coef = NULL;
    tab->partial_candidates = NULL;
    tab->redundant_rows = NULL;
    tab->dual_x_backup = NULL;
    tab->dual_rc_backup = NULL;
    tab->dual_basis_backup = NULL;
    tab->dual_basis_pos_backup = NULL;
    tab->dual_status_backup = NULL;

    /* Free perturbation backups (allocated separately during anti-cycling) */
    SAFE_FREE(tab->perturb_backup);
    SAFE_FREE(tab->perturb_backup_lb);
    SAFE_FREE(tab->primal_saved_lb);
    SAFE_FREE(tab->primal_saved_ub);

    /* Free LU factorization (not in arena) */
    lu_free(tab->lu);
    tab->lu = NULL;

    free(tab);
}

/* ============================================================================
 * Basis Management
 * ============================================================================ */

static int ensure_basis_workspace(SimplexTableau *tab, int nnz_needed) {
    if (!tab) return -1;
    if (nnz_needed < 1) nnz_needed = 1;

    SparseMatrix *B = tab->basis_work;
    if (!B) {
        tab->basis_work = sparse_create(tab->m, tab->m, nnz_needed);
        tab->basis_cache_valid = 0;
        tab->basis_cache_total_nnz = 0;
        return tab->basis_work ? 0 : -1;
    }

    if (B->nrows != tab->m || B->ncols != tab->m) {
        sparse_free(B);
        tab->basis_work = sparse_create(tab->m, tab->m, nnz_needed);
        tab->basis_cache_valid = 0;
        tab->basis_cache_total_nnz = 0;
        return tab->basis_work ? 0 : -1;
    }

    if (B->capacity < nnz_needed) {
        int *new_rowidx = (int*)realloc(B->rowidx, (size_t)nnz_needed * sizeof(int));
        if (!new_rowidx) return -1;
        B->rowidx = new_rowidx;

        double *new_values = (double*)realloc(B->values, (size_t)nnz_needed * sizeof(double));
        if (!new_values) return -1;
        B->values = new_values;
        B->capacity = nnz_needed;
    }

    return 0;
}

static void basis_build_record(SimplexTableau *tab,
                               int fastpath_hit,
                               int cols_rewritten,
                               unsigned long long tail_shift_bytes) {
    lp_telemetry_record_basis_build(tab ? tab->owner : NULL,
                                    fastpath_hit,
                                    cols_rewritten,
                                    tail_shift_bytes);
}

/* Behavioral counter for periodic policy triggers; kept separate from telemetry. */
static void runtime_record_periodic_refactor_trigger(SimplexSolver *solver,
                                                     int phase,
                                                     int lu_health_triggered) {
    if (!solver) return;
    if (!lu_health_triggered) {
        if (phase == 1) solver->policy.periodic_policy_refactors_phase1++;
        else if (phase == 2) solver->policy.periodic_policy_refactors_phase2++;
    }
    lp_telemetry_record_periodic_refactor_trigger(solver, phase, lu_health_triggered);
}

/* Build basis matrix from current basis into reusable workspace */
static SparseMatrix* build_basis_matrix(SimplexTableau *tab) {
    if (!tab || !tab->A_ext || !tab->basis) return NULL;

    const SparseMatrix *A = tab->A_ext;
    SparseMatrix *B = tab->basis_work;
    int changed = 0;
    int first_changed = tab->m;
    int last_changed = -1;
    int nnz = 0;

    /* Fast path: if all changed basis positions preserve column nnz, patch only
     * those column payloads in-place and keep colptr layout unchanged. */
    if (tab->basis_cache_valid &&
        B &&
        tab->basis_col_cache &&
        tab->basis_col_nnz_cache &&
        B->nrows == tab->m &&
        B->ncols == tab->m &&
        B->nnz == tab->basis_cache_total_nnz) {
        int old_total_nnz = tab->basis_cache_total_nnz;
        int total_nnz = tab->basis_cache_total_nnz;
        int same_layout = 1;

        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            int prev_j;
            int prev_nnz;
            int next_nnz;
            if (j < 0 || j >= A->ncols) return NULL;

            prev_j = tab->basis_col_cache[k];
            if (j == prev_j) continue;

            if (k < first_changed) first_changed = k;
            if (k > last_changed) last_changed = k;
            prev_nnz = tab->basis_col_nnz_cache[k];
            next_nnz = A->colptr[j + 1] - A->colptr[j];
            total_nnz += next_nnz - prev_nnz;
            changed++;
            if (next_nnz != prev_nnz) {
                same_layout = 0;
            }
        }

        if (changed == 0) {
            basis_build_record(tab, 1, 0, 0);
            return B;
        }

        if (total_nnz < 0) {
            tab->basis_cache_valid = 0;
            tab->basis_cache_total_nnz = 0;
        } else if (ensure_basis_workspace(tab, total_nnz) == 0) {
            B = tab->basis_work;
            if (same_layout && total_nnz == old_total_nnz) {
                for (int k = first_changed; k <= last_changed; k++) {
                    int j = tab->basis[k];
                    int prev_j = tab->basis_col_cache[k];
                    int dst;
                    int src;
                    int col_nnz;
                    if (j == prev_j) continue;

                    src = A->colptr[j];
                    col_nnz = tab->basis_col_nnz_cache[k];
                    dst = B->colptr[k];
                    if (col_nnz > 0) {
                        memcpy(B->rowidx + dst, A->rowidx + src, (size_t)col_nnz * sizeof(int));
                        memcpy(B->values + dst, A->values + src, (size_t)col_nnz * sizeof(double));
                    }
                    tab->basis_col_cache[k] = j;
                }
                basis_build_record(tab, 1, changed, 0);
                return B;
            }

            /* General incremental path: rewrite only changed columns from A_ext.
             * Unchanged columns are copied from cached basis payload while the
             * suffix tail is shifted in-place when span nnz changes. */
            if (first_changed >= 0 && first_changed < tab->m &&
                last_changed >= first_changed && last_changed < tab->m) {
                int old_block_start = B->colptr[first_changed];
                int old_block_end = B->colptr[last_changed + 1];
                int old_block_nnz = old_block_end - old_block_start;
                int new_block_nnz = 0;
                int old_tail_start = old_block_end;
                int old_tail_nnz = old_total_nnz - old_tail_start;
                int span_cols = last_changed - first_changed + 1;
                int unchanged_cols = span_cols - changed;
                int use_sparse_patch =
                    (unchanged_cols > 0 &&
                     unchanged_cols * 2 >= span_cols &&
                     old_block_nnz >= 256);
                unsigned long long tail_shift_bytes = 0;
                int changed_cols_rewritten = 0;

                for (int k = first_changed; k <= last_changed; k++) {
                    int j = tab->basis[k];
                    new_block_nnz += A->colptr[j + 1] - A->colptr[j];
                }

                {
                    int delta = new_block_nnz - old_block_nnz;
                    int scratch_start = 0;

                    if (use_sparse_patch) {
                        int scratch_end;

                        scratch_start = (old_total_nnz > total_nnz) ? old_total_nnz : total_nnz;
                        scratch_end = scratch_start + old_block_nnz;
                        if (scratch_end > B->capacity) {
                            if (ensure_basis_workspace(tab, scratch_end) != 0) {
                                tab->basis_cache_valid = 0;
                                tab->basis_cache_total_nnz = 0;
                                goto full_rebuild_basis;
                            }
                            B = tab->basis_work;
                        }

                        if (old_block_nnz > 0) {
                            memcpy(B->rowidx + scratch_start,
                                   B->rowidx + old_block_start,
                                   (size_t)old_block_nnz * sizeof(int));
                            memcpy(B->values + scratch_start,
                                   B->values + old_block_start,
                                   (size_t)old_block_nnz * sizeof(double));
                        }
                    }

                    if (old_tail_nnz > 0 && delta != 0) {
                        int new_tail_start = old_tail_start + delta;
                        memmove(B->rowidx + new_tail_start,
                                B->rowidx + old_tail_start,
                                (size_t)old_tail_nnz * sizeof(int));
                        memmove(B->values + new_tail_start,
                                B->values + old_tail_start,
                                (size_t)old_tail_nnz * sizeof(double));
                        tail_shift_bytes =
                            (unsigned long long)old_tail_nnz *
                            (unsigned long long)(sizeof(int) + sizeof(double));
                    }

                    {
                        int idx = old_block_start;
                        for (int k = first_changed; k <= last_changed; k++) {
                            int j = tab->basis[k];
                            int prev_j = tab->basis_col_cache[k];
                            int old_col_start = B->colptr[k];
                            int start = A->colptr[j];
                            int end = A->colptr[j + 1];
                            int col_nnz = end - start;
                            int col_changed = (j != prev_j);
                            B->colptr[k] = idx;
                            if (col_nnz > 0) {
                                if (!use_sparse_patch || col_changed) {
                                    memcpy(B->rowidx + idx, A->rowidx + start, (size_t)col_nnz * sizeof(int));
                                    memcpy(B->values + idx, A->values + start, (size_t)col_nnz * sizeof(double));
                                } else {
                                    int old_col_nnz = tab->basis_col_nnz_cache[k];
                                    int old_offset = old_col_start - old_block_start;
                                    if (old_col_nnz != col_nnz) {
                                        tab->basis_cache_valid = 0;
                                        tab->basis_cache_total_nnz = 0;
                                        goto full_rebuild_basis;
                                    }
                                    memcpy(B->rowidx + idx,
                                           B->rowidx + scratch_start + old_offset,
                                           (size_t)col_nnz * sizeof(int));
                                    memcpy(B->values + idx,
                                           B->values + scratch_start + old_offset,
                                           (size_t)col_nnz * sizeof(double));
                                }
                            }
                            if (col_changed) changed_cols_rewritten++;
                            tab->basis_col_cache[k] = j;
                            tab->basis_col_nnz_cache[k] = col_nnz;
                            idx += col_nnz;
                        }
                    }

                    if (delta != 0) {
                        for (int k = last_changed + 1; k <= tab->m; k++) {
                            B->colptr[k] += delta;
                        }
                    }
                }

                B->nnz = total_nnz;
                tab->basis_cache_total_nnz = total_nnz;
                tab->basis_cache_valid = 1;
                basis_build_record(tab, 1, changed_cols_rewritten, tail_shift_bytes);
                return B;
            }
        } else {
            tab->basis_cache_valid = 0;
            tab->basis_cache_total_nnz = 0;
        }

        tab->basis_cache_valid = 0;
        tab->basis_cache_total_nnz = 0;

    }

full_rebuild_basis:
    nnz = 0;
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (j < 0 || j >= A->ncols) return NULL;
        nnz += A->colptr[j + 1] - A->colptr[j];
    }

    if (ensure_basis_workspace(tab, nnz) != 0) return NULL;

    B = tab->basis_work;
    int idx = 0;
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        int start = A->colptr[j];
        int end = A->colptr[j + 1];
        int col_nnz = end - start;

        B->colptr[k] = idx;
        if (col_nnz > 0) {
            memcpy(B->rowidx + idx, A->rowidx + start, (size_t)col_nnz * sizeof(int));
            memcpy(B->values + idx, A->values + start, (size_t)col_nnz * sizeof(double));
            idx += col_nnz;
        }
        tab->basis_col_cache[k] = j;
        tab->basis_col_nnz_cache[k] = col_nnz;
    }
    B->colptr[tab->m] = idx;
    B->nnz = idx;
    tab->basis_cache_total_nnz = idx;
    tab->basis_cache_valid = 1;
    basis_build_record(tab, 0, tab->m, 0);

    return B;
}

/*
 * Attempt to repair a singular basis by replacing problematic columns
 * with slack/auxiliary variables. Returns 0 on success, -1 on failure.
 *
 * Strategy:
 * 1. Try swapping each basis column with any non-basic slack (not just same row)
 * 2. If that fails, try crash basis (all slacks where possible)
 */
static int repair_singular_basis(SimplexTableau *tab) {
    int m = tab->m;
    int n = tab->n;
    int num_struct = tab->model->num_vars;
    int repairs = 0;
    const int MAX_REPAIRS = 100;

    /* Build a copy of the basis for analysis */
    SparseMatrix *B = build_basis_matrix(tab);
    if (!B) return -1;

    /* Strategy 1: Try swapping structural variables with any non-basic slack */
    for (int attempt = 0; attempt < MAX_REPAIRS && repairs < MAX_REPAIRS; attempt++) {
        /* Try factorization */
        int status = lu_factorize(tab->lu, B);
        if (status == 0) {
            return 0;  /* Success */
        }

        /* Factorization failed - try replacing a basis variable with a non-basic one */
        int replaced = 0;

        /* Try each basis position */
        for (int k = m - 1; k >= 0 && !replaced; k--) {
            int j = tab->basis[k];

            /* Skip if this is already a slack/auxiliary */
            if (j >= num_struct) continue;

            /* Try any non-basic slack or auxiliary (not artificial) */
            for (int slack_idx = num_struct; slack_idx < n && !replaced; slack_idx++) {
                /* Skip if this is an artificial variable */
                int is_artificial = 0;
                for (int kk = 0; kk < tab->num_artificial; kk++) {
                    if (tab->artificial_vars[kk] == slack_idx) {
                        is_artificial = 1;
                        break;
                    }
                }
                if (is_artificial) continue;

                /* Skip if already basic or fixed */
                if (tab->var_status[slack_idx] == RALPH_BASIC) continue;
                if (tab->var_status[slack_idx] == RALPH_FIXED) continue;

                /* Swap: move j out of basis, slack_idx into basis */
                tab->var_status[j] = RALPH_NONBASIC_LOWER;
                tab->x[j] = tab->lb_ext[j];
                tab->var_status[slack_idx] = RALPH_BASIC;
                tab->basis[k] = slack_idx;
                tab->basis_pos[j] = -1;
                tab->basis_pos[slack_idx] = k;

                /* Rebuild B and test */
                B = build_basis_matrix(tab);
                if (!B) return -1;

                /* Test if this improved things */
                int test_status = lu_factorize(tab->lu, B);
                if (test_status == 0) {
                    return 0;  /* Success */
                }

                replaced = 1;
                repairs++;
            }
        }

        if (!replaced) break;
    }

    /* Strategy 2: Crash basis - try to use all slacks */
    /* Reset basis to logical basis (all slacks where possible) */
    for (int k = 0; k < m; k++) {
        int old_j = tab->basis[k];
        int slack_idx = num_struct + k;

        if (slack_idx < n && tab->var_status[slack_idx] != RALPH_FIXED) {
            /* Check if this slack is an artificial */
            int is_artificial = 0;
            for (int kk = 0; kk < tab->num_artificial; kk++) {
                if (tab->artificial_vars[kk] == slack_idx) {
                    is_artificial = 1;
                    break;
                }
            }

            if (!is_artificial) {
                /* Swap to slack */
                if (old_j != slack_idx) {
                    tab->var_status[old_j] = RALPH_NONBASIC_LOWER;
                    tab->x[old_j] = tab->lb_ext[old_j];
                    tab->basis_pos[old_j] = -1;
                }
                tab->var_status[slack_idx] = RALPH_BASIC;
                tab->basis[k] = slack_idx;
                tab->basis_pos[slack_idx] = k;
            }
        }
    }

    /* Rebuild and try */
    B = build_basis_matrix(tab);
    if (!B) return -1;

    int status = lu_factorize(tab->lu, B);

    return status;
}

int tableau_refactorize(SimplexTableau *tab) {
    double t_refactor_ms = lp_telemetry_timer_start();
    SimplexSolver *owner = tab ? tab->owner : NULL;
    int reason = RALPH_REFACTOR_REASON_OTHER;
    int updates_before = (tab && tab->lu) ? tab->lu->num_updates : 0;
    lp_telemetry_begin_refactor(owner, &reason);

    /* When Phase 2 has stuck artificials on redundant rows, move them to
     * the FIRST basis positions so LU processes their identity columns first.
     * This prevents partial pivoting from consuming the redundant rows
     * for structural columns before the artificial columns need them. */
    if (tab->phase == 2 && tab->num_redundant > 0 && tab->num_artificial > 0) {
        int next_pos = 0;
        for (int k = 0; k < tab->num_artificial && next_pos < tab->m; k++) {
            int art_j = tab->artificial_vars[k];
            if (tab->var_status[art_j] != RALPH_BASIC) continue;

            int cur_pos = tab->basis_pos[art_j];
            if (cur_pos == next_pos) { next_pos++; continue; }

            int other_j = tab->basis[next_pos];
            tab->basis[next_pos] = art_j;
            tab->basis[cur_pos] = other_j;
            tab->basis_pos[art_j] = next_pos;
            tab->basis_pos[other_j] = cur_pos;
            next_pos++;
        }
    }

    SparseMatrix *B = build_basis_matrix(tab);
    if (!B) return -1;

    /* Pass redundant row hints to LU for handling stuck artificials */
    tab->lu->redundant_rows = tab->redundant_rows;
    tab->lu->num_redundant = tab->num_redundant;

    /* Allow limited regularization for near-singular bases.
     * Phase 1: many artificial variables create near-singular bases.
     * Phase 2 with redundant rows: always allow (even after zeroing A_ext).
     * After zeroing, the sparse LU may still encounter zero pivots at zeroed
     * rows if its column ordering doesn't process artificials first. */
    if (tab->use_two_phase && (tab->phase == 1 ||
        (tab->phase == 2 && tab->num_redundant > 0))) {
        int reg_limit = RALPH_PHASE1_MAX_REGULARIZATIONS;
        if (tab->num_redundant > reg_limit) {
            reg_limit = tab->num_redundant;
        }
        if (reg_limit > tab->m) {
            reg_limit = tab->m;
        }
        tab->lu->allow_regularization = 1;
        tab->lu->max_regularizations = reg_limit;
    } else {
        tab->lu->allow_regularization = 0;
        tab->lu->max_regularizations = 0;
    }
    tab->lu->num_regularized = 0;

    /* When Phase 2 has redundant rows (not yet zeroed), relax pivot tolerance
     * to accept small but valid structural pivots. After zeroing, use normal
     * tolerance since the basis is well-conditioned. */
    double saved_tol = tab->lu->pivot_tol;
    if (tab->phase == 2 && tab->num_redundant > 0 && !tab->redundant_rows_zeroed) {
        tab->lu->pivot_tol = 1e-15;
    }

    int status = lu_factorize(tab->lu, B);

    tab->lu->pivot_tol = saved_tol;

    if (status != 0) {
        /* Factorization failed - try to repair the basis */
        status = repair_singular_basis(tab);
    }

    if (owner) {
        lp_telemetry_record_refactor_with_lu_timed(owner,
                                                   tab ? tab->phase : 0,
                                                   reason,
                                                   t_refactor_ms,
                                                   tab ? tab->m : 0,
                                                   tab ? tab->lu : NULL);
        periodic_feedback_record_refactor(owner, tab ? tab->phase : 0, reason, updates_before, status);
    }

    return status;
}

static inline int tableau_refactorize_with_reason(SimplexTableau *tab, int reason) {
    lp_telemetry_set_refactor_next_reason(tab ? tab->owner : NULL, reason);
    return tableau_refactorize(tab);
}

/* ============================================================================
 * Solution Computation
 * ============================================================================ */

int tableau_compute_solution(SimplexTableau *tab) {
    double t0_ms = lp_telemetry_timer_start();

    /* Compute x_B = B^{-1} * (b - N*x_N) */

    /* First compute b - N*x_N */
    vec_copy_data(tab->work1, tab->rhs, tab->m);

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] != RALPH_BASIC) {
            double xj = tab->x[j];
            if (fabs(xj) > RALPH_ZERO_TOL) {
                sparse_axpy_column(tab->A_ext, j, -xj, tab->work1);
            }
        }
    }

    /* Save original RHS for iterative refinement in pre-allocated workspace. */
    double *orig_rhs = tab->work4;
    vec_copy_data(orig_rhs, tab->work1, tab->m);

    /* Solve B * x_B = work1 */
    lu_solve(tab->lu, tab->work1, tab->work2);

    /* Update basic variable values */
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] = tab->work2[k];
    }

    /* Residual/refinement is expensive; run at most once per (iter, LU state). */
    {
        int do_residual_refine = 1;
        int lu_factorize_calls = -1;
        int lu_num_updates = -1;
        if (tab->lu) {
            lu_factorize_calls = tab->lu->telemetry.perf_factorize_calls;
            lu_num_updates = tab->lu->num_updates;
            if (tab->solution_last_residual_iter == tab->iterations &&
                tab->solution_last_residual_factorize_calls == lu_factorize_calls &&
                tab->solution_last_residual_num_updates == lu_num_updates) {
                do_residual_refine = 0;
            }
        }

        if (do_residual_refine) {
            double *basis_image = tab->y;  /* size m scratch */

            tab->solution_last_residual_iter = tab->iterations;
            tab->solution_last_residual_factorize_calls = lu_factorize_calls;
            tab->solution_last_residual_num_updates = lu_num_updates;

            /* Compute residual: r = rhs_orig - B*x_B */
            vec_set_zero(basis_image, tab->m);
            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                sparse_axpy_column(tab->A_ext, j, tab->x[j], basis_image);
            }

            /* work1 = original_rhs - B*x_B = residual */
            double max_residual = 0.0;
            for (int i = 0; i < tab->m; i++) {
                tab->work1[i] = orig_rhs[i] - basis_image[i];
                double absval = fabs(tab->work1[i]);
                if (absval > max_residual) max_residual = absval;
            }

            /* If residual is large, do bounded iterative refinement. */
            int max_refine_iters = solution_refine_iteration_budget(max_residual, RALPH_FEAS_TOL);
            for (int refine_iter = 0;
                 refine_iter < max_refine_iters && max_residual > RALPH_FEAS_TOL;
                 refine_iter++) {
                /* Solve B * correction = residual */
                lu_solve(tab->lu, tab->work1, tab->work2);

                /* Update solution: x_B += correction */
                for (int k = 0; k < tab->m; k++) {
                    tab->x[tab->basis[k]] += tab->work2[k];
                }

                /* Recompute residual */
                vec_set_zero(basis_image, tab->m);
                for (int k = 0; k < tab->m; k++) {
                    int j = tab->basis[k];
                    sparse_axpy_column(tab->A_ext, j, tab->x[j], basis_image);
                }
                max_residual = 0.0;
                for (int i = 0; i < tab->m; i++) {
                    tab->work1[i] = orig_rhs[i] - basis_image[i];
                    double absval = fabs(tab->work1[i]);
                    if (absval > max_residual) max_residual = absval;
                }
            }
        }
    }

    /* Compute objective value with SIMD reduction */
    double obj = 0.0;
    const double * restrict c = tab->c_ext;
    const double * restrict x = tab->x;
    const int n = tab->n;
    #pragma omp simd reduction(+:obj)
    for (int j = 0; j < n; j++) {
        obj += c[j] * x[j];
    }
    tab->obj_value = obj;

    if (tab->owner) {
        lp_telemetry_record_compute_solution_timed(tab->owner, tab->phase, t0_ms);
    }
    return 0;
}

int tableau_compute_reduced_costs(SimplexTableau *tab) {
    double t0_ms = lp_telemetry_timer_start();

    /* Compute dual values: y = B^{-T} * c_B
     * If c_B is sparse (many slacks with 0 cost), use sparse BTRAN
     */

    /* Count non-zeros in c_B */
    int nnz_cb = 0;
    for (int k = 0; k < tab->m; k++) {
        if (fabs(tab->c_ext[tab->basis[k]]) > RALPH_ZERO_TOL) {
            nnz_cb++;
        }
    }

    /* If c_B is sparse (less than 10% non-zeros), use sparse BTRAN */
    if (nnz_cb < tab->m / 10) {
        /* Use pre-allocated workspace (size m) for sparse indices/values */
        int p = 0;
        for (int k = 0; k < tab->m; k++) {
            double c = tab->c_ext[tab->basis[k]];
            if (fabs(c) > RALPH_ZERO_TOL) {
                tab->cb_sparse_idx[p] = k;
                tab->cb_sparse_val[p] = c;
                p++;
            }
        }
        lu_solve_transpose_sparse(tab->lu, nnz_cb, tab->cb_sparse_idx, tab->cb_sparse_val, tab->y);
    } else {
        /* Dense BTRAN */
        vec_set_zero(tab->work1, tab->m);
        for (int k = 0; k < tab->m; k++) {
            tab->work1[k] = tab->c_ext[tab->basis[k]];
        }
        lu_solve_transpose(tab->lu, tab->work1, tab->y);
    }

    /* Compute reduced costs: rc = c - A' * y
     * Using sparse matrix-transpose-vector multiply: O(nnz) instead of O(n*m) */

    /* First negate y, then compute c + A'*(-y) = c - A'*y */
    double * restrict w1 = tab->work1;
    const double * restrict y = tab->y;
    #pragma omp simd
    for (int i = 0; i < tab->m; i++) {
        w1[i] = -y[i];
    }

    /* rc = c */
    vec_copy_data(tab->rc, tab->c_ext, tab->n);

    /* rc += A' * (-y) = rc - A' * y */
    sparse_matvec_transpose_add(tab->A_ext, tab->work1, tab->rc);

    /* Zero out reduced costs for basic variables */
    double * restrict rc = tab->rc;
    const int * restrict basis = tab->basis;
    for (int k = 0; k < tab->m; k++) {
        rc[basis[k]] = 0.0;
    }

    /* Mark both duals and full rc as valid */
    tab->duals_valid = 1;
    tab->rc_all_valid = 1;

    /* Invalidate dual candidate list — RC recomputed from scratch */
    tab->dual_cand_valid = 0;

    /* Invalidate heap — must be rebuilt from scratch (T2.2) */
    tab->heap_size = 0;

    if (tab->owner) {
        lp_telemetry_record_compute_reduced_costs_timed(tab->owner, tab->phase, t0_ms);
    }
    return 0;
}

/* Compute only dual values y = B^{-T} * c_B (for lazy rc computation) */
int tableau_compute_duals(SimplexTableau *tab) {
    /* Count non-zeros in c_B */
    int nnz_cb = 0;
    for (int k = 0; k < tab->m; k++) {
        if (fabs(tab->c_ext[tab->basis[k]]) > RALPH_ZERO_TOL) {
            nnz_cb++;
        }
    }

    /* If c_B is sparse (less than 10% non-zeros), use sparse BTRAN */
    if (nnz_cb < tab->m / 10) {
        /* Use pre-allocated workspace (size m) for sparse indices/values */
        int p = 0;
        for (int k = 0; k < tab->m; k++) {
            double c = tab->c_ext[tab->basis[k]];
            if (fabs(c) > RALPH_ZERO_TOL) {
                tab->cb_sparse_idx[p] = k;
                tab->cb_sparse_val[p] = c;
                p++;
            }
        }
        lu_solve_transpose_sparse(tab->lu, nnz_cb, tab->cb_sparse_idx, tab->cb_sparse_val, tab->y);
    } else {
        /* Dense BTRAN */
        vec_set_zero(tab->work1, tab->m);
        for (int k = 0; k < tab->m; k++) {
            tab->work1[k] = tab->c_ext[tab->basis[k]];
        }
        lu_solve_transpose(tab->lu, tab->work1, tab->y);
    }

    tab->duals_valid = 1;
    tab->rc_all_valid = 0;  /* Full rc[] not computed */

    return 0;
}

/* Compute single reduced cost rc[j] = c[j] - A[:,j]' * y
 * Requires duals (y) to be valid. Returns the reduced cost.
 * Caches the result in tab->rc[j] for future use and incremental updates. */
static inline double tableau_get_rc(SimplexTableau *tab, int j) {
    /* If full rc[] is valid, just return it */
    if (tab->rc_all_valid) {
        return tab->rc[j];
    }

    /* Compute lazily: rc[j] = c[j] - A[:,j]' * y */
    double rc = tab->c_ext[j] - sparse_dot_column(tab->A_ext, j, tab->y);
    tab->rc[j] = rc;  /* Cache for incremental updates in simplex_pivot */
    return rc;
}

/* Invalidate reduced costs (call after basis change) */
static inline void tableau_invalidate_rc(SimplexTableau *tab) {
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
}

/* ============================================================================
 * Heap Pricing Infrastructure (T2.2)
 *
 * Binary max-heap of non-basic variable indices, keyed by improvement score.
 * Score = effective Dantzig improvement considering variable status:
 *   NONBASIC_LOWER with rc < 0: score = -rc
 *   NONBASIC_UPPER with rc > 0: score = +rc
 *   NONBASIC_FREE:              score = |rc|
 *   Otherwise (wrong sign):     score = 0 (sinks to bottom)
 * This eliminates stale entries: ineligible variables have score 0.
 * Maintained incrementally during RC updates in simplex_pivot().
 * ============================================================================ */

static inline double heap_score(const SimplexTableau *tab, int j) {
    if (simplex_smcp_excl_skip_var(tab, j)) return 0.0;
    double rc = tab->rc[j];
    VarStatus st = tab->var_status[j];
    if (st == RALPH_NONBASIC_LOWER && rc < 0) return -rc;
    if (st == RALPH_NONBASIC_UPPER && rc > 0) return rc;
    if (st == RALPH_NONBASIC_FREE) return fabs(rc);
    return 0.0;
}

static inline void heap_swap(SimplexTableau *tab, int a, int b) {
    int va = tab->heap[a], vb = tab->heap[b];
    tab->heap[a] = vb;
    tab->heap[b] = va;
    tab->heap_pos[va] = b;
    tab->heap_pos[vb] = a;
}

static void heap_sift_up(SimplexTableau *tab, int pos) {
    const int *heap = tab->heap;
    while (pos > 0) {
        int parent = (pos - 1) >> 1;
        if (heap_score(tab, heap[pos]) > heap_score(tab, heap[parent])) {
            heap_swap(tab, pos, parent);
            pos = parent;
        } else {
            break;
        }
    }
}

static void heap_sift_down(SimplexTableau *tab, int pos) {
    const int *heap = tab->heap;
    int size = tab->heap_size;
    for (;;) {
        int best = pos;
        int left = 2 * pos + 1;
        int right = left + 1;
        if (left < size && heap_score(tab, heap[left]) > heap_score(tab, heap[best]))
            best = left;
        if (right < size && heap_score(tab, heap[right]) > heap_score(tab, heap[best]))
            best = right;
        if (best == pos) break;
        heap_swap(tab, pos, best);
        pos = best;
    }
}

static void heap_build(SimplexTableau *tab) {
    tab->heap_size = 0;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) {
            tab->heap_pos[j] = -1;
        } else {
            tab->heap_pos[j] = tab->heap_size;
            tab->heap[tab->heap_size++] = j;
        }
    }
    /* Bottom-up heapify in O(n) */
    for (int i = (tab->heap_size >> 1) - 1; i >= 0; i--) {
        heap_sift_down(tab, i);
    }
}

static void heap_remove(SimplexTableau *tab, int var_j) {
    int pos = tab->heap_pos[var_j];
    if (pos < 0) return;  /* not in heap */
    tab->heap_pos[var_j] = -1;
    int last = --tab->heap_size;
    if (pos == last) return;  /* was last element */
    int moved = tab->heap[last];
    tab->heap[pos] = moved;
    tab->heap_pos[moved] = pos;
    /* Sift in the correct direction */
    if (pos > 0 && heap_score(tab, moved) > heap_score(tab, tab->heap[(pos - 1) >> 1])) {
        heap_sift_up(tab, pos);
    } else {
        heap_sift_down(tab, pos);
    }
}

static void heap_insert(SimplexTableau *tab, int var_j) {
    int pos = tab->heap_size++;
    tab->heap[pos] = var_j;
    tab->heap_pos[var_j] = pos;
    heap_sift_up(tab, pos);
}

static void heap_update(SimplexTableau *tab, int var_j) {
    int pos = tab->heap_pos[var_j];
    if (pos < 0) return;  /* basic var, not in heap */
    /* Sift up or down based on new score */
    if (pos > 0 && heap_score(tab, var_j) > heap_score(tab, tab->heap[(pos - 1) >> 1])) {
        heap_sift_up(tab, pos);
    } else {
        heap_sift_down(tab, pos);
    }
}

/* Heap-based Dantzig pricing: O(1) extraction of max-improvement variable.
 * Ineligible variables have score 0 and naturally sit at the bottom. */
int pricing_heap(SimplexTableau *tab, int *entering) {
    /* Lazy rebuild: heap was invalidated by full RC recomputation */
    if (tab->heap_size == 0 && tab->rc_all_valid) heap_build(tab);
    /* Pop ineligible entries (safety net for status changes missed by
     * incremental maintenance, e.g. bound flips in ratio test). */
    while (tab->heap_size > 0) {
        int j = tab->heap[0];
        double sc = heap_score(tab, j);
        if (sc >= RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
        /* Ineligible root — remove and try next */
        heap_remove(tab, j);
    }
    return 1;  /* optimal */
}

/* ============================================================================
 * Pricing (Entering Variable Selection)
 * ============================================================================ */

int pricing_dantzig(SimplexTableau *tab, int *entering) {
    /* Standard Dantzig pricing: most negative reduced cost */
    double best_rc = -RALPH_OPT_TOL;
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        /* Check if this variable can improve */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < best_rc) {
            best_rc = rc;
            *entering = j;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && -rc < best_rc) {
            best_rc = -rc;
            *entering = j;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > -best_rc) {
            best_rc = -fabs(rc);
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;  /* 1 = optimal */
}

/* Bland's rule pricing: choose smallest index among eligible variables.
 * Used as fallback when cycling is detected. */
int pricing_bland(SimplexTableau *tab, int *entering) {
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        /* Check if this variable can improve */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            *entering = j;
            return 0;  /* Return first eligible */
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
    }

    return 1;  /* 1 = optimal */
}

/* Bland-style pricing with one excluded variable index. */
static int pricing_bland_excluding(SimplexTableau *tab, int excluded_var, int *entering) {
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (j == excluded_var) continue;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
    }

    return 1;
}

static int pricing_bland_excluding_two(SimplexTableau *tab,
                                       int excluded_a,
                                       int excluded_b,
                                       int *entering) {
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (j == excluded_a || j == excluded_b) continue;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            *entering = j;
            return 0;
        }
    }

    return 1;
}

static void phase1_exclude_entering_var(int var,
                                        int ttl,
                                        int *exclude_a,
                                        int *ttl_a,
                                        int *exclude_b,
                                        int *ttl_b) {
    if (var < 0 || ttl <= 0 || !exclude_a || !ttl_a || !exclude_b || !ttl_b) return;
    /* Note: var is validated by callers (pricing functions return valid indices),
     * but we skip storing obviously invalid indices defensively. */

    if (*exclude_a == var || *ttl_a <= 0) {
        *exclude_a = var;
        *ttl_a = ttl;
        return;
    }
    if (*exclude_b == var || *ttl_b <= 0) {
        *exclude_b = var;
        *ttl_b = ttl;
        return;
    }

    if (*ttl_a <= *ttl_b) {
        *exclude_a = var;
        *ttl_a = ttl;
    } else {
        *exclude_b = var;
        *ttl_b = ttl;
    }
}

static void phase1_pivot_fail_recovery_maybe_exclude_entering(
    SimplexSolver *solver,
    int fail_repeat_count,
    int entering,
    int *excluded_entering_a,
    int *excluded_entering_ttl_a,
    int *excluded_entering_b,
    int *excluded_entering_ttl_b) {
    if (fail_repeat_count < PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER) return;
    phase1_exclude_entering_var(entering,
                                PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_ITERS,
                                excluded_entering_a,
                                excluded_entering_ttl_a,
                                excluded_entering_b,
                                excluded_entering_ttl_b);
    lp_telemetry_record_phase1_pivot_fail_recovery_exclusion(solver);
}

static void phase1_recompute_full_with_reason(SimplexSolver *solver,
                                              SimplexTableau *tab,
                                              int *rc_only_streak,
                                              LPPhase1RecomputeReason reason) {
    /* Full recompute is required after basis/LU/perturbation state changes. */
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);
    if (rc_only_streak) *rc_only_streak = 0;
    lp_telemetry_record_phase1_recompute(solver, reason);
}

static void phase1_recompute_full_no_reason(SimplexSolver *solver,
                                            SimplexTableau *tab,
                                            int *rc_only_streak) {
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);
    if (rc_only_streak) *rc_only_streak = 0;
    (void)solver;
}

static int phase1_recompute_rc_only_guarded(SimplexSolver *solver,
                                            SimplexTableau *tab,
                                            int *rc_only_streak) {
    /* RC-only refresh is safe only while basis/LU and primal x are unchanged.
     * Guard long RC-only streaks with a forced full recompute to bound drift. */
    if (rc_only_streak && *rc_only_streak >= PHASE1_RC_ONLY_STREAK_GUARD) {
        lp_telemetry_record_phase1_recompute_guard_forced_full(solver);
        phase1_recompute_full_no_reason(solver, tab, rc_only_streak);
        return 1;
    }
    tableau_compute_reduced_costs(tab);
    lp_telemetry_record_phase1_recompute_rc_only(solver);
    if (rc_only_streak) (*rc_only_streak)++;
    return 0;
}

static void phase1_recompute_dir_skip_safe(SimplexSolver *solver,
                                           SimplexTableau *tab,
                                           int degenerate_count,
                                           int no_pivot_streak,
                                           int *rc_only_streak,
                                           int *dir_skip_no_recompute_streak) {
    int no_recompute_streak = 0;
    if (dir_skip_no_recompute_streak) {
        no_recompute_streak = *dir_skip_no_recompute_streak;
    }
    if (lp_refactor_policy_phase1_dir_skip_should_skip_recompute(
            tab->m,
            degenerate_count,
            no_pivot_streak,
            no_recompute_streak)) {
        if (dir_skip_no_recompute_streak) {
            (*dir_skip_no_recompute_streak)++;
        }
        lp_telemetry_record_phase1_dir_stabilize_skip_no_recompute(solver);
        return;
    }
    if (dir_skip_no_recompute_streak && *dir_skip_no_recompute_streak > 0) {
        lp_telemetry_record_phase1_dir_stabilize_skip_guard_refresh(solver);
        *dir_skip_no_recompute_streak = 0;
    }
    int allow_rc_only = lp_refactor_policy_phase1_dir_skip_allow_rc_only(
        tab->m, degenerate_count, no_pivot_streak);
    if (!allow_rc_only) {
        phase1_recompute_full_with_reason(solver,
                                          tab,
                                          rc_only_streak,
                                          LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
        lp_telemetry_record_phase1_dir_stabilize_skip(solver, 1);
        return;
    }
    int used_full = phase1_recompute_rc_only_guarded(solver, tab, rc_only_streak);
    lp_telemetry_record_phase1_dir_stabilize_skip(solver, used_full);
    if (used_full) {
        lp_telemetry_record_phase1_recompute(solver, LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
    }
}

int pricing_steepest_edge(SimplexTableau *tab, int *entering) {
    /* Steepest edge pricing: max |rc_j| / sqrt(gamma_j)
     * Uses exact weights updated with the formula:
     *   gamma_j = ||B^{-1} * a_j||^2
     *
     * Optimization: Compare rc²/weight instead of |rc|/sqrt(weight)
     * to eliminate expensive sqrt() calls. Mathematically equivalent:
     *   |rc|/sqrt(w) > t  ⟺  rc²/w > t²
     */
    double best_ratio_sq = RALPH_OPT_TOL * RALPH_OPT_TOL;  /* Squared threshold */
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];
        double weight = tab->se_weights[j];
        if (weight < 1e-10) weight = 1.0;

        double ratio_sq = 0.0;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            ratio_sq = (rc * rc) / weight;
        }

        if (ratio_sq > best_ratio_sq) {
            best_ratio_sq = ratio_sq;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;
}

int pricing_devex(SimplexTableau *tab, int *entering) {
    /* Devex pricing: max |rc_j|² / gamma_j
     *
     * Uses approximate steepest edge weights with periodic reset.
     * Reference: Harris, "Pivot Selection Methods of the Devex LP Code", 1973
     */
    double best_ratio = RALPH_OPT_TOL * RALPH_OPT_TOL;  /* Squared tolerance */
    *entering = -1;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        double rc = tab->rc[j];
        double weight = tab->se_weights[j];
        if (weight < 1.0) weight = 1.0;  /* Devex weights are always >= 1 */

        double ratio = 0.0;

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            ratio = (rc * rc) / weight;
        }

        if (ratio > best_ratio) {
            best_ratio = ratio;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : 1;
}

/* Partial pricing with candidate list (hot set):
 *
 * Instead of scanning all n variables each iteration, we maintain a "hot set" of
 * promising variables. The algorithm:
 * 1. First scan the hot set for eligible variables
 * 2. If no good candidate in hot set, do a partial scan of remaining variables
 * 3. Selected variables are added to the hot set for future iterations
 * 4. Periodically clean the hot set (remove basic variables, refresh)
 *
 * This reduces O(n) to approximately O(hot_set_size + block_size) per iteration.
 */
#define PARTIAL_PRICE_BLOCK 100       /* Variables per partial scan block */
#define PARTIAL_PRICE_THRESHOLD 1e-6  /* Accept if |rc| > threshold */
#define PARTIAL_HOT_ACCEPT 1e-4       /* Accept immediately from hot set if |rc| > this */
#define DEVEX_PARTIAL_BLOCK 240
#define DEVEX_PARTIAL_ENABLE_M 400
#define DEVEX_PARTIAL_ENABLE_N 1200
#define DEVEX_PARTIAL_DEGEN_TRIGGER 20
#define DEVEX_PARTIAL_ITER_TRIGGER 4000
#define DEVEX_PARTIAL_FULL_RESCAN_MASK 1

/* Check if variable j is eligible for entering */
static inline int is_entering_eligible(SimplexTableau *tab, int j, double *rc_out) {
    if (j < 0 || j >= tab->n) return 0;  /* Bounds check */
    if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) return 0;

    /* Use lazy RC computation - computes on demand if not already cached */
    double rc = tableau_get_rc(tab, j);
    *rc_out = rc;

    if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -PARTIAL_PRICE_THRESHOLD) {
        return 1;
    } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > PARTIAL_PRICE_THRESHOLD) {
        return 1;
    } else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > PARTIAL_PRICE_THRESHOLD) {
        return 1;
    }
    return 0;
}

/* Add variable to hot set if not already present and not full */
static inline void add_to_hot_set(SimplexTableau *tab, int var) {
    /* Check if already in hot set */
    for (int i = 0; i < tab->partial_cand_count; i++) {
        if (tab->partial_candidates[i] == var) return;
    }
    /* Add if space available */
    if (tab->partial_cand_count < tab->partial_cand_capacity) {
        tab->partial_candidates[tab->partial_cand_count++] = var;
    }
}

int pricing_partial(SimplexTableau *tab, int *entering) {
    *entering = -1;

    /* Ensure duals are valid for lazy RC computation */
    if (!tab->duals_valid) {
        tableau_compute_duals(tab);
    }

    int n = tab->n;
    double best_rc_val = 0.0;
    int best_var = -1;

    /* Phase 1: Scan hot set first (fast path) */
    int write_idx = 0;
    for (int i = 0; i < tab->partial_cand_count; i++) {
        int j = tab->partial_candidates[i];

        /* Skip and remove basic variables from hot set */
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        /* Keep this variable in the compacted hot set */
        tab->partial_candidates[write_idx++] = j;

        double rc;
        if (is_entering_eligible(tab, j, &rc)) {
            double rc_abs = fabs(rc);

            /* Accept immediately if reduced cost is very attractive */
            if (rc_abs > PARTIAL_HOT_ACCEPT) {
                *entering = j;
                tab->partial_cand_count = write_idx;
                return 0;
            }

            /* Track best candidate seen */
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = j;
            }
        }
    }
    /* Update hot set count after compaction */
    tab->partial_cand_count = write_idx;

    /* Phase 2: Partial scan from current position */
    int start = tab->partial_price_pos;
    int scanned = 0;

    for (int i = 0; i < n && scanned < PARTIAL_PRICE_BLOCK; i++) {
        int j = (start + i) % n;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        scanned++;
        double rc;
        if (is_entering_eligible(tab, j, &rc)) {
            double rc_abs = fabs(rc);
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = j;
            }
        }
    }

    /* Update scan position for next call (round-robin) */
    tab->partial_price_pos = (start + PARTIAL_PRICE_BLOCK) % n;

    /* Use best variable found (if any) */
    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    /* Phase 3: Full scan if partial scan found nothing (rare) */
    for (int i = 0; i < n; i++) {
        double rc;
        if (is_entering_eligible(tab, i, &rc)) {
            double rc_abs = fabs(rc);
            if (rc_abs > fabs(best_rc_val)) {
                best_rc_val = rc;
                best_var = i;
            }
        }
    }

    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    return 1;  /* Optimal - no eligible variable found */
}

/* Devex-scored partial pricing.
 * Uses the same hot-set/round-robin idea as pricing_partial(), but keeps
 * Devex's rc^2/weight scoring to preserve pivot quality characteristics. */
static inline int devex_entering_eligible(SimplexTableau *tab, int j, double *score_out) {
    if (j < 0 || j >= tab->n) return 0;
    if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) return 0;

    double rc = tableau_get_rc(tab, j);
    double score = 0.0;

    if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
        score = rc * rc;
    } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
        score = rc * rc;
    } else if (tab->var_status[j] == RALPH_NONBASIC_FREE &&
               (rc > RALPH_OPT_TOL || rc < -RALPH_OPT_TOL)) {
        score = rc * rc;
    } else {
        return 0;
    }

    double weight = tab->se_weights[j];
    if (weight < 1.0) weight = 1.0;
    *score_out = score / weight;
    return *score_out > 0.0;
}

static int pricing_devex_partial(SimplexTableau *tab, int *entering) {
    *entering = -1;

    if (!tab->duals_valid) {
        tableau_compute_duals(tab);
    }

    double best_score = RALPH_OPT_TOL * RALPH_OPT_TOL;
    int best_var = -1;
    int n = tab->n;

    /* Phase 1: scan and compact the hot set. */
    int write_idx = 0;
    for (int i = 0; i < tab->partial_cand_count; i++) {
        int j = tab->partial_candidates[i];
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;
        tab->partial_candidates[write_idx++] = j;

        double score;
        if (devex_entering_eligible(tab, j, &score) && score > best_score) {
            best_score = score;
            best_var = j;
        }
    }
    tab->partial_cand_count = write_idx;

    /* Phase 2: bounded round-robin scan through the full variable space. */
    int start = tab->partial_price_pos;
    int scanned = 0;
    for (int i = 0; i < n && scanned < DEVEX_PARTIAL_BLOCK; i++) {
        int j = (start + i) % n;
        if (tab->var_status[j] == RALPH_BASIC || simplex_smcp_excl_skip_var(tab, j)) continue;

        scanned++;
        double score;
        if (devex_entering_eligible(tab, j, &score) && score > best_score) {
            best_score = score;
            best_var = j;
        }
    }
    tab->partial_price_pos = (start + DEVEX_PARTIAL_BLOCK) % n;

    if (best_var >= 0) {
        *entering = best_var;
        add_to_hot_set(tab, best_var);
        return 0;
    }

    /* Fallback for safety: if bounded scan missed a candidate, run full Devex. */
    if (pricing_devex(tab, entering) == 0) {
        add_to_hot_set(tab, *entering);
        return 0;
    }

    return 1;
}

static int phase2_use_adaptive_devex_partial(const SimplexTableau *tab,
                                             int iter,
                                             int degenerate_count,
                                             int use_bland,
                                             int pricing_strategy) {
    if (!tab) return 0;
    if (use_bland) return 0;
    if (pricing_strategy != 2) return 0;
    if (tab->m < DEVEX_PARTIAL_ENABLE_M || tab->n < DEVEX_PARTIAL_ENABLE_N) return 0;
    if (degenerate_count >= DEVEX_PARTIAL_DEGEN_TRIGGER) return 1;
    return iter >= DEVEX_PARTIAL_ITER_TRIGGER;
}

/* ============================================================================
 * Ratio Test (Leaving Variable Selection)
 * ============================================================================ */

/* Bland's ratio test: among ties, choose smallest index leaving variable */
int ratio_test_bland(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    /* Use hyper-sparse FTRAN for better performance on sparse columns */
    {
        double t_ftran_ms = lp_telemetry_timer_start();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }

    /* Zero FTRAN values for stuck artificial positions.
     * LU regularization (diagonal=1.0) produces meaningless values for
     * redundant rows. Zeroing prevents them from affecting the ratio test
     * and the solution update in simplex_pivot. */
    if (tab->num_redundant > 0) {
        for (int a = 0; a < tab->num_artificial; a++) {
            int art_j = tab->artificial_vars[a];
            if (tab->var_status[art_j] == RALPH_BASIC) {
                int pos = tab->basis_pos[art_j];
                if (pos >= 0 && pos < tab->m) {
                    tab->work2[pos] = 0.0;
                }
            }
        }
    }

    double dir = 1.0;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    /* Relative pivot filter: avoid accepting numerically tiny pivots. */
    int ftran_nnz = 0;
    double max_abs_dk = 0.0;
    for (int k = 0; k < tab->m; k++) {
        double abs_dk = fabs(tab->work2[k] * dir);
        if (abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
        if (abs_dk > max_abs_dk) {
            max_abs_dk = abs_dk;
        }
    }
    if (tab->owner) {
        lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
    }
    double pivot_tol = fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);

    *leaving = -1;
    *theta = RALPH_INFINITY;
    int leaving_var = tab->n;  /* Track actual variable index for Bland's tie-breaking */

    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        int j = tab->basis[k];
        double xj = tab->x[j];

        double ratio = RALPH_INFINITY;
        if (dk > pivot_tol) {
            ratio = (xj - tab->lb_ext[j]) / dk;
        } else if (dk < -pivot_tol) {
            ratio = (tab->ub_ext[j] - xj) / (-dk);
        }

        if (ratio < *theta - RALPH_FEAS_TOL) {
            *theta = ratio;
            *leaving = k;
            leaving_var = j;
        } else if (fabs(ratio - *theta) <= RALPH_FEAS_TOL && j < leaving_var) {
            /* Bland's rule: among ties, choose smallest variable index */
            *leaving = k;
            leaving_var = j;
        }
    }

    /* Check bound flip */
    double enter_range = tab->ub_ext[entering] - tab->lb_ext[entering];
    if (enter_range < *theta && enter_range < RALPH_INFINITY/2) {
        *theta = enter_range;
        *leaving = -2;
    }

    if (*theta >= RALPH_INFINITY/2) {
        return -1;  /* Unbounded */
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

/* Standard (non-Harris) ratio test:
 * - strict minimum ratio selection
 * - no Harris tolerance expansion
 */
static int ratio_test_standard(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    {
        double t_ftran_ms = lp_telemetry_timer_start();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }

    if (tab->num_redundant > 0) {
        for (int a = 0; a < tab->num_artificial; a++) {
            int art_j = tab->artificial_vars[a];
            if (tab->var_status[art_j] == RALPH_BASIC) {
                int pos = tab->basis_pos[art_j];
                if (pos >= 0 && pos < tab->m) {
                    tab->work2[pos] = 0.0;
                }
            }
        }
    }

    double dir = 1.0;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    int ftran_nnz = 0;
    double max_abs_dk = 0.0;
    for (int k = 0; k < tab->m; k++) {
        double abs_dk = fabs(tab->work2[k] * dir);
        if (abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
        if (abs_dk > max_abs_dk) {
            max_abs_dk = abs_dk;
        }
    }
    if (tab->owner) {
        lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
    }
    double pivot_tol = fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);

    *leaving = -1;
    *theta = RALPH_INFINITY;
    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        int j = tab->basis[k];
        double xj = tab->x[j];

        double ratio = RALPH_INFINITY;
        if (dk > pivot_tol) {
            ratio = (xj - tab->lb_ext[j]) / dk;
        } else if (dk < -pivot_tol) {
            ratio = (tab->ub_ext[j] - xj) / (-dk);
        }

        if (ratio < *theta - RALPH_FEAS_TOL) {
            *theta = ratio;
            *leaving = k;
        }
    }

    {
        double enter_range = tab->ub_ext[entering] - tab->lb_ext[entering];
        if (enter_range <= *theta && enter_range < RALPH_INFINITY / 2) {
            *theta = enter_range;
            *leaving = -2;
        }
    }

    if (*theta >= RALPH_INFINITY / 2) {
        return -1;
    }
    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

static int primal_ratio_test_with_policy(const SimplexSolver *solver,
                                         SimplexTableau *tab,
                                         int use_bland,
                                         int entering,
                                         int *leaving,
                                         double *theta) {
    if (use_bland) {
        return ratio_test_bland(tab, entering, leaving, theta);
    }
    if (solver && solver->ratio_test_mode == LP_RATIO_TEST_STANDARD) {
        return ratio_test_standard(tab, entering, leaving, theta);
    }
    return ratio_test_harris(tab, entering, leaving, theta);
}

int ratio_test_harris(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    /* Use hyper-sparse FTRAN for better performance on sparse columns */
    {
        double t_ftran_ms = lp_telemetry_timer_start();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }

    /* Zero FTRAN values for stuck artificial positions (see ratio_test_bland). */
    if (tab->num_redundant > 0) {
        for (int a = 0; a < tab->num_artificial; a++) {
            int art_j = tab->artificial_vars[a];
            if (tab->var_status[art_j] == RALPH_BASIC) {
                int pos = tab->basis_pos[art_j];
                if (pos >= 0 && pos < tab->m) {
                    tab->work2[pos] = 0.0;
                }
            }
        }
    }

    double dir = 1.0;  /* Direction of movement */
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    const int *basis = tab->basis;
    const double *x = tab->x;
    const double *lb = tab->lb_ext;
    const double *ub = tab->ub_ext;
    const double *work2 = tab->work2;

    /* Relative pivot filter: avoid numerically fragile leaving choices. */
    int ftran_nnz = 0;
    double max_abs_dk = 0.0;
    for (int k = 0; k < tab->m; k++) {
        double dk = (dir > 0.0) ? work2[k] : -work2[k];
        double abs_dk = fabs(dk);
        if (abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
        if (abs_dk > max_abs_dk) {
            max_abs_dk = abs_dk;
        }
    }
    if (tab->owner) {
        lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
    }
    double pivot_tol = fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);

    /* Single-pass Harris ratio test (merged from two passes)
     *
     * Harris ratio test allows small infeasibility (FEAS_TOL) when computing
     * theta_max, then selects among candidates within that tolerance.
     *
     * Tie-breaking strategy:
     * 1. Prefer non-degenerate pivots (ratio > tolerance)
     * 2. Among degenerate ties, prefer larger pivot for numerical stability
     */
    double theta_max = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_is_degen = 1;
    *leaving = -1;
    *theta = RALPH_INFINITY;

    /* Check bound on entering variable first (contributes to theta_max) */
    double enter_range = tab->ub_ext[entering] - tab->lb_ext[entering];
    if (enter_range < RALPH_INFINITY/2) {
        theta_max = enter_range;
    }

    /* Single pass: compute theta_max and select best leaving simultaneously */
    for (int k = 0; k < tab->m; k++) {
        double dk = (dir > 0.0) ? work2[k] : -work2[k];
        if (fabs(dk) < pivot_tol) continue;  /* Skip tiny pivots */

        int j = basis[k];
        double xj = x[j];

        double ratio_harris;  /* Ratio with Harris tolerance */
        double ratio_exact;   /* Exact ratio for selection */

        if (dk > 0) {
            /* Variable will decrease toward lower bound */
            double slack = xj - lb[j];
            ratio_harris = (slack + RALPH_FEAS_TOL) / dk;
            ratio_exact = slack / dk;
        } else {
            /* Variable will increase toward upper bound (dk < 0) */
            double slack = ub[j] - xj;
            ratio_harris = (slack + RALPH_FEAS_TOL) / (-dk);
            ratio_exact = slack / (-dk);
        }

        /* Update theta_max */
        if (ratio_harris < theta_max) {
            theta_max = ratio_harris;
        }

        /* Check if this is a valid candidate (within current theta_max + tolerance) */
        if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
            int is_degen = (ratio_exact < 1e-8);
            double pivot_size = fabs(dk);

            /* Selection criteria */
            int select = 0;
            if (*leaving < 0) {
                select = 1;  /* First candidate */
            } else if (!is_degen && best_is_degen) {
                select = 1;  /* Prefer non-degenerate */
            } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                select = 1;  /* Significantly larger pivot */
            }

            if (select) {
                best_pivot = pivot_size;
                best_is_degen = is_degen;
                *leaving = k;
                *theta = ratio_exact > 0 ? ratio_exact : 0;
            }
        }
    }

    /* After the pass, invalidate selection if it's no longer within theta_max
     * (theta_max may have decreased after we selected the candidate) */
    if (*leaving >= 0 && *theta > theta_max + RALPH_FEAS_TOL) {
        /* Re-scan for valid candidates - this is rare */
        best_pivot = 0.0;
        best_is_degen = 1;
        *leaving = -1;
        *theta = RALPH_INFINITY;

        for (int k = 0; k < tab->m; k++) {
            double dk = (dir > 0.0) ? work2[k] : -work2[k];
            if (fabs(dk) < pivot_tol) continue;

            int j = basis[k];
            double xj = x[j];
            double ratio_exact;

            if (dk > 0) {
                ratio_exact = (xj - lb[j]) / dk;
            } else {
                ratio_exact = (ub[j] - xj) / (-dk);
            }

            if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
                int is_degen = (ratio_exact < 1e-8);
                double pivot_size = fabs(dk);

                int select = 0;
                if (*leaving < 0) {
                    select = 1;
                } else if (!is_degen && best_is_degen) {
                    select = 1;
                } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                    select = 1;
                }

                if (select) {
                    best_pivot = pivot_size;
                    best_is_degen = is_degen;
                    *leaving = k;
                    *theta = ratio_exact > 0 ? ratio_exact : 0;
                }
            }
        }
    }

    /* Check for unbounded */
    if (theta_max >= RALPH_INFINITY/2) {
        *theta = RALPH_INFINITY;
        return -1;  /* Unbounded */
    }

    /* Check if entering variable hits its bound (bound flip) */
    if (enter_range <= theta_max && enter_range < RALPH_INFINITY/2) {
        if (*leaving < 0 || enter_range < *theta) {
            *theta = enter_range;
            *leaving = -2;  /* Special flag for bound flip */
        }
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

/* Fallback ratio test for already-computed direction (tab->work2) while
 * excluding a specific leaving position. Used to avoid repeated failing pivots.
 */
static int ratio_test_harris_excluding_current(SimplexTableau *tab, int entering,
                                               int exclude_pos, int *leaving, double *theta) {
    if (!tab || !leaving || !theta) return -1;

    double dir = 1.0;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    double max_abs_dk = 0.0;
    for (int k = 0; k < tab->m; k++) {
        if (k == exclude_pos) continue;
        double abs_dk = fabs(tab->work2[k] * dir);
        if (abs_dk > max_abs_dk) {
            max_abs_dk = abs_dk;
        }
    }
    double pivot_tol = fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);

    double theta_max = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_is_degen = 1;
    *leaving = -1;
    *theta = RALPH_INFINITY;

    double enter_range = tab->ub_ext[entering] - tab->lb_ext[entering];
    if (enter_range < RALPH_INFINITY / 2) {
        theta_max = enter_range;
    }

    for (int k = 0; k < tab->m; k++) {
        if (k == exclude_pos) continue;

        double dk = tab->work2[k] * dir;
        if (fabs(dk) < pivot_tol) continue;

        int j = tab->basis[k];
        double xj = tab->x[j];

        double ratio_harris;
        double ratio_exact;
        if (dk > 0) {
            double slack = xj - tab->lb_ext[j];
            ratio_harris = (slack + RALPH_FEAS_TOL) / dk;
            ratio_exact = slack / dk;
        } else {
            double slack = tab->ub_ext[j] - xj;
            ratio_harris = (slack + RALPH_FEAS_TOL) / (-dk);
            ratio_exact = slack / (-dk);
        }

        if (ratio_harris < theta_max) {
            theta_max = ratio_harris;
        }

        if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
            int is_degen = (ratio_exact < 1e-8);
            double pivot_size = fabs(dk);

            int select = 0;
            if (*leaving < 0) {
                select = 1;
            } else if (!is_degen && best_is_degen) {
                select = 1;
            } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                select = 1;
            }

            if (select) {
                best_pivot = pivot_size;
                best_is_degen = is_degen;
                *leaving = k;
                *theta = ratio_exact > 0 ? ratio_exact : 0;
            }
        }
    }

    if (theta_max >= RALPH_INFINITY / 2) {
        return -1;
    }

    if (enter_range <= theta_max && enter_range < RALPH_INFINITY / 2) {
        if (*leaving < 0 || enter_range < *theta) {
            *theta = enter_range;
            *leaving = -2;
        }
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

/* ============================================================================
 * Simplex Iteration
 * ============================================================================ */

static int simplex_pivot(SimplexTableau *tab,
                         int entering,
                         int leaving_pos,
                         double theta,
                         int repeat_pattern_count) {
    double dir = (tab->var_status[entering] == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;
    double x_enter_old = tab->x[entering];
    double *x = tab->x;
    int *basis = tab->basis;
    const double *work2 = tab->work2;
    double step = theta * dir;
    double gamma_e = 0.0;

    if (tab->trace_phase1_enabled) {
        tab->trace_last_entering = entering;
        tab->trace_last_leaving_pos = leaving_pos;
        tab->trace_last_theta = theta;
        tab->trace_last_fail_reason = PHASE1_PIVOT_FAIL_NONE;
        tab->trace_last_pivot = 0.0;
        tab->trace_last_dir_inf = 0.0;
        for (int k = 0; k < tab->m; k++) {
            double absval = fabs(tab->work2[k]);
            if (absval > tab->trace_last_dir_inf) {
                tab->trace_last_dir_inf = absval;
            }
        }
    }

    /* Update entering variable */
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        x[entering] += theta;
    } else {
        x[entering] -= theta;
    }

    /* Update basic variables and accumulate ||d_entering||^2 in one pass. */
    for (int k = 0; k < tab->m; k++) {
        double dk = work2[k];
        x[basis[k]] -= step * dk;
        gamma_e += dk * dk;
    }

    if (leaving_pos == -2) {
        /* Bound flip: entering variable goes to opposite bound */
        if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
            tab->var_status[entering] = RALPH_NONBASIC_UPPER;
            tab->x[entering] = tab->ub_ext[entering];
        } else {
            tab->var_status[entering] = RALPH_NONBASIC_LOWER;
            tab->x[entering] = tab->lb_ext[entering];
        }
        /* Status changed → heap score changed; re-sift to correct position */
        if (tab->pricing_strategy == 4) heap_update(tab, entering);
        return 0;
    }

    /* Normal pivot: swap entering and leaving */
    int leaving = basis[leaving_pos];
    double x_leave_old = x[leaving];
    VarStatus entering_old_status = tab->var_status[entering];

    /* Update basis */
    basis[leaving_pos] = entering;
    tab->basis_pos[entering] = leaving_pos;
    tab->basis_pos[leaving] = -1;

    /* Update variable status */
    tab->var_status[entering] = RALPH_BASIC;

    /* Leaving goes to appropriate bound */
    if (tab->work2[leaving_pos] * dir > 0) {
        tab->var_status[leaving] = RALPH_NONBASIC_LOWER;
        x[leaving] = tab->lb_ext[leaving];
    } else {
        tab->var_status[leaving] = RALPH_NONBASIC_UPPER;
        x[leaving] = tab->ub_ext[leaving];
    }

    /* Compute pivot row and steepest edge update data BEFORE LU update (using old basis) */
    double pivot = tab->work2[leaving_pos];
    double pivot_sq = pivot * pivot;

    if (tab->trace_phase1_enabled) {
        tab->trace_last_pivot = pivot;
    }

    if (!isfinite(pivot) || fabs(pivot) < RALPH_PIVOT_TOL) {
        if (tab->trace_phase1_enabled) {
            tab->trace_last_fail_reason = PHASE1_PIVOT_FAIL_SMALL_PIVOT;
        }
        goto pivot_fail_rollback;
    }

    /* gamma_e already computed with the basic-variable update loop above. */
    if (gamma_e < 1.0) gamma_e = 1.0;

    /* Always compute pivot row for incremental reduced cost updates
     * pivot_row = e_r^T * B^{-1}
     * This is also used for steepest edge weight updates
     * Use pre-allocated workspace to avoid malloc in hot path
     */
    double *pivot_row = tab->pivot_row;
    double *tau_helper = tab->tau_work;

    /* Use sparse BTRAN since e_leaving has only 1 non-zero */
    int rhs_idx = leaving_pos;
    double rhs_val = 1.0;
    {
        double t_btran_ms = lp_telemetry_timer_start();
        lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, pivot_row);
        if (tab->owner) {
            lp_telemetry_add_btran_timed(tab->owner, t_btran_ms);
        }
    }

    /* Weight update strategy:
     * - SE (pricing_strategy==1): always use exact tau BTRAN
     * - Devex (pricing_strategy==2): use exact tau BTRAN while artificials remain
     *   in the basis (Phase 1), then switch to cheap Devex formula once all
     *   artificials are driven out. Accuracy matters in Phase 1 for feasibility;
     *   speed matters in Phase 2 for the bulk of iterations.
     */
    int artificials_in_basis = 0;
    if (tab->pricing_strategy == 2 && tab->num_artificial > 0) {
        for (int k = 0; k < tab->num_artificial; k++) {
            if (tab->var_status[tab->artificial_vars[k]] == RALPH_BASIC) {
                artificials_in_basis = 1;
                break;
            }
        }
    }
    int use_true_se = (tab->pricing_strategy == 1 || tab->pricing_strategy == 5) || artificials_in_basis;
    if (use_true_se && fabs(pivot_sq) > RALPH_ZERO_TOL) {
        double t_btran_ms = lp_telemetry_timer_start();
        lu_solve_transpose(tab->lu, tab->work2, tau_helper);
        if (tab->owner) {
            lp_telemetry_add_btran_timed(tab->owner, t_btran_ms);
        }
    }

    /* Update LU factorization.
     * For very small pivots, skip eta updates and refactorize immediately to
     * avoid accumulating unstable updates on near-singular bases. */
    const int force_refactor = fabs(pivot) < RALPH_FORCE_REFACTOR_PIVOT_TOL;
    if (!tab->A_ext || entering < 0 || entering >= tab->A_ext->ncols) {
        if (tab->trace_phase1_enabled) {
            tab->trace_last_fail_reason = PHASE1_PIVOT_FAIL_INVALID_COLUMN;
        }
        goto pivot_fail_rollback;  /* Invalid state */
    }

    int lu_update_status = 0;
    int lu_reason = LU_FAIL_NONE;
    int update_reason = LU_FAIL_NONE;
    int refactor_forced_path = 0;
    int skip_se_update = 0;  /* Flag to skip SE update after reset */
    double growth_factor = (tab->lu) ? tab->lu->growth_factor : 0.0;
    int lu_num_updates = (tab->lu) ? tab->lu->num_updates : 0;
    double growth_threshold = (tab->lu && tab->lu->growth_refactor_threshold > 0.0)
        ? tab->lu->growth_refactor_threshold
        : RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
    BasisAction action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             lu_num_updates,
                                             growth_factor,
                                             growth_threshold);

    for (;;) {
        switch (action) {
            case BASIS_ACTION_UPDATE:
                sparse_get_column(tab->A_ext, entering, tab->work1);
                {
                    double t_lu_update_ms = lp_telemetry_timer_start();
                    lu_update_status = lu_update(tab->lu, leaving_pos, tab->work1);
                    if (tab->owner) {
                        lp_telemetry_add_lu_update_timed(tab->owner, t_lu_update_ms);
                    }
                }
                if (lu_update_status == 0) {
                    goto basis_update_done;
                }
                lu_update_status = -1;
                update_reason = (tab->lu) ? tab->lu->last_failure_reason : LU_FAIL_NONE;
                lu_reason = update_reason;
                growth_factor = (tab->lu) ? tab->lu->growth_factor : growth_factor;
                lu_num_updates = (tab->lu) ? tab->lu->num_updates : lu_num_updates;
                action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             lu_num_updates,
                                             growth_factor,
                                             growth_threshold);
                continue;

            case BASIS_ACTION_REFACTOR:
                refactor_forced_path = (lu_update_status == 0);
                {
                    int ref_reason = phase1_small_pivot_refactor_allowed(
                                         force_refactor,
                                         repeat_pattern_count,
                                         lu_num_updates)
                                     ? RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT
                                     : RALPH_REFACTOR_REASON_UPDATE_RECOVERY;
                    double t_refactor_ms = lp_telemetry_timer_start();
                    lu_update_status = tableau_refactorize_with_reason(tab, ref_reason);
                    if (tab->owner) {
                        lp_telemetry_add_refactor_runtime_timed(tab->owner, t_refactor_ms);
                    }
                }
                if (lu_update_status == 0) {
                    goto basis_update_done;
                }
                lu_update_status = -2;
                lu_reason = (tab->lu) ? tab->lu->last_failure_reason : lu_reason;
                growth_factor = (tab->lu) ? tab->lu->growth_factor : growth_factor;
                lu_num_updates = (tab->lu) ? tab->lu->num_updates : lu_num_updates;
                action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             lu_num_updates,
                                             growth_factor,
                                             growth_threshold);
                continue;

            case BASIS_ACTION_REPAIR:
                if (repair_singular_basis(tab) == 0) {
                    goto basis_update_done;
                }
                lu_update_status = -3;
                lu_reason = (tab->lu) ? tab->lu->last_failure_reason : lu_reason;
                growth_factor = (tab->lu) ? tab->lu->growth_factor : growth_factor;
                lu_num_updates = (tab->lu) ? tab->lu->num_updates : lu_num_updates;
                action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             lu_num_updates,
                                             growth_factor,
                                             growth_threshold);
                continue;

            case BASIS_ACTION_ABORT:
            default:
                if (tab->trace_phase1_enabled) {
                    int fail_lu_reason = lu_reason;
                    if (!refactor_forced_path &&
                        (update_reason == LU_FAIL_MAX_UPDATES ||
                         update_reason == LU_FAIL_SPIKE_POOL_FULL ||
                         update_reason == LU_FAIL_UPDATE_PIVOT_TOO_SMALL ||
                         update_reason == LU_FAIL_SINGULAR_UPDATE)) {
                        fail_lu_reason = update_reason;
                    }
                    tab->trace_last_fail_reason =
                        phase1_trace_reason_from_lu_failure(fail_lu_reason, refactor_forced_path);
                }
                goto pivot_fail_rollback;
        }
    }

basis_update_done:

    /* Update steepest edge pricing weights
     *
     * True Steepest Edge (exact formula):
     *   gamma_j_new = gamma_j - 2*(alpha_j/pivot)*tau_j + (alpha_j/pivot)^2 * gamma_e
     * where:
     *   alpha_j = pivot_row * a_j (pivot row entry)
     *   tau_j = d_j' * d_entering = a_j' * (B^{-T} * d_entering)
     *   gamma_e = ||d_entering||^2 (entering column norm squared)
     *
     * Devex approximation (simpler but much faster):
     *   gamma_j = max(gamma_j, (alpha_j^2 * gamma_e) / pivot^2)
     * This doesn't need tau_helper, making it O(n) instead of O(n*m).
     */
    skip_se_update = 0;
    if (tab->use_steepest_edge) {
        tab->devex_refcount++;
        double pivot_inv_sq = 1.0 / (pivot * pivot);

        /* Weight for leaving variable (now nonbasic): gamma_e / pivot^2 */
        double leaving_weight = gamma_e * pivot_inv_sq;
        if (leaving_weight < 1.0) leaving_weight = 1.0;
        if (leaving_weight > 1e8) leaving_weight = 1e8;
        tab->se_weights[leaving] = leaving_weight;

        /* Periodic reference reset: recalculate weights from column norms */
        if (tab->devex_refcount >= 2 * tab->n) {
            for (int j = 0; j < tab->n; j++) {
                double col_norm_sq = 0.0;
                for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                    col_norm_sq += tab->A_ext->values[p] * tab->A_ext->values[p];
                }
                tab->se_weights[j] = (col_norm_sq > 1.0) ? col_norm_sq : 1.0;
            }
            tab->devex_refcount = 0;
            skip_se_update = 1;  /* Don't overwrite fresh reset values */
        }
    }

    /* Update reduced costs and weights in a single merged loop.
     *
     * In lazy RC mode (rc_all_valid == 0), skip the O(n) incremental update
     * and just invalidate duals. This saves O(n * avg_col_nnz) per iteration
     * at the cost of O(m²) BTRAN to recompute duals next iteration.
     * For large n with partial pricing (examining ~200-500 vars), this is faster.
     */
    if (!tab->rc_all_valid) {
        /* Lazy RC mode: skip incremental updates, invalidate duals */
        tab->duals_valid = 0;
        tab->rc[entering] = 0.0;  /* Basic variables have rc = 0 */
        return 0;
    }

    if (fabs(pivot) > RALPH_PIVOT_TOL) {
        double rc_enter = tab->rc[entering];
        double rc_ratio = rc_enter / pivot;
        double pivot_inv = 1.0 / pivot;
        int do_se_update = tab->use_steepest_edge && !skip_se_update;
        int use_heap = (tab->pricing_strategy == 4);
        if (use_heap) heap_remove(tab, entering);  /* entering → basic */

        /* Row-scatter RC update: accumulate alpha_j = pivot_row · A[:,j] via CSR rows.
         * Instead of scanning ALL n columns (O(n × avg_col_nnz)), we scatter from
         * non-zero pivot_row entries only (O(pivot_nnz × avg_row_nnz)).
         * For sparse problems this is much faster: bandm pivot_row ~30 nnz vs n=472. */
        double *alpha = tab->csr_alpha;  /* [n] scratch, kept zeroed between calls */
        int *touched = NULL;  /* Track which alpha[j] were set, for cleanup */
        int num_touched = 0;

        if (tab->csr_rowptr && tab->csr_use_scatter) {
            /* Use flip_list as scratch for touched indices (size n, not in use here) */
            touched = tab->flip_list;
            num_touched = 0;

            for (int i = 0; i < tab->m; i++) {
                double pi = pivot_row[i];
                if (fabs(pi) < RALPH_ZERO_TOL) continue;
                for (int p = tab->csr_rowptr[i]; p < tab->csr_rowptr[i+1]; p++) {
                    int j = tab->csr_colidx[p];
                    if (alpha[j] == 0.0) {
                        touched[num_touched++] = j;
                    }
                    alpha[j] += pi * tab->csr_values[p];
                }
            }

            /* Apply RC updates and Devex weights from accumulated alpha */
            for (int t = 0; t < num_touched; t++) {
                int j = touched[t];
                double alpha_j = alpha[j];
                alpha[j] = 0.0;  /* Clean up for next call */

                if (tab->var_status[j] == RALPH_BASIC || j == entering) continue;

                tab->rc[j] -= rc_ratio * alpha_j;
                if (use_heap) heap_update(tab, j);

                if (do_se_update && j != leaving) {
                    double alpha_ratio = alpha_j * pivot_inv;
                    double new_weight;
                    if (use_true_se) {
                        double tau_j = sparse_dot_column(tab->A_ext, j, tau_helper);
                        new_weight = tab->se_weights[j]
                                   - 2.0 * alpha_ratio * tau_j
                                   + alpha_ratio * alpha_ratio * gamma_e;
                    } else {
                        double candidate = alpha_ratio * alpha_ratio * gamma_e;
                        new_weight = tab->se_weights[j] * 0.999;
                        if (candidate > new_weight) new_weight = candidate;
                    }
                    if (new_weight < 1.0) new_weight = 1.0;
                    if (new_weight > 1e8) new_weight = 1e8;
                    tab->se_weights[j] = new_weight;
                }
            }
        } else {
            /* Fallback: original column-scan (CSR not available) */
            for (int j = 0; j < tab->n; j++) {
                if (tab->var_status[j] == RALPH_BASIC) continue;
                if (j == entering) continue;

                double alpha_j = 0.0;
                for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                    alpha_j += pivot_row[tab->A_ext->rowidx[p]] * tab->A_ext->values[p];
                }

                tab->rc[j] -= rc_ratio * alpha_j;
                if (use_heap) heap_update(tab, j);

                if (do_se_update && j != leaving) {
                    double alpha_ratio = alpha_j * pivot_inv;
                    double new_weight;
                    if (use_true_se) {
                        double tau_j = sparse_dot_column(tab->A_ext, j, tau_helper);
                        new_weight = tab->se_weights[j]
                                   - 2.0 * alpha_ratio * tau_j
                                   + alpha_ratio * alpha_ratio * gamma_e;
                    } else {
                        double candidate = alpha_ratio * alpha_ratio * gamma_e;
                        new_weight = tab->se_weights[j] * 0.999;
                        if (candidate > new_weight) new_weight = candidate;
                    }
                    if (new_weight < 1.0) new_weight = 1.0;
                    if (new_weight > 1e8) new_weight = 1e8;
                    tab->se_weights[j] = new_weight;
                }
            }
        }

        /* Reduced cost for entering variable (now basic) is 0 */
        tab->rc[entering] = 0.0;

        /* Reduced cost for leaving variable (now non-basic) */
        tab->rc[leaving] = -rc_enter / pivot;
        if (use_heap) heap_insert(tab, leaving);  /* leaving → non-basic */
    }

    return 0;

pivot_fail_rollback:
    /* Restore pre-pivot basis/status bookkeeping so caller can recover from a
     * known-good basis by refactorizing and re-running pricing/ratio. */
    tab->basis[leaving_pos] = leaving;
    tab->basis_pos[leaving] = leaving_pos;
    tab->basis_pos[entering] = -1;
    tab->var_status[leaving] = RALPH_BASIC;
    tab->var_status[entering] = entering_old_status;
    tab->x[entering] = x_enter_old;
    tab->x[leaving] = x_leave_old;
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    return -1;
}

/* ============================================================================
 * Primal Simplex Algorithm
 * ============================================================================ */

SimplexSolver* simplex_create(LPModel *model) {
    if (!model) return NULL;

    SimplexSolver *solver = (SimplexSolver*)calloc(1, sizeof(SimplexSolver));
    if (!solver) return NULL;

    solver->model = model;
    solver->status = RALPH_STATUS_UNKNOWN;

    /* Default parameters */
    solver->max_iterations = RALPH_DEFAULT_MAX_ITER;
    solver->time_limit = RALPH_DEFAULT_TIME_LIMIT;
    solver->presolve = 1;  /* Enable presolve for performance */
    solver->scaling = 1;   /* Enable scaling for numerical stability */
    solver->pricing_strategy = 2;  /* Devex pricing (better than Dantzig) */
    solver->ratio_test_mode = LP_RATIO_TEST_HARRIS;
    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_HARRIS;
    solver->verbose = 0;
    solver->telemetry_enabled = 1;
    solver->trace_phase1 = 0;
    solver->deterministic = 0;
    solver->random_seed = 0U;
    solver->lp_threads = 0;
    solver->determinism_effective_threads = 0;
    solver->is_scaled = 0;
    solver->trace_phase1_first_fail_iter = -1;
    solver->trace_phase1_last_fail_iter = -1;
    solver->objective_limit = RALPH_INFINITY;
    solver->phase1_pricing = -1;  /* Default: disabled (use solver pricing) */
    solver->use_dual_bound_flip = 1;
    solver->use_dual_steepest_edge = 1;
    solver->glpk_strict_mode = 0;
    solver->smcp_tol_bnd = 1e-7;
    solver->smcp_tol_dj = 1e-7;
    solver->smcp_tol_piv = 1e-9;
    solver->smcp_excl = 1;
    solver->smcp_shift = 1;
    solver->smcp_aorn = 2;
    solver->method = 2;  /* Default: auto (dual first, primal fallback) */
    solver->lu_backend_policy = LP_LU_BACKEND_POLICY_AUTO;
    solver->lu_update_limit_override = -1;
    solver->lu_pivot_tol_override = 0.0;
    solver->lu_growth_guard_override = 0.0;
    solver->has_lp_progress_callback = 0;
    solver->has_lp_cancel_callback = 0;
    solver->progress_start_ms = 0.0;
    solver->warm_basis_last_attempted = 0;
    solver->warm_basis_last_applied = 0;
    solver->warm_basis_last_rejected = 0;
    solver->unbounded_valid = 0;
    solver->policy.basis_governor_mode = LP_BASIS_GOV_MODE_OFF;
    solver->policy.reinvert_controller_mode = LP_REINVERT_MODE_SHADOW;
    lp_basis_governor_set_mode(&solver->policy.basis_governor,
                               solver->policy.basis_governor_mode);
    solver->policy.soft_lu_cost_gate_enabled = 1;
    solver->policy.periodic_cost_gate_enabled = 1;
    solver->policy.dual_refactor_base_interval = 50;
    solver->policy.dual_rc_recompute_interval = 20;

    return solver;
}

int simplex_set_warm_basis(SimplexSolver *solver, int m, int n,
                           const int *basis, const VarStatus *var_status) {
    if (!solver) return -1;
    if (m < 0 || n < 0) return -1;
    if ((m > 0 && !basis) || (n > 0 && !var_status)) return -1;

    int *basis_copy = NULL;
    VarStatus *status_copy = NULL;

    if (m > 0) {
        basis_copy = (int*)malloc((size_t)m * sizeof(int));
        if (!basis_copy) return -1;
        memcpy(basis_copy, basis, (size_t)m * sizeof(int));
    }

    if (n > 0) {
        status_copy = (VarStatus*)malloc((size_t)n * sizeof(VarStatus));
        if (!status_copy) {
            free(basis_copy);
            return -1;
        }
        memcpy(status_copy, var_status, (size_t)n * sizeof(VarStatus));
    }

    free(solver->warm_basis);
    free(solver->warm_var_status);
    solver->warm_basis = basis_copy;
    solver->warm_var_status = status_copy;
    solver->warm_basis_m = m;
    solver->warm_basis_n = n;
    return 0;
}

void simplex_free(SimplexSolver *solver) {
    if (!solver) return;

    tableau_free(solver->tableau);
    free(solver->warm_basis);
    free(solver->warm_var_status);
    free(solver->solution);
    free(solver->dual_solution);
    free(solver->reduced_costs);
    free(solver->row_scale);
    free(solver->col_scale);
    free(solver->farkas_ray);
    free(solver->unbounded_ray);
    free(solver);
}

/*
 * Extract Farkas ray (certificate of infeasibility)
 *
 * When the LP is infeasible, the dual values y from Phase 1 satisfy:
 *   y'A >= 0 for all columns (adjusted for constraint sense)
 *   y'b < 0
 *
 * This proves no feasible solution exists via Farkas lemma.
 * The ray is stored in solver->farkas_ray for retrieval via API.
 */
static void extract_farkas_ray(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;
    int m = tab->m;

    /* Allocate if needed */
    if (!solver->farkas_ray) {
        solver->farkas_ray = (double*)calloc(m, sizeof(double));
    }
    if (!solver->farkas_ray) {
        solver->farkas_valid = 0;
        return;
    }

    /* The dual values y = c_B' * B^{-1} from Phase 1 give the Farkas ray.
     * IMPORTANT: This must be called while still in Phase 1, before restoring
     * the original objective. The Phase 1 c_ext has:
     *   - 0 for structural variables
     *   - 1 for artificial variables
     *
     * The returned ray satisfies y'A >= 0 for all original columns (in standard
     * form) and y'b_eff < 0, where b_eff accounts for constraint senses:
     *   - For <= constraints: b_eff = b
     *   - For >= constraints: b_eff = -b (since Ax >= b becomes -Ax <= -b)
     *   - For = constraints: b_eff = b (arbitrary sign)
     *
     * Row_sign tracks row normalization (when b < 0 was made positive) but we
     * return the ray in tableau space. Users apply sense transformations when
     * computing y'b. */

    /* Compute y = c_B' * B^{-1} via BTRAN with Phase 1 costs */
    tableau_compute_reduced_costs(tab);

    /* Copy dual values - these are the Farkas multipliers in tableau space */
    double max_abs = 0.0;
    for (int i = 0; i < m; i++) {
        solver->farkas_ray[i] = tab->y[i];
        double absval = fabs(tab->y[i]);
        if (absval > max_abs) max_abs = absval;
    }

    /* Validation: Farkas ray must be nontrivial */
    if (max_abs < 1e-9) {
        solver->farkas_valid = 0;
        if (solver->verbose) {
            LP_LOG_STDERR("[extract_farkas_ray] WARNING: Farkas ray is all zeros\n");
        }
        return;
    }

    /* Debug validation: verify y'b_tab < 0 (Farkas lemma requirement)
     * b_tab is the normalized RHS (all non-negative after row transformations).
     * For a valid certificate, the dot product must be negative. */
    double y_tab_dot_rhs = 0.0;
    for (int i = 0; i < m; i++) {
        y_tab_dot_rhs += tab->y[i] * tab->rhs[i];
    }

    if (y_tab_dot_rhs >= -1e-6) {
        /* This shouldn't happen if the Farkas extraction is correct */
        if (solver->verbose) {
            LP_LOG_STDERR("[extract_farkas_ray] WARNING: y'b_tab = %.6e (expected < 0)\n",
                    y_tab_dot_rhs);
        }
        /* Don't invalidate - this might be a borderline numerical case.
         * The ray can still be used, but user should be aware. */
    } else if (solver->verbose >= 2) {
        LP_LOG_STDERR("[extract_farkas_ray] y'b_tab = %.6e < 0 (valid)\n", y_tab_dot_rhs);
    }

    solver->farkas_valid = 1;

    if (solver->verbose >= 2) {
        LP_LOG_STDERR("[extract_farkas_ray] Valid certificate: ||y||_inf = %.6e\n", max_abs);
    }
}

/* Extract primal unbounded ray in original variable space.
 *
 * At unbounded detection, ratio test found no blocking leaving row for the
 * entering variable direction. With d = B^{-1} a_enter and direction sign dir,
 * the primal ray is:
 *   delta_enter = dir
 *   delta_basic = -(d * dir)
 * Non-basic non-entering variables stay fixed.
 */
static void extract_unbounded_ray(SimplexSolver *solver, int entering, double dir) {
    if (!solver || !solver->tableau || !solver->model) return;

    SimplexTableau *tab = solver->tableau;
    int n_orig = solver->model->num_vars;
    if (n_orig <= 0) {
        solver->unbounded_valid = 0;
        return;
    }

    if (!solver->unbounded_ray) {
        solver->unbounded_ray = (double*)calloc((size_t)n_orig, sizeof(double));
    }
    if (!solver->unbounded_ray) {
        solver->unbounded_valid = 0;
        return;
    }
    memset(solver->unbounded_ray, 0, (size_t)n_orig * sizeof(double));

    if (!(fabs(dir) > 0.5)) dir = 1.0;

    if (entering >= 0 && entering < n_orig) {
        solver->unbounded_ray[entering] = dir;
    }

    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (j >= 0 && j < n_orig) {
            solver->unbounded_ray[j] = -tab->work2[k] * dir;
        }
    }

    /* Sanity-check that the direction is non-trivial and objective-improving
     * in internal minimization space. */
    double max_abs = 0.0;
    double obj_dot = 0.0;
    for (int j = 0; j < n_orig; j++) {
        double v = solver->unbounded_ray[j];
        if (fabs(v) > max_abs) max_abs = fabs(v);
        obj_dot += solver->model->c[j] * solver->model->obj_sense * v;
    }

    if (max_abs <= 1e-14 || !(obj_dot < -1e-12)) {
        solver->unbounded_valid = 0;
        return;
    }

    solver->unbounded_valid = 1;
}

/* ============================================================================
 * Bound Perturbation for Degeneracy Prevention (Primal Simplex)
 * ============================================================================
 *
 * Adds small perturbations to bounds to break degeneracy and prevent cycling.
 * This is proactive (applied at start) vs reactive (Bland's rule after cycling).
 *
 * The perturbation scheme:
 * - Perturb lower bounds down by small epsilon
 * - Perturb upper bounds up by small epsilon
 * - Use pseudo-random scaling based on variable index for reproducibility
 */
#define PRIMAL_PERTURB_BASE 1e-6
#define PRIMAL_PERTURB_MULT 7

/* Linear scan is fine here: perturbation is infrequent and num_artificial << n. */
static int is_artificial_var(const SimplexTableau *tab, int var_idx) {
    if (!tab || !tab->artificial_vars || tab->num_artificial <= 0) {
        return 0;
    }
    for (int k = 0; k < tab->num_artificial; k++) {
        if (tab->artificial_vars[k] == var_idx) {
            return 1;
        }
    }
    return 0;
}

/* Mark basic rows backed by artificial variables as redundant hints for LU.
 * If only_infeasible is non-zero, only rows with bound-infeasible basic
 * artificials are marked. */
static int mark_basic_artificial_rows_redundant(SimplexTableau *tab, int only_infeasible) {
    if (!tab || !tab->redundant_rows) {
        return 0;
    }

    int marked = 0;
    for (int k = 0; k < tab->m; k++) {
        if (tab->redundant_rows[k]) continue;

        int bj = tab->basis[k];
        if (!is_artificial_var(tab, bj)) continue;

        if (only_infeasible) {
            if (tab->x[bj] >= tab->lb_ext[bj] - RALPH_FEAS_TOL &&
                tab->x[bj] <= tab->ub_ext[bj] + RALPH_FEAS_TOL) {
                continue;
            }
        }

        tab->redundant_rows[k] = 1;
        tab->num_redundant++;
        marked++;
    }

    return marked;
}

static void primal_remove_perturbation(SimplexTableau *tab);

static void primal_apply_perturbation(SimplexTableau *tab) {
    if (!simplex_smcp_shift_allows_perturb(tab)) return;
    int n = tab->n;

    /* Free any existing perturbation state */
    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);

    /* Save original bounds */
    tab->primal_saved_lb = (double*)calloc(n, sizeof(double));
    tab->primal_saved_ub = (double*)calloc(n, sizeof(double));
    if (!tab->primal_saved_lb || !tab->primal_saved_ub) {
        free(tab->primal_saved_lb);
        free(tab->primal_saved_ub);
        tab->primal_saved_lb = tab->primal_saved_ub = NULL;
        tab->primal_perturb_active = 0;
        return;
    }

    for (int j = 0; j < n; j++) {
        tab->primal_saved_lb[j] = tab->lb_ext[j];
        tab->primal_saved_ub[j] = tab->ub_ext[j];
    }

    /* Apply perturbations to bounds only.
     * Non-basic variable x values stay at their current (original) bound values.
     * This widens the feasible region so basic variables have positive slack.
     * Note: Do NOT update x values here - that would change the RHS and
     * potentially worsen numerical stability. The key insight is that
     * for the ratio test, only basic variable slacks matter, and those
     * are computed from (x_j - lb_j) where x_j is unchanged and lb_j is now lower. */
    for (int j = 0; j < n; j++) {
        /* In Phase 1, keep artificial bounds exact.
         * Perturbing artificials changes the feasibility objective geometry and
         * can produce false "optimal" Phase 1 terminations on hard instances. */
        if (tab->phase == 1 && is_artificial_var(tab, j)) {
            continue;
        }

        unsigned int offset = lp_determinism_seed_offset(tab->owner, j, 13U);
        unsigned int pattern = (unsigned int)((j * PRIMAL_PERTURB_MULT) % 13);
        double factor = 1.0 + (double)((pattern + offset) % 13U);

        /* Perturb finite lower bounds down */
        if (tab->lb_ext[j] > -RALPH_INFINITY / 2) {
            double eps = PRIMAL_PERTURB_BASE * factor * (1.0 + fabs(tab->lb_ext[j]));
            tab->lb_ext[j] -= eps;
        }

        /* Perturb finite upper bounds up */
        if (tab->ub_ext[j] < RALPH_INFINITY / 2) {
            double eps = PRIMAL_PERTURB_BASE * factor * (1.0 + fabs(tab->ub_ext[j]));
            tab->ub_ext[j] += eps;
        }
    }

    tab->primal_perturb_active = 1;
}

/* Scaled variant for stall-recovery re-perturbation.
 * scale > 1.0 widens the perturbation to break a different cycling pattern. */
static void primal_apply_perturbation_scaled(SimplexTableau *tab, double scale) {
    if (!simplex_smcp_shift_allows_perturb(tab)) return;
    int n = tab->n;

    /* If perturbation is already active, remove it first to start fresh */
    if (tab->primal_perturb_active) {
        primal_remove_perturbation(tab);
    }

    /* Allocate and save original bounds */
    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);
    tab->primal_saved_lb = (double*)calloc(n, sizeof(double));
    tab->primal_saved_ub = (double*)calloc(n, sizeof(double));
    if (!tab->primal_saved_lb || !tab->primal_saved_ub) {
        free(tab->primal_saved_lb);
        free(tab->primal_saved_ub);
        tab->primal_saved_lb = tab->primal_saved_ub = NULL;
        tab->primal_perturb_active = 0;
        return;
    }

    for (int j = 0; j < n; j++) {
        tab->primal_saved_lb[j] = tab->lb_ext[j];
        tab->primal_saved_ub[j] = tab->ub_ext[j];
    }

    double base = PRIMAL_PERTURB_BASE * scale;

    for (int j = 0; j < n; j++) {
        if (tab->phase == 1 && is_artificial_var(tab, j)) continue;

        unsigned int offset = lp_determinism_seed_offset(tab->owner, j, 13U);
        unsigned int pattern = (unsigned int)((j * PRIMAL_PERTURB_MULT) % 13);
        double factor = 1.0 + (double)((pattern + offset) % 13U);

        if (tab->lb_ext[j] > -RALPH_INFINITY / 2) {
            double eps = base * factor * (1.0 + fabs(tab->lb_ext[j]));
            tab->lb_ext[j] -= eps;
        }
        if (tab->ub_ext[j] < RALPH_INFINITY / 2) {
            double eps = base * factor * (1.0 + fabs(tab->ub_ext[j]));
            tab->ub_ext[j] += eps;
        }
    }

    tab->primal_perturb_active = 1;
}

static void primal_remove_perturbation(SimplexTableau *tab) {
    if (!tab->primal_perturb_active || !tab->primal_saved_lb || !tab->primal_saved_ub) {
        return;
    }

    /* Restore original bounds and reset non-basic variable values */
    for (int j = 0; j < tab->n; j++) {
        tab->lb_ext[j] = tab->primal_saved_lb[j];
        tab->ub_ext[j] = tab->primal_saved_ub[j];

        /* Reset non-basic variables to their proper bounds */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER) {
            tab->x[j] = tab->lb_ext[j];
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            tab->x[j] = tab->ub_ext[j];
        }
        /* Basic variables will be recomputed by tableau_compute_solution */
    }

    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);
    tab->primal_saved_lb = tab->primal_saved_ub = NULL;
    tab->primal_perturb_active = 0;
}

/* ============================================================================
 * Two-Phase Simplex Implementation
 * ============================================================================ */

/*
 * Phase 1: Minimize sum of artificial variables.
 * Returns 0 if feasible (all artificials driven to zero), -1 if infeasible.
 */
static int simplex_phase1(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;

    if (!tab->use_two_phase) {
        /* No artificials: check and restore feasibility via dual pivoting */
        tableau_compute_solution(tab);

        int infeasible = 0;
        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL ||
                tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
                infeasible = 1;
                break;
            }
        }

        if (!infeasible) {
            return 0;  /* Already feasible */
        }

        /* Restore feasibility via dual pivoting */
        for (int iter = 0; iter < solver->max_iterations; iter++) {
            if (lp_time_limit_exceeded(solver, iter)) {
                return -1;
            }
            tableau_compute_solution(tab);

            int most_infeas_k = -1;
            double max_infeas = RALPH_FEAS_TOL;

            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                double infeas = 0.0;

                if (tab->x[j] < tab->lb_ext[j]) {
                    infeas = tab->lb_ext[j] - tab->x[j];
                } else if (tab->x[j] > tab->ub_ext[j]) {
                    infeas = tab->x[j] - tab->ub_ext[j];
                }

                if (infeas > max_infeas) {
                    max_infeas = infeas;
                    most_infeas_k = k;
                }
            }

            if (most_infeas_k < 0) {
                return 0;  /* Feasible */
            }

            /* Dual pivot */
            int leaving = most_infeas_k;
            int j_leave = tab->basis[leaving];

            /* Find entering variable by dual ratio test */
            vec_set_zero(tab->work1, tab->m);
            tab->work1[leaving] = 1.0;
            lu_solve_transpose(tab->lu, tab->work1, tab->work2);

            int entering = -1;
            double best_ratio = RALPH_INFINITY;
            int dir = (tab->x[j_leave] < tab->lb_ext[j_leave]) ? 1 : -1;

            for (int jj = 0; jj < tab->n; jj++) {
                if (tab->var_status[jj] == RALPH_BASIC) continue;

                double alpha = sparse_dot_column(tab->A_ext, jj, tab->work2);
                if (fabs(alpha) < RALPH_PIVOT_TOL) continue;

                double rc = tab->rc[jj];
                double ratio = RALPH_INFINITY;

                if (dir > 0 && alpha > RALPH_PIVOT_TOL &&
                    tab->var_status[jj] == RALPH_NONBASIC_LOWER) {
                    ratio = -rc / alpha;
                } else if (dir > 0 && alpha < -RALPH_PIVOT_TOL &&
                           tab->var_status[jj] == RALPH_NONBASIC_UPPER) {
                    ratio = rc / (-alpha);
                } else if (dir < 0 && alpha < -RALPH_PIVOT_TOL &&
                           tab->var_status[jj] == RALPH_NONBASIC_LOWER) {
                    ratio = -rc / (-alpha);
                } else if (dir < 0 && alpha > RALPH_PIVOT_TOL &&
                           tab->var_status[jj] == RALPH_NONBASIC_UPPER) {
                    ratio = rc / alpha;
                }

                if (ratio >= 0 && ratio < best_ratio) {
                    best_ratio = ratio;
                    entering = jj;
                }
            }

            if (entering < 0) {
                extract_farkas_ray(solver);
                solver->status = RALPH_STATUS_INFEASIBLE;
                return -1;
            }

            /* Perform pivot */
            int col_nnz;
            const int *col_idx;
            const double *col_val;
            sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);
            lu_solve_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2);

            double theta = max_infeas / fabs(tab->work2[leaving]);
            {
                double t_pivot_ms = lp_telemetry_timer_start();
                simplex_pivot(tab, entering, leaving, theta, 0);
                lp_telemetry_record_pivot_timed(solver, 1, t_pivot_ms);
            }

            if (lu_needs_refactorization(tab->lu)) {
                tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
            }
            tableau_compute_reduced_costs(tab);
        }

        solver->status = RALPH_STATUS_ITERATION_LIMIT;
        return -1;
    }

    /* Two-phase method: Phase 1 minimizes sum of artificial variables */
    tab->phase = 1;

    if (solver->verbose) {
        LP_LOG_STDERR("[simplex_phase1] Starting Phase 1 with %d artificial variables, %d equalities\n",
                tab->num_artificial, tab->num_equalities);
    }

    /* Compute initial solution */
    tableau_compute_solution(tab);

    /* Check if we're already feasible (all artificials at zero) */
    double art_sum = 0.0;
    for (int k = 0; k < tab->num_artificial; k++) {
        int j = tab->artificial_vars[k];
        art_sum += fabs(tab->x[j]);
    }

    if (art_sum < RALPH_FEAS_TOL) {
        if (solver->verbose) {
            LP_LOG_STDERR("[simplex_phase1] Already feasible, skipping Phase 1\n");
        }
        phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
        return 0;
    }

    /* Cycling detection and anti-cycling measures */
    int degenerate_count = 0;
    const int DEGEN_THRESHOLD =
        (tab->m >= PHASE1_DEGEN_THRESHOLD_LARGE_M)
            ? PHASE1_DEGEN_THRESHOLD_LARGE
            : PHASE1_DEGEN_THRESHOLD_DEFAULT;    /* Switch to Bland's rule after this many */
    const int RECOMPUTE_INTERVAL = 25; /* Periodic drift correction in Phase 1 */
    int use_bland = 0;

    /* Phase 1 stall detection: track objective (art_sum) progress.
     * When Phase 1 stalls with Bland's rule, re-perturbation breaks the cycle.
     * primal_apply_perturbation_scaled skips artificial bounds (Phase 1 safe). */
    double last_obj_p1 = tab->obj_value;
    int stall_count_p1 = 0;
    const int P1_STALL_THRESHOLD =
        (tab->m >= PHASE1_DEGEN_THRESHOLD_LARGE_M)
            ? PHASE1_STALL_THRESHOLD_LARGE
            : PHASE1_STALL_THRESHOLD_DEFAULT;
    int perturb_attempts_p1 = 0;
    const int P1_MAX_PERTURB_ATTEMPTS = 15;
    int fail_entering = -1;
    int fail_leaving_pos = -1;
    int fail_reason = PHASE1_PIVOT_FAIL_NONE;
    int fail_repeat_count = 0;
    int ratio_breakdown_count = 0;
    int ratio_breakdown_last_entering = -1;
    int ratio_breakdown_same_entering_streak = 0;
    int excluded_entering_a = -1;
    int excluded_entering_ttl_a = 0;
    int excluded_entering_b = -1;
    int excluded_entering_ttl_b = 0;
    int dir_stabilize_cooldown = 0;
    int dir_stabilize_repeat_count = 0;
    int dir_stabilize_moderate_defer_pending = 0;
    int no_entering_cleanup_streak = 0;
    int phase1_no_pivot_streak = 0;
    int phase1_no_pivot_no_progress_streak = 0;
    double phase1_no_pivot_prev_art_sum = RALPH_INFINITY;
    double phase1_no_pivot_anchor_art_sum = RALPH_INFINITY;
    int phase1_no_pivot_progress_window_steps = 0;
    int phase1_no_pivot_force_pending = 0;
    LPPhase1NoPivotForceReason phase1_no_pivot_force_reason =
        LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
    int phase1_no_pivot_force_cooldown = 0;
    int phase1_no_pivot_ladder_rescue_cooldown = 0;
    int phase1_no_pivot_ladder_rescue_fail_streak = 0;
    int phase1_rc_only_streak = 0;
    int phase1_dir_skip_event_streak = 0;
    int phase1_dir_skip_no_recompute_streak = 0;
    int phase1_dir_escape_cooldown = 0;
    int phase1_force_pivot_attempt_budget = 0;
    int periodic_policy_cooldown = 0;
    double periodic_policy_pressure_decay = 0.0;
    int lu_soft_health_streak = 0;
    int phase1_pricing_strategy =
        (solver->phase1_pricing >= 0) ? solver->phase1_pricing : solver->pricing_strategy;
    int phase1_auto_dantzig_enabled = 0;

    /* Apply proactive perturbation in Phase 1 for highly-degenerate two-phase
     * problems. Phase 1 is inherently degenerate (many bases give art_sum=0).
     * Only apply when equality ratio is very high (>90%) — lower thresholds
     * cause -O3 code layout shifts that regress brandy (instruction cache
     * alignment sensitivity). For problems with fewer equalities (e.g.,
     * beaconfd at 81%), reactive perturbation via cycling detection suffices.
     * Perturbation is removed at Phase 1 completion (primal_remove_perturbation). */
    if (tab->use_two_phase && tab->num_equalities > (tab->m * 9) / 10) {
        primal_apply_perturbation(tab);
        if (solver->verbose) {
            LP_LOG_STDERR("[simplex_phase1] Proactive perturbation: %d equalities out of %d constraints (%.0f%%)\n",
                    tab->num_equalities, tab->m, 100.0 * tab->num_equalities / tab->m);
        }
        phase1_recompute_full_with_reason(solver,
                                          tab,
                                          &phase1_rc_only_streak,
                                          LP_PHASE1_RECOMPUTE_REASON_PERTURB);
    }

    /* Compute initial reduced costs */
    tableau_compute_reduced_costs(tab);
    if (phase1_pricing_strategy == 4) heap_build(tab);
    double phase1_hot_ms_prev = phase_hotpath_ms(solver, 1);

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tab->iterations = iter;
        tab->trace_phase1_iter = iter;
        if (lp_run_user_callbacks(solver, tab, RALPH_LP_PROGRESS_PHASE_1, iter, 0, 1) != 0) {
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_TIME_LIMIT;
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
            return -1;
        }
        if (excluded_entering_ttl_a > 0) {
            excluded_entering_ttl_a--;
            if (excluded_entering_ttl_a == 0) {
                excluded_entering_a = -1;
            }
        }
        if (excluded_entering_ttl_b > 0) {
            excluded_entering_ttl_b--;
            if (excluded_entering_ttl_b == 0) {
                excluded_entering_b = -1;
            }
        }
        if (dir_stabilize_cooldown > 0) {
            dir_stabilize_cooldown--;
        }
        if (phase1_no_pivot_force_cooldown > 0) {
            phase1_no_pivot_force_cooldown--;
        }
        if (phase1_no_pivot_ladder_rescue_cooldown > 0) {
            phase1_no_pivot_ladder_rescue_cooldown--;
        }
        if (phase1_dir_escape_cooldown > 0) {
            phase1_dir_escape_cooldown--;
        }
        if (periodic_policy_cooldown > 0) {
            periodic_policy_cooldown--;
        }
        if (periodic_policy_pressure_decay > 0.0) {
            periodic_policy_pressure_decay -= PHASE1_POLICY_PRESSURE_RECOVERY_STEP;
            if (periodic_policy_pressure_decay < 0.0) {
                periodic_policy_pressure_decay = 0.0;
            }
        }

        if (!phase1_auto_dantzig_enabled &&
            solver->phase1_pricing < 0 &&
            tab->use_two_phase &&
            tab->m >= PHASE1_AUTO_DANTZIG_MIN_M &&
            tab->m <= PHASE1_AUTO_DANTZIG_MAX_M &&
            degenerate_count >= PHASE1_AUTO_DANTZIG_DEGEN_TRIGGER) {
            phase1_pricing_strategy = 0;  /* Dantzig */
            phase1_auto_dantzig_enabled = 1;
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Switching pricing to Dantzig under large degenerate Phase 1 workload (m=%d, degen=%d)\n",
                        tab->m, degenerate_count);
            }
        }

        if (phase1_no_pivot_force_pending) {
            phase1_no_pivot_force_pending = 0;
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] No-pivot streak force refactor (%s)\n",
                        phase1_no_pivot_force_reason_string(phase1_no_pivot_force_reason));
            }
            phase1_no_pivot_force_reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
            lp_telemetry_record_phase1_dir_stabilize_refactor_trigger(
                solver,
                PHASE1_DIR_REFACTOR_TELEM_NO_PIVOT_FORCE);
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) == 0) {
                use_bland = 1;
                ratio_breakdown_count = 0;
                ratio_breakdown_last_entering = -1;
                ratio_breakdown_same_entering_streak = 0;
                phase1_dir_skip_event_streak = 0;
                phase1_dir_skip_no_recompute_streak = 0;
                phase1_dir_escape_cooldown = 0;
                phase1_no_pivot_progress_reset(
                    &phase1_no_pivot_no_progress_streak,
                    &phase1_no_pivot_prev_art_sum,
                    &phase1_no_pivot_anchor_art_sum,
                    &phase1_no_pivot_progress_window_steps);
                phase1_no_pivot_ladder_rescue_cooldown = 0;
                phase1_no_pivot_ladder_rescue_fail_streak = 0;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &phase1_rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR);
                continue;
            }
        }

        /* Pricing: select entering variable */
        int entering;
        int price_status;
        double t_pricing_ms = lp_telemetry_timer_start();

        if (use_bland) {
            price_status = pricing_bland(tab, &entering);
        } else if (phase1_pricing_strategy == 0) {
            price_status = pricing_dantzig(tab, &entering);
        } else if (phase1_pricing_strategy == 1) {
            price_status = pricing_steepest_edge(tab, &entering);
        } else if (phase1_pricing_strategy == 3) {
            price_status = pricing_partial(tab, &entering);
        } else if (phase1_pricing_strategy == 4) {
            price_status = pricing_heap(tab, &entering);
        } else {
            price_status = pricing_devex(tab, &entering);
        }

        if ((excluded_entering_ttl_a > 0 || excluded_entering_ttl_b > 0) &&
            entering >= 0 &&
            (entering == excluded_entering_a || entering == excluded_entering_b)) {
            int alt_entering = -1;
            int exclude_a = (excluded_entering_ttl_a > 0) ? excluded_entering_a : -1;
            int exclude_b = (excluded_entering_ttl_b > 0) ? excluded_entering_b : -1;
            if (pricing_bland_excluding_two(tab, exclude_a, exclude_b, &alt_entering) == 0) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Excluding unstable entering (%d,%d), using %d instead\n",
                            exclude_a, exclude_b, alt_entering);
                }
                entering = alt_entering;
            }
        }
        {
            lp_telemetry_record_pricing_timed(solver, 1, t_pricing_ms);
        }

        if (price_status != 0) {
            phase1_trace_record_no_entering(solver, iter, price_status);

            /* Optimal for Phase 1 - remove perturbation first, then check */
            primal_remove_perturbation(tab);

            /* Recompute solution without perturbation */
            tableau_compute_solution(tab);

            /* Check if all artificial variables are zero */
            art_sum = 0.0;
            for (int k = 0; k < tab->num_artificial; k++) {
                int j = tab->artificial_vars[k];
                art_sum += fabs(tab->x[j]);
            }

            if (art_sum > RALPH_FEAS_TOL) {
                /* Small residual might be fixable with a few more iterations.
                 * Use a relaxed tolerance (1e-4) to distinguish true infeasibility
                 * from numerical noise. */
                if (art_sum > 1e-4) {
                    no_entering_cleanup_streak = 0;
                    /* Revalidate on a freshly factorized basis before certifying infeasible.
                     * This guards against RC/solution drift on numerically hard instances. */
                    if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
                        tableau_compute_solution(tab);
                        tableau_compute_reduced_costs(tab);

                        double refined_art_sum = 0.0;
                        for (int k = 0; k < tab->num_artificial; k++) {
                            int j = tab->artificial_vars[k];
                            refined_art_sum += fabs(tab->x[j]);
                        }

                        if (refined_art_sum <= 1e-4) {
                            if (solver->verbose) {
                                LP_LOG_STDERR("[simplex_phase1] Refactorized cleanup: art_sum %g -> %g\n",
                                        art_sum, refined_art_sum);
                            }
                            continue;
                        }
                        art_sum = refined_art_sum;
                    }

                    /* Truly infeasible - extract Farkas ray from Phase 1 duals.
                     * The Phase 1 duals y = c_B^T * B^{-1} provide the certificate. */
                    if (solver->verbose) {
                        LP_LOG_STDERR("[simplex_phase1] INFEASIBLE: artificial sum = %g after %d iterations\n",
                                art_sum, iter);
                    }
                    extract_farkas_ray(solver);
                    solver->status = RALPH_STATUS_INFEASIBLE;
                    solver->iterations = iter;
                    phase1_trace_emit_summary(solver, RALPH_STATUS_INFEASIBLE);
                    return -1;
                }

                /* Small residual - try to clean up with a few more iterations */
                no_entering_cleanup_streak++;
                if (no_entering_cleanup_streak >= PHASE1_NO_ENTERING_CLEANUP_MAX_ITERS) {
                    if (solver->verbose) {
                        LP_LOG_STDERR("[simplex_phase1] Accepting Phase 1 feasibility after %d no-entering cleanup iterations (art_sum=%g)\n",
                                no_entering_cleanup_streak, art_sum);
                    }
                    solver->iterations = iter;
                    phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
                    return 0;
                }
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Cleanup phase: art_sum=%g, continuing...\n", art_sum);
                }
                phase1_recompute_rc_only_guarded(solver, tab, &phase1_rc_only_streak);
                continue;  /* Try more iterations to drive artificials to zero */
            }

            /* Success */
            no_entering_cleanup_streak = 0;
            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] Phase 1 complete: feasible in %d iterations\n", iter);
            }
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
            return 0;
        }

        no_entering_cleanup_streak = 0;

        /* Ratio test: select leaving variable */
        int leaving;
        double theta;
        double t_ratio_ms = lp_telemetry_timer_start();
        int ratio_status = primal_ratio_test_with_policy(solver,
                                                         tab,
                                                         0,
                                                         entering,
                                                         &leaving,
                                                         &theta);
        {
            lp_telemetry_record_ratio_timed(solver, 1, t_ratio_ms);
        }

        if (ratio_status != 0) {
            phase1_trace_record_no_entering(solver, iter, ratio_status);
            if (phase1_note_no_pivot_and_maybe_force(
                    solver,
                    tab->m,
                    degenerate_count,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
                    &phase1_no_pivot_streak,
                    &phase1_no_pivot_force_cooldown)) {
                phase1_no_pivot_force_pending = 1;
                phase1_no_pivot_force_reason =
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN;
            }

            phase1_no_pivot_progress_update(solver,
                                            tab,
                                            &phase1_no_pivot_prev_art_sum,
                                            &phase1_no_pivot_anchor_art_sum,
                                            &phase1_no_pivot_progress_window_steps,
                                            &phase1_no_pivot_no_progress_streak);
            {
                int no_pivot_ladder_threshold = 0;
                int no_pivot_force_mode_active =
                    (phase1_force_pivot_attempt_budget > 0);
                int no_pivot_ladder_step = phase1_no_pivot_ladder_step(
                    tab->m,
                    degenerate_count,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
                    phase1_no_pivot_streak,
                    phase1_no_pivot_no_progress_streak,
                    no_pivot_force_mode_active,
                    &no_pivot_ladder_threshold);
                no_pivot_ladder_step = phase1_no_pivot_ladder_apply_rescue_guard(
                    solver,
                    no_pivot_ladder_step,
                    phase1_no_pivot_ladder_rescue_cooldown,
                    phase1_no_pivot_ladder_rescue_fail_streak);
                if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_RETRY) {
                    lp_telemetry_record_phase1_no_pivot_ladder_retry(
                        solver,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN);
                    lp_telemetry_record_phase1_ratio_breakdown_retry(solver);
                    phase1_exclude_entering_var(entering,
                                                RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                                &excluded_entering_a,
                                                &excluded_entering_ttl_a,
                                                &excluded_entering_b,
                                                &excluded_entering_ttl_b);
                    use_bland = 1;
                    phase1_recompute_rc_only_guarded(solver,
                                                     tab,
                                                     &phase1_rc_only_streak);
                    continue;
                }
                if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                    int rescue_result = phase1_attempt_ladder_dual_rescue(
                        solver,
                        tab,
                        iter,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
                        LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN,
                        &phase1_no_pivot_ladder_rescue_cooldown,
                        &phase1_no_pivot_ladder_rescue_fail_streak,
                        &phase1_rc_only_streak,
                        &phase1_no_pivot_no_progress_streak,
                        &phase1_no_pivot_prev_art_sum,
                        &phase1_no_pivot_anchor_art_sum,
                        &phase1_no_pivot_progress_window_steps);
                    if (rescue_result == 1) {
                        if (solver->verbose >= 2) {
                            LP_LOG_STDERR("[simplex_phase1] No-pivot ladder dual rescue succeeded at iter %d (streak=%d no_progress=%d threshold=%d)\n",
                                    iter,
                                    phase1_no_pivot_streak,
                                    phase1_no_pivot_no_progress_streak,
                                    no_pivot_ladder_threshold);
                        }
                        ratio_breakdown_count = 0;
                        ratio_breakdown_last_entering = -1;
                        ratio_breakdown_same_entering_streak = 0;
                        continue;
                    }
                    if (rescue_result < 0) {
                        return -1;
                    }
                    use_bland = 1;
                    lp_telemetry_record_phase1_ratio_breakdown_retry(solver);
                    phase1_recompute_rc_only_guarded(solver,
                                                     tab,
                                                     &phase1_rc_only_streak);
                    continue;
                }
                lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN);
            }

            /* "Unbounded" in Phase 1 is typically numerical, not structural.
             * Try to recover via refactorization and conservative pricing first. */
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_RATIO_RECOVERY) == 0) {
                phase1_no_pivot_progress_reset(
                    &phase1_no_pivot_no_progress_streak,
                    &phase1_no_pivot_prev_art_sum,
                    &phase1_no_pivot_anchor_art_sum,
                    &phase1_no_pivot_progress_window_steps);
                phase1_no_pivot_ladder_rescue_cooldown = 0;
                phase1_no_pivot_ladder_rescue_fail_streak = 0;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &phase1_rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN);
                use_bland = 1;
                continue;
            }
            if (!use_bland) {
                use_bland = 1;
                phase1_recompute_rc_only_guarded(solver, tab, &phase1_rc_only_streak);
                continue;
            }

            /* Last-chance recovery before treating Phase-1 "unbounded" as
             * numerical breakdown. */
            int marked = mark_basic_artificial_rows_redundant(tab, 1);
            if (marked > 0) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Marked %d infeasible artificial rows as redundant after ratio-test breakdown\n",
                            marked);
                }
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
                    phase1_recompute_full_with_reason(
                        solver,
                        tab,
                        &phase1_rc_only_streak,
                        LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN);
                    continue;
                }
            }

            int rescue_status = dual_simplex_phase1_rescue(
                solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
            if (rescue_status == 0) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Dual rescue recovered after ratio-test breakdown at iter %d\n", iter);
                }
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &phase1_rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN);
                ratio_breakdown_count = 0;
                ratio_breakdown_last_entering = -1;
                ratio_breakdown_same_entering_streak = 0;
                continue;
            }
            if (solver->status == RALPH_STATUS_TIME_LIMIT) {
                primal_remove_perturbation(tab);
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
                return -1;
            }

            ratio_breakdown_count++;
            if (entering == ratio_breakdown_last_entering) {
                if (ratio_breakdown_same_entering_streak < 1000000) {
                    ratio_breakdown_same_entering_streak++;
                }
            } else {
                ratio_breakdown_last_entering = entering;
                ratio_breakdown_same_entering_streak = 1;
            }
            phase1_exclude_entering_var(entering,
                                        RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                        &excluded_entering_a,
                                        &excluded_entering_ttl_a,
                                        &excluded_entering_b,
                                        &excluded_entering_ttl_b);
            {
                int ratio_breakdown_limit = RALPH_PHASE1_RATIO_BREAKDOWN_LIMIT;
                if (tab->m >= PHASE1_DEGEN_THRESHOLD_LARGE_M &&
                    ratio_breakdown_same_entering_streak >= PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_THRESHOLD) {
                    int tightened_limit =
                        RALPH_PHASE1_RATIO_BREAKDOWN_LIMIT / PHASE1_RATIO_BREAKDOWN_REPEAT_TIGHTEN_DIVISOR;
                    if (tightened_limit < 4) tightened_limit = 4;
                    if (ratio_breakdown_limit > tightened_limit) {
                        ratio_breakdown_limit = tightened_limit;
                    }
                }
                if (ratio_breakdown_count < ratio_breakdown_limit) {
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR("[simplex_phase1] Continuing after ratio-test breakdown (count=%d, entering=%d streak=%d limit=%d), excluding entering for %d iterations\n",
                                ratio_breakdown_count,
                                entering,
                                ratio_breakdown_same_entering_streak,
                                ratio_breakdown_limit,
                                RALPH_PHASE1_ENTERING_EXCLUDE_ITERS);
                    }
                    use_bland = 1;
                    lp_telemetry_record_phase1_ratio_breakdown_retry(solver);
                    phase1_recompute_rc_only_guarded(solver, tab, &phase1_rc_only_streak);
                    continue;
                }
            }
            lp_telemetry_record_phase1_ratio_breakdown_escalation(solver);

            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] ERROR: unbounded in Phase 1 at iter %d (after recovery)\n", iter);
            }
            primal_remove_perturbation(tab);
            /* Treat unrecoverable Phase 1 "unbounded" as numerical breakdown. */
            solver->status = RALPH_STATUS_ITERATION_LIMIT;
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
            return -1;
        }

        /* Guard against numerically explosive search directions before pivoting.
         * Re-factorize and recompute ratio test from the same entering column. */
        double dir_inf = vec_abs_max(tab->work2, tab->m);
        if (dir_inf > RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
            double dir_inf_ratio =
                dir_inf / RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER;
            int cooldown_active = (dir_stabilize_cooldown > 0);
            int dir_stabilize_cooldown_target;
            int force_dir_refactor_extreme =
                lp_refactor_policy_phase1_dir_stabilize_force_extreme_ratio(
                    dir_inf_ratio,
                    cooldown_active);
            int force_dir_refactor_lu_health = lu_needs_refactorization(tab->lu);
            int force_pivot_mode_active =
                (phase1_force_pivot_attempt_budget > 0);
            int force_dir_refactor_guard_trigger =
                (force_dir_refactor_lu_health || force_pivot_mode_active);
            int dir_refactor_ladder_forced = 0;
            int lu_hard_trigger =
                phase1_dir_stabilize_lu_health_hard(tab->lu);
            int escape_triggered = 0;
            int escape_hard_bypass = 0;
            int suppress_lu_health = phase1_dir_stabilize_escape_gate_plan(
                tab->m,
                degenerate_count,
                phase1_dir_skip_event_streak,
                phase1_no_pivot_no_progress_streak,
                phase1_dir_escape_cooldown,
                force_dir_refactor_extreme,
                force_dir_refactor_guard_trigger,
                lu_hard_trigger,
                &phase1_dir_escape_cooldown,
                &escape_triggered,
                &escape_hard_bypass);
            if (escape_triggered) {
                lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                    solver,
                    PHASE1_DIR_ESCAPE_TELEM_TRIGGER);
            }
            if (escape_hard_bypass) {
                lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                    solver,
                    PHASE1_DIR_ESCAPE_TELEM_HARD_BYPASS);
            }
            if (suppress_lu_health) {
                if (force_dir_refactor_lu_health) {
                    force_dir_refactor_lu_health = 0;
                    lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                        solver,
                        PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_LU_HEALTH);
                }
                if (force_pivot_mode_active) {
                    force_pivot_mode_active = 0;
                    lp_telemetry_record_phase1_dir_stabilize_escape_gate(
                        solver,
                        PHASE1_DIR_ESCAPE_TELEM_SUPPRESS_FORCE_PIVOT_MODE);
                }
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Dir-stabilize escape gate suppressed forced direction-refactor path (iter=%d entering=%d dir_skip_streak=%d no_progress=%d cooldown=%d)\n",
                            iter,
                            entering,
                            phase1_dir_skip_event_streak,
                            phase1_no_pivot_no_progress_streak,
                            phase1_dir_escape_cooldown);
                }
            }
            if (phase1_force_pivot_refactor_relax_plan(
                    tab->m,
                    degenerate_count,
                    phase1_no_pivot_no_progress_streak,
                    force_pivot_mode_active,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health,
                    lu_hard_trigger,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_successes,
                    phase1_no_pivot_ladder_rescue_fail_streak)) {
                force_pivot_mode_active = 0;
                lp_telemetry_record_phase1_force_pivot_relax(solver);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Relaxed force-pivot refactor under stable LU + high dual-rescue success (iter=%d entering=%d no_progress=%d)\n",
                            iter,
                            entering,
                            phase1_no_pivot_no_progress_streak);
                }
            }
            if (phase1_force_extreme_refactor_relax_plan(
                    tab->m,
                    degenerate_count,
                    phase1_no_pivot_no_progress_streak,
                    dir_inf_ratio,
                    force_dir_refactor_extreme,
                    force_dir_refactor_lu_health,
                    lu_hard_trigger,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_attempts,
                    solver->telemetry.perf_phase1_no_pivot_ladder_dual_rescue_successes,
                    phase1_no_pivot_ladder_rescue_fail_streak)) {
                force_dir_refactor_extreme = 0;
                lp_telemetry_record_phase1_force_extreme_relax(solver);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Relaxed extreme-direction refactor under stable LU + high dual-rescue success (iter=%d entering=%d ratio=%.2f no_progress=%d)\n",
                            iter,
                            entering,
                            dir_inf_ratio,
                            phase1_no_pivot_no_progress_streak);
                }
            }
            int force_dir_refactor = force_dir_refactor_extreme ||
                                     force_dir_refactor_lu_health;
            int moderate_defer =
                lp_refactor_policy_phase1_dir_stabilize_should_defer_moderate(
                    dir_inf_ratio,
                    cooldown_active,
                    force_dir_refactor_lu_health,
                    dir_stabilize_moderate_defer_pending);
            if (dir_stabilize_repeat_count < 1000000) {
                dir_stabilize_repeat_count++;
            }
            dir_stabilize_cooldown_target =
                lp_refactor_policy_phase1_dir_stabilize_cooldown_updates(
                    tab->m, degenerate_count, dir_stabilize_repeat_count);

            if (cooldown_active) {
                lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(
                    solver,
                    dir_inf_ratio);
                if (force_dir_refactor) {
                    lp_telemetry_record_phase1_dir_stabilize_force(
                        solver,
                        force_dir_refactor_extreme,
                        force_dir_refactor_lu_health);
                }
            }

            if (moderate_defer && !force_dir_refactor && !force_pivot_mode_active) {
                dir_stabilize_moderate_defer_pending = 1;
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Moderate direction norm %.2e at iter %d (entering=%d), deferring one refactor and retrying pricing\n",
                            dir_inf, iter, entering);
                }
                phase1_exclude_entering_var(entering,
                                            RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                            &excluded_entering_a,
                                            &excluded_entering_ttl_a,
                                            &excluded_entering_b,
                                            &excluded_entering_ttl_b);
                if (phase1_dir_skip_event_streak < INT_MAX) {
                    phase1_dir_skip_event_streak++;
                }
                use_bland = 1;
                phase1_recompute_dir_skip_safe(solver,
                                               tab,
                                               degenerate_count,
                                               phase1_no_pivot_streak + 1,
                                               &phase1_rc_only_streak,
                                               &phase1_dir_skip_no_recompute_streak);
                if (phase1_activate_force_pivot_mode(
                        tab->m,
                        degenerate_count,
                        &phase1_dir_skip_event_streak,
                        &phase1_force_pivot_attempt_budget,
                        &phase1_no_pivot_force_pending,
                        &phase1_no_pivot_force_reason) &&
                    solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Force-pivot mode activated after repeated dir-skip/no-recompute (budget=%d)\n",
                            phase1_force_pivot_attempt_budget);
                }
                phase1_no_pivot_progress_update(solver,
                                                tab,
                                                &phase1_no_pivot_prev_art_sum,
                                                &phase1_no_pivot_anchor_art_sum,
                                                &phase1_no_pivot_progress_window_steps,
                                                &phase1_no_pivot_no_progress_streak);
                lp_telemetry_record_phase1_no_pivot_ladder_retry(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                if (phase1_note_no_pivot_and_maybe_force(
                        solver,
                        tab->m,
                        degenerate_count,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        &phase1_no_pivot_streak,
                        &phase1_no_pivot_force_cooldown)) {
                    phase1_no_pivot_force_pending = 1;
                    phase1_no_pivot_force_reason =
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                }
                if (!phase1_no_pivot_force_pending &&
                    phase1_dir_skip_ladder_rescue_due(phase1_dir_skip_event_streak)) {
                    int rescue_step = phase1_no_pivot_ladder_apply_rescue_guard(
                        solver,
                        PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE,
                        phase1_no_pivot_ladder_rescue_cooldown,
                        phase1_no_pivot_ladder_rescue_fail_streak);
                    if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                        int rescue_result = phase1_attempt_ladder_dual_rescue(
                            solver,
                            tab,
                            iter,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                            LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                            &phase1_no_pivot_ladder_rescue_cooldown,
                            &phase1_no_pivot_ladder_rescue_fail_streak,
                            &phase1_rc_only_streak,
                            &phase1_no_pivot_no_progress_streak,
                            &phase1_no_pivot_prev_art_sum,
                            &phase1_no_pivot_anchor_art_sum,
                            &phase1_no_pivot_progress_window_steps);
                        if (rescue_result == 1) continue;
                        if (rescue_result < 0) return -1;
                    } else if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                        lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                            solver,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                        phase1_no_pivot_force_pending = 1;
                        phase1_no_pivot_force_reason =
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    }
                }
                continue;
            }

            if (cooldown_active && !force_dir_refactor && !force_pivot_mode_active) {
                dir_stabilize_moderate_defer_pending = 0;
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Large direction norm %.2e at iter %d (entering=%d), skipping direction-stabilize refactor (cooldown=%d)\n",
                            dir_inf, iter, entering, dir_stabilize_cooldown);
                }
                if (dir_stabilize_cooldown_target > dir_stabilize_cooldown) {
                    dir_stabilize_cooldown = dir_stabilize_cooldown_target;
                }
                phase1_exclude_entering_var(entering,
                                            RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                            &excluded_entering_a,
                                            &excluded_entering_ttl_a,
                                            &excluded_entering_b,
                                            &excluded_entering_ttl_b);
                if (phase1_dir_skip_event_streak < INT_MAX) {
                    phase1_dir_skip_event_streak++;
                }
                use_bland = 1;
                phase1_recompute_dir_skip_safe(solver,
                                               tab,
                                               degenerate_count,
                                               phase1_no_pivot_streak + 1,
                                               &phase1_rc_only_streak,
                                               &phase1_dir_skip_no_recompute_streak);
                if (phase1_activate_force_pivot_mode(
                        tab->m,
                        degenerate_count,
                        &phase1_dir_skip_event_streak,
                        &phase1_force_pivot_attempt_budget,
                        &phase1_no_pivot_force_pending,
                        &phase1_no_pivot_force_reason) &&
                    solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Force-pivot mode activated after repeated dir-skip/no-recompute (budget=%d)\n",
                            phase1_force_pivot_attempt_budget);
                }
                phase1_no_pivot_progress_update(solver,
                                                tab,
                                                &phase1_no_pivot_prev_art_sum,
                                                &phase1_no_pivot_anchor_art_sum,
                                                &phase1_no_pivot_progress_window_steps,
                                                &phase1_no_pivot_no_progress_streak);
                lp_telemetry_record_phase1_no_pivot_ladder_retry(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                if (phase1_note_no_pivot_and_maybe_force(
                        solver,
                        tab->m,
                        degenerate_count,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        &phase1_no_pivot_streak,
                        &phase1_no_pivot_force_cooldown)) {
                    phase1_no_pivot_force_pending = 1;
                    phase1_no_pivot_force_reason =
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                }
                if (!phase1_no_pivot_force_pending &&
                    phase1_dir_skip_ladder_rescue_due(phase1_dir_skip_event_streak)) {
                    int rescue_step = phase1_no_pivot_ladder_apply_rescue_guard(
                        solver,
                        PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE,
                        phase1_no_pivot_ladder_rescue_cooldown,
                        phase1_no_pivot_ladder_rescue_fail_streak);
                    if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                        int rescue_result = phase1_attempt_ladder_dual_rescue(
                            solver,
                            tab,
                            iter,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                            LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                            &phase1_no_pivot_ladder_rescue_cooldown,
                            &phase1_no_pivot_ladder_rescue_fail_streak,
                            &phase1_rc_only_streak,
                            &phase1_no_pivot_no_progress_streak,
                            &phase1_no_pivot_prev_art_sum,
                            &phase1_no_pivot_anchor_art_sum,
                            &phase1_no_pivot_progress_window_steps);
                        if (rescue_result == 1) continue;
                        if (rescue_result < 0) return -1;
                    } else if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                        lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                            solver,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                        phase1_no_pivot_force_pending = 1;
                        phase1_no_pivot_force_reason =
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    }
                }
                continue;
            }
            phase1_no_pivot_progress_update(solver,
                                            tab,
                                            &phase1_no_pivot_prev_art_sum,
                                            &phase1_no_pivot_anchor_art_sum,
                                            &phase1_no_pivot_progress_window_steps,
                                            &phase1_no_pivot_no_progress_streak);
            if (!force_dir_refactor && !force_pivot_mode_active) {
                int ladder_threshold = 0;
                int ladder_step = phase1_no_pivot_ladder_step(
                    tab->m,
                    degenerate_count,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                    phase1_no_pivot_streak + 1,
                    phase1_no_pivot_no_progress_streak,
                    force_pivot_mode_active,
                    &ladder_threshold);
                ladder_step = phase1_no_pivot_ladder_apply_rescue_guard(
                    solver,
                    ladder_step,
                    phase1_no_pivot_ladder_rescue_cooldown,
                    phase1_no_pivot_ladder_rescue_fail_streak);
                if (ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                    int rescue_result = phase1_attempt_ladder_dual_rescue(
                        solver,
                        tab,
                        iter,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                        &phase1_no_pivot_ladder_rescue_cooldown,
                        &phase1_no_pivot_ladder_rescue_fail_streak,
                        &phase1_rc_only_streak,
                        &phase1_no_pivot_no_progress_streak,
                        &phase1_no_pivot_prev_art_sum,
                        &phase1_no_pivot_anchor_art_sum,
                        &phase1_no_pivot_progress_window_steps);
                    if (rescue_result == 1) continue;
                    if (rescue_result < 0) return -1;
                }
                if (ladder_step != PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                    lp_telemetry_record_phase1_no_pivot_ladder_retry(
                        solver,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR("[simplex_phase1] No-pivot ladder defers dir-stabilize refactor (iter=%d entering=%d no_progress=%d threshold=%d)\n",
                                iter, entering, phase1_no_pivot_no_progress_streak, ladder_threshold);
                    }
                    if (dir_stabilize_cooldown_target > dir_stabilize_cooldown) {
                        dir_stabilize_cooldown = dir_stabilize_cooldown_target;
                    }
                    phase1_exclude_entering_var(entering,
                                                RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                                &excluded_entering_a,
                                                &excluded_entering_ttl_a,
                                                &excluded_entering_b,
                                                &excluded_entering_ttl_b);
                    if (phase1_dir_skip_event_streak < INT_MAX) {
                        phase1_dir_skip_event_streak++;
                    }
                    use_bland = 1;
                    phase1_recompute_dir_skip_safe(solver,
                                                   tab,
                                                   degenerate_count,
                                                   phase1_no_pivot_streak + 1,
                                                   &phase1_rc_only_streak,
                                                   &phase1_dir_skip_no_recompute_streak);
                    if (phase1_activate_force_pivot_mode(
                            tab->m,
                            degenerate_count,
                            &phase1_dir_skip_event_streak,
                            &phase1_force_pivot_attempt_budget,
                            &phase1_no_pivot_force_pending,
                            &phase1_no_pivot_force_reason) &&
                        solver->verbose >= 2) {
                        LP_LOG_STDERR("[simplex_phase1] Force-pivot mode activated after repeated dir-skip/no-recompute (budget=%d)\n",
                                phase1_force_pivot_attempt_budget);
                    }
                    if (phase1_note_no_pivot_and_maybe_force(
                            solver,
                            tab->m,
                            degenerate_count,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                            &phase1_no_pivot_streak,
                            &phase1_no_pivot_force_cooldown)) {
                        phase1_no_pivot_force_pending = 1;
                        phase1_no_pivot_force_reason =
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    }
                    continue;
                }
                lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                dir_refactor_ladder_forced = 1;
            }
            phase1_dir_skip_event_streak = 0;
            phase1_dir_escape_cooldown = 0;
            dir_stabilize_moderate_defer_pending = 0;

            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[simplex_phase1] Large direction norm %.2e at iter %d (entering=%d), re-factorizing before pivot\n",
                        dir_inf, iter, entering);
            }
            int stabilized = 0;
            int original_entering = entering;
            for (int stab_try = 0; stab_try < 1; stab_try++) {
                int dir_refactor_trigger = 0;
                if (force_dir_refactor_extreme) {
                    dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_FORCE_EXTREME_DIR;
                } else if (force_dir_refactor_lu_health) {
                    dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_FORCE_LU_HEALTH;
                } else if (force_pivot_mode_active) {
                    dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_FORCE_PIVOT_MODE;
                } else if (dir_refactor_ladder_forced) {
                    dir_refactor_trigger = PHASE1_DIR_REFACTOR_TELEM_LADDER_FORCE;
                }
                lp_telemetry_record_phase1_dir_stabilize_refactor_trigger(
                    solver,
                    dir_refactor_trigger);
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) != 0) {
                    break;
                }
                dir_stabilize_cooldown = dir_stabilize_cooldown_target;
                ratio_status = primal_ratio_test_with_policy(solver,
                                                             tab,
                                                             0,
                                                             entering,
                                                             &leaving,
                                                             &theta);
                if (ratio_status != 0) {
                    phase1_trace_record_no_entering(solver, iter, ratio_status);
                    use_bland = 1;
                    phase1_recompute_full_with_reason(
                        solver,
                        tab,
                        &phase1_rc_only_streak,
                        LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR);
                    continue;
                }

                dir_inf = vec_abs_max(tab->work2, tab->m);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Direction norm after re-factorization: %.2e\n",
                            dir_inf);
                }
                if (dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
                    stabilized = 1;
                    break;
                }

                if (pricing_bland_excluding(tab, original_entering, &entering) != 0) {
                    break;
                }
                ratio_status = primal_ratio_test_with_policy(solver,
                                                             tab,
                                                             0,
                                                             entering,
                                                             &leaving,
                                                             &theta);
                if (ratio_status != 0) {
                    break;
                }
                dir_inf = vec_abs_max(tab->work2, tab->m);
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Alternate entering %d direction norm: %.2e\n",
                            entering, dir_inf);
                }
                if (dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
                    stabilized = 1;
                    break;
                }
            }

            if (!stabilized) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Skipping unstable entering column after stabilization attempts (iter=%d, entering=%d, dir_inf=%.2e)\n",
                            iter, entering, dir_inf);
                }
                phase1_exclude_entering_var(entering,
                                            RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                            &excluded_entering_a,
                                            &excluded_entering_ttl_a,
                                            &excluded_entering_b,
                                            &excluded_entering_ttl_b);
                dir_stabilize_cooldown = dir_stabilize_cooldown_target;
                use_bland = 1;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &phase1_rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP);
                phase1_no_pivot_progress_update(solver,
                                                tab,
                                                &phase1_no_pivot_prev_art_sum,
                                                &phase1_no_pivot_anchor_art_sum,
                                                &phase1_no_pivot_progress_window_steps,
                                                &phase1_no_pivot_no_progress_streak);
                lp_telemetry_record_phase1_no_pivot_ladder_retry(
                    solver,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                if (phase1_note_no_pivot_and_maybe_force(
                        solver,
                        tab->m,
                        degenerate_count,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                        &phase1_no_pivot_streak,
                        &phase1_no_pivot_force_cooldown)) {
                    phase1_no_pivot_force_pending = 1;
                    phase1_no_pivot_force_reason =
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                }
                if (!phase1_no_pivot_force_pending &&
                    phase1_dir_skip_ladder_rescue_due(phase1_dir_skip_event_streak)) {
                    int rescue_step = phase1_no_pivot_ladder_apply_rescue_guard(
                        solver,
                        PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE,
                        phase1_no_pivot_ladder_rescue_cooldown,
                        phase1_no_pivot_ladder_rescue_fail_streak);
                    if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                        int rescue_result = phase1_attempt_ladder_dual_rescue(
                            solver,
                            tab,
                            iter,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
                            LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP,
                            &phase1_no_pivot_ladder_rescue_cooldown,
                            &phase1_no_pivot_ladder_rescue_fail_streak,
                            &phase1_rc_only_streak,
                            &phase1_no_pivot_no_progress_streak,
                            &phase1_no_pivot_prev_art_sum,
                            &phase1_no_pivot_anchor_art_sum,
                            &phase1_no_pivot_progress_window_steps);
                        if (rescue_result == 1) continue;
                        if (rescue_result < 0) return -1;
                    } else if (rescue_step == PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR) {
                        lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                            solver,
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP);
                        phase1_no_pivot_force_pending = 1;
                        phase1_no_pivot_force_reason =
                            LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP;
                    }
                }
                continue;
            }
            dir_stabilize_cooldown = dir_stabilize_cooldown_target;
            use_bland = 1;
        }

        /* If we are retrying the same failing entering/leaving pair, force an
         * alternate leaving choice from the current direction to escape loops. */
        if (fail_repeat_count > 0 &&
            entering == fail_entering &&
            leaving == fail_leaving_pos &&
            leaving >= 0) {
            int alt_leaving = -1;
            double alt_theta = RALPH_INFINITY;
            if (ratio_test_harris_excluding_current(tab, entering, leaving,
                                                    &alt_leaving, &alt_theta) == 0 &&
                alt_leaving >= 0) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Using alternate leaving row %d instead of repeatedly failing row %d\n",
                            alt_leaving, leaving);
                }
                leaving = alt_leaving;
                theta = alt_theta;
            }
        }

        /* Track degeneracy and apply anti-cycling measures */
        if (theta < RALPH_FEAS_TOL) {
            degenerate_count++;

            /* In Phase 1, avoid reactive bound perturbation because it can
             * destabilize the feasibility objective; switch directly to Bland. */
            if (degenerate_count > DEGEN_THRESHOLD && !use_bland) {
                use_bland = 1;
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Switching to Bland's rule after %d degenerate pivots\n",
                            degenerate_count);
                }
            }
        }

        /* Perform pivot */
        int pivot_status;
        if (phase1_force_pivot_attempt_budget > 0) {
            phase1_force_pivot_attempt_budget--;
        }
        {
            double t_pivot_ms = lp_telemetry_timer_start();
            pivot_status = simplex_pivot(tab, entering, leaving, theta, fail_repeat_count);
            lp_telemetry_record_pivot_timed(solver, 1, t_pivot_ms);
        }
        if (pivot_status != 0) {
            if (phase1_note_no_pivot_and_maybe_force(
                    solver,
                    tab->m,
                    degenerate_count,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
                    &phase1_no_pivot_streak,
                    &phase1_no_pivot_force_cooldown)) {
                phase1_no_pivot_force_pending = 1;
                phase1_no_pivot_force_reason =
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL;
            }
            int pivot_fail_reason = tab->trace_last_fail_reason;
            if (entering == fail_entering &&
                leaving == fail_leaving_pos &&
                pivot_fail_reason == fail_reason) {
                fail_repeat_count++;
            } else {
                fail_entering = entering;
                fail_leaving_pos = leaving;
                fail_reason = pivot_fail_reason;
                fail_repeat_count = 1;
            }

            phase1_trace_record_pivot_failure(solver, tab, iter, fail_repeat_count);

            if (solver->verbose) {
                if (fail_repeat_count <= RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER ||
                    fail_repeat_count % 10 == 0) {
                    LP_LOG_STDERR("[simplex_phase1] Pivot failed at iter %d (repeat %d), attempting recovery\n",
                            iter, fail_repeat_count);
                }
            }

            /* First recovery attempt: choose a different leaving row for the
             * same entering column to avoid a numerically singular pivot pair. */
            if (leaving >= 0) {
                int alt_leaving = -1;
                double alt_theta = RALPH_INFINITY;
                if (ratio_test_harris_excluding_current(tab, entering, leaving,
                                                        &alt_leaving, &alt_theta) == 0 &&
                    alt_leaving >= 0 &&
                    alt_leaving != leaving) {
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR("[simplex_phase1] Retrying with alternate leaving row %d (failed row %d)\n",
                                alt_leaving, leaving);
                    }
                    int alt_pivot_status;
                    {
                        double t_pivot_ms = lp_telemetry_timer_start();
                        alt_pivot_status = simplex_pivot(tab, entering, alt_leaving, alt_theta, fail_repeat_count);
                        lp_telemetry_record_pivot_timed(solver, 1, t_pivot_ms);
                    }
                    if (alt_pivot_status == 0) {
                        fail_reason = PHASE1_PIVOT_FAIL_NONE;
                        fail_repeat_count = 0;
                        phase1_no_pivot_streak = 0;
                        continue;
                    } else {
                        phase1_trace_record_pivot_failure(solver, tab, iter, fail_repeat_count);
                    }
                }
            }

            if (fail_repeat_count >= RALPH_PHASE1_FAIL_REPEAT_LIMIT) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Repeated pivot failure (%d) for entering=%d leaving_pos=%d, terminating as ITERATION_LIMIT\n",
                            fail_repeat_count, entering, leaving);
                }
                primal_remove_perturbation(tab);
                solver->status = RALPH_STATUS_ITERATION_LIMIT;
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
                return -1;
            }

            if (fail_repeat_count >= PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER) {
                int no_pivot_force_mode_active =
                    (phase1_force_pivot_attempt_budget > 0);
                int no_pivot_ladder_step;
                phase1_no_pivot_progress_update(solver,
                                                tab,
                                                &phase1_no_pivot_prev_art_sum,
                                                &phase1_no_pivot_anchor_art_sum,
                                                &phase1_no_pivot_progress_window_steps,
                                                &phase1_no_pivot_no_progress_streak);
                no_pivot_ladder_step = phase1_no_pivot_ladder_step(
                    tab->m,
                    degenerate_count,
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
                    phase1_no_pivot_streak,
                    phase1_no_pivot_no_progress_streak,
                    no_pivot_force_mode_active,
                    NULL);
                no_pivot_ladder_step = phase1_no_pivot_ladder_apply_rescue_guard(
                    solver,
                    no_pivot_ladder_step,
                    phase1_no_pivot_ladder_rescue_cooldown,
                    phase1_no_pivot_ladder_rescue_fail_streak);
                if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
                    int rescue_result = phase1_attempt_ladder_dual_rescue(
                        solver,
                        tab,
                        iter,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
                        LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY,
                        &phase1_no_pivot_ladder_rescue_cooldown,
                        &phase1_no_pivot_ladder_rescue_fail_streak,
                        &phase1_rc_only_streak,
                        &phase1_no_pivot_no_progress_streak,
                        &phase1_no_pivot_prev_art_sum,
                        &phase1_no_pivot_anchor_art_sum,
                        &phase1_no_pivot_progress_window_steps);
                    if (rescue_result == 1) {
                        phase1_no_pivot_streak = 0;
                        fail_reason = PHASE1_PIVOT_FAIL_NONE;
                        fail_repeat_count = 0;
                        use_bland = 1;
                        continue;
                    }
                    if (rescue_result < 0) return -1;
                } else if (no_pivot_ladder_step == PHASE1_NO_PIVOT_LADDER_STEP_RETRY) {
                    lp_telemetry_record_phase1_no_pivot_ladder_retry(
                        solver,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL);
                } else {
                    lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
                        solver,
                        LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL);
                }
            }

            /* simplex_pivot can leave basis/LU partially updated on failure.
             * Try the same recovery ladder used in Phase 2. */
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                phase1_pivot_fail_recovery_maybe_exclude_entering(
                    solver,
                    fail_repeat_count,
                    entering,
                    &excluded_entering_a,
                    &excluded_entering_ttl_a,
                    &excluded_entering_b,
                    &excluded_entering_ttl_b);
                use_bland = 1;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &phase1_rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
                continue;
            }
            if (repair_singular_basis(tab) == 0) {
                phase1_pivot_fail_recovery_maybe_exclude_entering(
                    solver,
                    fail_repeat_count,
                    entering,
                    &excluded_entering_a,
                    &excluded_entering_ttl_a,
                    &excluded_entering_b,
                    &excluded_entering_ttl_b);
                use_bland = 1;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &phase1_rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
                continue;
            }

            /* Last structural recovery in Phase 1: if the problematic leaving
             * row is driven by an artificial basic variable, treat the row as
             * redundant and allow LU regularization to proceed. */
            if (leaving >= 0 && leaving < tab->m) {
                int leave_var = tab->basis[leaving];
                if (is_artificial_var(tab, leave_var) &&
                    tab->redundant_rows &&
                    !tab->redundant_rows[leaving]) {
                    tab->redundant_rows[leaving] = 1;
                    tab->num_redundant++;
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR("[simplex_phase1] Marking row %d as redundant due to stuck artificial %d\n",
                                leaving, leave_var);
                    }
                    if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                        phase1_pivot_fail_recovery_maybe_exclude_entering(
                            solver,
                            fail_repeat_count,
                            entering,
                            &excluded_entering_a,
                            &excluded_entering_ttl_a,
                            &excluded_entering_b,
                            &excluded_entering_ttl_b);
                        use_bland = 1;
                        phase1_recompute_full_with_reason(
                            solver,
                            tab,
                            &phase1_rc_only_streak,
                            LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
                        continue;
                    }
                }
            }

            /* Broader recovery for heavily degenerate Phase 1 states:
             * mark all currently-basic artificial rows as potentially redundant
             * and retry a full refactorization. */
            int marked = mark_basic_artificial_rows_redundant(tab, 0);
            if (marked > 0) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[simplex_phase1] Marked %d additional artificial rows as redundant for recovery\n",
                            marked);
                }
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                    phase1_pivot_fail_recovery_maybe_exclude_entering(
                        solver,
                        fail_repeat_count,
                        entering,
                        &excluded_entering_a,
                        &excluded_entering_ttl_a,
                        &excluded_entering_b,
                        &excluded_entering_ttl_b);
                    use_bland = 1;
                    phase1_recompute_full_with_reason(
                        solver,
                        tab,
                        &phase1_rc_only_streak,
                        LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
                    continue;
                }
            }

            /* Final fallback for stuck Phase 1 states: try a bounded dual-simplex
             * rescue on the current tableau (no recursion to primal simplex). */
            int rescue_status = dual_simplex_phase1_rescue(solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
            if (rescue_status == 0) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Dual rescue restored feasibility progress at iter %d\n", iter);
                }
                phase1_pivot_fail_recovery_maybe_exclude_entering(
                    solver,
                    fail_repeat_count,
                    entering,
                    &excluded_entering_a,
                    &excluded_entering_ttl_a,
                    &excluded_entering_b,
                    &excluded_entering_ttl_b);
                use_bland = 1;
                phase1_recompute_full_with_reason(
                    solver,
                    tab,
                    &phase1_rc_only_streak,
                    LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY);
                fail_reason = PHASE1_PIVOT_FAIL_NONE;
                fail_repeat_count = 0;
                continue;
            }
            if (solver->status == RALPH_STATUS_TIME_LIMIT) {
                primal_remove_perturbation(tab);
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
                return -1;
            }

            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_phase1] ERROR: all pivot recovery attempts failed at iter %d\n", iter);
            }
            primal_remove_perturbation(tab);
            /* Treat unrecoverable Phase 1 pivot breakdown as numerical breakdown. */
            solver->status = RALPH_STATUS_ITERATION_LIMIT;
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
            return -1;
        }
        phase1_no_pivot_streak = 0;
        phase1_no_pivot_progress_reset(
            &phase1_no_pivot_no_progress_streak,
            &phase1_no_pivot_prev_art_sum,
            &phase1_no_pivot_anchor_art_sum,
            &phase1_no_pivot_progress_window_steps);
        phase1_no_pivot_ladder_rescue_cooldown = 0;
        phase1_no_pivot_ladder_rescue_fail_streak = 0;
        fail_reason = PHASE1_PIVOT_FAIL_NONE;
        fail_repeat_count = 0;
        ratio_breakdown_count = 0;
        ratio_breakdown_last_entering = -1;
        ratio_breakdown_same_entering_streak = 0;
        phase1_dir_skip_event_streak = 0;
        phase1_dir_escape_cooldown = 0;
        dir_stabilize_moderate_defer_pending = 0;
        phase1_rc_only_streak = 0;
        phase1_dir_skip_no_recompute_streak = 0;
        excluded_entering_a = -1;
        excluded_entering_ttl_a = 0;
        excluded_entering_b = -1;
        excluded_entering_ttl_b = 0;

        /* Phase 1 stall detection: re-perturb when objective stalls.
         * This is critical for problems like recipe (80 artificials) where
         * Bland's rule grinds forever without making progress. */
        {
        double obj_tol_p1 = 1e-4 * (1.0 + fabs(last_obj_p1));
        double obj_change_p1 = fabs(tab->obj_value - last_obj_p1);
        if (obj_change_p1 < obj_tol_p1) {
            stall_count_p1++;
            if (stall_count_p1 >= P1_STALL_THRESHOLD) {
                perturb_attempts_p1++;
                if (perturb_attempts_p1 <= P1_MAX_PERTURB_ATTEMPTS) {
                    double scale = 1.0 + 2.0 * perturb_attempts_p1;
                    primal_apply_perturbation_scaled(tab, scale);
                    /* Reset Bland's to allow faster pricing */
                    use_bland = 0;
                    degenerate_count = 0;
                    stall_count_p1 = 0;
                    phase1_recompute_full_with_reason(
                        solver,
                        tab,
                        &phase1_rc_only_streak,
                        LP_PHASE1_RECOMPUTE_REASON_PERTURB);
                    if (phase1_pricing_strategy == 4) heap_build(tab);
                    if (solver->verbose) {
                        LP_LOG_STDERR("[simplex_phase1] Stall detected, re-perturbing (attempt %d, scale %.1f)\n",
                                perturb_attempts_p1, scale);
                    }
                }
            }
        } else {
            stall_count_p1 = 0;
            last_obj_p1 = tab->obj_value;
        }
        }

        /* Periodic refactorization */
        {
            double phase1_hot_ms_now = phase_hotpath_ms(solver, 1);
            double iter_hot_ms = phase1_hot_ms_now - phase1_hot_ms_prev;
            soft_lu_record_iter_cost(solver, 1, iter_hot_ms);
            lp_reinvert_controller_state_record_iter_cost(
                reinvert_state_for_phase(solver, 1), iter_hot_ms);
            phase1_hot_ms_prev = phase1_hot_ms_now;
        }
        LPLUHealthRefactorDecision lu_health_decision =
            lp_refactor_policy_lu_health_refactor_decision(tab->m,
                                                           tab->lu->use_ft_updates,
                                                           tab->lu->num_updates,
                                                           tab->lu->max_updates,
                                                           tab->lu->spike_pool_used,
                                                           tab->lu->spike_pool_capacity,
                                                           tab->lu->cond_estimate,
                                                           tab->lu->growth_factor,
                                                           lu_soft_health_streak);
        int lu_refactor_nominal = lu_health_decision.refactor_now;
        int lu_refactor_needed = lu_health_decision.refactor_now;
        int lu_soft_cost_deferred = 0;
        int cooldown_eligible = 0;
        double effective_policy_pressure = 0.0;
        LPPeriodicRefactorPolicy periodic_policy =
            compute_periodic_refactor_policy(tab, 1, use_bland, degenerate_count);
        LPPeriodicRefactorPolicy effective_policy = periodic_policy;
        int periodic_refactor = 0;
        int periodic_refactor_nominal = 0;
        int needs_refactor = lu_refactor_needed;
        int reinvert_periodic_candidate = 0;
        int reinvert_control_periodic = 0;
        LPReinvertShadowEval reinvert_shadow_eval;
        reinvert_shadow_eval_reset(&reinvert_shadow_eval);
        lu_soft_health_streak = lu_health_decision.soft_breach_streak_next;
        reinvert_phase1_pressure_safety_update(solver,
                                               iter,
                                               phase1_no_pivot_streak,
                                               phase1_no_pivot_no_progress_streak,
                                               ratio_breakdown_count,
                                               phase1_dir_skip_no_recompute_streak,
                                               lu_health_decision.hard_trigger);
        reinvert_control_periodic = reinvert_controller_controls_periodic_phase(solver, 1);
        if (lu_health_decision.hard_trigger) {
            periodic_policy_cooldown = 0;
            periodic_policy_pressure_decay = 0.0;
            soft_lu_reset_defer_streak(solver, 1);
            periodic_cost_reset_defer_streak(solver, 1);
        } else if (lu_refactor_needed) {
            periodic_cost_reset_defer_streak(solver, 1);
        } else if (!lu_health_decision.soft_trigger || !solver->policy.soft_lu_cost_gate_enabled) {
            soft_lu_reset_defer_streak(solver, 1);
        }
        if (lu_refactor_needed &&
            solver->policy.soft_lu_cost_gate_enabled &&
            lu_health_decision.soft_trigger &&
            !lu_health_decision.hard_trigger) {
            int cap_blocked = 0;
            int next_consecutive = 0;
            int should_defer = simplex_soft_lu_defer_plan_for_test(
                1,
                tab->m,
                use_bland,
                degenerate_count,
                tab->lu->num_updates,
                tab->lu->max_updates,
                tab->lu->spike_pool_used,
                tab->lu->spike_pool_capacity,
                tab->lu->cond_estimate,
                tab->lu->growth_factor,
                soft_lu_refactor_cost_ewma(solver, 1),
                soft_lu_iter_cost_ewma(solver, 1),
                soft_lu_consecutive_defers(solver, 1),
                NULL,
                &cap_blocked,
                &next_consecutive);
            if (should_defer) {
                lu_refactor_needed = 0;
                lu_soft_cost_deferred = 1;
                soft_lu_record_defer(solver, 1);
                soft_lu_set_consecutive_defers(solver, 1, next_consecutive);
            } else {
                soft_lu_reset_defer_streak(solver, 1);
                if (cap_blocked) {
                    soft_lu_record_cap_forced(solver, 1);
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR("[simplex_phase1] Soft LU defer cap reached; forcing periodic LU-health refactor (updates=%d/%d, degen=%d)\n",
                                tab->lu->num_updates,
                                tab->lu->max_updates,
                                degenerate_count);
                    }
                }
            }
        }
        if (lu_soft_cost_deferred) {
            int soft_policy_cooldown = phase1_soft_lu_policy_cooldown_updates(
                tab->m,
                degenerate_count,
                periodic_policy.interval);
            if (soft_policy_cooldown > periodic_policy_cooldown) {
                periodic_policy_cooldown = soft_policy_cooldown;
                lp_telemetry_record_phase1_soft_lu_policy_cooldown_defer(solver);
            }
        }
        if (!lu_refactor_needed) {
            cooldown_eligible = lp_refactor_policy_phase1_cooldown_eligible(tab->m,
                                                                            degenerate_count,
                                                                            solver->policy.periodic_policy_refactors_phase1,
                                                                            tab->lu->spike_pool_used,
                                                                            tab->lu->spike_pool_capacity,
                                                                            tab->lu->cond_estimate,
                                                                            tab->lu->growth_factor);
            if (cooldown_eligible && periodic_policy_pressure_decay > 0.0) {
                effective_policy.run_pressure = clamp_unit_interval(
                    effective_policy.run_pressure - periodic_policy_pressure_decay);
            }
            effective_policy_pressure = effective_policy.run_pressure;
            periodic_refactor = should_run_periodic_refactor(tab,
                                                             iter,
                                                             &effective_policy,
                                                             use_bland,
                                                             degenerate_count);
            periodic_refactor_nominal = periodic_refactor;
            reinvert_periodic_candidate = periodic_refactor_nominal;
            reinvert_shadow_prepare_phase(solver,
                                          tab,
                                          1,
                                          iter,
                                          &lu_health_decision,
                                          periodic_refactor_nominal,
                                          periodic_policy.min_update_age,
                                          periodic_policy_cooldown,
                                          reinvert_control_periodic,
                                          &reinvert_periodic_candidate,
                                          &reinvert_shadow_eval);
            if (reinvert_control_periodic) {
                periodic_refactor = reinvert_periodic_candidate;
                periodic_refactor_nominal = periodic_refactor;
                periodic_cost_reset_defer_streak(solver, 1);
            } else {
                if (periodic_refactor &&
                    cooldown_eligible &&
                    periodic_policy_cooldown > 0) {
                    periodic_refactor = 0;
                }
                if (periodic_refactor) {
                    if (solver->policy.periodic_cost_gate_enabled) {
                        int cap_blocked = 0;
                        int next_consecutive = 0;
                        int gate_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE;
                        int should_defer = simplex_periodic_cost_defer_plan_for_test(
                            1,
                            tab->m,
                            use_bland,
                            degenerate_count,
                            tab->lu->num_updates,
                            tab->lu->max_updates,
                            tab->lu->spike_pool_used,
                            tab->lu->spike_pool_capacity,
                            tab->lu->cond_estimate,
                            tab->lu->growth_factor,
                            soft_lu_refactor_cost_ewma(solver, 1),
                            soft_lu_iter_cost_ewma(solver, 1),
                            periodic_cost_refactor_samples(solver, 1),
                            periodic_cost_iter_samples(solver, 1),
                            periodic_cost_consecutive_defers(solver, 1),
                            &gate_reason,
                            NULL,
                            &cap_blocked,
                            &next_consecutive);
                        periodic_cost_record_gate_reason(
                            solver, 1, (LPPeriodicCostDampenReason)gate_reason);
                        if (should_defer) {
                            periodic_refactor = 0;
                            periodic_cost_record_defer(solver, 1);
                            periodic_cost_set_consecutive_defers(solver, 1, next_consecutive);
                            if (solver->verbose >= 2) {
                                LP_LOG_STDERR("[simplex_phase1] Deferred policy periodic refactor by cost gate (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                                        tab->lu->num_updates,
                                        tab->lu->max_updates,
                                        degenerate_count,
                                        soft_lu_iter_cost_ewma(solver, 1),
                                        soft_lu_refactor_cost_ewma(solver, 1));
                            }
                        } else {
                            periodic_cost_reset_defer_streak(solver, 1);
                            if (cap_blocked) {
                                periodic_cost_record_cap_forced(solver, 1);
                                if (solver->verbose >= 2) {
                                    LP_LOG_STDERR("[simplex_phase1] Policy periodic defer cap reached; forcing periodic policy refactor (updates=%d/%d, degen=%d)\n",
                                            tab->lu->num_updates,
                                            tab->lu->max_updates,
                                            degenerate_count);
                                }
                            } else if (solver->verbose >= 3) {
                                LP_LOG_STDERR("[simplex_phase1] Policy periodic cost gate blocked defer: %s\n",
                                        lp_refactor_policy_periodic_cost_dampen_reason_string(
                                            (LPPeriodicCostDampenReason)gate_reason));
                            }
                        }
                    } else {
                        periodic_cost_reset_defer_streak(solver, 1);
                    }
                }
            }
            needs_refactor = periodic_refactor;
        } else {
            reinvert_shadow_prepare_phase(solver,
                                          tab,
                                          1,
                                          iter,
                                          &lu_health_decision,
                                          0,
                                          periodic_policy.min_update_age,
                                          periodic_policy_cooldown,
                                          0,
                                          NULL,
                                          &reinvert_shadow_eval);
        }
        if (periodic_refactor) {
            periodic_feedback_set_hint(solver, 1, periodic_policy.interval, effective_policy_pressure);
        }
        {
            int shadow_refactor = lp_basis_governor_shadow_decide(
                LP_BASIS_GOV_PHASE1,
                lu_refactor_nominal,
                periodic_refactor_nominal);
            int governed_refactor = lp_basis_governor_decide_refactor(
                &solver->policy.basis_governor,
                LP_BASIS_GOV_PHASE1,
                lu_refactor_nominal,
                periodic_refactor_nominal,
                needs_refactor);
            if (solver->telemetry_enabled) {
                lp_basis_governor_observe_refactor(
                    &solver->policy.basis_governor,
                    LP_BASIS_GOV_PHASE1,
                    shadow_refactor,
                    governed_refactor);
            }
            needs_refactor = governed_refactor;
        }
        reinvert_shadow_finalize_phase(solver, 1, &reinvert_shadow_eval, needs_refactor);

        if (needs_refactor) {
            phase1_dir_skip_no_recompute_streak = 0;
            soft_lu_reset_defer_streak(solver, 1);
            periodic_cost_reset_defer_streak(solver, 1);
            runtime_record_periodic_refactor_trigger(solver, 1, lu_refactor_needed);
            double t_refactor_ms = lp_telemetry_timer_start();
            int rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
            double refactor_elapsed_ms = lp_telemetry_timer_elapsed_ms(t_refactor_ms);
            if (rc_refactor == 0) {
                soft_lu_record_refactor_cost(solver, 1, refactor_elapsed_ms);
                lp_reinvert_controller_state_record_refactor_cost(
                    reinvert_state_for_phase(solver, 1), refactor_elapsed_ms);
            }
            if (lu_refactor_needed && rc_refactor == 0) {
                lu_soft_health_streak = 0;
            }
            if (!lu_refactor_needed && periodic_refactor) {
                if (rc_refactor == 0 && cooldown_eligible) {
                    int cooldown = lp_refactor_policy_phase1_cooldown_window_updates(periodic_policy.interval);
                    if (cooldown > periodic_policy_cooldown) {
                        periodic_policy_cooldown = cooldown;
                    }
                    periodic_policy_pressure_decay += PHASE1_POLICY_PRESSURE_DECAY_STEP;
                    if (periodic_policy_pressure_decay > PHASE1_POLICY_PRESSURE_DECAY_MAX) {
                        periodic_policy_pressure_decay = PHASE1_POLICY_PRESSURE_DECAY_MAX;
                    }
                } else if (rc_refactor != 0) {
                    periodic_policy_cooldown = 0;
                    periodic_policy_pressure_decay = 0.0;
                }
            }
            if (rc_refactor != 0) {
                if (periodic_refactor) {
                    if (solver->verbose) {
                        LP_LOG_STDERR("[simplex_phase1] Periodic refactorization failed at iter %d, continuing with existing LU\n",
                                iter);
                    }
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    continue;
                }

                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] Refactorization failed at iter %d, trying dual rescue\n", iter);
                }

                int marked = mark_basic_artificial_rows_redundant(tab, 1);
                if (marked > 0) {
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR("[simplex_phase1] Marked %d infeasible artificial rows as redundant after refactorization failure\n",
                                marked);
                    }
                    if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
                        tableau_compute_solution(tab);
                        tableau_compute_reduced_costs(tab);
                        continue;
                    }
                }

                int rescue_status = dual_simplex_phase1_rescue(solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
                if (rescue_status == 0) {
                    if (solver->verbose) {
                        LP_LOG_STDERR("[simplex_phase1] Dual rescue recovered after refactorization failure at iter %d\n", iter);
                    }
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    fail_reason = PHASE1_PIVOT_FAIL_NONE;
                    fail_repeat_count = 0;
                    continue;
                }
                if (solver->status == RALPH_STATUS_TIME_LIMIT) {
                    primal_remove_perturbation(tab);
                    solver->iterations = iter;
                    phase1_trace_emit_summary(solver, RALPH_STATUS_TIME_LIMIT);
                    return -1;
                }

                if (repair_singular_basis(tab) == 0) {
                    if (solver->verbose) {
                        LP_LOG_STDERR("[simplex_phase1] Basis repair recovered after refactorization failure at iter %d\n", iter);
                    }
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    continue;
                }

                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_phase1] ERROR: all refactorization recoveries failed at iter %d\n", iter);
                }
                primal_remove_perturbation(tab);
                /* Treat unrecoverable Phase 1 refactorization failure as numerical breakdown. */
                solver->status = RALPH_STATUS_ITERATION_LIMIT;
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
                return -1;
            }
            /* Recompute primal solution and reduced costs after refactorization. */
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
            if (solver->pricing_strategy == 4) heap_build(tab);
        } else if (lu_soft_cost_deferred && solver->verbose >= 2) {
            LP_LOG_STDERR("[simplex_phase1] Deferred soft LU-health periodic refactor (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                    tab->lu->num_updates,
                    tab->lu->max_updates,
                    degenerate_count,
                    soft_lu_iter_cost_ewma(solver, 1),
                    soft_lu_refactor_cost_ewma(solver, 1));
        } else if (iter > 0 && iter % RECOMPUTE_INTERVAL == 0) {
            /* Drift control even when LU updates are still accepted. */
            phase1_dir_skip_no_recompute_streak = 0;
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
            if (solver->pricing_strategy == 4) heap_build(tab);
        }
    }

    /* Iteration limit exceeded */
    primal_remove_perturbation(tab);
    if (solver->verbose) {
        LP_LOG_STDERR("[simplex_phase1] Iteration limit (%d) reached\n", solver->max_iterations);
    }
    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    solver->iterations = solver->max_iterations;
    phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
    return -1;
}

/*
 * Transition from Phase 1 to Phase 2.
 * - Switch objective from Phase 1 (sum of artificials) to original objective
 * - Handle artificial variables still in basis (at zero value)
 * - Recompute reduced costs with new objective
 */
static int simplex_transition_phase2(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;

    if (!tab->use_two_phase) {
        return 0;  /* Not using two-phase */
    }

    if (solver->verbose) {
        LP_LOG_STDOUT("[simplex_transition] Transitioning to Phase 2 (m=%d, num_art=%d, num_eq=%d)\n",
               tab->m, tab->num_artificial, tab->num_equalities);
        fflush(stdout);
    }

    tab->phase = 2;

    /* Switch to original objective coefficients */
    for (int j = 0; j < tab->n; j++) {
        tab->c_ext[j] = tab->c_original[j];
    }

    /* Handle artificial variables still in basis.
     * If an artificial variable is basic at value zero, we need to pivot it out
     * and replace it with an eligible non-artificial variable.
     *
     * Strategy:
     * 1. Compute the tableau row for the artificial's basis position
     * 2. Search ALL non-basic non-artificial variables for a non-zero pivot
     * 3. Prefer structural variables, then slacks
     * 4. Track stuck artificials (redundant rows) for special handling */
    int art_in_basis = 0;
    int art_stuck = 0;

    /* Reset redundant row tracking.
     * Mark ALL rows that have artificial variables as potentially redundant.
     * This is because the constraint matrix may be rank-deficient (redundant constraints),
     * and any of these rows could cause singularity during LU factorization.
     *
     * Artificial variables have identity columns in A_ext (coefficient 1.0 in exactly one row).
     * Find the row for each artificial by looking at its column in the sparse matrix. */
    memset(tab->redundant_rows, 0, tab->m * sizeof(int));
    tab->num_redundant = 0;

    /* Build a map from artificial variable index k to its constraint row.
     * We'll use this to mark rows as redundant when artificials get stuck. */
    int *artificial_to_row = (int*)calloc(tab->num_artificial, sizeof(int));
    if (artificial_to_row) {
        for (int k = 0; k < tab->num_artificial; k++) {
            int art_j = tab->artificial_vars[k];
            /* Find the row this artificial corresponds to by looking at A_ext column. */
            artificial_to_row[k] = -1;  /* Default: unknown */
            for (int p = tab->A_ext->colptr[art_j]; p < tab->A_ext->colptr[art_j + 1]; p++) {
                int row = tab->A_ext->rowidx[p];
                double val = tab->A_ext->values[p];
                if (fabs(val - 1.0) < RALPH_ZERO_TOL) {
                    artificial_to_row[k] = row;
                    break;
                }
            }
        }
    }


    for (int k = 0; k < tab->num_artificial; k++) {
        int art_j = tab->artificial_vars[k];
        if (tab->var_status[art_j] == RALPH_BASIC) {
            art_in_basis++;

            /* Find the basis position of this artificial */
            int basis_pos = tab->basis_pos[art_j];
            if (basis_pos < 0) continue;

            /* Compute the tableau row: e_i^T * B^{-1} * A
             * First get e_i^T * B^{-1} via BTRAN */
            vec_set_zero(tab->work1, tab->m);
            tab->work1[basis_pos] = 1.0;
            lu_solve_transpose(tab->lu, tab->work1, tab->work2);  /* work2 = e_i^T * B^{-1} */

            /* Now search for a non-artificial non-basic variable with non-zero coefficient.
             * Priority: structural variables first, then slacks */
            int found_replacement = 0;
            int best_j = -1;
            double best_coef = 0.0;

            /* Pass 1: Structural variables (prefer these) */
            for (int j = 0; j < tab->model->num_vars && !found_replacement; j++) {
                if (tab->var_status[j] == RALPH_BASIC) continue;
                if (tab->var_status[j] == RALPH_FIXED) continue;

                /* Compute tableau coefficient: (e_i^T B^{-1}) * A[:,j] */
                double coef = sparse_dot_column(tab->A_ext, j, tab->work2);

                if (fabs(coef) > fabs(best_coef)) {
                    best_coef = coef;
                    best_j = j;
                }

                /* Accept immediately if coefficient is large enough */
                if (fabs(coef) > 0.1) {
                    found_replacement = 1;
                    best_j = j;
                }
            }

            /* Pass 2: Slack variables (if no good structural found) */
            if (!found_replacement) {
                for (int j = tab->model->num_vars; j < tab->n; j++) {
                    /* Skip artificial variables */
                    int is_artificial = 0;
                    for (int kk = 0; kk < tab->num_artificial; kk++) {
                        if (tab->artificial_vars[kk] == j) {
                            is_artificial = 1;
                            break;
                        }
                    }
                    if (is_artificial) continue;
                    if (tab->var_status[j] == RALPH_BASIC) continue;
                    if (tab->var_status[j] == RALPH_FIXED) continue;

                    double coef = sparse_dot_column(tab->A_ext, j, tab->work2);

                    if (fabs(coef) > fabs(best_coef)) {
                        best_coef = coef;
                        best_j = j;
                    }

                    if (fabs(coef) > 0.1) {
                        found_replacement = 1;
                        best_j = j;
                        break;
                    }
                }
            }

            /* Try to pivot if we found any candidate */
            if (best_j >= 0 && fabs(best_coef) > RALPH_PIVOT_TOL) {
                /* Pivot with zero theta since artificial is at zero value */
                if (simplex_pivot(tab, best_j, basis_pos, 0.0, 0) == 0) {
                    found_replacement = 1;
                    if (solver->verbose) {
                        LP_LOG_STDERR("[simplex_transition] Pivoted out artificial %d with var %d (coef=%.2e)\n",
                                art_j, best_j, best_coef);
                    }
                } else {
                    found_replacement = 0;
                }
            }

            if (!found_replacement) {
                /* Artificial is stuck in basis - this row is truly redundant.
                 * Mark its original constraint row for special handling.
                 * Note: We mark the original constraint row, not basis_pos,
                 * because the constraint row index is stable while basis_pos changes. */
                art_stuck++;
                int orig_row = (artificial_to_row && k >= 0 && k < tab->num_artificial) ?
                               artificial_to_row[k] : basis_pos;
                if (orig_row >= 0 && orig_row < tab->m && !tab->redundant_rows[orig_row]) {
                    tab->redundant_rows[orig_row] = 1;
                    tab->num_redundant++;
                }

                if (solver->verbose) {
                    LP_LOG_STDERR("[simplex_transition] Warning: artificial var %d stuck in basis row %d "
                            "(orig constraint row %d, best_coef=%.2e, redundant row)\n",
                            art_j, basis_pos, orig_row, best_coef);
                }
            }
        }
    }

    free(artificial_to_row);
    artificial_to_row = NULL;

    if (solver->verbose) {
        LP_LOG_STDERR("[simplex_transition] %d artificial variables were in basis, %d stuck (%d redundant rows)\n",
                art_in_basis, art_stuck, tab->num_redundant);
    }

    /* Handle all artificial variables for Phase 2:
     * - Non-basic: fix at [0,0] (FIXED status)
     * - Basic (stuck): set cost=0, bounds=[0,0]. The artificial stays in
     *   basis at value 0. Fixing bounds to [0,0] ensures the ratio test
     *   treats it as a degenerate variable that blocks unbounded steps
     *   (prevents spurious UNBOUNDED from regularized LU giving non-zero
     *   FTRAN values in redundant rows). */
    for (int k = 0; k < tab->num_artificial; k++) {
        int art_j = tab->artificial_vars[k];
        tab->lb_ext[art_j] = 0.0;
        tab->ub_ext[art_j] = 0.0;
        tab->c_ext[art_j] = 0.0;
        tab->x[art_j] = 0.0;
        if (tab->var_status[art_j] != RALPH_BASIC) {
            tab->var_status[art_j] = RALPH_FIXED;
        }
    }

    /* Zero redundant rows in A_ext and RHS.
     * Stuck artificials indicate truly redundant constraints (linearly dependent
     * on other constraints). By zeroing the row in A_ext (except the artificial's
     * own 1.0 coefficient) and zeroing the RHS, we make the basis well-conditioned:
     *   - Artificial column has identity-like structure: 1.0 at its row, 0 elsewhere
     *   - All other columns have 0 at redundant rows
     * This eliminates the need for LU regularization and prevents garbage values
     * in FTRAN/BTRAN results for redundant row positions. */
    if (tab->num_redundant > 0) {
        /* Build a quick lookup for artificial variable columns */
        int *is_art_col = (int*)calloc(tab->n, sizeof(int));
        if (is_art_col) {
            for (int k = 0; k < tab->num_artificial; k++) {
                is_art_col[tab->artificial_vars[k]] = 1;
            }

            /* Zero redundant rows in A_ext for non-artificial columns */
            for (int j = 0; j < tab->n; j++) {
                if (is_art_col[j]) continue;  /* Keep artificial 1.0 entries */
                for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                    int row = tab->A_ext->rowidx[p];
                    if (tab->redundant_rows[row]) {
                        tab->A_ext->values[p] = 0.0;
                    }
                }
            }

            free(is_art_col);
        }

        /* Zero RHS for redundant rows */
        for (int i = 0; i < tab->m; i++) {
            if (tab->redundant_rows[i]) {
                tab->rhs[i] = 0.0;
            }
        }

        if (solver->verbose) {
            LP_LOG_STDERR("[simplex_transition] Zeroed %d redundant rows in A_ext and RHS\n",
                    tab->num_redundant);
        }

        /* Mark that redundant rows have been zeroed. This tells tableau_refactorize
         * to skip the relaxed pivot tolerance (which would corrupt non-zeroed rows).
         * Regularization is still allowed — if the sparse LU processes columns out
         * of order, it may need to regularize a zeroed row, which is correct
         * (diagonal=1.0 encodes "x_art = 0" for the artificial at that row). */
        tab->redundant_rows_zeroed = 1;

        /* Invalidate LU symbolic analysis cache. The A_ext values changed (zeroed
         * rows) but the CSC structure didn't, so the fingerprint would still match.
         * Without invalidation, the sparse LU reuses a stale elimination order. */
        tab->lu->sym_valid = 0;
        tab->basis_cache_valid = 0;
        tab->basis_cache_total_nnz = 0;
    }

    /* Refactorize basis for Phase 2.
     * With redundant rows zeroed in A_ext, stuck artificial columns provide
     * identity-like structure that makes the basis well-conditioned. */
    if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PHASE_TRANSITION) != 0) {
        if (solver->verbose) {
            LP_LOG_STDERR("[simplex_transition] Refactorization failed, attempting basis repair...\n");
        }
        if (repair_singular_basis(tab) != 0) {
            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_transition] ERROR: basis repair failed during transition\n");
            }
            return -1;
        }
    }

    /* Recompute reduced costs with new objective */
    tableau_compute_reduced_costs(tab);

    /* Recompute solution */
    tableau_compute_solution(tab);


    if (solver->verbose) {
        LP_LOG_STDERR("[simplex_transition] Phase 2 objective value: %g\n", tab->obj_value);
    }

    return 0;
}

/* Confirm Phase-2 optimality on a fresh basis.
 * Incremental RC updates can occasionally mark a large degenerate basis as
 * optimal too early; this pass re-factorizes and re-prices strictly before
 * returning OPTIMAL. */
static int phase2_confirm_optimality(SimplexSolver *solver, int iter, int *entering_out) {
    SimplexTableau *tab;
    int rc;
    int entering = -1;

    if (!solver || !solver->tableau) return -1;
    tab = solver->tableau;

    {
        double t_refactor_ms = lp_telemetry_timer_start();
        rc = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
        lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
    }
    if (rc != 0) {
        if (repair_singular_basis(tab) != 0) {
            solver->status = RALPH_STATUS_ERROR;
            solver->iterations = iter;
            return -1;
        }
    }

    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    if (pricing_dantzig(tab, &entering) != 0) {
        if (entering_out) *entering_out = -1;
        return 1;  /* confirmed optimal */
    }

    if (entering_out) *entering_out = entering;
    return 0;  /* not optimal yet */
}

/* Phase 2: Optimize */
static int simplex_phase2(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;

    tab->phase = 2;

    /* For two-phase problems, force early refactorization to reset numerical
     * state after the transition. Redundant rows were zeroed in A_ext during
     * the transition, so the basis matrix is now well-conditioned. */
    if (tab->use_two_phase) {
        tab->lu->num_updates = tab->lu->max_updates;
        {
            double t_refactor_ms = lp_telemetry_timer_start();
            int rc = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PHASE_TRANSITION);
            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            if (rc != 0) {
                if (repair_singular_basis(tab) != 0) {
                    solver->status = RALPH_STATUS_ERROR;
                    return -1;
                }
            }
        }
    }

    /* D4: Apply proactive perturbation when arriving from dual fallback.
     * The dual failed on a degenerate problem, so proactive perturbation saves
     * the ~30 wasted degenerate pivots before reactive perturbation kicks in.
     * Do NOT apply for two-phase transitions — the post-transition basis is fragile. */
    int perturbation_active = 0;
    if (solver->from_dual_fallback && !tab->use_two_phase) {
        primal_apply_perturbation(tab);
        perturbation_active = 1;
    }

    /* Compute initial solution for Phase 2 */
    tableau_compute_solution(tab);

    /* Cycling detection: track consecutive degenerate pivots */
    int degenerate_count = 0;
    int non_degen_streak = 0;
    const int DEGEN_THRESHOLD = 50;  /* Switch to Bland's rule after this many */
    const int NON_DEGEN_THRESHOLD = 100;  /* Non-degenerate pivots to turn Bland off */
    int use_bland = 0;

    /* Stall detection: track objective progress for re-perturbation.
     * Mirrors dual_simplex.c stall detection (lines 1137-1165).
     * When Phase 2 stalls (no objective progress for STALL_THRESHOLD iters),
     * remove perturbation, re-apply with scaled magnitude, and reset Bland's.
     * This is independent of the degenerate pivot counter above. */
    double last_obj_p2 = tab->obj_value;
    int stall_count_p2 = 0;
    const int P2_STALL_THRESHOLD = 50;
    int perturb_attempts_p2 = 0;
    const int P2_MAX_PERTURB_ATTEMPTS = 15;

    /* For two-phase problems after transition, start with Bland's rule for the first
     * few pivots to avoid numerical issues with the post-transition basis.
     * The transition may leave the basis in a fragile state where aggressive pricing
     * selects entering variables that cause LU update failures. */
    int bland_start_iters = tab->use_two_phase ? 20 : 0;
    int periodic_policy_cooldown = 0;
    double periodic_policy_pressure_decay = 0.0;
    int lu_soft_health_streak = 0;

    /* Compute initial reduced costs.
     * After two-phase transition, ALWAYS compute full RCs because Bland's rule
     * (used for the first bland_start_iters) reads tab->rc[] directly.
     * For non-two-phase, partial pricing can use lazy mode (duals only). */
    if (solver->pricing_strategy == 3 && !tab->use_two_phase) {
        tableau_compute_duals(tab);  /* Lazy mode: duals only */
    } else {
        tableau_compute_reduced_costs(tab);
        if (solver->pricing_strategy == 4) heap_build(tab);
    }
    double phase2_hot_ms_prev = phase_hotpath_ms(solver, 2);

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tab->iterations = iter;
        if (lp_run_user_callbacks(solver, tab, RALPH_LP_PROGRESS_PHASE_2, iter, 0, 1) != 0) {
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_TIME_LIMIT;
            solver->iterations = iter;
            return -1;
        }

        if (periodic_policy_cooldown > 0) {
            periodic_policy_cooldown--;
        }
        if (periodic_policy_pressure_decay > 0.0) {
            periodic_policy_pressure_decay -= PHASE2_POLICY_PRESSURE_RECOVERY_STEP;
            if (periodic_policy_pressure_decay < 0.0) {
                periodic_policy_pressure_decay = 0.0;
            }
        }

        /* T3.1: Objective limit early-exit (internal minimization space) */
        if (solver->objective_limit < RALPH_INFINITY &&
            tab->obj_value >= solver->objective_limit) {
            primal_remove_perturbation(tab);
            tableau_compute_solution(tab);
            solver->status = RALPH_STATUS_OBJ_LIMIT;
            solver->iterations = iter;
            solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
            return 0;
        }

        /* Reduced costs are updated incrementally in simplex_pivot().
         * Full recomputation only needed:
         * - After refactorization (for numerical stability)
         * - Periodically to correct drift
         */

        /* Pricing: select entering variable */
        int entering;
        int price_status;
        double t_pricing_ms = lp_telemetry_timer_start();
        int adaptive_devex_partial =
            phase2_use_adaptive_devex_partial(tab,
                                              iter,
                                              degenerate_count,
                                              (use_bland || iter < bland_start_iters),
                                              solver->pricing_strategy);

        if (use_bland || iter < bland_start_iters) {
            /* Use Bland's rule to prevent cycling or for initial stability */
            price_status = pricing_bland(tab, &entering);
        } else if (solver->pricing_strategy == 0) {
            price_status = pricing_dantzig(tab, &entering);
        } else if (solver->pricing_strategy == 1) {
            price_status = pricing_steepest_edge(tab, &entering);
        } else if (solver->pricing_strategy == 3) {
            price_status = pricing_partial(tab, &entering);
        } else if (solver->pricing_strategy == 4) {
            price_status = pricing_heap(tab, &entering);
        } else {
            if (adaptive_devex_partial &&
                (iter & DEVEX_PARTIAL_FULL_RESCAN_MASK) != 0) {
                price_status = pricing_devex_partial(tab, &entering);
            } else {
                price_status = pricing_devex(tab, &entering);
            }
        }
        {
            lp_telemetry_record_pricing_timed(solver, 2, t_pricing_ms);
        }

        if (price_status != 0) {
            int confirm = phase2_confirm_optimality(solver, iter, &entering);
            if (confirm < 0) {
                primal_remove_perturbation(tab);
                return -1;
            }
            if (confirm > 0) {
                /* Optimal - remove perturbation and finalize */
                primal_remove_perturbation(tab);
                solver->status = RALPH_STATUS_OPTIMAL;
                solver->iterations = iter;
                solver->degenerate_pivots = degenerate_count;
                tableau_compute_solution(tab);
                solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
                return 0;
            }
        }

        /* Ratio test: select leaving variable */
        int leaving;
        double theta;
        int ratio_status;
        double t_ratio_ms = lp_telemetry_timer_start();

        ratio_status = primal_ratio_test_with_policy(solver,
                                                     tab,
                                                     use_bland,
                                                     entering,
                                                     &leaving,
                                                     &theta);
        {
            lp_telemetry_record_ratio_timed(solver, 2, t_ratio_ms);
        }

        if (ratio_status != 0) {
            /* No leaving variable found — possibly unbounded.
             * Stale LU factors can produce spurious theta=inf (e.g., lotfi).
             * Refactorize and retry once before declaring UNBOUNDED. */
            int refactor_rc;
            {
                double t_refactor_ms = lp_telemetry_timer_start();
                refactor_rc = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
                lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            }
            if (refactor_rc == 0) {
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                if (solver->pricing_strategy == 4) heap_build(tab);

                /* Re-price: the entering variable may no longer be eligible */
                t_pricing_ms = lp_telemetry_timer_start();
                if (use_bland || iter < bland_start_iters) {
                    price_status = pricing_bland(tab, &entering);
                } else if (solver->pricing_strategy == 0) {
                    price_status = pricing_dantzig(tab, &entering);
                } else if (solver->pricing_strategy == 1) {
                    price_status = pricing_steepest_edge(tab, &entering);
                } else if (solver->pricing_strategy == 3) {
                    price_status = pricing_partial(tab, &entering);
                } else if (solver->pricing_strategy == 4) {
                    price_status = pricing_heap(tab, &entering);
                } else {
                    if (adaptive_devex_partial &&
                        (iter & DEVEX_PARTIAL_FULL_RESCAN_MASK) != 0) {
                        price_status = pricing_devex_partial(tab, &entering);
                    } else {
                        price_status = pricing_devex(tab, &entering);
                    }
                }
                {
                    lp_telemetry_record_pricing_timed(solver, 2, t_pricing_ms);
                }

                if (price_status != 0) {
                    int confirm = phase2_confirm_optimality(solver, iter, &entering);
                    if (confirm < 0) {
                        primal_remove_perturbation(tab);
                        return -1;
                    }
                    if (confirm > 0) {
                        /* Actually optimal after refactorization */
                        primal_remove_perturbation(tab);
                        solver->status = RALPH_STATUS_OPTIMAL;
                        solver->iterations = iter;
                        solver->degenerate_pivots = degenerate_count;
                        tableau_compute_solution(tab);
                        solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
                        return 0;
                    }
                }

                /* Retry ratio test with fresh LU */
                t_ratio_ms = lp_telemetry_timer_start();
                ratio_status = primal_ratio_test_with_policy(solver,
                                                             tab,
                                                             use_bland,
                                                             entering,
                                                             &leaving,
                                                             &theta);
                {
                    lp_telemetry_record_ratio_timed(solver, 2, t_ratio_ms);
                }
            }

            if (ratio_status != 0) {
                double dir = (tab->var_status[entering] == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;
                extract_unbounded_ray(solver, entering, dir);
                primal_remove_perturbation(tab);
                solver->status = RALPH_STATUS_UNBOUNDED;
                solver->iterations = iter;
                return -1;
            }
            /* Recovery succeeded — fall through to pivot */
        }

        /* Track degenerate/near-degenerate pivots for cycling prevention
         *
         * Strategy:
         * 1. After 30 degenerate pivots: apply bound perturbation
         * 2. After 100 more degenerate pivots: switch to Bland's rule
         * 3. After 100 non-degenerate pivots: reset and try faster methods
         */
        {
        const double NEAR_DEGEN_TOL = 1e-3;
        const int PERTURB_THRESHOLD = 30;

        if (theta < NEAR_DEGEN_TOL) {
            degenerate_count++;
            non_degen_streak = 0;

            /* First try perturbation */
            if (degenerate_count >= PERTURB_THRESHOLD && !perturbation_active && !use_bland) {
                primal_apply_perturbation(tab);
                perturbation_active = 1;
                if (solver->verbose) {
                    LP_LOG_STDOUT("Iter %d: Applying perturbation due to degeneracy\n", iter);
                }
            }

            /* If still cycling after perturbation, use Bland's rule */
            if (degenerate_count >= DEGEN_THRESHOLD && !use_bland) {
                use_bland = 1;
                if (solver->verbose) {
                    LP_LOG_STDOUT("Iter %d: Switching to Bland's rule due to potential cycling\n", iter);
                }
            }
        } else {
            /* Only reset after many consecutive non-degenerate pivots */
            non_degen_streak++;
            degenerate_count = 0;
            if (use_bland && non_degen_streak >= NON_DEGEN_THRESHOLD) {
                use_bland = 0;
                non_degen_streak = 0;
                if (solver->verbose) {
                    LP_LOG_STDOUT("Iter %d: Turning off Bland's rule after %d non-degenerate pivots\n",
                           iter, NON_DEGEN_THRESHOLD);
                }
            }
        }
        }  /* end degeneracy tracking block */

        /* Perform pivot */
        int pivot_rc;
        {
            double t_pivot_ms = lp_telemetry_timer_start();
            pivot_rc = simplex_pivot(tab, entering, leaving, theta, 0);
            lp_telemetry_record_pivot_timed(solver, 2, t_pivot_ms);
        }
        if (pivot_rc != 0) {
            if (solver->verbose) {
                LP_LOG_STDERR("[primal_simplex] Pivot failed at iter %d (entering=%d, leaving=%d, theta=%e), attempting recovery\n",
                        iter, entering, leaving, theta);
            }
            /* Pivot failed - the basis was partially updated in simplex_pivot.
             * Try to recover by refactorizing the current (post-pivot) basis. */
            int rc_refactor;
            {
                double t_refactor_ms = lp_telemetry_timer_start();
                rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY);
                lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            }
            if (rc_refactor == 0) {
                /* Refactorization succeeded - recompute and continue */
                tableau_compute_solution(tab);
                if (solver->pricing_strategy == 3) {
                    tableau_compute_duals(tab);
                } else {
                    tableau_compute_reduced_costs(tab);
                    if (solver->pricing_strategy == 4) heap_build(tab);
                }
                if (solver->verbose) {
                    LP_LOG_STDERR("[primal_simplex] Recovery via refactorization at iter %d\n", iter);
                }
                continue;
            }
            /* Refactorization failed - try basis repair */
            if (repair_singular_basis(tab) == 0) {
                tableau_compute_solution(tab);
                if (solver->pricing_strategy == 3) {
                    tableau_compute_duals(tab);
                } else {
                    tableau_compute_reduced_costs(tab);
                    if (solver->pricing_strategy == 4) heap_build(tab);
                }
                if (solver->verbose) {
                    LP_LOG_STDERR("[primal_simplex] Recovery via basis repair at iter %d\n", iter);
                }
                continue;
            }
            /* All recovery attempts failed */
            if (solver->verbose) {
                LP_LOG_STDERR("[primal_simplex] ERROR: all recovery attempts failed at iter %d\n", iter);
            }
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }

        /* Refactorize if needed.
         * For two-phase problems, periodic refresh is adaptive (interval + LU health). */
        {
            double phase2_hot_ms_now = phase_hotpath_ms(solver, 2);
            double iter_hot_ms = phase2_hot_ms_now - phase2_hot_ms_prev;
            soft_lu_record_iter_cost(solver, 2, iter_hot_ms);
            lp_reinvert_controller_state_record_iter_cost(
                reinvert_state_for_phase(solver, 2), iter_hot_ms);
            phase2_hot_ms_prev = phase2_hot_ms_now;
        }
        LPLUHealthRefactorDecision lu_health_decision =
            lp_refactor_policy_lu_health_refactor_decision(tab->m,
                                                           tab->lu->use_ft_updates,
                                                           tab->lu->num_updates,
                                                           tab->lu->max_updates,
                                                           tab->lu->spike_pool_used,
                                                           tab->lu->spike_pool_capacity,
                                                           tab->lu->cond_estimate,
                                                           tab->lu->growth_factor,
                                                           lu_soft_health_streak);
        int lu_refactor_nominal = lu_health_decision.refactor_now;
        int lu_refactor_needed = lu_health_decision.refactor_now;
        int lu_soft_cost_deferred = 0;
        int needs_refactor = lu_refactor_needed;
        int periodic_refactor = 0;
        int periodic_refactor_nominal = 0;
        int reinvert_periodic_candidate = 0;
        int reinvert_control_periodic =
            reinvert_controller_controls_periodic_phase(solver, 2);
        int cooldown_eligible = 0;
        double effective_policy_pressure = 0.0;
        LPPeriodicRefactorPolicy periodic_policy = {0, 0, 0.0, 0.0};
        LPReinvertShadowEval reinvert_shadow_eval;
        reinvert_shadow_eval_reset(&reinvert_shadow_eval);
        lu_soft_health_streak = lu_health_decision.soft_breach_streak_next;
        if (lu_health_decision.hard_trigger) {
            periodic_policy_cooldown = 0;
            periodic_policy_pressure_decay = 0.0;
            soft_lu_reset_defer_streak(solver, 2);
            periodic_cost_reset_defer_streak(solver, 2);
        } else if (lu_refactor_needed) {
            periodic_cost_reset_defer_streak(solver, 2);
        } else if (!lu_health_decision.soft_trigger || !solver->policy.soft_lu_cost_gate_enabled) {
            soft_lu_reset_defer_streak(solver, 2);
        }
        if (lu_refactor_needed &&
            solver->policy.soft_lu_cost_gate_enabled &&
            lu_health_decision.soft_trigger &&
            !lu_health_decision.hard_trigger) {
            int cap_blocked = 0;
            int next_consecutive = 0;
            int should_defer = simplex_soft_lu_defer_plan_for_test(
                2,
                tab->m,
                use_bland,
                degenerate_count,
                tab->lu->num_updates,
                tab->lu->max_updates,
                tab->lu->spike_pool_used,
                tab->lu->spike_pool_capacity,
                tab->lu->cond_estimate,
                tab->lu->growth_factor,
                soft_lu_refactor_cost_ewma(solver, 2),
                soft_lu_iter_cost_ewma(solver, 2),
                soft_lu_consecutive_defers(solver, 2),
                NULL,
                &cap_blocked,
                &next_consecutive);
            if (should_defer) {
                lu_refactor_needed = 0;
                lu_soft_cost_deferred = 1;
                soft_lu_record_defer(solver, 2);
                soft_lu_set_consecutive_defers(solver, 2, next_consecutive);
                needs_refactor = 0;
            } else {
                soft_lu_reset_defer_streak(solver, 2);
                if (cap_blocked) {
                    soft_lu_record_cap_forced(solver, 2);
                    if (solver->verbose >= 2) {
                        LP_LOG_STDERR("[primal_simplex] Soft LU defer cap reached; forcing periodic LU-health refactor (updates=%d/%d, degen=%d)\n",
                                tab->lu->num_updates,
                                tab->lu->max_updates,
                                degenerate_count);
                    }
                }
            }
        }
        if (!lu_refactor_needed) {
            LPPeriodicRefactorPolicy effective_policy;
            periodic_policy = compute_periodic_refactor_policy(tab, 2, use_bland, degenerate_count);
            effective_policy = periodic_policy;
            cooldown_eligible = lp_refactor_policy_phase2_cooldown_eligible(tab->m,
                                                                            degenerate_count,
                                                                            tab->lu->spike_pool_used,
                                                                            tab->lu->spike_pool_capacity,
                                                                            tab->lu->cond_estimate,
                                                                            tab->lu->growth_factor);
            if (cooldown_eligible && periodic_policy_pressure_decay > 0.0) {
                effective_policy.run_pressure = clamp_unit_interval(
                    effective_policy.run_pressure - periodic_policy_pressure_decay);
            }
            effective_policy_pressure = effective_policy.run_pressure;
            periodic_refactor = should_run_periodic_refactor(tab,
                                                             iter,
                                                             &effective_policy,
                                                             use_bland,
                                                             degenerate_count);
            periodic_refactor_nominal = periodic_refactor;
            reinvert_periodic_candidate = periodic_refactor_nominal;
            reinvert_shadow_prepare_phase(solver,
                                          tab,
                                          2,
                                          iter,
                                          &lu_health_decision,
                                          periodic_refactor_nominal,
                                          periodic_policy.min_update_age,
                                          periodic_policy_cooldown,
                                          reinvert_control_periodic,
                                          &reinvert_periodic_candidate,
                                          &reinvert_shadow_eval);
            if (reinvert_control_periodic) {
                periodic_refactor = reinvert_periodic_candidate;
                periodic_refactor_nominal = periodic_refactor;
                periodic_cost_reset_defer_streak(solver, 2);
            } else {
                if (periodic_refactor &&
                    cooldown_eligible &&
                    periodic_policy_cooldown > 0) {
                    periodic_refactor = 0;
                }
                if (periodic_refactor) {
                    if (solver->policy.periodic_cost_gate_enabled) {
                        int cap_blocked = 0;
                        int next_consecutive = 0;
                        int gate_reason = LP_PERIODIC_COST_DAMPEN_BLOCK_INVALID_PHASE;
                        int should_defer = simplex_periodic_cost_defer_plan_for_test(
                            2,
                            tab->m,
                            use_bland,
                            degenerate_count,
                            tab->lu->num_updates,
                            tab->lu->max_updates,
                            tab->lu->spike_pool_used,
                            tab->lu->spike_pool_capacity,
                            tab->lu->cond_estimate,
                            tab->lu->growth_factor,
                            soft_lu_refactor_cost_ewma(solver, 2),
                            soft_lu_iter_cost_ewma(solver, 2),
                            periodic_cost_refactor_samples(solver, 2),
                            periodic_cost_iter_samples(solver, 2),
                            periodic_cost_consecutive_defers(solver, 2),
                            &gate_reason,
                            NULL,
                            &cap_blocked,
                            &next_consecutive);
                        periodic_cost_record_gate_reason(
                            solver, 2, (LPPeriodicCostDampenReason)gate_reason);
                        if (should_defer) {
                            periodic_refactor = 0;
                            periodic_cost_record_defer(solver, 2);
                            periodic_cost_set_consecutive_defers(solver, 2, next_consecutive);
                            if (solver->verbose >= 2) {
                                LP_LOG_STDERR("[primal_simplex] Deferred policy periodic refactor by cost gate (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                                        tab->lu->num_updates,
                                        tab->lu->max_updates,
                                        degenerate_count,
                                        soft_lu_iter_cost_ewma(solver, 2),
                                        soft_lu_refactor_cost_ewma(solver, 2));
                            }
                        } else {
                            periodic_cost_reset_defer_streak(solver, 2);
                            if (cap_blocked) {
                                periodic_cost_record_cap_forced(solver, 2);
                                if (solver->verbose >= 2) {
                                    LP_LOG_STDERR("[primal_simplex] Policy periodic defer cap reached; forcing periodic policy refactor (updates=%d/%d, degen=%d)\n",
                                            tab->lu->num_updates,
                                            tab->lu->max_updates,
                                            degenerate_count);
                                }
                            } else if (solver->verbose >= 3) {
                                LP_LOG_STDERR("[primal_simplex] Policy periodic cost gate blocked defer: %s\n",
                                        lp_refactor_policy_periodic_cost_dampen_reason_string(
                                            (LPPeriodicCostDampenReason)gate_reason));
                            }
                        }
                    } else {
                        periodic_cost_reset_defer_streak(solver, 2);
                    }
                }
            }
            needs_refactor = periodic_refactor;
        } else {
            reinvert_shadow_prepare_phase(solver,
                                          tab,
                                          2,
                                          iter,
                                          &lu_health_decision,
                                          0,
                                          periodic_policy.min_update_age,
                                          periodic_policy_cooldown,
                                          0,
                                          NULL,
                                          &reinvert_shadow_eval);
        }
        if (periodic_refactor) {
            periodic_feedback_set_hint(solver, 2, periodic_policy.interval, effective_policy_pressure);
        }
        {
            int shadow_refactor = lp_basis_governor_shadow_decide(
                LP_BASIS_GOV_PHASE2,
                lu_refactor_nominal,
                periodic_refactor_nominal);
            int governed_refactor = lp_basis_governor_decide_refactor(
                &solver->policy.basis_governor,
                LP_BASIS_GOV_PHASE2,
                lu_refactor_nominal,
                periodic_refactor_nominal,
                needs_refactor);
            if (solver->telemetry_enabled) {
                lp_basis_governor_observe_refactor(
                    &solver->policy.basis_governor,
                    LP_BASIS_GOV_PHASE2,
                    shadow_refactor,
                    governed_refactor);
            }
            needs_refactor = governed_refactor;
        }
        reinvert_shadow_finalize_phase(solver, 2, &reinvert_shadow_eval, needs_refactor);

        if (needs_refactor) {
            soft_lu_reset_defer_streak(solver, 2);
            periodic_cost_reset_defer_streak(solver, 2);
            runtime_record_periodic_refactor_trigger(solver, 2, lu_refactor_needed);
            int rc_refactor;
            double refactor_elapsed_ms = 0.0;
            {
                double t_refactor_ms = lp_telemetry_timer_start();
                rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
                refactor_elapsed_ms = lp_telemetry_timer_elapsed_ms(t_refactor_ms);
                lp_telemetry_add_refactor_runtime_ms(solver, refactor_elapsed_ms);
            }
            if (rc_refactor == 0) {
                soft_lu_record_refactor_cost(solver, 2, refactor_elapsed_ms);
                lp_reinvert_controller_state_record_refactor_cost(
                    reinvert_state_for_phase(solver, 2), refactor_elapsed_ms);
            }
            if (lu_refactor_needed && rc_refactor == 0) {
                lu_soft_health_streak = 0;
            }
            if (!lu_refactor_needed && periodic_refactor) {
                if (rc_refactor == 0 && cooldown_eligible) {
                    int cooldown = lp_refactor_policy_phase2_cooldown_window_updates(periodic_policy.interval);
                    if (cooldown > periodic_policy_cooldown) {
                        periodic_policy_cooldown = cooldown;
                    }
                    periodic_policy_pressure_decay += PHASE2_POLICY_PRESSURE_DECAY_STEP;
                    if (periodic_policy_pressure_decay > PHASE2_POLICY_PRESSURE_DECAY_MAX) {
                        periodic_policy_pressure_decay = PHASE2_POLICY_PRESSURE_DECAY_MAX;
                    }
                } else if (rc_refactor != 0) {
                    periodic_policy_cooldown = 0;
                    periodic_policy_pressure_decay = 0.0;
                }
            }
            if (rc_refactor != 0) {
                if (solver->verbose) {
                    LP_LOG_STDERR("[primal_simplex] ERROR: refactorization failed at iter %d, attempting repair\n", iter);
                }
                /* Try to repair the singular basis */
                if (repair_singular_basis(tab) != 0) {
                    if (solver->verbose) {
                        LP_LOG_STDERR("[primal_simplex] ERROR: basis repair failed at iter %d\n", iter);
                    }
                    primal_remove_perturbation(tab);
                    solver->status = RALPH_STATUS_ERROR;
                    solver->iterations = iter;
                    return -1;
                }
                if (solver->verbose) {
                    LP_LOG_STDERR("[primal_simplex] Basis repaired at iter %d\n", iter);
                }
            }
            /* After refactorization, recompute solution to eliminate drift */
            tableau_compute_solution(tab);
            /* For partial pricing, use lazy RC computation (duals only).
             * For other strategies, compute full RC for incremental updates. */
            if (solver->pricing_strategy == 3) {
                tableau_compute_duals(tab);  /* Lazy mode: duals only */
            } else {
                tableau_compute_reduced_costs(tab);  /* Full RC for incremental updates */
                if (solver->pricing_strategy == 4) heap_build(tab);
            }
        } else if (lu_soft_cost_deferred && solver->verbose >= 2) {
            LP_LOG_STDERR("[primal_simplex] Deferred soft LU-health periodic refactor (updates=%d/%d, degen=%d, iter_ewma=%.3fms, ref_ewma=%.3fms)\n",
                    tab->lu->num_updates,
                    tab->lu->max_updates,
                    degenerate_count,
                    soft_lu_iter_cost_ewma(solver, 2),
                    soft_lu_refactor_cost_ewma(solver, 2));
        }

        /* Periodically recompute solution and reduced costs to correct numerical drift.
         * For large, highly-degenerate phase-2 runs with healthy LU metrics, we relax
         * cadence to reduce full-vector recompute overhead. */
        {
            int periodic_recompute_interval =
                lp_refactor_policy_phase2_periodic_recompute_interval(
                    tab->m,
                    use_bland,
                    degenerate_count,
                    tab->lu->spike_pool_used,
                    tab->lu->spike_pool_capacity,
                    tab->lu->cond_estimate,
                    tab->lu->growth_factor);
            if (iter > 0 &&
                periodic_recompute_interval > 0 &&
                (iter % periodic_recompute_interval) == 0) {
                tableau_compute_solution(tab);
                /* For partial pricing, use lazy RC. For others, full RC. */
                if (solver->pricing_strategy == 3) {
                    tableau_compute_duals(tab);  /* Lazy mode */
                } else {
                    tableau_compute_reduced_costs(tab);  /* Full recomputation */
                    if (solver->pricing_strategy == 4) heap_build(tab);
                }
                if (solver->verbose) {
                    int leave_var = (leaving >= 0) ? tab->basis[leaving] : leaving;
                    LP_LOG_STDOUT("Iter %d: obj = %.6f, enter=%d, leave=%d, theta=%.2e, rc=%.2e\n",
                           iter, tab->obj_value, entering, leave_var, theta, tab->rc[entering]);
                }
            }
        }

        /* Stall detection: check objective progress after each pivot.
         * If objective hasn't improved for P2_STALL_THRESHOLD iterations,
         * remove+re-apply perturbation with progressive scaling to break
         * the cycling pattern. This catches cases where Bland's rule is
         * active but making negligible progress (O(2^n) worst case).
         * Independent of the degeneracy counter — triggered by objective stagnation. */
        {
        double obj_tol_p2 = 1e-4 * (1.0 + fabs(last_obj_p2));
        double obj_change_p2 = fabs(tab->obj_value - last_obj_p2);
        if (obj_change_p2 < obj_tol_p2) {
            stall_count_p2++;
            if (stall_count_p2 >= P2_STALL_THRESHOLD &&
                perturb_attempts_p2 < PHASE2_DEGEN_ESCAPE_MAX_ATTEMPTS &&
                tab->m >= PHASE2_DEGEN_ESCAPE_MIN_M &&
                degenerate_count >= PHASE2_DEGEN_ESCAPE_DEGEN_TRIGGER &&
                solver->policy.periodic_policy_refactors_phase2 >= PHASE2_DEGEN_ESCAPE_POLICY_TRIGGER) {
                double scale = 4.0 + 2.0 * (double)perturb_attempts_p2;
                primal_apply_perturbation_scaled(tab, scale);
                perturbation_active = 1;
                use_bland = 1;
                degenerate_count = 0;
                /* Keep Bland active briefly before allowing fast pricing again. */
                non_degen_streak = -PHASE2_DEGEN_ESCAPE_BLAND_HOLD_ITERS;
                stall_count_p2 = 0;
                perturb_attempts_p2++;
                tableau_compute_solution(tab);
                if (solver->pricing_strategy == 3) {
                    tableau_compute_duals(tab);
                } else {
                    tableau_compute_reduced_costs(tab);
                    if (solver->pricing_strategy == 4) heap_build(tab);
                }
                if (solver->verbose) {
                    LP_LOG_STDOUT("Iter %d: Phase 2 degen-escape (scale %.1f)\n", iter, scale);
                }
                continue;
            }
            if (stall_count_p2 >= P2_STALL_THRESHOLD) {
                perturb_attempts_p2++;
                if (perturb_attempts_p2 <= P2_MAX_PERTURB_ATTEMPTS) {
                    double scale = 1.0 + 2.0 * perturb_attempts_p2;
                    primal_apply_perturbation_scaled(tab, scale);
                    perturbation_active = 1;
                    /* Reset Bland's rule — fresh perturbation should break the
                     * cycle, allowing faster pricing to make progress again */
                    use_bland = 0;
                    degenerate_count = 0;
                    non_degen_streak = 0;
                    stall_count_p2 = 0;
                    /* Recompute after perturbation change */
                    tableau_compute_solution(tab);
                    if (solver->pricing_strategy == 3) {
                        tableau_compute_duals(tab);
                    } else {
                        tableau_compute_reduced_costs(tab);
                        if (solver->pricing_strategy == 4) heap_build(tab);
                    }
                    if (solver->verbose) {
                        LP_LOG_STDOUT("Iter %d: Phase 2 stall detected, re-perturbing (attempt %d, scale %.1f)\n",
                               iter, perturb_attempts_p2, scale);
                    }
                }
                /* else: exhausted attempts, fall through to iteration limit */
            }
        } else {
            stall_count_p2 = 0;
            last_obj_p2 = tab->obj_value;
        }
        }  /* end stall detection block */

    }

    primal_remove_perturbation(tab);
    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    solver->iterations = solver->max_iterations;
    return -1;
}

/* ============================================================================
 * Triangular crash basis (Maros LTSF)
 *
 * Replace slack variables (from <= rows) in the initial basis with structural
 * columns that have good pivot elements. This reduces Phase 1 iterations
 * by starting closer to a feasible basis.
 *
 * IMPORTANT: Only displace slacks (from <= rows). Never displace artificials
 * or surplus variables — these are needed for Phase 1 feasibility tracking.
 *
 * Algorithm:
 *   Pass 1: Scan structural columns for singletons — if the singleton element
 *           is in an eligible row with |a_ij| > PIVOT_TOL, swap into basis.
 *   Pass 2: Scan remaining columns for the best pivot in eligible unclaimed rows.
 *
 * Returns: number of structural columns placed in basis.
 * ============================================================================ */
static int crash_triangular(SimplexTableau *tab, int verbose) {
    if (!tab || !tab->A_ext) return 0;

    int m = tab->m;
    int n_structural = tab->model->num_vars;
    SparseMatrix *A = tab->A_ext;
    int placed = 0;

    /* Identify which rows are eligible for crash (only slack-basic rows).
     * A row is eligible if its basic variable is a slack (not artificial/surplus).
     * Slacks are aux variables with +1 coefficient in their row. Artificials
     * and surplus variables must not be displaced. */
    int *row_eligible = (int *)calloc(m, sizeof(int));
    if (!row_eligible) return 0;

    for (int i = 0; i < m; i++) {
        int bv = tab->basis[i];
        /* Only eligible if basic var is an auxiliary (slack/surplus/artificial) */
        if (bv >= n_structural) {
            /* Check if this is a simple slack or surplus: |coeff| == 1, zero cost.
             * Slacks have +1 coeff, surplus have -1 coeff — both displaceable. */
            int is_slack = 0;
            for (int p = A->colptr[bv]; p < A->colptr[bv + 1]; p++) {
                if (A->rowidx[p] == i) {
                    if ((fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL ||
                         fabs(A->values[p] + 1.0) < RALPH_ZERO_TOL) &&
                        fabs(tab->c_ext[bv]) < RALPH_ZERO_TOL) {
                        is_slack = 1;
                    }
                    break;
                }
            }
            row_eligible[i] = is_slack;
        }
    }

    /* Track which rows have been claimed by a structural variable */
    int *row_claimed = (int *)calloc(m, sizeof(int));
    int *col_used = (int *)calloc(n_structural, sizeof(int));
    if (!row_claimed || !col_used) {
        free(row_eligible); free(row_claimed); free(col_used);
        return 0;
    }

    /* Pass 1: Singletons in eligible rows.
     * For singletons, we can exactly compute x_j = rhs[i] / a[i,j].
     * Only accept if x_j is within bounds [lb, ub]. */
    for (int j = 0; j < n_structural; j++) {
        if (fabs(tab->ub_ext[j] - tab->lb_ext[j]) < RALPH_ZERO_TOL) continue;

        int nnz = 0;
        int singleton_row = -1;
        double singleton_val = 0.0;

        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int row = A->rowidx[p];
            if (row < m) {
                nnz++;
                singleton_row = row;
                singleton_val = A->values[p];
            }
        }

        if (nnz == 1 && singleton_row >= 0 &&
            row_eligible[singleton_row] && !row_claimed[singleton_row] &&
            fabs(singleton_val) > RALPH_PIVOT_TOL) {
            /* Check feasibility: x_j = rhs / a_ij */
            double xval = tab->rhs[singleton_row] / singleton_val;
            if (xval < tab->lb_ext[j] - RALPH_FEAS_TOL ||
                xval > tab->ub_ext[j] + RALPH_FEAS_TOL) continue;

            int old_basic = tab->basis[singleton_row];
            tab->var_status[old_basic] = RALPH_NONBASIC_LOWER;
            tab->x[old_basic] = tab->lb_ext[old_basic];
            tab->basis_pos[old_basic] = -1;

            tab->basis[singleton_row] = j;
            tab->basis_pos[j] = singleton_row;
            tab->var_status[j] = RALPH_BASIC;

            row_claimed[singleton_row] = 1;
            col_used[j] = 1;
            placed++;
        }
    }

    /* Pass 2: Multi-element columns in eligible unclaimed rows */
    for (int j = 0; j < n_structural; j++) {
        if (col_used[j]) continue;
        if (fabs(tab->ub_ext[j] - tab->lb_ext[j]) < RALPH_ZERO_TOL) continue;

        int unclaimed_nnz = 0;
        int best_row = -1;
        double best_val = 0.0;
        double best_actual_val = 0.0;

        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int row = A->rowidx[p];
            if (row < m && row_eligible[row] && !row_claimed[row]) {
                unclaimed_nnz++;
                double absval = fabs(A->values[p]);
                if (absval > best_val) {
                    best_val = absval;
                    best_actual_val = A->values[p];
                    best_row = row;
                }
            }
        }

        if (best_row >= 0 && best_val > RALPH_PIVOT_TOL && unclaimed_nnz <= m / 2 + 1) {
            /* Approximate feasibility: x_j ~ rhs[best_row] / a[best_row,j].
             * For multi-element columns this is approximate (ignores other basics),
             * but filters out clearly infeasible placements. */
            double approx_xval = tab->rhs[best_row] / best_actual_val;
            if (approx_xval < tab->lb_ext[j] - RALPH_FEAS_TOL ||
                approx_xval > tab->ub_ext[j] + RALPH_FEAS_TOL) continue;

            int old_basic = tab->basis[best_row];
            tab->var_status[old_basic] = RALPH_NONBASIC_LOWER;
            tab->x[old_basic] = tab->lb_ext[old_basic];
            tab->basis_pos[old_basic] = -1;

            tab->basis[best_row] = j;
            tab->basis_pos[j] = best_row;
            tab->var_status[j] = RALPH_BASIC;

            row_claimed[best_row] = 1;
            col_used[j] = 1;
            placed++;
        }
    }

    free(row_eligible);
    free(row_claimed);
    free(col_used);

    if (verbose && placed > 0) {
        LP_LOG_STDOUT("[crash] Placed %d structural columns in basis (of %d rows)\n",
               placed, m);
    }

    return placed;
}

static void reset_solver_perf(SimplexSolver *solver) {
    lp_telemetry_reset_solver(solver);
    if (!solver) return;
    solver->policy.periodic_policy_refactors_phase1 = 0;
    solver->policy.periodic_policy_refactors_phase2 = 0;
}

static int lp_time_limit_exceeded(SimplexSolver *solver, int iter) {
    if (!solver) return 0;
    if (solver->time_limit <= 0.0 || solver->time_limit >= RALPH_INFINITY / 2.0) {
        return 0;
    }
    const double time_limit_sec = solver->time_limit * 1.05;

    double now_ms = lp_telemetry_now_ms();
    if (solver->progress_start_ms <= 0.0) {
        solver->progress_start_ms = now_ms;
    }

    double elapsed_sec = (now_ms - solver->progress_start_ms) / 1000.0;
    if (elapsed_sec <= time_limit_sec) {
        return 0;
    }

    solver->status = RALPH_STATUS_TIME_LIMIT;
    solver->iterations = iter;
    return 1;
}

static int lp_run_user_callbacks(SimplexSolver *solver,
                                 const SimplexTableau *tab,
                                 RalphLPProgressPhase phase,
                                 int iter,
                                 int force_emit,
                                 int honor_progress_cancel) {
    if (!solver) return 0;
    if (lp_time_limit_exceeded(solver, iter)) return 1;

    if (solver->has_lp_cancel_callback && solver->lp_cancel_callback.should_cancel) {
        if (solver->lp_cancel_callback.should_cancel(solver->lp_cancel_callback.user_data) != 0) {
            solver->status = RALPH_STATUS_TIME_LIMIT;
            solver->iterations = iter;
            return 1;
        }
    }

    if (!solver->has_lp_progress_callback || !solver->lp_progress_callback.on_progress) {
        return 0;
    }

    int stride = solver->lp_progress_callback.every_n_iterations;
    if (stride <= 0) stride = 1;
    if (!force_emit && iter > 0 && (iter % stride) != 0) {
        return 0;
    }

    RalphLPProgressInfo info;
    memset(&info, 0, sizeof(info));
    info.phase = phase;
    info.iteration = iter;
    info.status = solver->status;
    if (solver->progress_start_ms > 0.0) {
        double elapsed_ms = lp_telemetry_now_ms() - solver->progress_start_ms;
        if (elapsed_ms > 0.0) info.elapsed_time_sec = elapsed_ms / 1000.0;
    }
    if (solver->model) {
        if (tab) {
            info.objective = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
        } else {
            info.objective = solver->obj_value;
        }
    }

    if (solver->verify &&
        (solver->status == RALPH_STATUS_OPTIMAL ||
         solver->status == RALPH_STATUS_IMPRECISE ||
         solver->status == RALPH_STATUS_OBJ_LIMIT)) {
        info.quality_available = 1;
        info.primal_infeas = solver->verify_primal_infeas;
        info.bound_infeas = solver->verify_bound_infeas;
        info.dual_infeas = solver->verify_dual_infeas;
        info.comp_slack = solver->verify_comp_slack;
        info.obj_error = solver->verify_obj_error;
        info.cond_estimate = solver->verify_cond_estimate;
    }

    int rc = solver->lp_progress_callback.on_progress(
        solver->lp_progress_callback.user_data, &info);
    if (honor_progress_cancel && rc != 0) {
        solver->status = RALPH_STATUS_TIME_LIMIT;
        solver->iterations = iter;
        return 1;
    }
    return 0;
}

static void configure_tableau_for_solver(SimplexSolver *solver, SimplexTableau *tab) {
    if (!solver || !tab) return;

    if (!lp_basis_governor_mode_is_valid(solver->policy.basis_governor_mode)) {
        solver->policy.basis_governor_mode = LP_BASIS_GOV_MODE_OFF;
    }
    if (!lp_reinvert_controller_mode_is_valid(solver->policy.reinvert_controller_mode)) {
        solver->policy.reinvert_controller_mode = LP_REINVERT_MODE_SHADOW;
    }
    lp_basis_governor_set_mode(&solver->policy.basis_governor,
                               solver->policy.basis_governor_mode);

    tab->owner = solver;
    tab->use_steepest_edge = (solver->pricing_strategy == 1 || solver->pricing_strategy == 2
                              || solver->pricing_strategy == 5);
    tab->pricing_strategy = solver->pricing_strategy;
    tab->trace_phase1_enabled = solver->trace_phase1;
    if (tab->lu) {
        int enable_supernode = 0;
        tab->lu->telemetry_enabled = solver->telemetry_enabled;
        if (solver->policy.basis_governor_mode == LP_BASIS_GOV_MODE_OFF) {
            tab->lu->basis_governor = NULL;
        } else {
            tab->lu->basis_governor = &solver->policy.basis_governor;
        }

        tab->lu->mkz_enabled = 1;
        if (solver->lu_supernode) {
            enable_supernode = 1;
        } else if (tab->m > 300) {
            enable_supernode = 1;
        }

        /* Runtime backend path is currently LUF+FT only.
         * Keep BFCP backend ids as API surface, but do not pseudo-map CBG/CGR
         * to unrelated sparse/dense toggles in simplex internals. */
        (void)solver->lu_backend_policy;
        tab->lu->sn_enabled = enable_supernode ? 1 : 0;

        if (solver->lu_update_limit_override > 0) {
            tab->lu->max_updates = solver->lu_update_limit_override;
        }
        if (solver->lu_pivot_tol_override > 0.0) {
            tab->lu->pivot_tol = solver->lu_pivot_tol_override;
        }
        if (solver->lu_growth_guard_override > 0.0) {
            tab->lu->growth_refactor_threshold = solver->lu_growth_guard_override;
        } else {
            tab->lu->growth_refactor_threshold = RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
        }
    }
    tab->trace_phase1_iter = -1;
    tab->trace_last_entering = -1;
    tab->trace_last_leaving_pos = -1;
    tab->trace_last_theta = 0.0;
    tab->trace_last_pivot = 0.0;
    tab->trace_last_dir_inf = 0.0;
    tab->trace_last_fail_reason = PHASE1_PIVOT_FAIL_NONE;
}

/* Create, configure, and factorize a primal tableau. */
static int setup_primal_tableau(SimplexSolver *solver, int allow_crash) {
    if (!solver) return -1;

    if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Creating tableau...\n");
    solver->tableau = tableau_create_ex(solver->model, solver->force_two_phase, 0);
    if (!solver->tableau) {
        solver->status = RALPH_STATUS_ERROR;
        return -1;
    }

    SimplexTableau *tab = solver->tableau;
    configure_tableau_for_solver(solver, tab);

    if (solver->verbose) {
        LP_LOG_STDOUT("[simplex_solve] Tableau: n=%d (extended), m=%d\n", tab->n, tab->m);
    }

    int warm_basis_applied = 0;
    solver->warm_basis_last_attempted = 0;
    solver->warm_basis_last_applied = 0;
    solver->warm_basis_last_rejected = 0;
    if (solver->warm_basis && solver->warm_var_status) {
        solver->warm_basis_last_attempted = 1;
        int warm_rc = tableau_apply_warm_basis(tab,
                                               solver->warm_basis_m,
                                               solver->warm_basis_n,
                                               solver->warm_basis,
                                               solver->warm_var_status);
        if (warm_rc != 0 && solver->verbose) {
            LP_LOG_STDOUT("[simplex_solve] Warm basis rejected; using cold-start basis\n");
        } else if (warm_rc == 0 && solver->verbose) {
            LP_LOG_STDOUT("[simplex_solve] Warm basis accepted\n");
        }
        if (warm_rc == 0) {
            warm_basis_applied = 1;
            solver->warm_basis_last_applied = 1;
        } else {
            solver->warm_basis_last_rejected = 1;
        }

        /* Consume staged warm basis once per solve attempt. */
        free(solver->warm_basis);
        solver->warm_basis = NULL;
        free(solver->warm_var_status);
        solver->warm_var_status = NULL;
        solver->warm_basis_m = 0;
        solver->warm_basis_n = 0;
    }

    int *saved_basis = NULL;
    int *saved_basis_pos = NULL;
    VarStatus *saved_var_status = NULL;

    if (allow_crash && !warm_basis_applied) {
        saved_basis = (int *)malloc(tab->m * sizeof(int));
        saved_basis_pos = (int *)malloc(tab->n * sizeof(int));
        saved_var_status = (VarStatus *)malloc(tab->n * sizeof(VarStatus));
        if (saved_basis && saved_basis_pos && saved_var_status) {
            memcpy(saved_basis, tab->basis, tab->m * sizeof(int));
            memcpy(saved_basis_pos, tab->basis_pos, tab->n * sizeof(int));
            memcpy(saved_var_status, tab->var_status, tab->n * sizeof(VarStatus));
        }
        crash_triangular(tab, solver->verbose);
    }

    if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Factorizing initial basis...\n");
    double t_refactor_ms = lp_telemetry_timer_start();
    int factorize_ok = (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_SETUP) == 0);
    lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);

    if (!factorize_ok && allow_crash && saved_basis) {
        if (solver->verbose)
            LP_LOG_STDOUT("[simplex_solve] Crash basis singular, restoring original basis\n");
        memcpy(tab->basis, saved_basis, tab->m * sizeof(int));
        memcpy(tab->basis_pos, saved_basis_pos, tab->n * sizeof(int));
        memcpy(tab->var_status, saved_var_status, tab->n * sizeof(VarStatus));
        for (int j = 0; j < tab->n; j++)
            tab->x[j] = tab->lb_ext[j];
        t_refactor_ms = lp_telemetry_timer_start();
        factorize_ok = (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_SETUP) == 0);
        lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
    }

    if (!factorize_ok) {
        free(saved_basis);
        free(saved_basis_pos);
        free(saved_var_status);
        solver->status = RALPH_STATUS_ERROR;
        return -1;
    }

    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    if (allow_crash && saved_basis) {
        int crash_infeasible = 0;
        for (int i = 0; i < tab->m; i++) {
            int bv = tab->basis[i];
            double val = tab->x[bv];
            if (val < tab->lb_ext[bv] - RALPH_FEAS_TOL ||
                val > tab->ub_ext[bv] + RALPH_FEAS_TOL) {
                crash_infeasible = 1;
                if (solver->verbose)
                    LP_LOG_STDOUT("[crash] Basic var %d in row %d: x=%.6e outside [%.6e, %.6e], reverting\n",
                           bv, i, val, tab->lb_ext[bv], tab->ub_ext[bv]);
                break;
            }
        }
        if (crash_infeasible) {
            if (solver->verbose)
                LP_LOG_STDOUT("[crash] Post-verify failed, restoring original basis\n");
            memcpy(tab->basis, saved_basis, tab->m * sizeof(int));
            memcpy(tab->basis_pos, saved_basis_pos, tab->n * sizeof(int));
            memcpy(tab->var_status, saved_var_status, tab->n * sizeof(VarStatus));
            for (int j = 0; j < tab->n; j++)
                tab->x[j] = tab->lb_ext[j];
            t_refactor_ms = lp_telemetry_timer_start();
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_SETUP) != 0) {
                lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
                free(saved_basis);
                free(saved_basis_pos);
                free(saved_var_status);
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        }
    }
    free(saved_basis);
    free(saved_basis_pos);
    free(saved_var_status);

    if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Initial factorization OK\n");
    return 0;
}

int simplex_solve(SimplexSolver *solver) {
    if (!solver || !solver->model) return -1;

    clock_t start = clock();
    solver->progress_start_ms = lp_telemetry_now_ms();
    lp_determinism_apply_runtime(solver);

    /* Invalidate cached outputs from any previous solve.
     * This prevents stale primal/dual data from being reused when the current
     * solve fails before producing new solution vectors. */
    free(solver->solution);
    solver->solution = NULL;
    free(solver->dual_solution);
    solver->dual_solution = NULL;
    free(solver->reduced_costs);
    solver->reduced_costs = NULL;
    solver->farkas_valid = 0;
    solver->unbounded_valid = 0;

    solver->trace_phase1_pivot_failures = 0;
    solver->trace_phase1_fail_small_pivot = 0;
    solver->trace_phase1_fail_invalid_column = 0;
    solver->trace_phase1_fail_lu_max_updates = 0;
    solver->trace_phase1_fail_lu_spike_pool_full = 0;
    solver->trace_phase1_fail_lu_update_pivot_small = 0;
    solver->trace_phase1_fail_lu_singular_update = 0;
    solver->trace_phase1_fail_factor_singular = 0;
    solver->trace_phase1_fail_refactor_forced_other = 0;
    solver->trace_phase1_fail_refactor_after_update_other = 0;
    solver->trace_phase1_no_entering_events = 0;
    solver->trace_phase1_first_fail_iter = -1;
    solver->trace_phase1_last_fail_iter = -1;
    solver->trace_phase1_signature = solver->trace_phase1 ? 1469598103934665603ULL : 0ULL;
    solver->verify_primal_infeas = 0.0;
    solver->verify_bound_infeas = 0.0;
    solver->verify_dual_infeas = 0.0;
    solver->verify_comp_slack = 0.0;
    solver->verify_obj_error = 0.0;
    solver->verify_cond_estimate = 0.0;

    if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Starting...\n");

    /* Finalize model if needed (required before scaling) */
    if (!solver->model->A) {
        if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Finalizing model...\n");
        if (lp_model_finalize(solver->model) != 0) {
            if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] ERROR: lp_model_finalize failed\n");
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }
    }

    if (solver->verbose) {
        LP_LOG_STDOUT("[simplex_solve] Model: %d vars, %d cons, %d nnz\n",
               solver->model->num_vars, solver->model->num_cons,
               solver->model->A ? solver->model->A->nnz : 0);
    }

    /* Apply scaling if enabled */
    if (solver->scaling) {
        if (apply_scaling(solver) != 0) {
            /* Scaling failed, continue without scaling */
            solver->is_scaled = 0;
        }
    }

    reset_solver_perf(solver);

    SimplexTableau *tab = NULL;

    /* T1.3: Method dispatch — dual simplex path.
     * For methods 1/2, avoid creating/factorizing a primal tableau up front. */
    if (solver->method == 1 || solver->method == 2) {
        if (solver->verbose)
            LP_LOG_STDOUT("[simplex_solve] Trying dual simplex path (method=%d)\n", solver->method);

        double t_dual_ms = lp_telemetry_timer_start();
        int drc = dual_simplex_solve_from_scratch_v2(solver);
        lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_DUAL, t_dual_ms);

        if (drc == 0 && solver->method == 2) {
            if (solver->status == RALPH_STATUS_OPTIMAL) {
                /* Auto mode: Ax=b sanity check before committing.
                 * Catches catastrophically wrong dual results. */
                SimplexTableau *dtab = solver->tableau;
                int bad = 0;
                for (int i = 0; i < dtab->m && !bad; i++) {
                    double ax = 0.0;
                    for (int j = 0; j < dtab->n; j++) {
                        if (fabs(dtab->x[j]) < RALPH_ZERO_TOL) continue;
                        for (int p = dtab->A_ext->colptr[j]; p < dtab->A_ext->colptr[j+1]; p++) {
                            if (dtab->A_ext->rowidx[p] == i) {
                                ax += dtab->A_ext->values[p] * dtab->x[j];
                                break;
                            }
                        }
                    }
                    if (fabs(ax - dtab->rhs[i]) > 1e-4) bad = 1;
                }
                if (bad) {
                    if (solver->verbose)
                        LP_LOG_STDOUT("[simplex_solve] Dual solution failed Ax=b check, falling back to primal\n");
                    drc = -1;
                }
            } else {
                /* Auto mode: don't trust dual INFEASIBLE/OBJ_LIMIT — fall back.
                 * Dual infeasibility detection is unreliable; primal Phase 1 is robust. */
                if (solver->verbose)
                    LP_LOG_STDOUT("[simplex_solve] Dual returned non-optimal status %d, falling back to primal\n",
                           solver->status);
                drc = -1;
            }
        }

        if (drc == 0) {
            /* Commit to dual result */
            solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            unscale_solution(solver);
            restore_model(solver);

            /* Auto mode: always verify to flag suboptimal dual solutions.
             * Dual can terminate with feasible but non-optimal basis. */
            if (solver->status == RALPH_STATUS_OPTIMAL &&
                (solver->verify || solver->method == 2))
                verify_solution(solver);

            /* Auto mode: if verify downgraded to IMPRECISE, fall back to primal
             * rather than returning a bad dual solution. */
            if (solver->method == 2 && solver->status == RALPH_STATUS_IMPRECISE) {
                if (solver->verbose)
                    LP_LOG_STDOUT("[simplex_solve] Dual solution imprecise, falling back to primal\n");
                solver->status = RALPH_STATUS_UNKNOWN;
                drc = -1;  /* Trigger primal fallback below */
            } else {
                lp_run_user_callbacks(solver,
                                      solver->tableau,
                                      RALPH_LP_PROGRESS_PHASE_DUAL,
                                      solver->iterations,
                                      1,
                                      0);
                return 0;
            }
        }

        if (solver->status == RALPH_STATUS_TIME_LIMIT) {
            solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            restore_model(solver);
            lp_run_user_callbacks(solver,
                                  solver->tableau,
                                  RALPH_LP_PROGRESS_PHASE_DUAL,
                                  solver->iterations,
                                  1,
                                  0);
            return -1;
        }

        /* Dual failed or rejected — fall back to primal (method=2) or error (method=1) */
        if (solver->method == 2) {
            solver->from_dual_fallback = 1;
            if (solver->verbose)
                LP_LOG_STDOUT("[simplex_solve] Falling back to primal\n");

            if (solver->tableau) {
                tableau_free(solver->tableau);
                solver->tableau = NULL;
            }

            double t_setup_ms = lp_telemetry_timer_start();
            if (setup_primal_tableau(solver, solver->crash) != 0) {
                return -1;
            }
            lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_PRIMAL_SETUP, t_setup_ms);
            tab = solver->tableau;
        } else {
            solver->status = RALPH_STATUS_ERROR;
            solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            return -1;
        }
    } else {
        double t_setup_ms = lp_telemetry_timer_start();
        if (setup_primal_tableau(solver, solver->crash && solver->method == 0) != 0) {
            return -1;
        }
        lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_PRIMAL_SETUP, t_setup_ms);
        tab = solver->tableau;
    }

    /* T3.4: Override pricing strategy for Phase 1 if configured.
     * Two-phase simplex requires full pricing during Phase 1 — partial pricing
     * can miss improving directions for artificial variables. Default to Devex
     * for Phase 1 when partial/heap pricing is selected. */
    int saved_pricing = solver->pricing_strategy;
    int saved_tab_pricing = tab->pricing_strategy;
    int saved_tab_se = tab->use_steepest_edge;
    if (solver->phase1_pricing >= 0) {
        solver->pricing_strategy = solver->phase1_pricing;
        tab->pricing_strategy = solver->phase1_pricing;
        tab->use_steepest_edge = (solver->phase1_pricing == 1 || solver->phase1_pricing == 2
                                  || solver->phase1_pricing == 5);
    } else if (tab->use_two_phase && (solver->pricing_strategy == 3 || solver->pricing_strategy == 4)) {
        solver->pricing_strategy = 2;  /* Devex for Phase 1 */
        tab->pricing_strategy = 2;
        tab->use_steepest_edge = 1;
    }

    /* Phase 1: Find feasible solution */
    if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Starting Phase 1...\n");
    {
        double t_phase1_ms = lp_telemetry_timer_start();
        if (simplex_phase1(solver) != 0) {
            lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_PHASE1, t_phase1_ms);
            solver->pricing_strategy = saved_pricing;  /* T3.4: restore pricing */
            tab->pricing_strategy = saved_tab_pricing;
            tab->use_steepest_edge = saved_tab_se;
            if (solver->status == RALPH_STATUS_INFEASIBLE) {
                if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Phase 1: INFEASIBLE\n");
                lp_run_user_callbacks(solver,
                                      tab,
                                      RALPH_LP_PROGRESS_PHASE_1,
                                      solver->iterations,
                                      1,
                                      0);
                return 0;  /* Infeasible is a valid result */
            }
            return -1;
        }
        lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_PHASE1, t_phase1_ms);
    }
    solver->pricing_strategy = saved_pricing;  /* T3.4: restore pricing for Phase 2 */
    tab->pricing_strategy = saved_tab_pricing;
    tab->use_steepest_edge = saved_tab_se;
    if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Phase 1 complete\n");

    /* Transition to Phase 2 if using two-phase simplex */
    int two_phase_failed = 0;
    if (tab->use_two_phase) {
        if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Transitioning to Phase 2...\n");
        double t_transition_ms = lp_telemetry_timer_start();
        if (simplex_transition_phase2(solver) != 0) {
            lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_TRANSITION, t_transition_ms);
            two_phase_failed = 1;
        } else {
            lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_TRANSITION, t_transition_ms);
            if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Phase 2 transition complete\n");
        }
    }

    /* Phase 2: Optimize (skip if transition failed) */
    if (!two_phase_failed) {
        double t_phase2_ms = lp_telemetry_timer_start();
        int status = simplex_phase2(solver);
        lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_PHASE2, t_phase2_ms);
        (void)status;  /* Status is set in solver->status directly */
        if (solver->status == RALPH_STATUS_ERROR && tab->use_two_phase) {
            two_phase_failed = 1;
        }
    }

    /* If two-phase failed, try dual simplex as a one-shot fallback.
     * Only for method=0 (explicit primal) to avoid circular chains with
     * method=2 (which already tried dual before falling back to primal).
     * The Phase 2 transition can fail on problems with redundant rows (e.g.,
     * assignment LPs, beaconfd) where stuck artificials make the basis singular. */
    if (two_phase_failed && solver->method == 0) {
        if (solver->verbose) {
            LP_LOG_STDOUT("[simplex_solve] Two-phase failed, trying dual simplex\n");
        }
        tableau_free(solver->tableau);
        solver->tableau = NULL;
        restore_model(solver);
        solver->is_scaled = 0;
        solver->status = RALPH_STATUS_UNKNOWN;
        double t_dual_ms = lp_telemetry_timer_start();
        int dual_result = dual_simplex_solve_from_scratch_v2(solver);
        lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_DUAL, t_dual_ms);
        solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
        if (solver->tableau) {
            tableau_free(solver->tableau);
            solver->tableau = NULL;
        }
        return dual_result;
    } else if (two_phase_failed) {
        /* method=2 already tried dual before primal — don't chain again */
        solver->status = RALPH_STATUS_ERROR;
        solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
        tableau_free(solver->tableau);
        solver->tableau = NULL;
        return -1;
    }

    solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;

    /* Compute true objective from structural variables only.
     * Auxiliary variables (slacks/surplus/artificials) have zero cost in Phase 2.
     * Phase 1 already certifies feasibility or infeasibility. */
    if (solver->status == RALPH_STATUS_OPTIMAL) {
        double true_obj = 0.0;
        for (int j = 0; j < solver->model->num_vars; j++) {
            true_obj += tab->c_ext[j] * tab->x[j];
        }
        solver->obj_value = true_obj * solver->model->obj_sense + solver->model->obj_offset;
    }

    /* Copy solution */
    if (solver->status == RALPH_STATUS_OPTIMAL) {
        solver->solution = (double*)calloc(solver->model->num_vars, sizeof(double));
        solver->dual_solution = (double*)calloc(solver->model->num_cons, sizeof(double));
        solver->reduced_costs = (double*)calloc(solver->model->num_vars, sizeof(double));

        if (solver->solution && solver->dual_solution && solver->reduced_costs) {
            for (int j = 0; j < solver->model->num_vars; j++) {
                solver->solution[j] = tab->x[j];
                solver->reduced_costs[j] = tab->rc[j] * solver->model->obj_sense;
            }
            for (int i = 0; i < solver->model->num_cons; i++) {
                solver->dual_solution[i] = tab->y[i] * solver->model->obj_sense;
            }
        }

        /* Unscale solution if scaling was applied */
        unscale_solution(solver);
    }

    /* Unscale unbounded ray if scaling was applied. */
    if (solver->is_scaled && solver->unbounded_valid &&
        solver->unbounded_ray && solver->col_scale) {
        for (int j = 0; j < solver->model->num_vars; j++) {
            solver->unbounded_ray[j] *= solver->col_scale[j];
        }
    }

    /* Restore original model if scaling was applied */
    restore_model(solver);

    /* Post-solve verification (T2.3 + T3.6) — runs on original-space solution */
    if (solver->verify && solver->status == RALPH_STATUS_OPTIMAL) {
        verify_solution(solver);
    }

    lp_run_user_callbacks(solver,
                          tab,
                          (tab && tab->phase == 1) ?
                              RALPH_LP_PROGRESS_PHASE_1 :
                              RALPH_LP_PROGRESS_PHASE_2,
                          solver->iterations,
                          1,
                          0);

    return (solver->status == RALPH_STATUS_OPTIMAL) ? 0 : -1;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

void lp_print_stats(const SimplexSolver *solver) {
    if (!solver) return;

    LP_LOG_STDOUT("\n=== Simplex Statistics ===\n");
    LP_LOG_STDOUT("Status: %d\n", solver->status);
    LP_LOG_STDOUT("Iterations: %d\n", solver->iterations);
    LP_LOG_STDOUT("Solve time: %.3f seconds\n", solver->solve_time);

    if (solver->status == RALPH_STATUS_OPTIMAL) {
        LP_LOG_STDOUT("Objective: %.10f\n", solver->obj_value);
    }
}
