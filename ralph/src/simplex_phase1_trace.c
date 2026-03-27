/*
 * simplex_phase1_trace.c - Phase 1 trace recording and stagnation detection.
 *
 * Zero behavior change — pure code motion from simplex.c (R3.9).
 */

#include "simplex_phase1_trace.h"
#include "simplex_internal.h"
#include "simplex_phase1_recovery.h"
#include "lp_refactor_policy.h"
#include "lp_glpk_strict.h"
#include "lp_log.h"
#include "ralph_lp.h"

#include <math.h>

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

int phase1_trace_reason_from_lu_failure(int lu_reason, int forced_refactor_path) {
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

