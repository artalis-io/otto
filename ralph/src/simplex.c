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
#include "lp_glpk_strict.h"
#include "lp_refactor_policy.h"
#include "lp_policy_glpk_compat.h"
#include "lu_update_backend.h"
#include "lp_log.h"
#include "ralph_lp.h"
#include "simplex_internal.h"
#include "simplex_pricing.h"
#include "simplex_ratio.h"
#include "simplex_perturb.h"
#include "simplex_phase1_recovery.h"
#include "simplex_phase1_zones.h"
#include "simplex_phase1_stabilize.h"
#include "simplex_refactor_schedule.h"

/* Forward declarations */
int lp_model_finalize(LPModel *model);
static int lp_run_user_callbacks(SimplexSolver *solver,
                                 const SimplexTableau *tab,
                                 RalphLPProgressPhase phase,
                                 int iter,
                                 int force_emit,
                                 int honor_progress_cancel);
static int lp_time_limit_exceeded(SimplexSolver *solver, int iter);

/* PHASE1_WINDOW_PRESSURE_EVENT_* now in simplex_phase1_recovery.h */
/* PHASE1_PIVOT_FAIL_* enum now in simplex_phase1_recovery.h */

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

#define PHASE2_DEGEN_ESCAPE_MIN_M 1200
#define PHASE2_DEGEN_ESCAPE_DEGEN_TRIGGER 120
#define PHASE2_DEGEN_ESCAPE_POLICY_TRIGGER 200
#define PHASE2_DEGEN_ESCAPE_MAX_ATTEMPTS 2
#define PHASE2_DEGEN_ESCAPE_BLAND_HOLD_ITERS 16
/* Refactor scheduling constants/helpers moved to simplex_refactor_schedule.c (R3.7) */

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

