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
#include "lp.h"

/* Forward declarations */
int lp_model_finalize(LPModel *model);

static inline double perf_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

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
#define PHASE1_DIR_STABILIZE_COOLDOWN_ITERS 8
#define PHASE1_DIR_INF_FORCE_REFACTOR_MULT 100.0

typedef struct {
    int interval;
    int min_update_age;
    double interval_pressure;
    double run_pressure;
} PeriodicRefactorPolicy;

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

static double periodic_feedback_bias_for_phase(const SimplexSolver *owner, int phase) {
    if (!owner) return 0.0;
    if (phase == 1) return owner->periodic_feedback_bias_phase1;
    if (phase == 2) return owner->periodic_feedback_bias_phase2;
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
        owner->periodic_feedback_hint_interval_phase1 = interval;
        owner->periodic_feedback_hint_pressure_phase1 = run_pressure;
    } else if (phase == 2) {
        owner->periodic_feedback_hint_interval_phase2 = interval;
        owner->periodic_feedback_hint_pressure_phase2 = run_pressure;
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
        bias_ptr = &owner->periodic_feedback_bias_phase1;
        last_reason_ptr = &owner->periodic_feedback_last_reason_phase1;
        last_interval_ptr = &owner->periodic_feedback_last_interval_phase1;
        hint_interval_ptr = &owner->periodic_feedback_hint_interval_phase1;
        hint_pressure_ptr = &owner->periodic_feedback_hint_pressure_phase1;
    } else {
        bias_ptr = &owner->periodic_feedback_bias_phase2;
        last_reason_ptr = &owner->periodic_feedback_last_reason_phase2;
        last_interval_ptr = &owner->periodic_feedback_last_interval_phase2;
        hint_interval_ptr = &owner->periodic_feedback_hint_interval_phase2;
        hint_pressure_ptr = &owner->periodic_feedback_hint_pressure_phase2;
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

static int periodic_interval_bounds(int phase, int *min_interval, int *max_interval) {
    if (!min_interval || !max_interval) return 0;
    if (phase == 1) {
        *min_interval = PHASE1_PERIODIC_REFACTOR_MIN_INTERVAL;
        *max_interval = PHASE1_PERIODIC_REFACTOR_MAX_INTERVAL;
        return 1;
    }
    if (phase == 2) {
        *min_interval = PHASE2_PERIODIC_REFACTOR_MIN_INTERVAL;
        *max_interval = PHASE2_PERIODIC_REFACTOR_MAX_INTERVAL;
        return 1;
    }
    return 0;
}

static int phase2_large_degenerate_relax_ok(int phase,
                                            int m,
                                            int use_bland,
                                            int degenerate_count,
                                            int spike_pool_used,
                                            int spike_pool_capacity,
                                            double cond_estimate,
                                            double growth_factor) {
    if (phase != 2) return 0;
    if (m < PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_M) return 0;
    if (use_bland) return 0;
    if (degenerate_count < PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_COUNT) return 0;
    if (spike_pool_capacity > 0 &&
        spike_pool_used * 100 >
            spike_pool_capacity * PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_SPIKE_PCT) {
        return 0;
    }
    if (isfinite(cond_estimate) && cond_estimate > PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_COND) {
        return 0;
    }
    if (isfinite(growth_factor) && growth_factor > PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MAX_GROWTH) {
        return 0;
    }
    return 1;
}

static double compute_large_basis_pressure(int m) {
    if (m <= PERIODIC_REFACTOR_SIZE_START_M) return 0.0;
    if (m >= PERIODIC_REFACTOR_SIZE_FULL_M) return 1.0;
    return (double)(m - PERIODIC_REFACTOR_SIZE_START_M) /
           (double)(PERIODIC_REFACTOR_SIZE_FULL_M - PERIODIC_REFACTOR_SIZE_START_M);
}

static double compute_lu_health_pressure(int spike_pool_used,
                                         int spike_pool_capacity,
                                         double cond_estimate,
                                         double growth_factor,
                                         int use_bland,
                                         int degenerate_count) {
    double deg_pressure = use_bland ? 1.0 : clamp_unit_interval((double)degenerate_count / 20.0);
    double spike_pressure = 0.0;
    double cond_pressure = 0.0;
    double growth_pressure = 0.0;
    double health_pressure;

    if (spike_pool_capacity > 0 && spike_pool_used > 0) {
        spike_pressure = clamp_unit_interval((double)spike_pool_used / (double)spike_pool_capacity);
    }
    if (isfinite(cond_estimate) && cond_estimate > 1.0) {
        cond_pressure = clamp_unit_interval(log10(cond_estimate) / 8.0);
    }
    if (isfinite(growth_factor) && growth_factor > 1.0) {
        growth_pressure = clamp_unit_interval(growth_factor / RALPH_LU_GROWTH_REFACTOR_THRESHOLD);
    }

    health_pressure = 0.45 * deg_pressure +
                      0.25 * spike_pressure +
                      0.15 * cond_pressure +
                      0.15 * growth_pressure;

    if (deg_pressure > health_pressure) health_pressure = deg_pressure;
    if (growth_pressure > health_pressure) health_pressure = growth_pressure;
    return clamp_unit_interval(health_pressure);
}

static PeriodicRefactorPolicy build_periodic_refactor_policy_from_metrics(int phase,
                                                                           int m,
                                                                           int max_updates,
                                                                           int num_updates,
                                                                           int spike_pool_used,
                                                                           int spike_pool_capacity,
                                                                           double cond_estimate,
                                                                           double growth_factor,
                                                                           int use_bland,
                                                                           int degenerate_count,
                                                                           double feedback_bias) {
    PeriodicRefactorPolicy policy = {0, 0, 0.0, 0.0};
    int min_interval = 0;
    int max_interval = 0;
    int base_interval;
    int size_interval;
    int relax_span;
    double size_pressure;
    double health_pressure;
    double update_pressure = 0.0;

    if (!periodic_interval_bounds(phase, &min_interval, &max_interval)) return policy;
    if (phase2_large_degenerate_relax_ok(phase,
                                         m,
                                         use_bland,
                                         degenerate_count,
                                         spike_pool_used,
                                         spike_pool_capacity,
                                         cond_estimate,
                                         growth_factor) &&
        min_interval < PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MIN_INTERVAL) {
        min_interval = PHASE2_PERIODIC_REFACTOR_LARGE_DEGEN_MIN_INTERVAL;
    }

    base_interval = (phase == 1)
        ? ((max_updates > 0) ? (max_updates / 3) : min_interval)
        : ((max_updates > 0) ? (max_updates / 4) : min_interval);
    if (base_interval < min_interval) base_interval = min_interval;
    if (base_interval > max_interval) base_interval = max_interval;

    size_pressure = compute_large_basis_pressure(m);
    size_interval = base_interval -
                    (int)(size_pressure * (double)(base_interval - min_interval) + 0.5);
    if (size_interval < min_interval) size_interval = min_interval;
    if (size_interval > max_interval) size_interval = max_interval;

    health_pressure = compute_lu_health_pressure(spike_pool_used,
                                                 spike_pool_capacity,
                                                 cond_estimate,
                                                 growth_factor,
                                                 use_bland,
                                                 degenerate_count);
    policy.interval_pressure = (health_pressure > size_pressure) ? health_pressure : size_pressure;
    policy.interval_pressure = clamp_unit_interval(policy.interval_pressure + feedback_bias);

    relax_span = max_interval - size_interval;
    relax_span = (relax_span * PERIODIC_REFACTOR_RELAX_NUM) / PERIODIC_REFACTOR_RELAX_DEN;
    if (relax_span < 0) relax_span = 0;

    policy.interval = size_interval +
                      (int)(((1.0 - policy.interval_pressure) * (double)relax_span) + 0.5);
    if (policy.interval < min_interval) policy.interval = min_interval;
    if (policy.interval > max_interval) policy.interval = max_interval;
    if (max_updates >= min_interval && policy.interval > max_updates) {
        policy.interval = max_updates;
    }

    if (max_updates > 0 && num_updates > 0) {
        update_pressure = clamp_unit_interval((double)num_updates / (double)max_updates);
    }
    policy.run_pressure = policy.interval_pressure;
    if (update_pressure > policy.run_pressure) policy.run_pressure = update_pressure;
    if (use_bland || degenerate_count >= 20) policy.run_pressure = 1.0;

    policy.min_update_age = policy.interval / 2;
    if (policy.run_pressure >= 0.85) {
        int early_age = policy.interval / 3;
        if (early_age > 0 && early_age < policy.min_update_age) {
            policy.min_update_age = early_age;
        }
    }
    if (policy.min_update_age < PERIODIC_REFACTOR_MIN_UPDATE_AGE) {
        policy.min_update_age = PERIODIC_REFACTOR_MIN_UPDATE_AGE;
    }
    if (policy.min_update_age > policy.interval) {
        policy.min_update_age = policy.interval;
    }

    return policy;
}

static PeriodicRefactorPolicy compute_periodic_refactor_policy(const SimplexTableau *tab,
                                                               int phase,
                                                               int use_bland,
                                                               int degenerate_count) {
    PeriodicRefactorPolicy policy = {0, 0, 0.0, 0.0};
    double feedback_bias = 0.0;

    if (!tab || !tab->lu || !tab->use_two_phase) return policy;
    feedback_bias = periodic_feedback_bias_for_phase(tab->owner, phase);
    return build_periodic_refactor_policy_from_metrics(phase,
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

static int periodic_refactor_should_run_metrics(int iter,
                                                int num_updates,
                                                const PeriodicRefactorPolicy *policy,
                                                int use_bland,
                                                int degenerate_count) {
    if (!policy || policy->interval <= 0 || iter <= 0 || num_updates <= 0) return 0;
    if (num_updates < policy->min_update_age) return 0;
    if ((num_updates % policy->interval) != 0) return 0;

    if (use_bland || degenerate_count >= 20) return 1;
    return policy->run_pressure >= PERIODIC_REFACTOR_PRESSURE_TRIGGER;
}

static int should_run_periodic_refactor(const SimplexTableau *tab,
                                        int iter,
                                        const PeriodicRefactorPolicy *policy,
                                        int use_bland,
                                        int degenerate_count) {
    if (!tab || !tab->lu) return 0;
    return periodic_refactor_should_run_metrics(iter,
                                                tab->lu->num_updates,
                                                policy,
                                                use_bland,
                                                degenerate_count);
}

/*
 * Centralized basis-update policy used by simplex_pivot().
 * lu_update_status convention:
 *   0  = no LU update attempt yet
 *  -1  = LU update failed
 *  -2  = refactorization failed
 *  -3  = repair failed
 */
static BasisAction choose_basis_action(double pivot,
                                       int force_refactor,
                                       int lu_update_status,
                                       int lu_reason,
                                       int repeat_pattern,
                                       double growth_factor) {
    (void)lu_reason;

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
    if (force_refactor ||
        repeat_pattern >= RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER ||
        growth_factor > RALPH_LU_GROWTH_REFACTOR_THRESHOLD) {
        return BASIS_ACTION_REFACTOR;
    }
    return BASIS_ACTION_UPDATE;
}

int simplex_choose_basis_action_for_test(double pivot,
                                         int force_refactor,
                                         int lu_update_status,
                                         int lu_reason,
                                         int repeat_pattern,
                                         double growth_factor) {
    return (int)choose_basis_action(pivot,
                                    force_refactor,
                                    lu_update_status,
                                    lu_reason,
                                    repeat_pattern,
                                    growth_factor);
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
                                            int *interval_out,
                                            double *pressure_out) {
    PeriodicRefactorPolicy policy = build_periodic_refactor_policy_from_metrics(phase,
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
    if (interval_out) *interval_out = policy.interval;
    if (pressure_out) *pressure_out = policy.run_pressure;
    return periodic_refactor_should_run_metrics(iter,
                                                num_updates,
                                                &policy,
                                                use_bland,
                                                degenerate_count);
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

    fprintf(stderr,
            "[phase1_trace] event=no_entering iter=%d code=%d\n",
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

    fprintf(stderr,
            "[phase1_trace] event=pivot_fail iter=%d repeat=%d entering=%d leaving=%d theta=%.12e reason=%s pivot=%.12e dir_inf=%.12e\n",
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

    fprintf(stderr,
            "[phase1_trace] summary status=%s piv_fail=%d small_pivot=%d invalid_col=%d lu_max_updates=%d lu_spike_pool_full=%d lu_update_pivot_small=%d lu_singular_update=%d factor_singular=%d refactor_forced_other=%d refactor_after_update_other=%d no_entering=%d first_iter=%d last_iter=%d sig=0x%016llx\n",
            ralph_status_string(phase1_status),
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
            printf("[verify] IMPRECISE: primal=%.2e bound=%.2e dual=%.2e cs=%.2e obj=%.2e cond=%.2e\n",
                   max_primal_infeas, max_bound_infeas, max_dual_infeas,
                   max_comp_slack, obj_rel_error, cond);
        }
    } else if (solver->verbose) {
        printf("[verify] OK: primal=%.2e bound=%.2e dual=%.2e cs=%.2e obj=%.2e cond=%.2e\n",
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
        /* double arrays: y, work1, work2, rhs, row_sign, pivot_row, tau_work (m each) */
        7 * (size_t)m * sizeof(double) +
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
        !tab->work1 || !tab->work2 || !tab->work3 || !tab->rhs || !tab->row_sign ||
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

/* Build basis matrix from current basis into reusable workspace */
static SparseMatrix* build_basis_matrix(SimplexTableau *tab) {
    if (!tab || !tab->A_ext || !tab->basis) return NULL;

    const SparseMatrix *A = tab->A_ext;
    SparseMatrix *B = tab->basis_work;
    int changed = 0;
    int first_changed = tab->m;
    int last_changed = -1;

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
                return B;
            }

            /* General incremental path: rewrite only the [first_changed, last_changed]
             * basis span and shift the suffix tail when the span nnz changes. */
            if (first_changed >= 0 && first_changed < tab->m &&
                last_changed >= first_changed && last_changed < tab->m) {
                int old_block_start = B->colptr[first_changed];
                int old_block_end = B->colptr[last_changed + 1];
                int old_block_nnz = old_block_end - old_block_start;
                int new_block_nnz = 0;
                int old_tail_start = old_block_end;
                int old_tail_nnz = old_total_nnz - old_tail_start;

                for (int k = first_changed; k <= last_changed; k++) {
                    int j = tab->basis[k];
                    new_block_nnz += A->colptr[j + 1] - A->colptr[j];
                }

                {
                    int delta = new_block_nnz - old_block_nnz;
                    if (old_tail_nnz > 0 && delta != 0) {
                        int new_tail_start = old_tail_start + delta;
                        memmove(B->rowidx + new_tail_start,
                                B->rowidx + old_tail_start,
                                (size_t)old_tail_nnz * sizeof(int));
                        memmove(B->values + new_tail_start,
                                B->values + old_tail_start,
                                (size_t)old_tail_nnz * sizeof(double));
                    }

                    {
                        int idx = old_block_start;
                        for (int k = first_changed; k <= last_changed; k++) {
                            int j = tab->basis[k];
                            int start = A->colptr[j];
                            int end = A->colptr[j + 1];
                            int col_nnz = end - start;
                            B->colptr[k] = idx;
                            if (col_nnz > 0) {
                                memcpy(B->rowidx + idx, A->rowidx + start, (size_t)col_nnz * sizeof(int));
                                memcpy(B->values + idx, A->values + start, (size_t)col_nnz * sizeof(double));
                            }
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
                return B;
            }
        } else {
            tab->basis_cache_valid = 0;
            tab->basis_cache_total_nnz = 0;
        }

        tab->basis_cache_valid = 0;
        tab->basis_cache_total_nnz = 0;

    }

    int nnz = 0;
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
    double t_refactor_ms = perf_now_ms();
    SimplexSolver *owner = tab ? tab->owner : NULL;
    int reason = RALPH_REFACTOR_REASON_OTHER;
    int updates_before = (tab && tab->lu) ? tab->lu->num_updates : 0;
    if (owner) {
        reason = owner->perf_refactor_next_reason;
        owner->perf_refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
    }

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
        double elapsed_ms = perf_now_ms() - t_refactor_ms;
        owner->perf_refactor_all_ms += elapsed_ms;
        owner->perf_refactor_count++;
        owner->perf_refactor_last_ms = elapsed_ms;
        if (elapsed_ms > owner->perf_refactor_max_ms) {
            owner->perf_refactor_max_ms = elapsed_ms;
        }
        owner->perf_refactor_last_reason = reason;
        switch ((RalphRefactorReason)reason) {
            case RALPH_REFACTOR_REASON_SETUP:
                owner->perf_refactor_reason_setup++;
                break;
            case RALPH_REFACTOR_REASON_PHASE_TRANSITION:
                owner->perf_refactor_reason_transition++;
                break;
            case RALPH_REFACTOR_REASON_PERIODIC:
                owner->perf_refactor_reason_periodic++;
                break;
            case RALPH_REFACTOR_REASON_RATIO_RECOVERY:
                owner->perf_refactor_reason_ratio_recovery++;
                break;
            case RALPH_REFACTOR_REASON_PIVOT_RECOVERY:
                owner->perf_refactor_reason_pivot_recovery++;
                break;
            case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT:
                owner->perf_refactor_reason_forced_small_pivot++;
                break;
            case RALPH_REFACTOR_REASON_UPDATE_RECOVERY:
                owner->perf_refactor_reason_update_recovery++;
                break;
            case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE:
                owner->perf_refactor_reason_direction_stabilize++;
                break;
            case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP:
                owner->perf_refactor_reason_infeas_cleanup++;
                break;
            case RALPH_REFACTOR_REASON_OTHER:
            default:
                owner->perf_refactor_reason_other++;
                break;
        }

        owner->perf_refactor_last_m = tab->m;
        if (tab->lu) {
            owner->perf_refactor_last_k = tab->lu->perf_last_k;
            owner->perf_refactor_last_nnz_B = tab->lu->perf_last_basis_nnz;
        } else {
            owner->perf_refactor_last_k = 0;
            owner->perf_refactor_last_nnz_B = 0;
        }

        if (tab->phase == 1) {
            owner->perf_phase1_refactor_ms += elapsed_ms;
            owner->perf_phase1_refactor_calls++;
        } else if (tab->phase == 2) {
            owner->perf_phase2_refactor_ms += elapsed_ms;
            owner->perf_phase2_refactor_calls++;
        }

        periodic_feedback_record_refactor(owner, tab ? tab->phase : 0, reason, updates_before, status);
    }

    return status;
}

static inline int tableau_refactorize_with_reason(SimplexTableau *tab, int reason) {
    if (tab && tab->owner) {
        tab->owner->perf_refactor_next_reason = reason;
    }
    return tableau_refactorize(tab);
}

/* ============================================================================
 * Solution Computation
 * ============================================================================ */

int tableau_compute_solution(SimplexTableau *tab) {
    double t0_ms = perf_now_ms();

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

    /* Save original RHS for iterative refinement */
    double *orig_rhs = (double*)calloc(tab->m, sizeof(double));
    if (orig_rhs) {
        vec_copy_data(orig_rhs, tab->work1, tab->m);
    }

    /* Solve B * x_B = work1 */
    lu_solve(tab->lu, tab->work1, tab->work2);

    /* Update basic variable values */
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] = tab->work2[k];
    }

    /* Iterative refinement: check residual and correct if needed */
    if (orig_rhs) {
        /* Compute residual: r = b - B*x_B */
        /* work3 will hold B*x_B */
        vec_set_zero(tab->work3, tab->m);
        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            sparse_axpy_column(tab->A_ext, j, tab->x[j], tab->work3);
        }

        /* work1 = original_rhs - B*x_B = residual */
        double max_residual = 0.0;
        for (int i = 0; i < tab->m; i++) {
            tab->work1[i] = orig_rhs[i] - tab->work3[i];
            double absval = fabs(tab->work1[i]);
            if (absval > max_residual) max_residual = absval;
        }

        /* If residual is large, do iterative refinement */
        int max_refine_iters = 5;
        for (int refine_iter = 0; refine_iter < max_refine_iters && max_residual > RALPH_FEAS_TOL; refine_iter++) {
            /* Solve B * correction = residual */
            lu_solve(tab->lu, tab->work1, tab->work2);

            /* Update solution: x_B += correction */
            for (int k = 0; k < tab->m; k++) {
                tab->x[tab->basis[k]] += tab->work2[k];
            }

            /* Recompute residual */
            vec_set_zero(tab->work3, tab->m);
            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                sparse_axpy_column(tab->A_ext, j, tab->x[j], tab->work3);
            }
            max_residual = 0.0;
            for (int i = 0; i < tab->m; i++) {
                tab->work1[i] = orig_rhs[i] - tab->work3[i];
                double absval = fabs(tab->work1[i]);
                if (absval > max_residual) max_residual = absval;
            }
        }

        free(orig_rhs);
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
        double elapsed_ms = perf_now_ms() - t0_ms;
        tab->owner->perf_compute_solution_ms += elapsed_ms;
        if (tab->phase == 1) {
            tab->owner->perf_phase1_compute_solution_ms += elapsed_ms;
            tab->owner->perf_phase1_compute_solution_calls++;
        } else if (tab->phase == 2) {
            tab->owner->perf_phase2_compute_solution_ms += elapsed_ms;
            tab->owner->perf_phase2_compute_solution_calls++;
        }
    }
    return 0;
}

int tableau_compute_reduced_costs(SimplexTableau *tab) {
    double t0_ms = perf_now_ms();

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
        double elapsed_ms = perf_now_ms() - t0_ms;
        tab->owner->perf_compute_rc_ms += elapsed_ms;
        if (tab->phase == 1) {
            tab->owner->perf_phase1_compute_rc_ms += elapsed_ms;
            tab->owner->perf_phase1_compute_rc_calls++;
        } else if (tab->phase == 2) {
            tab->owner->perf_phase2_compute_rc_ms += elapsed_ms;
            tab->owner->perf_phase2_compute_rc_calls++;
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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
    if (tab->var_status[j] == RALPH_BASIC) return 0;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
    if (tab->var_status[j] == RALPH_BASIC) return 0;

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
        if (tab->var_status[j] == RALPH_BASIC) continue;
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
        if (tab->var_status[j] == RALPH_BASIC) continue;

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
        double t_ftran_ms = perf_now_ms();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            tab->owner->perf_ftran_ms += perf_now_ms() - t_ftran_ms;
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
    double max_abs_dk = 0.0;
    for (int k = 0; k < tab->m; k++) {
        double abs_dk = fabs(tab->work2[k] * dir);
        if (abs_dk > max_abs_dk) {
            max_abs_dk = abs_dk;
        }
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

int ratio_test_harris(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    /* Use hyper-sparse FTRAN for better performance on sparse columns */
    {
        double t_ftran_ms = perf_now_ms();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            tab->owner->perf_ftran_ms += perf_now_ms() - t_ftran_ms;
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
    double max_abs_dk = 0.0;
    for (int k = 0; k < tab->m; k++) {
        double dk = (dir > 0.0) ? work2[k] : -work2[k];
        double abs_dk = fabs(dk);
        if (abs_dk > max_abs_dk) {
            max_abs_dk = abs_dk;
        }
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
        double t_btran_ms = perf_now_ms();
        lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, pivot_row);
        if (tab->owner) {
            tab->owner->perf_btran_ms += perf_now_ms() - t_btran_ms;
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
        double t_btran_ms = perf_now_ms();
        lu_solve_transpose(tab->lu, tab->work2, tau_helper);
        if (tab->owner) {
            tab->owner->perf_btran_ms += perf_now_ms() - t_btran_ms;
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
    BasisAction action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             growth_factor);

    for (;;) {
        switch (action) {
            case BASIS_ACTION_UPDATE:
                sparse_get_column(tab->A_ext, entering, tab->work1);
                {
                    double t_lu_update_ms = perf_now_ms();
                    lu_update_status = lu_update(tab->lu, leaving_pos, tab->work1);
                    if (tab->owner) {
                        tab->owner->perf_lu_update_ms += perf_now_ms() - t_lu_update_ms;
                    }
                }
                if (lu_update_status == 0) {
                    goto basis_update_done;
                }
                lu_update_status = -1;
                update_reason = (tab->lu) ? tab->lu->last_failure_reason : LU_FAIL_NONE;
                lu_reason = update_reason;
                growth_factor = (tab->lu) ? tab->lu->growth_factor : growth_factor;
                action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             growth_factor);
                continue;

            case BASIS_ACTION_REFACTOR:
                refactor_forced_path = (lu_update_status == 0);
                {
                    int ref_reason = force_refactor
                                     ? RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT
                                     : RALPH_REFACTOR_REASON_UPDATE_RECOVERY;
                    double t_refactor_ms = perf_now_ms();
                    lu_update_status = tableau_refactorize_with_reason(tab, ref_reason);
                    if (tab->owner) {
                        tab->owner->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
                    }
                }
                if (lu_update_status == 0) {
                    goto basis_update_done;
                }
                lu_update_status = -2;
                lu_reason = (tab->lu) ? tab->lu->last_failure_reason : lu_reason;
                growth_factor = (tab->lu) ? tab->lu->growth_factor : growth_factor;
                action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             growth_factor);
                continue;

            case BASIS_ACTION_REPAIR:
                if (repair_singular_basis(tab) == 0) {
                    goto basis_update_done;
                }
                lu_update_status = -3;
                lu_reason = (tab->lu) ? tab->lu->last_failure_reason : lu_reason;
                growth_factor = (tab->lu) ? tab->lu->growth_factor : growth_factor;
                action = choose_basis_action(pivot,
                                             force_refactor,
                                             lu_update_status,
                                             lu_reason,
                                             repeat_pattern_count,
                                             growth_factor);
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
    solver->verbose = 0;
    solver->trace_phase1 = 0;
    solver->is_scaled = 0;
    solver->trace_phase1_first_fail_iter = -1;
    solver->trace_phase1_last_fail_iter = -1;
    solver->objective_limit = RALPH_INFINITY;
    solver->phase1_pricing = -1;  /* Default: disabled (use solver pricing) */
    solver->use_dual_bound_flip = 1;
    solver->use_dual_steepest_edge = 1;
    solver->method = 2;  /* Default: auto (dual first, primal fallback) */

    return solver;
}

void simplex_free(SimplexSolver *solver) {
    if (!solver) return;

    tableau_free(solver->tableau);
    free(solver->solution);
    free(solver->dual_solution);
    free(solver->reduced_costs);
    free(solver->row_scale);
    free(solver->col_scale);
    free(solver->farkas_ray);
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
            fprintf(stderr, "[extract_farkas_ray] WARNING: Farkas ray is all zeros\n");
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
            fprintf(stderr, "[extract_farkas_ray] WARNING: y'b_tab = %.6e (expected < 0)\n",
                    y_tab_dot_rhs);
        }
        /* Don't invalidate - this might be a borderline numerical case.
         * The ray can still be used, but user should be aware. */
    } else if (solver->verbose >= 2) {
        fprintf(stderr, "[extract_farkas_ray] y'b_tab = %.6e < 0 (valid)\n", y_tab_dot_rhs);
    }

    solver->farkas_valid = 1;

    if (solver->verbose >= 2) {
        fprintf(stderr, "[extract_farkas_ray] Valid certificate: ||y||_inf = %.6e\n", max_abs);
    }
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

        /* Pseudo-random perturbation factor */
        double factor = 1.0 + (j * PRIMAL_PERTURB_MULT) % 13;

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

        double factor = 1.0 + (j * PRIMAL_PERTURB_MULT) % 13;

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
                double t_pivot_ms = perf_now_ms();
                simplex_pivot(tab, entering, leaving, theta, 0);
                double pivot_elapsed_ms = perf_now_ms() - t_pivot_ms;
                solver->perf_pivot_ms += pivot_elapsed_ms;
                solver->perf_phase1_pivot_ms += pivot_elapsed_ms;
                solver->perf_phase1_pivot_calls++;
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
        fprintf(stderr, "[simplex_phase1] Starting Phase 1 with %d artificial variables, %d equalities\n",
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
            fprintf(stderr, "[simplex_phase1] Already feasible, skipping Phase 1\n");
        }
        phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
        return 0;
    }

    /* Cycling detection and anti-cycling measures */
    int degenerate_count = 0;
    const int DEGEN_THRESHOLD = 50;    /* Switch to Bland's rule after this many */
    const int RECOMPUTE_INTERVAL = 25; /* Periodic drift correction in Phase 1 */
    int use_bland = 0;

    /* Phase 1 stall detection: track objective (art_sum) progress.
     * When Phase 1 stalls with Bland's rule, re-perturbation breaks the cycle.
     * primal_apply_perturbation_scaled skips artificial bounds (Phase 1 safe). */
    double last_obj_p1 = tab->obj_value;
    int stall_count_p1 = 0;
    const int P1_STALL_THRESHOLD = 50;
    int perturb_attempts_p1 = 0;
    const int P1_MAX_PERTURB_ATTEMPTS = 15;
    int fail_entering = -1;
    int fail_leaving_pos = -1;
    int fail_reason = PHASE1_PIVOT_FAIL_NONE;
    int fail_repeat_count = 0;
    int ratio_breakdown_count = 0;
    int excluded_entering_a = -1;
    int excluded_entering_ttl_a = 0;
    int excluded_entering_b = -1;
    int excluded_entering_ttl_b = 0;
    int dir_stabilize_cooldown = 0;

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
            fprintf(stderr, "[simplex_phase1] Proactive perturbation: %d equalities out of %d constraints (%.0f%%)\n",
                    tab->num_equalities, tab->m, 100.0 * tab->num_equalities / tab->m);
        }
        tableau_compute_solution(tab);
    }

    /* Compute initial reduced costs */
    tableau_compute_reduced_costs(tab);
    if (solver->pricing_strategy == 4) heap_build(tab);

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tab->iterations = iter;
        tab->trace_phase1_iter = iter;
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

        /* Pricing: select entering variable */
        int entering;
        int price_status;
        double t_pricing_ms = perf_now_ms();

        if (use_bland) {
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
                    fprintf(stderr,
                            "[simplex_phase1] Excluding unstable entering (%d,%d), using %d instead\n",
                            exclude_a, exclude_b, alt_entering);
                }
                entering = alt_entering;
            }
        }
        {
            double pricing_elapsed_ms = perf_now_ms() - t_pricing_ms;
            solver->perf_pricing_ms += pricing_elapsed_ms;
            solver->perf_phase1_pricing_ms += pricing_elapsed_ms;
            solver->perf_phase1_pricing_calls++;
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
                                fprintf(stderr, "[simplex_phase1] Refactorized cleanup: art_sum %g -> %g\n",
                                        art_sum, refined_art_sum);
                            }
                            continue;
                        }
                        art_sum = refined_art_sum;
                    }

                    /* Truly infeasible - extract Farkas ray from Phase 1 duals.
                     * The Phase 1 duals y = c_B^T * B^{-1} provide the certificate. */
                    if (solver->verbose) {
                        fprintf(stderr, "[simplex_phase1] INFEASIBLE: artificial sum = %g after %d iterations\n",
                                art_sum, iter);
                    }
                    extract_farkas_ray(solver);
                    solver->status = RALPH_STATUS_INFEASIBLE;
                    solver->iterations = iter;
                    phase1_trace_emit_summary(solver, RALPH_STATUS_INFEASIBLE);
                    return -1;
                }

                /* Small residual - try to clean up with a few more iterations */
                if (solver->verbose) {
                    fprintf(stderr, "[simplex_phase1] Cleanup phase: art_sum=%g, continuing...\n", art_sum);
                }
                tableau_compute_reduced_costs(tab);
                continue;  /* Try more iterations to drive artificials to zero */
            }

            /* Success */
            if (solver->verbose) {
                fprintf(stderr, "[simplex_phase1] Phase 1 complete: feasible in %d iterations\n", iter);
            }
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
            return 0;
        }

        /* Ratio test: select leaving variable */
        int leaving;
        double theta;
        double t_ratio_ms = perf_now_ms();
        int ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
        {
            double ratio_elapsed_ms = perf_now_ms() - t_ratio_ms;
            solver->perf_ratio_ms += ratio_elapsed_ms;
            solver->perf_phase1_ratio_ms += ratio_elapsed_ms;
            solver->perf_phase1_ratio_calls++;
        }

        if (ratio_status != 0) {
            phase1_trace_record_no_entering(solver, iter, ratio_status);

            /* "Unbounded" in Phase 1 is typically numerical, not structural.
             * Try to recover via refactorization and conservative pricing first. */
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_RATIO_RECOVERY) == 0) {
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                use_bland = 1;
                continue;
            }
            if (!use_bland) {
                use_bland = 1;
                tableau_compute_reduced_costs(tab);
                continue;
            }

            /* Last-chance recovery before treating Phase-1 "unbounded" as
             * numerical breakdown. */
            int marked = mark_basic_artificial_rows_redundant(tab, 1);
            if (marked > 0) {
                if (solver->verbose >= 2) {
                    fprintf(stderr,
                            "[simplex_phase1] Marked %d infeasible artificial rows as redundant after ratio-test breakdown\n",
                            marked);
                }
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    continue;
                }
            }

            int rescue_status = dual_simplex_phase1_rescue(
                solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
            if (rescue_status == 0) {
                if (solver->verbose) {
                    fprintf(stderr, "[simplex_phase1] Dual rescue recovered after ratio-test breakdown at iter %d\n", iter);
                }
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                ratio_breakdown_count = 0;
                continue;
            }

            ratio_breakdown_count++;
            phase1_exclude_entering_var(entering,
                                        RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                        &excluded_entering_a,
                                        &excluded_entering_ttl_a,
                                        &excluded_entering_b,
                                        &excluded_entering_ttl_b);
            if (ratio_breakdown_count < RALPH_PHASE1_RATIO_BREAKDOWN_LIMIT) {
                if (solver->verbose >= 2) {
                    fprintf(stderr,
                            "[simplex_phase1] Continuing after ratio-test breakdown (count=%d), excluding entering %d for %d iterations\n",
                            ratio_breakdown_count, entering, RALPH_PHASE1_ENTERING_EXCLUDE_ITERS);
                }
                use_bland = 1;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                continue;
            }

            if (solver->verbose) {
                fprintf(stderr, "[simplex_phase1] ERROR: unbounded in Phase 1 at iter %d (after recovery)\n", iter);
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
            double force_refactor_trigger =
                RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER * PHASE1_DIR_INF_FORCE_REFACTOR_MULT;
            int force_dir_refactor = (dir_inf > force_refactor_trigger) ||
                                     lu_needs_refactorization(tab->lu);
            if (dir_stabilize_cooldown > 0 && !force_dir_refactor) {
                if (solver->verbose >= 2) {
                    fprintf(stderr,
                            "[simplex_phase1] Large direction norm %.2e at iter %d (entering=%d), skipping direction-stabilize refactor (cooldown=%d)\n",
                            dir_inf, iter, entering, dir_stabilize_cooldown);
                }
                phase1_exclude_entering_var(entering,
                                            RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                            &excluded_entering_a,
                                            &excluded_entering_ttl_a,
                                            &excluded_entering_b,
                                            &excluded_entering_ttl_b);
                use_bland = 1;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                continue;
            }

            if (solver->verbose >= 2) {
                fprintf(stderr,
                        "[simplex_phase1] Large direction norm %.2e at iter %d (entering=%d), re-factorizing before pivot\n",
                        dir_inf, iter, entering);
            }
            int stabilized = 0;
            int original_entering = entering;
            for (int stab_try = 0; stab_try < 2; stab_try++) {
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_DIRECTION_STABILIZE) != 0) {
                    break;
                }
                dir_stabilize_cooldown = PHASE1_DIR_STABILIZE_COOLDOWN_ITERS;
                ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
                if (ratio_status != 0) {
                    phase1_trace_record_no_entering(solver, iter, ratio_status);
                    use_bland = 1;
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    continue;
                }

                dir_inf = vec_abs_max(tab->work2, tab->m);
                if (solver->verbose >= 2) {
                    fprintf(stderr,
                            "[simplex_phase1] Direction norm after re-factorization: %.2e\n",
                            dir_inf);
                }
                if (dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
                    stabilized = 1;
                    break;
                }

                if (pricing_bland_excluding(tab, original_entering, &entering) != 0) {
                    break;
                }
                ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
                if (ratio_status != 0) {
                    break;
                }
                dir_inf = vec_abs_max(tab->work2, tab->m);
                if (solver->verbose >= 2) {
                    fprintf(stderr,
                            "[simplex_phase1] Alternate entering %d direction norm: %.2e\n",
                            entering, dir_inf);
                }
                if (dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER) {
                    stabilized = 1;
                    break;
                }
            }

            if (!stabilized) {
                if (solver->verbose >= 2) {
                    fprintf(stderr,
                            "[simplex_phase1] Skipping unstable entering column after stabilization attempts (iter=%d, entering=%d, dir_inf=%.2e)\n",
                            iter, entering, dir_inf);
                }
                phase1_exclude_entering_var(entering,
                                            RALPH_PHASE1_ENTERING_EXCLUDE_ITERS,
                                            &excluded_entering_a,
                                            &excluded_entering_ttl_a,
                                            &excluded_entering_b,
                                            &excluded_entering_ttl_b);
                dir_stabilize_cooldown = PHASE1_DIR_STABILIZE_COOLDOWN_ITERS;
                use_bland = 1;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                continue;
            }
            dir_stabilize_cooldown = 0;
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
                    fprintf(stderr,
                            "[simplex_phase1] Using alternate leaving row %d instead of repeatedly failing row %d\n",
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
                    fprintf(stderr, "[simplex_phase1] Switching to Bland's rule after %d degenerate pivots\n",
                            degenerate_count);
                }
            }
        }

        /* Perform pivot */
        int pivot_status;
        {
            double t_pivot_ms = perf_now_ms();
            pivot_status = simplex_pivot(tab, entering, leaving, theta, fail_repeat_count);
            double pivot_elapsed_ms = perf_now_ms() - t_pivot_ms;
            solver->perf_pivot_ms += pivot_elapsed_ms;
            solver->perf_phase1_pivot_ms += pivot_elapsed_ms;
            solver->perf_phase1_pivot_calls++;
        }
        if (pivot_status != 0) {
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
                    fprintf(stderr,
                            "[simplex_phase1] Pivot failed at iter %d (repeat %d), attempting recovery\n",
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
                        fprintf(stderr,
                                "[simplex_phase1] Retrying with alternate leaving row %d (failed row %d)\n",
                                alt_leaving, leaving);
                    }
                    int alt_pivot_status;
                    {
                        double t_pivot_ms = perf_now_ms();
                        alt_pivot_status = simplex_pivot(tab, entering, alt_leaving, alt_theta, fail_repeat_count);
                        double pivot_elapsed_ms = perf_now_ms() - t_pivot_ms;
                        solver->perf_pivot_ms += pivot_elapsed_ms;
                        solver->perf_phase1_pivot_ms += pivot_elapsed_ms;
                        solver->perf_phase1_pivot_calls++;
                    }
                    if (alt_pivot_status == 0) {
                        fail_reason = PHASE1_PIVOT_FAIL_NONE;
                        fail_repeat_count = 0;
                        continue;
                    } else {
                        phase1_trace_record_pivot_failure(solver, tab, iter, fail_repeat_count);
                    }
                }
            }

            if (fail_repeat_count >= RALPH_PHASE1_FAIL_REPEAT_LIMIT) {
                if (solver->verbose) {
                    fprintf(stderr,
                            "[simplex_phase1] Repeated pivot failure (%d) for entering=%d leaving_pos=%d, terminating as ITERATION_LIMIT\n",
                            fail_repeat_count, entering, leaving);
                }
                primal_remove_perturbation(tab);
                solver->status = RALPH_STATUS_ITERATION_LIMIT;
                solver->iterations = iter;
                phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
                return -1;
            }

            /* simplex_pivot can leave basis/LU partially updated on failure.
             * Try the same recovery ladder used in Phase 2. */
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                continue;
            }
            if (repair_singular_basis(tab) == 0) {
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
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
                        fprintf(stderr,
                                "[simplex_phase1] Marking row %d as redundant due to stuck artificial %d\n",
                                leaving, leave_var);
                    }
                    if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                        tableau_compute_solution(tab);
                        tableau_compute_reduced_costs(tab);
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
                    fprintf(stderr,
                            "[simplex_phase1] Marked %d additional artificial rows as redundant for recovery\n",
                            marked);
                }
                if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY) == 0) {
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    continue;
                }
            }

            /* Final fallback for stuck Phase 1 states: try a bounded dual-simplex
             * rescue on the current tableau (no recursion to primal simplex). */
            int rescue_status = dual_simplex_phase1_rescue(solver, tab->m * RALPH_PHASE1_DUAL_RESCUE_MULT);
            if (rescue_status == 0) {
                if (solver->verbose) {
                    fprintf(stderr, "[simplex_phase1] Dual rescue restored feasibility progress at iter %d\n", iter);
                }
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                fail_reason = PHASE1_PIVOT_FAIL_NONE;
                fail_repeat_count = 0;
                continue;
            }

            if (solver->verbose) {
                fprintf(stderr, "[simplex_phase1] ERROR: all pivot recovery attempts failed at iter %d\n", iter);
            }
            primal_remove_perturbation(tab);
            /* Treat unrecoverable Phase 1 pivot breakdown as numerical breakdown. */
            solver->status = RALPH_STATUS_ITERATION_LIMIT;
            solver->iterations = iter;
            phase1_trace_emit_summary(solver, RALPH_STATUS_ITERATION_LIMIT);
            return -1;
        }
        fail_reason = PHASE1_PIVOT_FAIL_NONE;
        fail_repeat_count = 0;
        ratio_breakdown_count = 0;
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
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    if (solver->pricing_strategy == 4) heap_build(tab);
                    if (solver->verbose) {
                        fprintf(stderr,
                                "[simplex_phase1] Stall detected, re-perturbing (attempt %d, scale %.1f)\n",
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
        int lu_refactor_needed = lu_needs_refactorization(tab->lu);
        PeriodicRefactorPolicy periodic_policy =
            compute_periodic_refactor_policy(tab, 1, use_bland, degenerate_count);
        int periodic_refactor = (!lu_refactor_needed &&
                                 should_run_periodic_refactor(tab,
                                                              iter,
                                                              &periodic_policy,
                                                              use_bland,
                                                              degenerate_count));
        int needs_refactor = lu_refactor_needed || periodic_refactor;
        if (periodic_refactor) {
            periodic_feedback_set_hint(solver, 1, periodic_policy.interval, periodic_policy.run_pressure);
        }

        if (needs_refactor) {
            if (lu_refactor_needed) {
                solver->perf_refactor_periodic_lu_health++;
                solver->perf_phase1_refactor_periodic_lu_health++;
            } else if (periodic_refactor) {
                solver->perf_refactor_periodic_policy++;
                solver->perf_phase1_refactor_periodic_policy++;
            }
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC) != 0) {
                if (periodic_refactor) {
                    if (solver->verbose) {
                        fprintf(stderr,
                                "[simplex_phase1] Periodic refactorization failed at iter %d, continuing with existing LU\n",
                                iter);
                    }
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    continue;
                }

                if (solver->verbose) {
                    fprintf(stderr, "[simplex_phase1] Refactorization failed at iter %d, trying dual rescue\n", iter);
                }

                int marked = mark_basic_artificial_rows_redundant(tab, 1);
                if (marked > 0) {
                    if (solver->verbose >= 2) {
                        fprintf(stderr,
                                "[simplex_phase1] Marked %d infeasible artificial rows as redundant after refactorization failure\n",
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
                        fprintf(stderr, "[simplex_phase1] Dual rescue recovered after refactorization failure at iter %d\n", iter);
                    }
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    fail_reason = PHASE1_PIVOT_FAIL_NONE;
                    fail_repeat_count = 0;
                    continue;
                }

                if (repair_singular_basis(tab) == 0) {
                    if (solver->verbose) {
                        fprintf(stderr, "[simplex_phase1] Basis repair recovered after refactorization failure at iter %d\n", iter);
                    }
                    tableau_compute_solution(tab);
                    tableau_compute_reduced_costs(tab);
                    continue;
                }

                if (solver->verbose) {
                    fprintf(stderr, "[simplex_phase1] ERROR: all refactorization recoveries failed at iter %d\n", iter);
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
        } else if (iter > 0 && iter % RECOMPUTE_INTERVAL == 0) {
            /* Drift control even when LU updates are still accepted. */
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
            if (solver->pricing_strategy == 4) heap_build(tab);
        }
    }

    /* Iteration limit exceeded */
    primal_remove_perturbation(tab);
    if (solver->verbose) {
        fprintf(stderr, "[simplex_phase1] Iteration limit (%d) reached\n", solver->max_iterations);
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
        printf("[simplex_transition] Transitioning to Phase 2 (m=%d, num_art=%d, num_eq=%d)\n",
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
                        fprintf(stderr, "[simplex_transition] Pivoted out artificial %d with var %d (coef=%.2e)\n",
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
                    fprintf(stderr, "[simplex_transition] Warning: artificial var %d stuck in basis row %d "
                            "(orig constraint row %d, best_coef=%.2e, redundant row)\n",
                            art_j, basis_pos, orig_row, best_coef);
                }
            }
        }
    }

    free(artificial_to_row);
    artificial_to_row = NULL;

    if (solver->verbose) {
        fprintf(stderr, "[simplex_transition] %d artificial variables were in basis, %d stuck (%d redundant rows)\n",
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
            fprintf(stderr, "[simplex_transition] Zeroed %d redundant rows in A_ext and RHS\n",
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
            fprintf(stderr, "[simplex_transition] Refactorization failed, attempting basis repair...\n");
        }
        if (repair_singular_basis(tab) != 0) {
            if (solver->verbose) {
                fprintf(stderr, "[simplex_transition] ERROR: basis repair failed during transition\n");
            }
            return -1;
        }
    }

    /* Recompute reduced costs with new objective */
    tableau_compute_reduced_costs(tab);

    /* Recompute solution */
    tableau_compute_solution(tab);


    if (solver->verbose) {
        fprintf(stderr, "[simplex_transition] Phase 2 objective value: %g\n", tab->obj_value);
    }

    return 0;
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
            double t_refactor_ms = perf_now_ms();
            int rc = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PHASE_TRANSITION);
            solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
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

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tab->iterations = iter;

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
        double t_pricing_ms = perf_now_ms();
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
            double pricing_elapsed_ms = perf_now_ms() - t_pricing_ms;
            solver->perf_pricing_ms += pricing_elapsed_ms;
            solver->perf_phase2_pricing_ms += pricing_elapsed_ms;
        }
        solver->perf_phase2_pricing_calls++;

        if (price_status != 0) {
            /* Optimal - remove perturbation and finalize */
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_OPTIMAL;
            solver->iterations = iter;
            solver->degenerate_pivots = degenerate_count;
            tableau_compute_solution(tab);

            solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
            return 0;
        }

        /* Ratio test: select leaving variable */
        int leaving;
        double theta;
        int ratio_status;
        double t_ratio_ms = perf_now_ms();

        if (use_bland) {
            ratio_status = ratio_test_bland(tab, entering, &leaving, &theta);
        } else {
            ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
        }
        {
            double ratio_elapsed_ms = perf_now_ms() - t_ratio_ms;
            solver->perf_ratio_ms += ratio_elapsed_ms;
            solver->perf_phase2_ratio_ms += ratio_elapsed_ms;
        }
        solver->perf_phase2_ratio_calls++;

        if (ratio_status != 0) {
            /* No leaving variable found — possibly unbounded.
             * Stale LU factors can produce spurious theta=inf (e.g., lotfi).
             * Refactorize and retry once before declaring UNBOUNDED. */
            int refactor_rc;
            {
                double t_refactor_ms = perf_now_ms();
                refactor_rc = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
                solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
            }
            if (refactor_rc == 0) {
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                if (solver->pricing_strategy == 4) heap_build(tab);

                /* Re-price: the entering variable may no longer be eligible */
                t_pricing_ms = perf_now_ms();
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
                    double pricing_elapsed_ms = perf_now_ms() - t_pricing_ms;
                    solver->perf_pricing_ms += pricing_elapsed_ms;
                    solver->perf_phase2_pricing_ms += pricing_elapsed_ms;
                }
                solver->perf_phase2_pricing_calls++;

                if (price_status != 0) {
                    /* Actually optimal after refactorization */
                    primal_remove_perturbation(tab);
                    solver->status = RALPH_STATUS_OPTIMAL;
                    solver->iterations = iter;
                    solver->degenerate_pivots = degenerate_count;
                    tableau_compute_solution(tab);
                    solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
                    return 0;
                }

                /* Retry ratio test with fresh LU */
                t_ratio_ms = perf_now_ms();
                if (use_bland) {
                    ratio_status = ratio_test_bland(tab, entering, &leaving, &theta);
                } else {
                    ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
                }
                {
                    double ratio_elapsed_ms = perf_now_ms() - t_ratio_ms;
                    solver->perf_ratio_ms += ratio_elapsed_ms;
                    solver->perf_phase2_ratio_ms += ratio_elapsed_ms;
                }
                solver->perf_phase2_ratio_calls++;
            }

            if (ratio_status != 0) {
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
                    printf("Iter %d: Applying perturbation due to degeneracy\n", iter);
                }
            }

            /* If still cycling after perturbation, use Bland's rule */
            if (degenerate_count >= DEGEN_THRESHOLD && !use_bland) {
                use_bland = 1;
                if (solver->verbose) {
                    printf("Iter %d: Switching to Bland's rule due to potential cycling\n", iter);
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
                    printf("Iter %d: Turning off Bland's rule after %d non-degenerate pivots\n",
                           iter, NON_DEGEN_THRESHOLD);
                }
            }
        }
        }  /* end degeneracy tracking block */

        /* Perform pivot */
        int pivot_rc;
        {
            double t_pivot_ms = perf_now_ms();
            pivot_rc = simplex_pivot(tab, entering, leaving, theta, 0);
            double pivot_elapsed_ms = perf_now_ms() - t_pivot_ms;
            solver->perf_pivot_ms += pivot_elapsed_ms;
            solver->perf_phase2_pivot_ms += pivot_elapsed_ms;
            solver->perf_phase2_pivot_calls++;
        }
        if (pivot_rc != 0) {
            if (solver->verbose) {
                fprintf(stderr, "[primal_simplex] Pivot failed at iter %d (entering=%d, leaving=%d, theta=%e), attempting recovery\n",
                        iter, entering, leaving, theta);
            }
            /* Pivot failed - the basis was partially updated in simplex_pivot.
             * Try to recover by refactorizing the current (post-pivot) basis. */
            int rc_refactor;
            {
                double t_refactor_ms = perf_now_ms();
                rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PIVOT_RECOVERY);
                solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
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
                    fprintf(stderr, "[primal_simplex] Recovery via refactorization at iter %d\n", iter);
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
                    fprintf(stderr, "[primal_simplex] Recovery via basis repair at iter %d\n", iter);
                }
                continue;
            }
            /* All recovery attempts failed */
            if (solver->verbose) {
                fprintf(stderr, "[primal_simplex] ERROR: all recovery attempts failed at iter %d\n", iter);
            }
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }

        /* Refactorize if needed.
         * For two-phase problems, periodic refresh is adaptive (interval + LU health). */
        int lu_refactor_needed = lu_needs_refactorization(tab->lu);
        int needs_refactor = lu_refactor_needed;
        int periodic_refactor = 0;
        PeriodicRefactorPolicy periodic_policy = {0, 0, 0.0, 0.0};
        if (!needs_refactor) {
            periodic_policy = compute_periodic_refactor_policy(tab, 2, use_bland, degenerate_count);
            periodic_refactor = should_run_periodic_refactor(tab,
                                                             iter,
                                                             &periodic_policy,
                                                             use_bland,
                                                             degenerate_count);
            needs_refactor = periodic_refactor;
        }
        if (periodic_refactor) {
            periodic_feedback_set_hint(solver, 2, periodic_policy.interval, periodic_policy.run_pressure);
        }

        if (needs_refactor) {
            if (lu_refactor_needed) {
                solver->perf_refactor_periodic_lu_health++;
                solver->perf_phase2_refactor_periodic_lu_health++;
            } else if (periodic_refactor) {
                solver->perf_refactor_periodic_policy++;
                solver->perf_phase2_refactor_periodic_policy++;
            }
            int rc_refactor;
            {
                double t_refactor_ms = perf_now_ms();
                rc_refactor = tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PERIODIC);
                solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
            }
            if (rc_refactor != 0) {
                if (solver->verbose) {
                    fprintf(stderr, "[primal_simplex] ERROR: refactorization failed at iter %d, attempting repair\n", iter);
                }
                /* Try to repair the singular basis */
                if (repair_singular_basis(tab) != 0) {
                    if (solver->verbose) {
                        fprintf(stderr, "[primal_simplex] ERROR: basis repair failed at iter %d\n", iter);
                    }
                    primal_remove_perturbation(tab);
                    solver->status = RALPH_STATUS_ERROR;
                    solver->iterations = iter;
                    return -1;
                }
                if (solver->verbose) {
                    fprintf(stderr, "[primal_simplex] Basis repaired at iter %d\n", iter);
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
        }

        /* Periodically recompute solution and reduced costs to correct numerical drift */
        if (iter > 0 && iter % 200 == 0) {
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
                printf("Iter %d: obj = %.6f, enter=%d, leave=%d, theta=%.2e, rc=%.2e\n",
                       iter, tab->obj_value, entering, leave_var, theta, tab->rc[entering]);
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
                        printf("Iter %d: Phase 2 stall detected, re-perturbing (attempt %d, scale %.1f)\n",
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
        printf("[crash] Placed %d structural columns in basis (of %d rows)\n",
               placed, m);
    }

    return placed;
}

static void reset_solver_perf(SimplexSolver *solver) {
    if (!solver) return;
    solver->perf_primal_setup_ms = 0.0;
    solver->perf_dual_ms = 0.0;
    solver->perf_phase1_ms = 0.0;
    solver->perf_transition_ms = 0.0;
    solver->perf_phase2_ms = 0.0;
    solver->perf_pricing_ms = 0.0;
    solver->perf_ratio_ms = 0.0;
    solver->perf_pivot_ms = 0.0;
    solver->perf_refactor_ms = 0.0;
    solver->perf_ftran_ms = 0.0;
    solver->perf_btran_ms = 0.0;
    solver->perf_lu_update_ms = 0.0;
    solver->perf_compute_solution_ms = 0.0;
    solver->perf_compute_rc_ms = 0.0;
    solver->perf_refactor_all_ms = 0.0;
    solver->perf_refactor_count = 0;
    solver->perf_refactor_last_ms = 0.0;
    solver->perf_refactor_max_ms = 0.0;
    solver->perf_refactor_last_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->perf_refactor_next_reason = RALPH_REFACTOR_REASON_OTHER;
    solver->perf_refactor_reason_setup = 0;
    solver->perf_refactor_reason_transition = 0;
    solver->perf_refactor_reason_periodic = 0;
    solver->perf_refactor_reason_ratio_recovery = 0;
    solver->perf_refactor_reason_pivot_recovery = 0;
    solver->perf_refactor_reason_forced_small_pivot = 0;
    solver->perf_refactor_reason_update_recovery = 0;
    solver->perf_refactor_reason_direction_stabilize = 0;
    solver->perf_refactor_reason_infeas_cleanup = 0;
    solver->perf_refactor_reason_other = 0;
    solver->perf_refactor_periodic_policy = 0;
    solver->perf_refactor_periodic_lu_health = 0;
    solver->perf_refactor_last_m = 0;
    solver->perf_refactor_last_k = 0;
    solver->perf_refactor_last_nnz_B = 0;

    solver->perf_phase1_pricing_ms = 0.0;
    solver->perf_phase1_ratio_ms = 0.0;
    solver->perf_phase1_pivot_ms = 0.0;
    solver->perf_phase1_refactor_ms = 0.0;
    solver->perf_phase1_compute_solution_ms = 0.0;
    solver->perf_phase1_compute_rc_ms = 0.0;
    solver->perf_phase1_pricing_calls = 0;
    solver->perf_phase1_ratio_calls = 0;
    solver->perf_phase1_pivot_calls = 0;
    solver->perf_phase1_refactor_calls = 0;
    solver->perf_phase1_compute_solution_calls = 0;
    solver->perf_phase1_compute_rc_calls = 0;
    solver->perf_phase1_refactor_periodic_policy = 0;
    solver->perf_phase1_refactor_periodic_lu_health = 0;

    solver->perf_phase2_pricing_ms = 0.0;
    solver->perf_phase2_ratio_ms = 0.0;
    solver->perf_phase2_pivot_ms = 0.0;
    solver->perf_phase2_refactor_ms = 0.0;
    solver->perf_phase2_compute_solution_ms = 0.0;
    solver->perf_phase2_compute_rc_ms = 0.0;
    solver->perf_phase2_pricing_calls = 0;
    solver->perf_phase2_ratio_calls = 0;
    solver->perf_phase2_pivot_calls = 0;
    solver->perf_phase2_refactor_calls = 0;
    solver->perf_phase2_compute_solution_calls = 0;
    solver->perf_phase2_compute_rc_calls = 0;
    solver->perf_phase2_refactor_periodic_policy = 0;
    solver->perf_phase2_refactor_periodic_lu_health = 0;

    solver->periodic_feedback_bias_phase1 = 0.0;
    solver->periodic_feedback_bias_phase2 = 0.0;
    solver->periodic_feedback_last_reason_phase1 = RALPH_REFACTOR_REASON_OTHER;
    solver->periodic_feedback_last_reason_phase2 = RALPH_REFACTOR_REASON_OTHER;
    solver->periodic_feedback_last_interval_phase1 = 0;
    solver->periodic_feedback_last_interval_phase2 = 0;
    solver->periodic_feedback_hint_interval_phase1 = 0;
    solver->periodic_feedback_hint_interval_phase2 = 0;
    solver->periodic_feedback_hint_pressure_phase1 = 0.0;
    solver->periodic_feedback_hint_pressure_phase2 = 0.0;
}

static void configure_tableau_for_solver(SimplexSolver *solver, SimplexTableau *tab) {
    if (!solver || !tab) return;

    tab->owner = solver;
    tab->use_steepest_edge = (solver->pricing_strategy == 1 || solver->pricing_strategy == 2
                              || solver->pricing_strategy == 5);
    tab->pricing_strategy = solver->pricing_strategy;
    tab->trace_phase1_enabled = solver->trace_phase1;
    if (tab->lu) {
        if (solver->lu_supernode)
            tab->lu->sn_enabled = 1;
        else if (tab->m > 300)
            tab->lu->sn_enabled = 1;
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

    if (solver->verbose) printf("[simplex_solve] Creating tableau...\n");
    solver->tableau = tableau_create_ex(solver->model, solver->force_two_phase, 0);
    if (!solver->tableau) {
        solver->status = RALPH_STATUS_ERROR;
        return -1;
    }

    SimplexTableau *tab = solver->tableau;
    configure_tableau_for_solver(solver, tab);

    if (solver->verbose) {
        printf("[simplex_solve] Tableau: n=%d (extended), m=%d\n", tab->n, tab->m);
    }

    int *saved_basis = NULL;
    int *saved_basis_pos = NULL;
    VarStatus *saved_var_status = NULL;

    if (allow_crash) {
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

    if (solver->verbose) printf("[simplex_solve] Factorizing initial basis...\n");
    double t_refactor_ms = perf_now_ms();
    int factorize_ok = (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_SETUP) == 0);
    solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;

    if (!factorize_ok && allow_crash && saved_basis) {
        if (solver->verbose)
            printf("[simplex_solve] Crash basis singular, restoring original basis\n");
        memcpy(tab->basis, saved_basis, tab->m * sizeof(int));
        memcpy(tab->basis_pos, saved_basis_pos, tab->n * sizeof(int));
        memcpy(tab->var_status, saved_var_status, tab->n * sizeof(VarStatus));
        for (int j = 0; j < tab->n; j++)
            tab->x[j] = tab->lb_ext[j];
        t_refactor_ms = perf_now_ms();
        factorize_ok = (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_SETUP) == 0);
        solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
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
                    printf("[crash] Basic var %d in row %d: x=%.6e outside [%.6e, %.6e], reverting\n",
                           bv, i, val, tab->lb_ext[bv], tab->ub_ext[bv]);
                break;
            }
        }
        if (crash_infeasible) {
            if (solver->verbose)
                printf("[crash] Post-verify failed, restoring original basis\n");
            memcpy(tab->basis, saved_basis, tab->m * sizeof(int));
            memcpy(tab->basis_pos, saved_basis_pos, tab->n * sizeof(int));
            memcpy(tab->var_status, saved_var_status, tab->n * sizeof(VarStatus));
            for (int j = 0; j < tab->n; j++)
                tab->x[j] = tab->lb_ext[j];
            t_refactor_ms = perf_now_ms();
            if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_SETUP) != 0) {
                solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
                free(saved_basis);
                free(saved_basis_pos);
                free(saved_var_status);
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            solver->perf_refactor_ms += perf_now_ms() - t_refactor_ms;
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        }
    }

    free(saved_basis);
    free(saved_basis_pos);
    free(saved_var_status);

    if (solver->verbose) printf("[simplex_solve] Initial factorization OK\n");
    return 0;
}