int simplex_smcp_excl_skip_var(const SimplexTableau *tab, int j) {
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

#define PHASE1_STAGNATION_WINDOW_ITERS 96
/* PHASE1_STAGNATION_ESCAPE_COOLDOWN_ITERS in simplex_internal.h */
/* PHASE1_STAGNATION_ESCAPE_FAIL_COOLDOWN_ITERS in simplex_internal.h */
/* PHASE1_STAGNATION_MIN_M in simplex_internal.h */
#define PHASE1_STAGNATION_MAX_ESCAPES_PER_SOLVE 4
/* PHASE1_STAGNATION_ESCAPE_RUNTIME in simplex_internal.h */

int simplex_phase1_stagnation_escape_decision_for_test(
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
    int cooldown_remaining) {
    return lp_refactor_policy_phase1_stagnation_escape_decision(
        window_iters,
        obj_delta,
        obj_anchor,
        retry_defers,
        no_pivot_events,
        update_recovery_refactors,
        refactors,
        recompute_ratio_breakdown,
        recompute_dir_skip,
        recompute_dir_refactor,
        recompute_pivot_fail,
        recompute_perturb,
        cooldown_remaining);
}

void phase1_stagnation_window_begin(SimplexSolver *solver,
                                           const SimplexTableau *tab,
                                           int iter) {
    if (!solver || !tab) return;
    if (iter < 0) iter = 0;
    solver->policy.phase1_stagnation.window_start_iter = iter;
    solver->policy.phase1_stagnation.window_start_obj = tab->obj_value;
    solver->policy.phase1_stagnation.window_retry_base =
        solver->telemetry.perf_phase1_no_pivot_ladder_retry_defers;
    solver->policy.phase1_stagnation.window_no_pivot_base =
        solver->telemetry.perf_phase1_no_pivot_events;
    solver->policy.phase1_stagnation.window_refactor_base =
        solver->telemetry.perf_phase1_refactor_calls;
    solver->policy.phase1_stagnation.window_update_recovery_base =
        solver->telemetry.perf_refactor_reason_update_recovery;
    solver->policy.phase1_stagnation.window_recompute_ratio_base =
        solver->telemetry.perf_phase1_recompute_after_ratio_breakdown;
    solver->policy.phase1_stagnation.window_recompute_dir_skip_base =
        solver->telemetry.perf_phase1_recompute_after_dir_skip;
    solver->policy.phase1_stagnation.window_recompute_dir_refactor_base =
        solver->telemetry.perf_phase1_recompute_after_dir_refactor;
    solver->policy.phase1_stagnation.window_recompute_pivot_fail_base =
        solver->telemetry.perf_phase1_recompute_after_pivot_fail_recovery;
    solver->policy.phase1_stagnation.window_recompute_perturb_base =
        solver->telemetry.perf_phase1_recompute_after_perturb;
}

__attribute__((noinline, unused)) int phase1_stagnation_escape_should_trigger(
    SimplexSolver *solver,
    const SimplexTableau *tab,
    int iter) {
    int start_iter;
    int window_iters;
    double obj_anchor;
    double obj_delta;
    int retry_defers;
    int no_pivot_events;
    int update_recovery_refactors;
    int refactors;
    int recompute_ratio_breakdown;
    int recompute_dir_skip;
    int recompute_dir_refactor;
    int recompute_pivot_fail;
    int recompute_perturb;
    int should_trigger;

    if (!solver || !tab) return 0;
    if (!lp_glpk_strict_allow_phase1_stagnation_escape(
            solver->glpk_strict_mode)) {
        return 0;
    }
    if (iter < 0) iter = 0;
    if (tab->m < PHASE1_STAGNATION_MIN_M) return 0;
    if (solver->policy.phase1_stagnation.escape_triggers >=
        PHASE1_STAGNATION_MAX_ESCAPES_PER_SOLVE) {
        return 0;
    }

    start_iter = solver->policy.phase1_stagnation.window_start_iter;
    if (start_iter < 0) {
        phase1_stagnation_window_begin(solver, tab, iter);
        return 0;
    }

    window_iters = iter - start_iter;
    if (window_iters < PHASE1_STAGNATION_WINDOW_ITERS) return 0;

    obj_anchor = solver->policy.phase1_stagnation.window_start_obj;
    obj_delta = tab->obj_value - obj_anchor;
    retry_defers = solver->telemetry.perf_phase1_no_pivot_ladder_retry_defers -
                   solver->policy.phase1_stagnation.window_retry_base;
    no_pivot_events = solver->telemetry.perf_phase1_no_pivot_events -
                      solver->policy.phase1_stagnation.window_no_pivot_base;
    refactors = solver->telemetry.perf_phase1_refactor_calls -
                solver->policy.phase1_stagnation.window_refactor_base;
    update_recovery_refactors = solver->telemetry.perf_refactor_reason_update_recovery -
                                solver->policy.phase1_stagnation.window_update_recovery_base;
    recompute_ratio_breakdown =
        solver->telemetry.perf_phase1_recompute_after_ratio_breakdown -
        solver->policy.phase1_stagnation.window_recompute_ratio_base;
    recompute_dir_skip = solver->telemetry.perf_phase1_recompute_after_dir_skip -
                         solver->policy.phase1_stagnation.window_recompute_dir_skip_base;
    recompute_dir_refactor =
        solver->telemetry.perf_phase1_recompute_after_dir_refactor -
        solver->policy.phase1_stagnation.window_recompute_dir_refactor_base;
    recompute_pivot_fail =
        solver->telemetry.perf_phase1_recompute_after_pivot_fail_recovery -
        solver->policy.phase1_stagnation.window_recompute_pivot_fail_base;
    recompute_perturb = solver->telemetry.perf_phase1_recompute_after_perturb -
                        solver->policy.phase1_stagnation.window_recompute_perturb_base;

    if (retry_defers < 0) retry_defers = 0;
    if (no_pivot_events < 0) no_pivot_events = 0;
    if (refactors < 0) refactors = 0;
    if (update_recovery_refactors < 0) update_recovery_refactors = 0;
    if (recompute_ratio_breakdown < 0) recompute_ratio_breakdown = 0;
    if (recompute_dir_skip < 0) recompute_dir_skip = 0;
    if (recompute_dir_refactor < 0) recompute_dir_refactor = 0;
    if (recompute_pivot_fail < 0) recompute_pivot_fail = 0;
    if (recompute_perturb < 0) recompute_perturb = 0;

    solver->policy.phase1_stagnation.last_window_iters = window_iters;
    solver->policy.phase1_stagnation.last_obj_delta = obj_delta;
    solver->policy.phase1_stagnation.last_retry_defers = retry_defers;
    solver->policy.phase1_stagnation.last_no_pivot_events = no_pivot_events;
    solver->policy.phase1_stagnation.last_update_recovery_refactors =
        update_recovery_refactors;
    solver->policy.phase1_stagnation.last_refactors = refactors;
    solver->policy.phase1_stagnation.last_recompute_ratio = recompute_ratio_breakdown;
    solver->policy.phase1_stagnation.last_recompute_dir_skip = recompute_dir_skip;
    solver->policy.phase1_stagnation.last_recompute_dir_refactor = recompute_dir_refactor;
    solver->policy.phase1_stagnation.last_recompute_pivot_fail = recompute_pivot_fail;
    solver->policy.phase1_stagnation.last_recompute_perturb = recompute_perturb;
    solver->policy.phase1_stagnation.last_retry_defer_ratio =
        (no_pivot_events > 0)
            ? ((double)retry_defers / (double)no_pivot_events)
            : 0.0;
    solver->policy.phase1_stagnation.last_update_recovery_ratio =
        (refactors > 0)
            ? ((double)update_recovery_refactors / (double)refactors)
            : 0.0;

    should_trigger = lp_refactor_policy_phase1_stagnation_escape_decision(
        window_iters,
        obj_delta,
        obj_anchor,
        retry_defers,
        no_pivot_events,
        update_recovery_refactors,
        refactors,
        recompute_ratio_breakdown,
        recompute_dir_skip,
        recompute_dir_refactor,
        recompute_pivot_fail,
        recompute_perturb,
        0);
    if (should_trigger && solver->policy.phase1_stagnation.escape_cooldown > 0) {
        solver->policy.phase1_stagnation.escape_cooldown_blocks++;
        should_trigger = 0;
    }
    if (should_trigger) {
        solver->policy.phase1_stagnation.escape_triggers++;
    }
    phase1_stagnation_window_begin(solver, tab, iter);
    return should_trigger;
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

int simplex_choose_basis_action_for_test(double pivot,
                                         int force_refactor,
                                         int lu_update_status,
                                         int lu_reason,
                                         int repeat_pattern,
                                         int lu_num_updates,
                                         double growth_factor) {
    (void)lu_reason;
    return (int)lp_refactor_policy_choose_basis_action(
        pivot,
        force_refactor,
        lu_update_status,
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
    LPPeriodicRefactorPlan plan = lp_refactor_policy_periodic_plan(
        phase,
        iter,
        m,
        max_updates,
        num_updates,
        spike_pool_used,
        spike_pool_capacity,
        cond_estimate,
        growth_factor,
        use_bland,
        degenerate_count,
        feedback_bias,
        0,
        periodic_policy_cooldown,
        periodic_policy_pressure_decay);

    if (interval_out) *interval_out = plan.policy.interval;
    if (pressure_out) *pressure_out = plan.effective_run_pressure;
    return plan.should_run;
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

static int phase1_dual_rescue_guard_step(SimplexSolver *solver,
                                         int rescue_cooldown_iters,
                                         int rescue_fail_streak,
                                         int ladder_context) {
    if (solver &&
        !lp_glpk_strict_allow_phase1_dual_rescue(solver->glpk_strict_mode)) {
        return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
    }
    if (rescue_fail_streak >= PHASE1_NO_PIVOT_LADDER_RESCUE_FAIL_CAP) {
        if (ladder_context) {
            lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(solver, 1);
        } else {
            lp_telemetry_record_phase1_direct_dual_rescue_guard(solver, 1);
        }
        return PHASE1_NO_PIVOT_LADDER_STEP_FORCE_REFACTOR;
    }
    if (rescue_cooldown_iters > 0) {
        if (ladder_context) {
            lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(solver, 0);
        } else {
            lp_telemetry_record_phase1_direct_dual_rescue_guard(solver, 0);
        }
        return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
    }
    return PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE;
}

double phase1_artificial_abs_sum(const SimplexTableau *tab) {
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

int phase1_no_pivot_ladder_step(
    SimplexSolver *solver,
    int m,
    int degenerate_count,
    LPPhase1NoPivotForceReason reason,
    int no_pivot_streak,
    int no_progress_streak,
    int force_pivot_mode_active,
    int *refactor_threshold_out) {
    if (solver &&
        !lp_glpk_strict_allow_phase1_no_pivot_ladder(
            solver->glpk_strict_mode)) {
        if (refactor_threshold_out) *refactor_threshold_out = 0;
        return PHASE1_NO_PIVOT_LADDER_STEP_RETRY;
    }
    return lp_refactor_policy_phase1_no_pivot_ladder_step(
        m,
        degenerate_count,
        (int)reason,
        no_pivot_streak,
        no_progress_streak,
        force_pivot_mode_active,
        refactor_threshold_out);
}

int phase1_no_pivot_ladder_apply_rescue_guard(
    SimplexSolver *solver,
    int ladder_step,
    int rescue_cooldown_iters,
    int rescue_fail_streak) {
    if (ladder_step != PHASE1_NO_PIVOT_LADDER_STEP_DUAL_RESCUE) {
        return ladder_step;
    }
    return phase1_dual_rescue_guard_step(
        solver, rescue_cooldown_iters, rescue_fail_streak, 1);
}

int phase1_direct_dual_rescue_guard_plan(SimplexSolver *solver,
                                         int rescue_cooldown_iters,
                                         int rescue_fail_streak) {
    return phase1_dual_rescue_guard_step(
        solver, rescue_cooldown_iters, rescue_fail_streak, 0);
}

int phase1_dir_stabilize_escape_gate_plan(int m,
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
    return lp_refactor_policy_phase1_dir_stabilize_escape_gate_plan(
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

int phase1_force_pivot_refactor_relax_plan(
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
    return lp_refactor_policy_phase1_force_pivot_refactor_relax_plan(
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

int phase1_force_extreme_refactor_relax_plan(
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
    return lp_refactor_policy_phase1_force_extreme_refactor_relax_plan(
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

int phase1_force_extreme_tiny_theta_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak) {
    return lp_refactor_policy_phase1_force_extreme_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak);
}

int phase1_force_extreme_bound_flip_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int bound_flip_followup_streak) {
    return lp_refactor_policy_phase1_force_extreme_bound_flip_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        bound_flip_followup_streak);
}

int phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak,
    double pivot_ratio) {
    return lp_refactor_policy_phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak,
        pivot_ratio);
}

int phase1_soft_lu_policy_cooldown_updates(int m,
                                                  int degenerate_count,
                                                  int periodic_interval) {
    return lp_refactor_policy_phase1_soft_lu_policy_cooldown_updates(
        m,
        degenerate_count,
        periodic_interval);
}

int simplex_phase1_no_pivot_force_plan_for_test(int m,
                                                 int degenerate_count,
                                                 int streak,
                                                 int cooldown,
                                                 int reason,
                                                 int *next_streak_out,
                                                 int *next_cooldown_out) {
    P1ProgressState tmp_ps;
    int should_force;
    memset(&tmp_ps, 0, sizeof(tmp_ps));
    tmp_ps.no_pivot_streak = streak;
    tmp_ps.no_pivot_force_cooldown = cooldown;
    should_force = p1_progress_note_no_pivot(NULL,
                                             m,
                                             degenerate_count,
                                             (LPPhase1NoPivotForceReason)reason,
                                             &tmp_ps);
    if (next_streak_out) *next_streak_out = tmp_ps.no_pivot_streak;
    if (next_cooldown_out) *next_cooldown_out = tmp_ps.no_pivot_force_cooldown;
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
        NULL,
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

int simplex_phase1_direct_dual_rescue_guard_plan_for_test(
    int rescue_cooldown_iters,
    int rescue_fail_streak) {
    return phase1_direct_dual_rescue_guard_plan(
        NULL, rescue_cooldown_iters, rescue_fail_streak);
}

int simplex_phase1_dir_skip_rescue_cadence_plan_for_test(
    int dir_skip_event_streak) {
    return lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(
        dir_skip_event_streak);
}

int simplex_phase1_force_pivot_mode_plan_for_test(int m,
                                                   int degenerate_count,
                                                   int queue_force_pending,
                                                   int dir_skip_event_streak,
                                                   int active_budget,
                                                   int *next_streak_out,
                                                   int *next_budget_out,
                                                   int *next_pending_out) {
    int streak = dir_skip_event_streak;
    int budget = active_budget;
    int pending = 0;
    LPPhase1NoPivotForceReason force_reason =
        LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
    P1ProgressState tmp_ps;
    memset(&tmp_ps, 0, sizeof(tmp_ps));
    tmp_ps.force_pivot_attempt_budget = budget;
    int activated = p1_progress_activate_force_pivot(
        NULL,
        m,
        degenerate_count,
        queue_force_pending,
        &streak,
        &tmp_ps,
        &pending,
        &force_reason);
    if (next_streak_out) *next_streak_out = streak;
    if (next_budget_out) *next_budget_out = tmp_ps.force_pivot_attempt_budget;
    if (next_pending_out) *next_pending_out = pending;
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

int simplex_phase1_force_extreme_tiny_theta_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak) {
    return phase1_force_extreme_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak);
}

int simplex_phase1_force_extreme_bound_flip_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int bound_flip_followup_streak) {
    return phase1_force_extreme_bound_flip_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        bound_flip_followup_streak);
}

int simplex_phase1_force_extreme_catastrophic_tiny_theta_relax_plan_for_test(
    int m,
    int degenerate_count,
    int no_progress_streak,
    double dir_inf_ratio,
    int force_extreme_dir,
    int force_lu_health,
    int lu_hard_trigger,
    int tiny_theta_followup_streak,
    double pivot_ratio) {
    return phase1_force_extreme_catastrophic_tiny_theta_relax_plan(
        m,
        degenerate_count,
        no_progress_streak,
        dir_inf_ratio,
        force_extreme_dir,
        force_lu_health,
        lu_hard_trigger,
        tiny_theta_followup_streak,
        pivot_ratio);
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

void phase1_trace_record_no_entering(SimplexSolver *solver, int iter, int status_code) {
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

void phase1_trace_record_pivot_failure(SimplexSolver *solver,
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

void phase1_trace_emit_summary(SimplexSolver *solver, RalphStatus phase1_status) {
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

double vec_abs_max(const double *x, int n) {
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
        cond = lu_get_cond_estimate(solver->tableau->lu);
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
int repair_singular_basis(SimplexTableau *tab) {
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
    int updates_before = (tab && tab->lu) ? lu_get_num_updates(tab->lu) : 0;
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

    /* Pass redundant row hints and regularization config to LU.
     * Phase 1: many artificial variables create near-singular bases.
     * Phase 2 with redundant rows: always allow (even after zeroing A_ext).
     * After zeroing, the sparse LU may still encounter zero pivots at zeroed
     * rows if its column ordering doesn't process artificials first. */
    {
        int allow = 0;
        int reg_limit = 0;
        if (tab->use_two_phase && (tab->phase == 1 ||
            (tab->phase == 2 && tab->num_redundant > 0))) {
            allow = 1;
            reg_limit = RALPH_PHASE1_MAX_REGULARIZATIONS;
            if (tab->num_redundant > reg_limit) {
                reg_limit = tab->num_redundant;
            }
            if (reg_limit > tab->m) {
                reg_limit = tab->m;
            }
        }
        lu_configure_regularization(tab->lu, allow, reg_limit,
                                    tab->redundant_rows, tab->num_redundant);
    }

    /* When Phase 2 has redundant rows (not yet zeroed), relax pivot tolerance
     * to accept small but valid structural pivots. After zeroing, use normal
     * tolerance since the basis is well-conditioned. */
    double saved_tol = lu_get_pivot_tol(tab->lu);
    if (tab->phase == 2 && tab->num_redundant > 0 && !tab->redundant_rows_zeroed) {
        lu_set_pivot_tol(tab->lu, 1e-15);
    }

    int status = lu_factorize(tab->lu, B);

    lu_set_pivot_tol(tab->lu, saved_tol);

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

/* tableau_refactorize_with_reason is now a static inline in simplex_internal.h */

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
            lu_factorize_calls = (int)lu_get_factorize_calls(tab->lu);
            lu_num_updates = lu_get_num_updates(tab->lu);
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
        if (tab->phase == 1) {
            lp_telemetry_record_phase1_compute_solution_context(
                tab->owner,
                (LPPhase1ComputeContext)tab->phase1_compute_solution_context);
            tab->phase1_compute_solution_context = LP_PHASE1_COMPUTE_CTX_OTHER;
        }
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
        if (tab->phase == 1) {
            lp_telemetry_record_phase1_compute_rc_context(
                tab->owner,
                (LPPhase1ComputeContext)tab->phase1_compute_rc_context);
            tab->phase1_compute_rc_context = LP_PHASE1_COMPUTE_CTX_OTHER;
        }
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
double tableau_get_rc(SimplexTableau *tab, int j) {
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


/* PHASE1_FORCE_EXTREME_TINY_THETA_RELAX_REFACTOR_* moved to simplex_internal.h */
/* Failed-stabilize retry + recompute helpers moved to simplex_phase1_stabilize.c (R3.6) */
/* ============================================================================
 * Simplex Iteration
 * ============================================================================ */

int simplex_pivot(SimplexTableau *tab,
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
    double growth_factor = (tab->lu) ? lu_get_growth_factor(tab->lu) : 0.0;
    int lu_num_updates = (tab->lu) ? lu_get_num_updates(tab->lu) : 0;
    double growth_threshold = (tab->lu && lu_get_growth_refactor_threshold(tab->lu) > 0.0)
        ? lu_get_growth_refactor_threshold(tab->lu)
        : RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
    LPBasisAction action = lp_refactor_policy_choose_basis_action(
        pivot,
        force_refactor,
        lu_update_status,
        repeat_pattern_count,
        lu_num_updates,
        growth_factor,
        growth_threshold);

    for (;;) {
        switch (action) {
            case LP_BASIS_ACTION_UPDATE:
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
                update_reason = (tab->lu) ? lu_get_last_failure_reason(tab->lu) : LU_FAIL_NONE;
                lu_reason = update_reason;
                growth_factor = (tab->lu) ? lu_get_growth_factor(tab->lu) : growth_factor;
                lu_num_updates = (tab->lu) ? lu_get_num_updates(tab->lu) : lu_num_updates;
                action = lp_refactor_policy_choose_basis_action(
                    pivot,
                    force_refactor,
                    lu_update_status,
                    repeat_pattern_count,
                    lu_num_updates,
                    growth_factor,
                    growth_threshold);
                continue;

            case LP_BASIS_ACTION_REFACTOR:
                refactor_forced_path = (lu_update_status == 0);
                {
                    int ref_reason = lp_refactor_policy_phase1_small_pivot_refactor_allowed(
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
                lu_reason = (tab->lu) ? lu_get_last_failure_reason(tab->lu) : lu_reason;
                growth_factor = (tab->lu) ? lu_get_growth_factor(tab->lu) : growth_factor;
                lu_num_updates = (tab->lu) ? lu_get_num_updates(tab->lu) : lu_num_updates;
                action = lp_refactor_policy_choose_basis_action(
                    pivot,
                    force_refactor,
                    lu_update_status,
                    repeat_pattern_count,
                    lu_num_updates,
                    growth_factor,
                    growth_threshold);
                continue;

            case LP_BASIS_ACTION_REPAIR:
                if (repair_singular_basis(tab) == 0) {
                    goto basis_update_done;
                }
                lu_update_status = -3;
                lu_reason = (tab->lu) ? lu_get_last_failure_reason(tab->lu) : lu_reason;
                growth_factor = (tab->lu) ? lu_get_growth_factor(tab->lu) : growth_factor;
                lu_num_updates = (tab->lu) ? lu_get_num_updates(tab->lu) : lu_num_updates;
                action = lp_refactor_policy_choose_basis_action(
                    pivot,
                    force_refactor,
                    lu_update_status,
                    repeat_pattern_count,
                    lu_num_updates,
                    growth_factor,
                    growth_threshold);
                continue;

            case LP_BASIS_ACTION_ABORT:
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
            if (tab->phase == 2 && tab->owner) {
                lp_telemetry_record_phase2_devex_reset(tab->owner,
                                                       tab->devex_refcount);
            }
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
    solver->lu_factorization_type = LP_GLPK_BFCP_FACTORIZATION_LUF;
    solver->lu_backend_policy = LP_LU_BACKEND_POLICY_AUTO;
    solver->lu_update_limit_override = -1;
    solver->lu_pivot_tol_override = 0.0;
    solver->lu_growth_guard_override = 0.0;
    solver->lu_strict_lane_active = 0;
    solver->lu_strict_prefer_dense_ge_numeric = 0;
    solver->lu_strict_allow_supernode_lane = 1;
    solver->lu_strict_allow_symbolic_full_retry = 1;
    solver->lu_strict_allow_top_level_dense_fallback = 1;
    solver->has_lp_progress_callback = 0;
    solver->has_lp_cancel_callback = 0;
    solver->progress_start_ms = 0.0;
    solver->warm_basis_last_attempted = 0;
    solver->warm_basis_last_applied = 0;
    solver->warm_basis_last_rejected = 0;
    solver->unbounded_valid = 0;
    lp_refactor_policy_config_defaults(&solver->refactor_config);
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
void extract_farkas_ray(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;
    int m = tab->m;

    /* Grow the reusable certificate buffer when the tableau dimension changes. */
    if (!solver->farkas_ray || solver->farkas_ray_capacity < m) {
        free(solver->farkas_ray);
        solver->farkas_ray = (double*)calloc((size_t)m, sizeof(double));
        solver->farkas_ray_capacity = solver->farkas_ray ? m : 0;
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

    if (!solver->unbounded_ray || solver->unbounded_ray_capacity < n_orig) {
        free(solver->unbounded_ray);
        solver->unbounded_ray = (double*)calloc((size_t)n_orig, sizeof(double));
        solver->unbounded_ray_capacity = solver->unbounded_ray ? n_orig : 0;
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
 * Two-Phase Simplex Implementation
 * ============================================================================ */

/*
 * Phase 1: Minimize sum of artificial variables.
 * Returns 0 if feasible (all artificials driven to zero), -1 if infeasible.
 */
static int simplex_phase1(SimplexSolver *solver) {
    solver->current_phase = SIMPLEX_PHASE_1;
    SimplexTableau *tab = solver->tableau;

    if (!tab->use_two_phase) {
        /* No artificials: check and restore feasibility via dual pivoting */
        tab->phase1_compute_solution_context = LP_PHASE1_COMPUTE_CTX_INIT;
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
                solver->current_phase = SIMPLEX_PHASE_INFEASIBLE;
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
    tab->phase1_compute_solution_context = LP_PHASE1_COMPUTE_CTX_INIT;
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

    P1RecoveryState rs;
    p1_recovery_init(&rs, solver, tab);

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
                                          &rs.numerical.rc_only_streak,
                                          LP_PHASE1_RECOMPUTE_REASON_PERTURB);
    }

    /* Compute initial reduced costs */
    tab->phase1_compute_rc_context = LP_PHASE1_COMPUTE_CTX_INIT;
    tableau_compute_reduced_costs(tab);
    if (rs.cycling.pricing_strategy == 4) heap_build(tab);
    double phase1_hot_ms_prev = phase_hotpath_ms(solver, 1);
#if PHASE1_STAGNATION_ESCAPE_RUNTIME
    if (tab->m >= PHASE1_STAGNATION_MIN_M) {
        phase1_stagnation_window_begin(solver, tab, 0);
    }
#endif

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
        /* Zone 1: tick cooldowns */
        p1_zone_tick_cooldowns(solver, tab, &rs);

        /* Zone 2: pre-iteration (auto-Dantzig, no-pivot force, stagnation escape) */
        {
            P1IterContext ctx = {0};
            P1ZoneResult pre_result = p1_zone_pre_iter(solver, tab, &rs, iter, &ctx);
            if (pre_result == P1_ZONE_CONTINUE) continue;
            if (pre_result == P1_ZONE_RETURN_FAIL) {
                primal_remove_perturbation(tab);
                solver->status = RALPH_STATUS_ITERATION_LIMIT;
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
                return -1;
            }
        }

        /* Zone 3: pricing + ratio test */
        P1IterContext zctx = {0};
        zctx.phase1_hot_ms_prev = phase1_hot_ms_prev;
        {
            P1ZoneResult price_result = p1_zone_pricing(solver, tab, &rs, iter, &zctx);
            if (price_result == P1_ZONE_CONTINUE) continue;
            if (price_result == P1_ZONE_RETURN_OK) return 0;
            if (price_result == P1_ZONE_RETURN_FAIL) return -1;
        }
        int entering = zctx.entering;
        int leaving = zctx.leaving;
        double theta = zctx.theta;
        int ratio_status = zctx.ratio_status;

        /* Zone 4: ratio breakdown */
        if (ratio_status != 0) {
            P1ZoneResult rb_result = p1_zone_ratio_breakdown(solver, tab, &rs, iter, &zctx);
            if (rb_result == P1_ZONE_CONTINUE) continue;
            if (rb_result == P1_ZONE_RETURN_FAIL) return -1;
        }

        /* Zone 5: direction guard (stabilize, defer, retry) */
        {
            zctx.entering = entering;
            zctx.leaving = leaving;
            zctx.theta = theta;
            P1ZoneResult dg_result = p1_zone_direction_guard(solver, tab, &rs, iter, &zctx);
            entering = zctx.entering;
            leaving = zctx.leaving;
            theta = zctx.theta;
            if (dg_result == P1_ZONE_CONTINUE) continue;
            if (dg_result == P1_ZONE_RETURN_FAIL) return -1;
        }

        /* Zone 6: pivot (alt-leaving, degeneracy, pivot call, failure handling) */
        zctx.entering = entering;
        zctx.leaving = leaving;
        zctx.theta = theta;
        {
            P1ZoneResult piv_result = p1_zone_pivot(solver, tab, &rs, iter, &zctx);
            if (piv_result == P1_ZONE_CONTINUE) continue;
            if (piv_result == P1_ZONE_RETURN_FAIL) return -1;
        }
        /* Zone 7: post-pivot (reset + stall detect + periodic refactor) */
        p1_zone_post_pivot_reset(solver, tab, &rs);
        p1_zone_stall_detect(solver, tab, &rs);
        {
            P1ZoneResult pp_result = p1_zone_periodic_refactor(solver, tab, &rs, iter, &zctx);
            phase1_hot_ms_prev = zctx.phase1_hot_ms_prev;
            if (pp_result == P1_ZONE_CONTINUE) continue;
            if (pp_result == P1_ZONE_RETURN_FAIL) return -1;
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
    solver->current_phase = SIMPLEX_PHASE_TRANSITION;
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
        lu_invalidate_symbolic_cache(tab->lu);
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
    solver->current_phase = SIMPLEX_PHASE_2;
    SimplexTableau *tab = solver->tableau;

    tab->phase = 2;

    /* For two-phase problems, force early refactorization to reset numerical
     * state after the transition. Redundant rows were zeroed in A_ext during
     * the transition, so the basis matrix is now well-conditioned. */
    if (tab->use_two_phase) {
        lu_force_refactorization(tab->lu);
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
    int last_entering = -1;
    int last_leaving = -1;
    int repeat_entering_streak = 0;
    int repeat_leaving_streak = 0;

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
    if (perturbation_active && solver->telemetry_enabled) {
        solver->telemetry.perf_phase2_perturb_applied++;
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

        periodic_policy_cooldown =
            lp_refactor_policy_periodic_cooldown_tick(periodic_policy_cooldown);
        periodic_policy_pressure_decay =
            lp_refactor_policy_periodic_pressure_decay_recover(
                2, periodic_policy_pressure_decay);

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
        if (solver->telemetry_enabled) {
            if (use_bland || iter < bland_start_iters) {
                solver->telemetry.perf_phase2_bland_pricing_iters++;
            } else if (solver->pricing_strategy == 2 &&
                       adaptive_devex_partial &&
                       (iter & DEVEX_PARTIAL_FULL_RESCAN_MASK) != 0) {
                solver->telemetry.perf_phase2_adaptive_devex_partial_iters++;
            }
        }

        price_status = pricing_dispatch(tab, solver->pricing_strategy,
                                        (use_bland || iter < bland_start_iters),
                                        adaptive_devex_partial, iter, &entering);
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
                solver->current_phase = SIMPLEX_PHASE_OPTIMAL;
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
            int degen_episode =
                (degenerate_count > 0 || use_bland || perturbation_active);
            {
                double t_refactor_ms = lp_telemetry_timer_start();
                refactor_rc = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
                lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            }
            if (degen_episode) {
                lp_telemetry_record_phase2_degenerate_refactor(
                    solver,
                    RALPH_REFACTOR_REASON_RATIO_RECOVERY,
                    0,
                    lp_telemetry_refactor_reason_is_safety_forced(
                        RALPH_REFACTOR_REASON_RATIO_RECOVERY));
            }
            if (refactor_rc == 0) {
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                if (solver->pricing_strategy == 4) heap_build(tab);

                /* Re-price: the entering variable may no longer be eligible */
                t_pricing_ms = lp_telemetry_timer_start();
                price_status = pricing_dispatch(tab, solver->pricing_strategy,
                                                (use_bland || iter < bland_start_iters),
                                                adaptive_devex_partial, iter, &entering);
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
                        solver->current_phase = SIMPLEX_PHASE_OPTIMAL;
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
                solver->current_phase = SIMPLEX_PHASE_UNBOUNDED;
                solver->status = RALPH_STATUS_UNBOUNDED;
                solver->iterations = iter;
                return -1;
            }
            /* Recovery succeeded — fall through to pivot */
        }

        {
            double phase2_dir_inf = 0.0;
            int phase2_dir_nnz = 0;
            double phase2_pivot_abs = 0.0;

            phase1_direction_shape_from_vector(
                tab->work2,
                tab->m,
                leaving,
                &phase2_dir_inf,
                &phase2_dir_nnz,
                &phase2_pivot_abs);
            lp_telemetry_record_phase2_pivot_geometry(
                solver,
                theta,
                phase2_dir_inf,
                phase2_pivot_abs);
        }
        if (solver->telemetry_enabled) {
            if (entering == last_entering) {
                repeat_entering_streak++;
                solver->telemetry.perf_phase2_repeat_entering_events++;
                if (repeat_entering_streak >
                    solver->telemetry.perf_phase2_repeat_entering_max_streak) {
                    solver->telemetry.perf_phase2_repeat_entering_max_streak =
                        repeat_entering_streak;
                }
            } else {
                repeat_entering_streak = 0;
            }
            last_entering = entering;

            if (leaving >= 0 && leaving == last_leaving) {
                repeat_leaving_streak++;
                solver->telemetry.perf_phase2_repeat_leaving_events++;
                if (repeat_leaving_streak >
                    solver->telemetry.perf_phase2_repeat_leaving_max_streak) {
                    solver->telemetry.perf_phase2_repeat_leaving_max_streak =
                        repeat_leaving_streak;
                }
            } else {
                repeat_leaving_streak = 0;
            }
            last_leaving = (leaving >= 0) ? leaving : -1;
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
            if (solver->telemetry_enabled && degenerate_count == 0) {
                solver->telemetry.perf_phase2_degenerate_episodes++;
            }
            degenerate_count++;
            if (solver->telemetry_enabled &&
                degenerate_count > solver->telemetry.perf_phase2_degenerate_streak_max) {
                solver->telemetry.perf_phase2_degenerate_streak_max = degenerate_count;
            }
            non_degen_streak = 0;

            /* First try perturbation */
            if (degenerate_count >= PERTURB_THRESHOLD && !perturbation_active && !use_bland) {
                primal_apply_perturbation(tab);
                perturbation_active = 1;
                if (solver->telemetry_enabled) {
                    solver->telemetry.perf_phase2_perturb_applied++;
                }
                if (solver->verbose) {
                    LP_LOG_STDOUT("Iter %d: Applying perturbation due to degeneracy\n", iter);
                }
            }

            /* If still cycling after perturbation, use Bland's rule */
            if (degenerate_count >= DEGEN_THRESHOLD && !use_bland) {
                use_bland = 1;
                if (solver->telemetry_enabled) {
                    solver->telemetry.perf_phase2_bland_enter_episodes++;
                }
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
                if (solver->telemetry_enabled) {
                    solver->telemetry.perf_phase2_bland_exit_episodes++;
                }
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
            int degen_episode =
                (degenerate_count > 0 || use_bland || perturbation_active);
            {
                double t_refactor_ms = lp_telemetry_timer_start();
                rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY);
                lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            }
            if (degen_episode) {
                lp_telemetry_record_phase2_degenerate_refactor(
                    solver,
                    RALPH_REFACTOR_REASON_PIVOT_RECOVERY,
                    0,
                    lp_telemetry_refactor_reason_is_safety_forced(
                        RALPH_REFACTOR_REASON_PIVOT_RECOVERY));
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
                                                           lu_get_use_ft_updates(tab->lu),
                                                           lu_get_num_updates(tab->lu),
                                                           lu_get_max_updates(tab->lu),
                                                           lu_get_spike_pool_used(tab->lu),
                                                           lu_get_spike_pool_capacity(tab->lu),
                                                           lu_get_cond_estimate(tab->lu),
                                                           lu_get_growth_factor(tab->lu),
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
        double periodic_feedback_bias = periodic_feedback_bias_for_phase(solver, 2);
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
                lu_get_num_updates(tab->lu),
                lu_get_max_updates(tab->lu),
                lu_get_spike_pool_used(tab->lu),
                lu_get_spike_pool_capacity(tab->lu),
                lu_get_cond_estimate(tab->lu),
                lu_get_growth_factor(tab->lu),
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
                                lu_get_num_updates(tab->lu),
                                lu_get_max_updates(tab->lu),
                                degenerate_count);
                    }
                }
            }
        }
        if (!lu_refactor_needed) {
            LPPeriodicRefactorPlan periodic_plan = lp_refactor_policy_periodic_plan(
                2,
                iter,
                tab->m,
                lu_get_max_updates(tab->lu),
                lu_get_num_updates(tab->lu),
                lu_get_spike_pool_used(tab->lu),
                lu_get_spike_pool_capacity(tab->lu),
                lu_get_cond_estimate(tab->lu),
                lu_get_growth_factor(tab->lu),
                use_bland,
                degenerate_count,
                periodic_feedback_bias,
                0,
                periodic_policy_cooldown,
                periodic_policy_pressure_decay);
            periodic_policy = periodic_plan.policy;
            cooldown_eligible = periodic_plan.cooldown_eligible;
            effective_policy_pressure = periodic_plan.effective_run_pressure;
            periodic_refactor = periodic_plan.should_run;
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
                            lu_get_num_updates(tab->lu),
                            lu_get_max_updates(tab->lu),
                            lu_get_spike_pool_used(tab->lu),
                            lu_get_spike_pool_capacity(tab->lu),
                            lu_get_cond_estimate(tab->lu),
                            lu_get_growth_factor(tab->lu),
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
                                        lu_get_num_updates(tab->lu),
                                        lu_get_max_updates(tab->lu),
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
                                            lu_get_num_updates(tab->lu),
                                            lu_get_max_updates(tab->lu),
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
            int degen_episode =
                (degenerate_count > 0 || use_bland || perturbation_active);
            {
                double t_refactor_ms = lp_telemetry_timer_start();
                rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
                refactor_elapsed_ms = lp_telemetry_timer_elapsed_ms(t_refactor_ms);
                lp_telemetry_add_refactor_runtime_ms(solver, refactor_elapsed_ms);
            }
            if (degen_episode) {
                lp_telemetry_record_phase2_degenerate_refactor(
                    solver,
                    RALPH_REFACTOR_REASON_PERIODIC,
                    lu_refactor_needed,
                    lp_telemetry_refactor_reason_is_safety_forced(
                        RALPH_REFACTOR_REASON_PERIODIC));
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
                lp_refactor_policy_periodic_post_refactor_update(
                    2,
                    cooldown_eligible,
                    periodic_policy.interval,
                    rc_refactor,
                    &periodic_policy_cooldown,
                    &periodic_policy_pressure_decay);
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
                    lu_get_num_updates(tab->lu),
                    lu_get_max_updates(tab->lu),
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
                    lu_get_spike_pool_used(tab->lu),
                    lu_get_spike_pool_capacity(tab->lu),
                    lu_get_cond_estimate(tab->lu),
                    lu_get_growth_factor(tab->lu));
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
                periodic_policy_refactor_count(solver, 2) >= PHASE2_DEGEN_ESCAPE_POLICY_TRIGGER) {
                double scale = 4.0 + 2.0 * (double)perturb_attempts_p2;
                primal_apply_perturbation_scaled(tab, scale);
                perturbation_active = 1;
                if (solver->telemetry_enabled) {
                    solver->telemetry.perf_phase2_degen_escape_triggers++;
                    solver->telemetry.perf_phase2_perturb_applied++;
                    if (!use_bland) {
                        solver->telemetry.perf_phase2_bland_enter_episodes++;
                    }
                }
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
                    if (solver->telemetry_enabled) {
                        solver->telemetry.perf_phase2_perturb_applied++;
                        if (use_bland) {
                            solver->telemetry.perf_phase2_bland_exit_episodes++;
                        }
                    }
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
    periodic_policy_refactor_reset(solver);
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
    int update_cap = 0;

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
        lu_set_telemetry_enabled(tab->lu, solver->telemetry_enabled);
        lu_set_owner(tab->lu, solver);
        if (solver->policy.basis_governor_mode == LP_BASIS_GOV_MODE_OFF) {
            lu_set_basis_governor(tab->lu, NULL);
        } else {
            lu_set_basis_governor(tab->lu, &solver->policy.basis_governor);
        }

        lu_set_mkz_enabled(tab->lu, 1);
        if (solver->lu_supernode) {
            enable_supernode = 1;
        } else if (tab->m > 300) {
            enable_supernode = 1;
        }

        lu_apply_backend_policy(tab->lu, solver->lu_backend_policy);
        lu_set_sn_enabled(tab->lu, enable_supernode ? 1 : 0);

        if (solver->lu_update_limit_override > 0) {
            lu_set_max_updates(tab->lu, solver->lu_update_limit_override);
            update_cap = lu_update_backend_storage_capacity(tab->lu);
            if (update_cap > 0 && lu_get_max_updates(tab->lu) > update_cap) {
                lu_set_max_updates(tab->lu, update_cap);
            }
        }
        if (solver->lu_pivot_tol_override > 0.0) {
            lu_set_pivot_tol(tab->lu, solver->lu_pivot_tol_override);
        }
        if (solver->lu_growth_guard_override > 0.0) {
            lu_set_growth_refactor_threshold(tab->lu, solver->lu_growth_guard_override);
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

static int tableau_build_csr_from_current_matrix(SimplexTableau *tab) {
    if (!tab || !tab->A_ext) return -1;

    SAFE_FREE(tab->csr_rowptr);
    SAFE_FREE(tab->csr_colidx);
    SAFE_FREE(tab->csr_values);
    SAFE_FREE(tab->csr_alpha);

    int csr_m = tab->m;
    int csr_n = tab->n;
    int csr_nnz = tab->A_ext->colptr[csr_n];
    tab->csr_rowptr = (int*)calloc((size_t)csr_m + 1, sizeof(int));
    tab->csr_colidx = (int*)malloc((size_t)csr_nnz * sizeof(int));
    tab->csr_values = (double*)malloc((size_t)csr_nnz * sizeof(double));
    tab->csr_alpha = (double*)calloc((size_t)csr_n, sizeof(double));
    if (!tab->csr_rowptr || !tab->csr_colidx || !tab->csr_values || !tab->csr_alpha) {
        return -1;
    }

    for (int j = 0; j < csr_n; j++) {
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            tab->csr_rowptr[tab->A_ext->rowidx[p] + 1]++;
        }
    }
    for (int i = 0; i < csr_m; i++) {
        tab->csr_rowptr[i + 1] += tab->csr_rowptr[i];
    }

    int *pos = (int*)tab->csr_alpha;
    memset(pos, 0, (size_t)csr_m * sizeof(int));
    for (int j = 0; j < csr_n; j++) {
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            int row = tab->A_ext->rowidx[p];
            int dest = tab->csr_rowptr[row] + pos[row];
            tab->csr_colidx[dest] = j;
            tab->csr_values[dest] = tab->A_ext->values[p];
            pos[row]++;
        }
    }
    memset(tab->csr_alpha, 0, (size_t)csr_n * sizeof(double));

    double density = 0.0;
    if (csr_m > 0 && csr_n > 0) {
        density = (double)csr_nnz / ((double)csr_m * (double)csr_n);
    }
    tab->csr_use_scatter = (density < 0.02);
    return 0;
}

static int lp_augment_row_normalize(const LPAugmentRow *row,
                                    char *sense_out,
                                    double *sign_out,
                                    double *rhs_out) {
    if (!row || !sense_out || !sign_out || !rhs_out) return -1;

    char sense = row->sense;
    if (sense != 'L' && sense != 'G' && sense != 'E') return -1;

    double sign = 1.0;
    if (row->rhs < 0.0) {
        sign = -1.0;
        if (sense == 'L') sense = 'G';
        else if (sense == 'G') sense = 'L';
    }

    *sense_out = sense;
    *sign_out = sign;
    *rhs_out = fabs(row->rhs);
    return 0;
}

static SimplexTableau *tableau_clone_with_augmented_rows(SimplexSolver *solver,
                                                         const LPAugmentRow *rows,
                                                         int num_rows,
                                                         int warm_m,
                                                         int warm_n,
                                                         const int *warm_basis,
                                                         const VarStatus *warm_var_status) {
    if (!solver || !solver->tableau || !solver->tableau->A_ext ||
        !rows || num_rows <= 0 || !warm_basis || !warm_var_status) {
        return NULL;
    }

    SimplexTableau *src = solver->tableau;
    LPModel *model = solver->model ? solver->model : src->model;
    if (!model) return NULL;

    int extra_aux = 0;
    int extra_art = 0;
    int extra_eq = 0;
    int extra_nnz = 0;
    for (int i = 0; i < num_rows; i++) {
        char norm_sense = 'L';
        double row_sign = 1.0;
        double rhs = 0.0;
        if (lp_augment_row_normalize(&rows[i], &norm_sense, &row_sign, &rhs) != 0) {
            return NULL;
        }
        (void)row_sign;
        (void)rhs;
        if (rows[i].nnz < 0) return NULL;
        extra_nnz += rows[i].nnz;
        if (norm_sense == 'L') {
            extra_aux += 1;
        } else if (norm_sense == 'G') {
            extra_aux += 2;
            extra_art += 1;
        } else {
            extra_aux += 1;
            extra_art += 1;
            extra_eq += 1;
        }
    }

    SimplexTableau *dst = (SimplexTableau*)calloc(1, sizeof(SimplexTableau));
    if (!dst) return NULL;

    dst->model = model;
    dst->m = src->m + num_rows;
    dst->n = src->n + extra_aux;
    dst->num_aux = src->num_aux + extra_aux;
    dst->num_equalities = src->num_equalities + extra_eq;
    dst->use_two_phase = (src->use_two_phase || extra_art > 0) ? 1 : 0;

    if (tableau_alloc_arrays(dst, dst->num_aux, src->num_artificial + extra_art) != 0) {
        tableau_free(dst);
        return NULL;
    }

    memcpy(dst->lb_ext, src->lb_ext, (size_t)src->n * sizeof(double));
    memcpy(dst->ub_ext, src->ub_ext, (size_t)src->n * sizeof(double));
    memcpy(dst->c_original, src->c_original, (size_t)src->n * sizeof(double));
    memcpy(dst->rhs, src->rhs, (size_t)src->m * sizeof(double));
    memcpy(dst->row_sign, src->row_sign, (size_t)src->m * sizeof(double));
    memcpy(dst->aux_row, src->aux_row, (size_t)src->num_aux * sizeof(int));
    memcpy(dst->aux_coef, src->aux_coef, (size_t)src->num_aux * sizeof(double));
    memcpy(dst->artificial_vars, src->artificial_vars,
           (size_t)src->num_artificial * sizeof(int));
    memcpy(dst->redundant_rows, src->redundant_rows, (size_t)src->m * sizeof(int));
    dst->num_redundant = src->num_redundant;
    dst->redundant_rows_zeroed = src->redundant_rows_zeroed;
    dst->perturb_scale = src->perturb_scale;
    dst->positive_edge_mode = src->positive_edge_mode;
    dst->partial_price_pos = src->partial_price_pos;
    dst->phase = 1;
    dst->solution_last_residual_iter = -1;
    dst->solution_last_residual_factorize_calls = -1;
    dst->solution_last_residual_num_updates = -1;

    SparseTriplets *trips = triplets_create(dst->m, dst->n,
                                            src->A_ext->nnz + extra_nnz + extra_aux);
    if (!trips) {
        tableau_free(dst);
        return NULL;
    }

    for (int j = 0; j < src->n; j++) {
        for (int p = src->A_ext->colptr[j]; p < src->A_ext->colptr[j + 1]; p++) {
            if (triplets_add(trips, src->A_ext->rowidx[p], j, src->A_ext->values[p]) != 0) {
                triplets_free(trips);
                tableau_free(dst);
                return NULL;
            }
        }
    }

    int next_aux = src->n;
    int next_aux_map = src->num_aux;
    int next_art = src->num_artificial;
    for (int i = 0; i < num_rows; i++) {
        char norm_sense = 'L';
        double row_sign = 1.0;
        double rhs = 0.0;
        int row_idx = src->m + i;
        if (lp_augment_row_normalize(&rows[i], &norm_sense, &row_sign, &rhs) != 0) {
            triplets_free(trips);
            tableau_free(dst);
            return NULL;
        }

        dst->rhs[row_idx] = rhs;
        dst->row_sign[row_idx] = row_sign;

        for (int k = 0; k < rows[i].nnz; k++) {
            int col = rows[i].indices[k];
            if (col < 0 || col >= model->num_vars) {
                triplets_free(trips);
                tableau_free(dst);
                return NULL;
            }
            double value = rows[i].values[k] * row_sign;
            if (fabs(value) <= RALPH_ZERO_TOL) continue;
            if (triplets_add(trips, row_idx, col, value) != 0) {
                triplets_free(trips);
                tableau_free(dst);
                return NULL;
            }
        }

        if (norm_sense == 'L') {
            if (triplets_add(trips, row_idx, next_aux, 1.0) != 0) {
                triplets_free(trips);
                tableau_free(dst);
                return NULL;
            }
            dst->lb_ext[next_aux] = 0.0;
            dst->ub_ext[next_aux] = RALPH_INFINITY;
            dst->c_original[next_aux] = 0.0;
            dst->aux_row[next_aux_map] = row_idx;
            dst->aux_coef[next_aux_map] = 1.0;
            next_aux_map++;
            next_aux++;
        } else if (norm_sense == 'G') {
            int surplus_idx = next_aux++;
            int artificial_idx = next_aux++;
            if (triplets_add(trips, row_idx, surplus_idx, -1.0) != 0 ||
                triplets_add(trips, row_idx, artificial_idx, 1.0) != 0) {
                triplets_free(trips);
                tableau_free(dst);
                return NULL;
            }
            dst->lb_ext[surplus_idx] = 0.0;
            dst->ub_ext[surplus_idx] = RALPH_INFINITY;
            dst->c_original[surplus_idx] = 0.0;
            dst->aux_row[next_aux_map] = row_idx;
            dst->aux_coef[next_aux_map] = -1.0;
            next_aux_map++;

            dst->lb_ext[artificial_idx] = 0.0;
            dst->ub_ext[artificial_idx] = RALPH_INFINITY;
            dst->c_original[artificial_idx] = 0.0;
            dst->aux_row[next_aux_map] = row_idx;
            dst->aux_coef[next_aux_map] = 1.0;
            next_aux_map++;
            dst->artificial_vars[next_art++] = artificial_idx;
        } else {
            int artificial_idx = next_aux++;
            if (triplets_add(trips, row_idx, artificial_idx, 1.0) != 0) {
                triplets_free(trips);
                tableau_free(dst);
                return NULL;
            }
            dst->lb_ext[artificial_idx] = 0.0;
            dst->ub_ext[artificial_idx] = RALPH_INFINITY;
            dst->c_original[artificial_idx] = 0.0;
            dst->aux_row[next_aux_map] = row_idx;
            dst->aux_coef[next_aux_map] = 1.0;
            next_aux_map++;
            dst->artificial_vars[next_art++] = artificial_idx;
        }
    }
    dst->num_artificial = next_art;

    dst->A_ext = triplets_to_csc(trips);
    triplets_free(trips);
    if (!dst->A_ext) {
        tableau_free(dst);
        return NULL;
    }

    if (tableau_build_csr_from_current_matrix(dst) != 0) {
        tableau_free(dst);
        return NULL;
    }

    vec_set_zero(dst->c_ext, dst->n);
    for (int k = 0; k < dst->num_artificial; k++) {
        int art_j = dst->artificial_vars[k];
        if (art_j >= 0 && art_j < dst->n &&
            dst->ub_ext[art_j] > dst->lb_ext[art_j] + RALPH_ZERO_TOL) {
            dst->c_ext[art_j] = 1.0;
        }
    }

    tableau_init_weights(dst);

    dst->lu = lu_create(dst->m);
    if (!dst->lu) {
        tableau_free(dst);
        return NULL;
    }

    configure_tableau_for_solver(solver, dst);

    solver->warm_basis_last_attempted = 1;
    solver->warm_basis_last_applied = 0;
    solver->warm_basis_last_rejected = 0;
    if (tableau_apply_warm_basis(dst, warm_m, warm_n, warm_basis, warm_var_status) != 0) {
        solver->warm_basis_last_rejected = 1;
        tableau_free(dst);
        return NULL;
    }
    solver->warm_basis_last_applied = 1;

    {
        double t_refactor_ms = lp_telemetry_timer_start();
        int factorize_ok = (tableau_refactorize_with_reason(dst, RALPH_REFACTOR_REASON_SETUP) == 0);
        lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
        if (!factorize_ok) {
            tableau_free(dst);
            return NULL;
        }
    }

    tableau_compute_solution(dst);
    tableau_compute_reduced_costs(dst);
    return dst;
}

int simplex_prepare_augmented_primal_tableau(SimplexSolver *solver,
                                             const LPAugmentRow *rows,
                                             int num_rows,
                                             int warm_m,
                                             int warm_n,
                                             const int *warm_basis,
                                             const VarStatus *warm_var_status) {
    if (!solver || !solver->tableau || !rows || num_rows <= 0 ||
        !warm_basis || !warm_var_status) {
        return -1;
    }

    SimplexTableau *old_tab = solver->tableau;
    SimplexTableau *new_tab = tableau_clone_with_augmented_rows(solver,
                                                                rows,
                                                                num_rows,
                                                                warm_m,
                                                                warm_n,
                                                                warm_basis,
                                                                warm_var_status);
    if (!new_tab) {
        return -1;
    }

    solver->tableau = new_tab;
    tableau_free(old_tab);
    return 0;
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

int simplex_prepare_primal_tableau(SimplexSolver *solver, int allow_crash) {
    if (!solver) return -1;
    return setup_primal_tableau(solver, allow_crash);
}

static void simplex_invalidate_cached_outputs(SimplexSolver *solver) {
    if (!solver) return;

    free(solver->solution);
    solver->solution = NULL;
    free(solver->dual_solution);
    solver->dual_solution = NULL;
    free(solver->reduced_costs);
    solver->reduced_costs = NULL;
    solver->farkas_valid = 0;
    solver->unbounded_valid = 0;
}

static void simplex_reset_run_state(SimplexSolver *solver) {
    if (!solver) return;

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
}

static int simplex_finish_prepared_primal_solve(SimplexSolver *solver, clock_t start) {
    if (!solver || !solver->tableau) return -1;

    SimplexTableau *tab = solver->tableau;

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
            solver->pricing_strategy = saved_pricing;
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
                return 0;
            }
            return -1;
        }
        lp_telemetry_add_solver_stage_timed(solver, LP_SOLVER_STAGE_PHASE1, t_phase1_ms);
    }
    solver->pricing_strategy = saved_pricing;
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
        (void)status;
        if (solver->status == RALPH_STATUS_ERROR && tab->use_two_phase) {
            two_phase_failed = 1;
        }
    }

    /* If two-phase failed, try dual simplex as a one-shot fallback.
     * Only for method=0 (explicit primal) to avoid circular chains with
     * method=2 (which already tried dual before falling back to primal). */
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

int simplex_resolve_prepared_primal_tableau(SimplexSolver *solver) {
    if (!solver || !solver->tableau) return -1;

    clock_t start = clock();
    solver->progress_start_ms = lp_telemetry_now_ms();
    lp_determinism_apply_runtime(solver);
    simplex_invalidate_cached_outputs(solver);
    simplex_reset_run_state(solver);
    reset_solver_perf(solver);

    if (solver->verbose) {
        LP_LOG_STDOUT("[simplex_solve] Resolving prepared primal tableau...\n");
    }

    return simplex_finish_prepared_primal_solve(solver, start);
}

int simplex_solve(SimplexSolver *solver) {
    if (!solver || !solver->model) return -1;

    solver->current_phase = SIMPLEX_PHASE_INIT;
    clock_t start = clock();
    solver->progress_start_ms = lp_telemetry_now_ms();
    lp_determinism_apply_runtime(solver);

    /* Invalidate cached outputs from any previous solve.
     * This prevents stale primal/dual data from being reused when the current
     * solve fails before producing new solution vectors. */
    simplex_invalidate_cached_outputs(solver);
    simplex_reset_run_state(solver);

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
    }
    return simplex_finish_prepared_primal_solve(solver, start);
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