int simplex_solve(SimplexSolver *solver) {
    if (!solver || !solver->model) return -1;

    clock_t start = clock();

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

    if (solver->verbose) printf("[simplex_solve] Starting...\n");

    /* Finalize model if needed (required before scaling) */
    if (!solver->model->A) {
        if (solver->verbose) printf("[simplex_solve] Finalizing model...\n");
        if (lp_model_finalize(solver->model) != 0) {
            if (solver->verbose) printf("[simplex_solve] ERROR: lp_model_finalize failed\n");
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }
    }

    if (solver->verbose) {
        printf("[simplex_solve] Model: %d vars, %d cons, %d nnz\n",
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
            printf("[simplex_solve] Trying dual simplex path (method=%d)\n", solver->method);

        double t_dual_ms = perf_now_ms();
        int drc = dual_simplex_solve_from_scratch_v2(solver);
        solver->perf_dual_ms += perf_now_ms() - t_dual_ms;

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
                        printf("[simplex_solve] Dual solution failed Ax=b check, falling back to primal\n");
                    drc = -1;
                }
            } else {
                /* Auto mode: don't trust dual INFEASIBLE/OBJ_LIMIT — fall back.
                 * Dual infeasibility detection is unreliable; primal Phase 1 is robust. */
                if (solver->verbose)
                    printf("[simplex_solve] Dual returned non-optimal status %d, falling back to primal\n",
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
                    printf("[simplex_solve] Dual solution imprecise, falling back to primal\n");
                solver->status = RALPH_STATUS_UNKNOWN;
                drc = -1;  /* Trigger primal fallback below */
            } else {
                return 0;
            }
        }

        /* Dual failed or rejected — fall back to primal (method=2) or error (method=1) */
        if (solver->method == 2) {
            solver->from_dual_fallback = 1;
            if (solver->verbose)
                printf("[simplex_solve] Falling back to primal\n");

            if (solver->tableau) {
                tableau_free(solver->tableau);
                solver->tableau = NULL;
            }

            double t_setup_ms = perf_now_ms();
            if (setup_primal_tableau(solver, solver->crash) != 0) {
                return -1;
            }
            solver->perf_primal_setup_ms += perf_now_ms() - t_setup_ms;
            tab = solver->tableau;
        } else {
            solver->status = RALPH_STATUS_ERROR;
            solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            return -1;
        }
    } else {
        double t_setup_ms = perf_now_ms();
        if (setup_primal_tableau(solver, solver->crash && solver->method == 0) != 0) {
            return -1;
        }
        solver->perf_primal_setup_ms += perf_now_ms() - t_setup_ms;
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
    if (solver->verbose) printf("[simplex_solve] Starting Phase 1...\n");
    {
        double t_phase1_ms = perf_now_ms();
        if (simplex_phase1(solver) != 0) {
            solver->perf_phase1_ms += perf_now_ms() - t_phase1_ms;
            solver->pricing_strategy = saved_pricing;  /* T3.4: restore pricing */
            tab->pricing_strategy = saved_tab_pricing;
            tab->use_steepest_edge = saved_tab_se;
            if (solver->status == RALPH_STATUS_INFEASIBLE) {
                if (solver->verbose) printf("[simplex_solve] Phase 1: INFEASIBLE\n");
                return 0;  /* Infeasible is a valid result */
            }
            return -1;
        }
        solver->perf_phase1_ms += perf_now_ms() - t_phase1_ms;
    }
    solver->pricing_strategy = saved_pricing;  /* T3.4: restore pricing for Phase 2 */
    tab->pricing_strategy = saved_tab_pricing;
    tab->use_steepest_edge = saved_tab_se;
    if (solver->verbose) printf("[simplex_solve] Phase 1 complete\n");

    /* Transition to Phase 2 if using two-phase simplex */
    int two_phase_failed = 0;
    if (tab->use_two_phase) {
        if (solver->verbose) printf("[simplex_solve] Transitioning to Phase 2...\n");
        double t_transition_ms = perf_now_ms();
        if (simplex_transition_phase2(solver) != 0) {
            solver->perf_transition_ms += perf_now_ms() - t_transition_ms;
            two_phase_failed = 1;
        } else {
            solver->perf_transition_ms += perf_now_ms() - t_transition_ms;
            if (solver->verbose) printf("[simplex_solve] Phase 2 transition complete\n");
        }
    }

    /* Phase 2: Optimize (skip if transition failed) */
    if (!two_phase_failed) {
        double t_phase2_ms = perf_now_ms();
        int status = simplex_phase2(solver);
        solver->perf_phase2_ms += perf_now_ms() - t_phase2_ms;
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
            printf("[simplex_solve] Two-phase failed, trying dual simplex\n");
        }
        tableau_free(solver->tableau);
        solver->tableau = NULL;
        restore_model(solver);
        solver->is_scaled = 0;
        solver->status = RALPH_STATUS_UNKNOWN;
        double t_dual_ms = perf_now_ms();
        int dual_result = dual_simplex_solve_from_scratch_v2(solver);
        solver->perf_dual_ms += perf_now_ms() - t_dual_ms;
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

    /* Restore original model if scaling was applied */
    restore_model(solver);

    /* Post-solve verification (T2.3 + T3.6) — runs on original-space solution */
    if (solver->verify && solver->status == RALPH_STATUS_OPTIMAL) {
        verify_solution(solver);
    }

    return (solver->status == RALPH_STATUS_OPTIMAL) ? 0 : -1;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

void lp_print_stats(const SimplexSolver *solver) {
    if (!solver) return;

    printf("\n=== Simplex Statistics ===\n");
    printf("Status: %d\n", solver->status);
    printf("Iterations: %d\n", solver->iterations);
    printf("Solve time: %.3f seconds\n", solver->solve_time);

    if (solver->status == RALPH_STATUS_OPTIMAL) {
        printf("Objective: %.10f\n", solver->obj_value);
    }
}
