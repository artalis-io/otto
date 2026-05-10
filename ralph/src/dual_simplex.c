/*
 * Ralph - Dual Simplex Method Implementation
 *
 * The dual simplex maintains dual feasibility (optimality conditions)
 * while working to achieve primal feasibility. Essential for:
 * - Re-optimization after adding cuts
 * - Re-optimization after fixing variables (in MIP)
 * - Starting from dual feasible basis
 * - Fresh solves on problems where dual start is beneficial
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "lp.h"
#include "lp_glpk_strict.h"
#include "lu_update_backend.h"
#include "lp_log.h"
#include "lp_policy_glpk_compat.h"

/* Forward declarations */
SimplexTableau* tableau_create(LPModel *model);
SimplexTableau* tableau_create_dual(LPModel *model);
int tableau_refactorize(SimplexTableau *tab);
int tableau_compute_solution(SimplexTableau *tab);
int tableau_compute_reduced_costs(SimplexTableau *tab);
void tableau_free(SimplexTableau *tab);

static void phase1_rescue_compute_solution(SimplexTableau *tab) {
    tab->phase1_compute_solution_context = LP_PHASE1_COMPUTE_CTX_DUAL_RESCUE;
    tableau_compute_solution(tab);
}

static void phase1_rescue_compute_reduced_costs(SimplexTableau *tab) {
    tab->phase1_compute_rc_context = LP_PHASE1_COMPUTE_CTX_DUAL_RESCUE;
    tableau_compute_reduced_costs(tab);
}

/* Dual candidate-list pricing constants (T2.2) */
#define DUAL_CAND_CAPACITY    200    /* Max candidates in dual hot set */
#define DUAL_CAND_RC_THRESH   1e-4   /* |rc| threshold for candidate inclusion */
#define DUAL_REINVERT_HARD_BURST_WINDOW_ITERS 64
#define DUAL_REINVERT_HARD_BURST_DEMOTE_COUNT 6
#define DUAL_PHASE1_RESCUE_PROGRESS_LIMIT 16

/* Bound perturbation for degeneracy prevention (defined below) */
static void apply_bound_perturbation(SimplexTableau *tab);
static void remove_bound_perturbation(SimplexTableau *tab);
static void dse_init_approx(SimplexTableau *tab);

/* Forward declaration for dual feasibility function (non-static for simplex.c access) */
int make_dual_feasible(SimplexTableau *tab, int obj_sense, int allow_bound_flip);
static int dual_smcp_excl_skip_var(const SimplexTableau *tab, int var);

typedef struct {
    const SimplexTableau *tab;
    int smcp_excl;
    double fixed_width_tol;
} DualWorkingExclusionCtx;

static DualWorkingExclusionCtx dual_working_exclusion_ctx(const SimplexTableau *tab) {
    int smcp_excl = 1;
    int smcp_shift = 1;
    double tol_bnd = 1e-12;
    if (tab && tab->owner) {
        smcp_excl = tab->owner->smcp_excl;
        smcp_shift = tab->owner->smcp_shift;
        tol_bnd = tab->owner->smcp_tol_bnd;
    }
    return (DualWorkingExclusionCtx) {
        tab,
        smcp_excl,
        lp_policy_glpk_working_fixed_width_tol(smcp_shift, tol_bnd)
    };
}

static inline int dual_working_exclusion_skip_var(const DualWorkingExclusionCtx *ctx,
                                                  int var) {
    const SimplexTableau *tab = ctx ? ctx->tab : NULL;
    VarStatus st;
    if (!tab || var < 0 || var >= tab->n) return 0;
    st = tab->var_status[var];
    if (tab->model && tab->free_split_col && tab->free_split_orig) {
        int mate = -1;
        if (var < tab->model->num_vars) {
            mate = tab->free_split_col[var];
        } else if (var < tab->num_structural_ext) {
            mate = tab->free_split_orig[var];
        }
        if (mate >= 0 && mate < tab->n && tab->var_status[mate] == RALPH_BASIC) {
            return 1;
        }
    }
    if (st == RALPH_FIXED) return 1;
    if (ctx->smcp_excl == LP_GLPK_SMCP_EXCL_OFF) return 0;
    if (st != RALPH_NONBASIC_LOWER && st != RALPH_NONBASIC_UPPER) return 0;
    if (tab->lb_ext[var] <= -RALPH_INFINITY / 2.0 ||
        tab->ub_ext[var] >= RALPH_INFINITY / 2.0) {
        return 0;
    }
    return fabs(tab->ub_ext[var] - tab->lb_ext[var]) <= ctx->fixed_width_tol;
}

static double dual_max_reduced_cost_violation(const SimplexTableau *tab) {
    double max_viol = 0.0;
    DualWorkingExclusionCtx excl_ctx;

    if (!tab) return RALPH_INFINITY;
    excl_ctx = dual_working_exclusion_ctx(tab);
    for (int j = 0; j < tab->n; j++) {
        double viol = 0.0;
        if (tab->var_status[j] == RALPH_BASIC) continue;
        if (dual_working_exclusion_skip_var(&excl_ctx, j)) continue;
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER &&
            tab->rc[j] < -RALPH_OPT_TOL) {
            viol = -tab->rc[j];
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER &&
                   tab->rc[j] > RALPH_OPT_TOL) {
            viol = tab->rc[j];
        } else if (tab->var_status[j] == RALPH_NONBASIC_FREE &&
                   fabs(tab->rc[j]) > RALPH_OPT_TOL) {
            viol = fabs(tab->rc[j]);
        }
        if (viol > max_viol) max_viol = viol;
    }
    return max_viol;
}

static void configure_dual_tableau_for_solver(SimplexSolver *solver, SimplexTableau *tab) {
    int update_cap = 0;

    if (!solver || !tab) return;
    tab->owner = solver;

    if (!tab->lu) return;

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

/* Dual refactor quality signals (GLPK-like control intent):
 * - basis age since last reinversion
 * - degeneracy/stall pressure
 * - repeated ratio/pivot pathology streaks
 */
typedef struct {
    int last_refactor_iter;
    int ratio_fail_streak;
    int theta_nonpos_streak;
    int pivot_fail_streak;
    int flip_only_streak;
} DualRefactorQualityState;

#define DUAL_QUALITY_RATIO_FAIL_REFACTOR_STREAK 2
#define DUAL_QUALITY_PIVOT_FAIL_REFACTOR_STREAK 2
#define DUAL_QUALITY_THETA_NONPOS_REFACTOR_STREAK 8
#define DUAL_QUALITY_FLIP_ONLY_REFACTOR_STREAK 8

static void dual_quality_init(DualRefactorQualityState *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

static void dual_quality_on_refactor(DualRefactorQualityState *state, int iter) {
    if (!state) return;
    state->last_refactor_iter = iter;
    state->ratio_fail_streak = 0;
    state->theta_nonpos_streak = 0;
    state->pivot_fail_streak = 0;
    state->flip_only_streak = 0;
}

static void dual_quality_record_ratio_failure(DualRefactorQualityState *state) {
    if (!state) return;
    state->ratio_fail_streak++;
    state->theta_nonpos_streak = 0;
    state->flip_only_streak = 0;
}

static void dual_quality_record_ratio_success(DualRefactorQualityState *state,
                                              int entering,
                                              double theta) {
    if (!state) return;
    state->ratio_fail_streak = 0;
    if (entering == -2) {
        state->flip_only_streak++;
    } else {
        state->flip_only_streak = 0;
    }
    if (entering >= 0 && theta <= 0.0) {
        state->theta_nonpos_streak++;
    } else {
        state->theta_nonpos_streak = 0;
    }
    if (entering >= 0 && theta > 0.0 && state->pivot_fail_streak > 0) {
        state->pivot_fail_streak--;
    }
}

static void dual_quality_record_pivot_failure(DualRefactorQualityState *state) {
    if (!state) return;
    state->pivot_fail_streak++;
}

static int dual_quality_periodic_refactor_signal(
    const SimplexTableau *tab,
    const DualRefactorQualityState *state,
    int iter,
    int base_interval,
    int degenerate_count,
    int stall_count,
    int perturb_attempts) {
    int interval = base_interval;
    int age;
    int m = tab ? tab->m : 0;

    if (!state) return 0;

    /* Large systems pay more per refactor; prefer longer nominal cadence. */
    if (m >= 2000 && interval > 36) interval = 36;
    else if (m >= 1200 && interval > 40) interval = 40;
    else if (m >= 600 && interval > 45) interval = 45;

    /* Tighten cadence when quality pressure rises. */
    if (degenerate_count >= 40 && interval > 20) interval /= 2;
    else if (degenerate_count >= 20 && interval > 30) interval = (2 * interval) / 3;
    if (stall_count >= 20 && interval > 16) interval = 16;
    if (perturb_attempts >= 2 && interval > 20) interval = 20;
    if (interval < 8) interval = 8;

    /* Pathology streaks override nominal cadence. */
    if (state->ratio_fail_streak >= DUAL_QUALITY_RATIO_FAIL_REFACTOR_STREAK ||
        state->pivot_fail_streak >= DUAL_QUALITY_PIVOT_FAIL_REFACTOR_STREAK ||
        state->theta_nonpos_streak >= DUAL_QUALITY_THETA_NONPOS_REFACTOR_STREAK ||
        state->flip_only_streak >= DUAL_QUALITY_FLIP_ONLY_REFACTOR_STREAK) {
        return 1;
    }

    age = iter - state->last_refactor_iter;
    return age >= interval;
}

static double dual_average_solve_density(long long sol_nnz_total, int samples, int m) {
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

static void dual_reinvert_hard_trigger_safety_step_core(int iter,
                                                         int hard_trigger_total,
                                                         int *last_total_io,
                                                         int *last_iter_io,
                                                         int *burst_io,
                                                         int *demoted_io) {
    int last_total;
    int last_iter;
    int burst;
    int demoted;
    int delta;

    if (!last_total_io || !last_iter_io || !burst_io || !demoted_io) return;

    last_total = *last_total_io;
    last_iter = *last_iter_io;
    burst = *burst_io;
    demoted = *demoted_io;

    if (iter < 0) iter = 0;
    if (hard_trigger_total < 0) hard_trigger_total = 0;
    if (last_total < 0) last_total = 0;
    if (burst < 0) burst = 0;

    if (hard_trigger_total < last_total) {
        last_total = hard_trigger_total;
        last_iter = -1;
        burst = 0;
    }

    delta = hard_trigger_total - last_total;
    if (delta > 0) {
        if (last_iter >= 0 && (iter - last_iter) <= DUAL_REINVERT_HARD_BURST_WINDOW_ITERS) {
            burst += delta;
        } else {
            burst = delta;
        }
        last_total = hard_trigger_total;
        last_iter = iter;
    } else if (last_iter >= 0 && (iter - last_iter) > DUAL_REINVERT_HARD_BURST_WINDOW_ITERS) {
        burst = 0;
    }

    if (burst >= DUAL_REINVERT_HARD_BURST_DEMOTE_COUNT) {
        demoted = 1;
    }

    *last_total_io = last_total;
    *last_iter_io = last_iter;
    *burst_io = burst;
    *demoted_io = demoted ? 1 : 0;
}

void dual_reinvert_hard_trigger_safety_step_for_test(int iter,
                                                      int hard_trigger_total,
                                                      int *last_total_io,
                                                      int *last_iter_io,
                                                      int *burst_io,
                                                      int *demoted_io) {
    dual_reinvert_hard_trigger_safety_step_core(iter,
                                                hard_trigger_total,
                                                last_total_io,
                                                last_iter_io,
                                                burst_io,
                                                demoted_io);
}

int dual_reinvert_effective_mode_for_test(int configured_mode, int demoted) {
    if (configured_mode == LP_REINVERT_MODE_CONTROL_ALL && demoted) {
        return LP_REINVERT_MODE_SHADOW;
    }
    return configured_mode;
}

static int dual_governor_refactor_decision(
    SimplexSolver *solver,
    SimplexTableau *tab,
    const DualRefactorQualityState *quality,
    int iter,
    int base_interval,
    int degenerate_count,
    int stall_count,
    int perturb_attempts) {
    int lu_refactor_needed;
    int quality_refactor;
    int quality_refactor_nominal;
    int need_refactor;
    int shadow_refactor;
    int governed_refactor;
    int reinvert_mode;
    int reinvert_mode_effective;
    int reinvert_active = 0;
    LPReinvertControllerSignals reinvert_signals;
    LPReinvertControllerDecision reinvert_shadow;
    int reinvert_suggested_refactor;
    LPReinvertControllerState *reinvert_state;
    double dual_hot_ms;
    double dual_iter_hot_ms;
    double ftran_density;
    double btran_density;

    if (!solver || !tab || !tab->lu || !quality) return 0;

    lu_refactor_needed = lu_needs_refactorization(tab->lu);
    quality_refactor = dual_quality_periodic_refactor_signal(tab,
                                                             quality,
                                                             iter,
                                                             base_interval,
                                                             degenerate_count,
                                                             stall_count,
                                                             perturb_attempts);
    quality_refactor_nominal = quality_refactor;

    reinvert_mode = solver->policy.reinvert_controller_mode;
    if (!lp_reinvert_controller_mode_is_valid(reinvert_mode)) {
        reinvert_mode = LP_REINVERT_MODE_SHADOW;
    }
    if (reinvert_mode == LP_REINVERT_MODE_CONTROL_ALL) {
        int demoted_before = solver->policy.reinvert_dual.control_demoted;
        dual_reinvert_hard_trigger_safety_step_core(
            iter,
            solver->telemetry.perf_dual_lu_hard_trigger,
            &solver->policy.reinvert_dual.hard_trigger_last_total,
            &solver->policy.reinvert_dual.hard_trigger_last_iter,
            &solver->policy.reinvert_dual.hard_trigger_burst,
            &solver->policy.reinvert_dual.control_demoted);
        if (!demoted_before && solver->policy.reinvert_dual.control_demoted) {
            solver->policy.reinvert_dual.control_demotions++;
        }
    }
    reinvert_mode_effective =
        dual_reinvert_effective_mode_for_test(reinvert_mode,
                                              solver->policy.reinvert_dual.control_demoted);
    if (reinvert_mode_effective != LP_REINVERT_MODE_OFF) {
        reinvert_active = 1;
        reinvert_state = &solver->policy.reinvert_state_dual;
        dual_hot_ms = solver->telemetry.perf_ratio_ms +
                      solver->telemetry.perf_pivot_ms +
                      solver->telemetry.perf_compute_solution_ms +
                      solver->telemetry.perf_compute_rc_ms;
        dual_iter_hot_ms = dual_hot_ms - solver->policy.reinvert_dual.last_hot_ms;
        solver->policy.reinvert_dual.last_hot_ms = dual_hot_ms;
        lp_reinvert_controller_state_record_iter_cost(reinvert_state, dual_iter_hot_ms);
        lp_reinvert_controller_state_record_update_age_ratio(reinvert_state,
                                                             lu_get_num_updates(tab->lu),
                                                             lu_get_max_updates(tab->lu));
        ftran_density = dual_average_solve_density(solver->telemetry.perf_ftran_sol_nnz_total,
                                                   solver->telemetry.perf_ftran_nnz_samples,
                                                   tab->m);
        btran_density = dual_average_solve_density(solver->telemetry.perf_btran_sol_nnz_total,
                                                   solver->telemetry.perf_btran_nnz_samples,
                                                   tab->m);
        lp_reinvert_controller_state_record_solve_density(reinvert_state,
                                                          ftran_density,
                                                          btran_density);

        reinvert_signals.phase = 2;
        reinvert_signals.iter = iter;
        reinvert_signals.m = tab->m;
        reinvert_signals.num_updates = lu_get_num_updates(tab->lu);
        reinvert_signals.max_updates = lu_get_max_updates(tab->lu);
        reinvert_signals.periodic_due = quality_refactor_nominal ? 1 : 0;
        reinvert_signals.min_update_age = (base_interval > 0) ? (base_interval / 2) : 8;
        reinvert_signals.cooldown_updates = 0;
        reinvert_signals.hard_lu_trigger = lu_refactor_needed ? 1 : 0;
        reinvert_signals.soft_lu_trigger = 0;
        reinvert_signals.ftran_density = ftran_density;
        reinvert_signals.btran_density = btran_density;

        reinvert_shadow = lp_reinvert_controller_decide(reinvert_state, &reinvert_signals);
        if (reinvert_mode_effective == LP_REINVERT_MODE_CONTROL_ALL && !lu_refactor_needed) {
            switch ((LPReinvertDecision)reinvert_shadow.decision) {
                case LP_REINVERT_DECISION_FORCE:
                    quality_refactor = 1;
                    break;
                case LP_REINVERT_DECISION_DEFER:
                    quality_refactor = 0;
                    break;
                case LP_REINVERT_DECISION_ALLOW:
                default:
                    quality_refactor = quality_refactor_nominal ? 1 : 0;
                    break;
            }
            quality_refactor_nominal = quality_refactor;
        }
        reinvert_suggested_refactor =
            (reinvert_shadow.decision == LP_REINVERT_DECISION_FORCE) ||
            (reinvert_shadow.decision == LP_REINVERT_DECISION_ALLOW &&
             quality_refactor_nominal);
        lp_reinvert_controller_state_apply_decision(reinvert_state, &reinvert_shadow);
    }

    need_refactor = (lu_refactor_needed || quality_refactor) ? 1 : 0;
    shadow_refactor = lp_basis_governor_shadow_decide(LP_BASIS_GOV_PHASE_DUAL,
                                                      lu_refactor_needed,
                                                      quality_refactor_nominal);
    governed_refactor = lp_basis_governor_decide_refactor(&solver->policy.basis_governor,
                                                           LP_BASIS_GOV_PHASE_DUAL,
                                                           lu_refactor_needed,
                                                           quality_refactor_nominal,
                                                           need_refactor);
    if (solver->telemetry_enabled) {
        lp_basis_governor_observe_refactor(&solver->policy.basis_governor,
                                           LP_BASIS_GOV_PHASE_DUAL,
                                           shadow_refactor,
                                           governed_refactor);
    }
    if (reinvert_active) {
        lp_telemetry_record_reinvert_shadow(solver,
                                            0,
                                            reinvert_shadow.decision,
                                            reinvert_shadow.reason,
                                            reinvert_suggested_refactor,
                                            governed_refactor);
    }

    return governed_refactor;
}

/*
 * Extract Farkas ray (certificate of infeasibility) for dual simplex.
 *
 * When dual simplex detects infeasibility (no entering variable found),
 * the current dual values y = c_B' * B^{-1} provide a Farkas certificate:
 *   y'A >= 0 for all columns (at appropriate bounds)
 *   y'b < 0
 *
 * This proves no feasible solution exists via Farkas lemma.
 */
static void extract_farkas_ray_dual(SimplexSolver *solver) {
    SimplexTableau *tab = solver->tableau;
    tab->owner = solver;
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

    /* Ensure we have fresh dual values */
    tableau_compute_reduced_costs(tab);

    /* Copy the dual values - these are the Farkas multipliers */
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
            LP_LOG_STDERR("[extract_farkas_ray_dual] WARNING: Farkas ray is all zeros\n");
        }
        return;
    }

    /* Debug validation: verify y'b_tab < 0 (Farkas lemma requirement) */
    double y_tab_dot_rhs = 0.0;
    for (int i = 0; i < m; i++) {
        y_tab_dot_rhs += tab->y[i] * tab->rhs[i];
    }

    if (y_tab_dot_rhs >= -1e-6) {
        if (solver->verbose) {
            LP_LOG_STDERR("[extract_farkas_ray_dual] WARNING: y'b_tab = %.6e (expected < 0)\n",
                    y_tab_dot_rhs);
        }
    } else if (solver->verbose >= 2) {
        LP_LOG_STDERR("[extract_farkas_ray_dual] y'b_tab = %.6e < 0 (valid)\n", y_tab_dot_rhs);
    }

    solver->farkas_valid = 1;

    if (solver->verbose >= 2) {
        LP_LOG_STDERR("[extract_farkas_ray_dual] Valid certificate: ||y||_inf = %.6e\n", max_abs);
    }
}

static int dual_time_limit_exceeded(SimplexSolver *solver, int iter) {
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

static int dual_allow_startup_bound_flip(const SimplexSolver *solver) {
    if (!solver) return 0;
    if (!lp_glpk_strict_allow_dual_startup_bound_flip(
            solver->glpk_strict_mode)) {
        return 0;
    }
    if (!solver->use_dual_bound_flip) return 0;
    /* FLIP mode applies bound flips iteratively during ratio steps. */
    if (solver->dual_ratio_test_mode == LP_DUAL_RATIO_TEST_FLIP) return 0;
    return 1;
}

static int dual_run_user_callbacks(SimplexSolver *solver,
                                   const SimplexTableau *tab,
                                   int iter,
                                   int force_emit,
                                   int honor_progress_cancel) {
    if (!solver) return 0;
    if (dual_time_limit_exceeded(solver, iter)) return 1;

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
    info.phase = RALPH_LP_PROGRESS_PHASE_DUAL;
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

static int dual_try_one_shot_recovery(SimplexSolver *solver,
                                      SimplexTableau *tab,
                                      int use_dse,
                                      int lu_hard_start,
                                      int *recovery_used,
                                      int iter,
                                      const char *reason_tag) {
    const int min_window = 128;
    const int max_window = 384;
    int iter_window;

    if (!solver || !tab || !recovery_used || *recovery_used) return 0;
    if (!lp_glpk_strict_allow_dual_one_shot_recovery(
            solver->glpk_strict_mode)) {
        return 0;
    }
    if (strcmp(reason_tag ? reason_tag : "", "ratio_no_entering") != 0) {
        return 0;
    }
    if (solver->telemetry.perf_dual_lu_hard_trigger > lu_hard_start) {
        return 0;
    }
    iter_window = tab->m / 3;
    if (iter_window < min_window) iter_window = min_window;
    if (iter_window > max_window) iter_window = max_window;
    if (iter > iter_window) {
        return 0;
    }

    *recovery_used = 1;
    solver->dual_ratio_test_mode = LP_DUAL_RATIO_TEST_HARRIS;
    solver->use_dual_bound_flip = 0;

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_v2] One-shot recovery at iter %d (%s): full refactor + Harris + noflip\n",
               iter, reason_tag ? reason_tag : "unspecified");
    }

    lp_telemetry_set_refactor_next_reason(solver, RALPH_REFACTOR_REASON_RATIO_RECOVERY);
    {
        double t_refactor_ms = lp_telemetry_timer_start();
        int rc_ref = tableau_refactorize(tab);
        lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
        if (rc_ref != 0) {
            return 0;
        }
    }

    tab->dse_initialized = 0;
    if (use_dse) dse_init_approx(tab);
    phase1_rescue_compute_solution(tab);
    phase1_rescue_compute_reduced_costs(tab);
    return 1;
}

/* ============================================================================
 * Dual Ratio Test
 * ============================================================================ */

/* Harris tolerance allows slightly suboptimal ratios if they provide
 * numerically more stable pivot elements. */
#define HARRIS_TOL 1e-6

typedef struct {
    double strict_pivot_floor;
    double base_pivot_floor;
    double strict_theta_floor;
    double permissive_theta_floor;
    double hard_refactor_floor;
    int flip_round_cap;
} DualRatioAdaptiveConfig;

static void dual_ratio_adaptive_config_defaults(DualRatioAdaptiveConfig *cfg) {
    if (!cfg) return;
    cfg->strict_pivot_floor = 10.0 * RALPH_PIVOT_TOL;
    cfg->base_pivot_floor = RALPH_PIVOT_TOL;
    cfg->strict_theta_floor = 0.0;
    cfg->permissive_theta_floor = -RALPH_OPT_TOL;
    cfg->hard_refactor_floor = 1e-4;
    cfg->flip_round_cap = 3;
}

static void dual_ratio_adaptive_config_core(int m,
                                            double lu_pivot_tol,
                                            double cond_estimate,
                                            double growth_factor,
                                            DualRatioAdaptiveConfig *cfg) {
    double strict_mult = 10.0;
    double base_mult = 1.0;
    double theta_floor = 0.0;
    double hard_mult = 1024.0;
    double base_tol;
    double stress;

    if (!cfg) return;
    dual_ratio_adaptive_config_defaults(cfg);

    base_tol = RALPH_PIVOT_TOL;
    if (isfinite(lu_pivot_tol) && lu_pivot_tol > base_tol) {
        base_tol = lu_pivot_tol;
    }
    if (!isfinite(cond_estimate) || cond_estimate <= 0.0) cond_estimate = 1.0;
    if (!isfinite(growth_factor) || growth_factor <= 0.0) growth_factor = 1.0;
    stress = cond_estimate * growth_factor;

    if (cond_estimate >= 1e9 || growth_factor >= 1e5 || stress >= 1e12) {
        strict_mult = 20.0;
        base_mult = 5.0;
        theta_floor = 1e-10;
        hard_mult = 65536.0;
    } else if (cond_estimate >= 1e7 || growth_factor >= 1e3 || stress >= 1e10) {
        strict_mult = 14.0;
        base_mult = 2.0;
        theta_floor = 5e-11;
        hard_mult = 16384.0;
    } else if (cond_estimate >= 1e6 || growth_factor >= 1e2 || stress >= 1e8) {
        strict_mult = 12.0;
        base_mult = 1.5;
        theta_floor = 1e-11;
        hard_mult = 4096.0;
    }

    /* For very large models, avoid over-pruning admissible candidates. */
    if (m >= 2500) {
        if (strict_mult > 8.0) strict_mult -= 2.0;
        if (base_mult > 1.0) base_mult *= 0.75;
    } else if (m >= 1500 && strict_mult > 9.0) {
        strict_mult -= 1.0;
    }

    cfg->base_pivot_floor = base_tol * base_mult;
    cfg->strict_pivot_floor = base_tol * strict_mult;
    if (cfg->base_pivot_floor < base_tol) cfg->base_pivot_floor = base_tol;
    if (cfg->strict_pivot_floor < cfg->base_pivot_floor) {
        cfg->strict_pivot_floor = cfg->base_pivot_floor;
    }

    cfg->strict_theta_floor = theta_floor;
    cfg->permissive_theta_floor = -RALPH_OPT_TOL;
    cfg->hard_refactor_floor = cfg->base_pivot_floor * hard_mult;
    if (cfg->hard_refactor_floor < 1e-4) cfg->hard_refactor_floor = 1e-4;
    if (cfg->hard_refactor_floor > 1e-2) cfg->hard_refactor_floor = 1e-2;

    if (m >= 3000) cfg->flip_round_cap = 4;
    else if (m >= 1200) cfg->flip_round_cap = 3;
    else cfg->flip_round_cap = 2;
}

static void dual_ratio_adaptive_config_for_tableau(const SimplexTableau *tab,
                                                   DualRatioAdaptiveConfig *cfg) {
    int m = tab ? tab->m : 0;
    double pivot_tol = RALPH_PIVOT_TOL;
    double cond_estimate = 1.0;
    double growth_factor = 1.0;
    if (tab && tab->lu) {
        pivot_tol = lu_get_pivot_tol(tab->lu);
        cond_estimate = lu_get_cond_estimate(tab->lu);
        growth_factor = lu_get_growth_factor(tab->lu);
    }
    if (tab && tab->owner &&
        !lp_glpk_strict_use_dual_adaptive_ratio_thresholds(
            tab->owner->glpk_strict_mode)) {
        dual_ratio_adaptive_config_defaults(cfg);
        if (isfinite(pivot_tol) && pivot_tol > cfg->base_pivot_floor) {
            cfg->base_pivot_floor = pivot_tol;
            cfg->strict_pivot_floor = 10.0 * pivot_tol;
            cfg->hard_refactor_floor = cfg->base_pivot_floor * 1024.0;
            if (cfg->hard_refactor_floor < 1e-4) cfg->hard_refactor_floor = 1e-4;
            if (cfg->hard_refactor_floor > 1e-2) cfg->hard_refactor_floor = 1e-2;
        }
        return;
    }
    if (tab && tab->model && tab->model->num_integers > 0) {
        /* Keep MIP node-LP path on conservative legacy ratio thresholds. */
        dual_ratio_adaptive_config_defaults(cfg);
        return;
    }
    dual_ratio_adaptive_config_core(m,
                                    pivot_tol,
                                    cond_estimate,
                                    growth_factor,
                                    cfg);
}

int dual_smcp_shift_allows_perturb_for_test(int smcp_shift) {
    return smcp_shift != 0;
}

static inline int dual_smcp_shift_allows_perturb(const SimplexTableau *tab) {
    if (!tab || !tab->owner) return 1;
    return dual_smcp_shift_allows_perturb_for_test(tab->owner->smcp_shift);
}

int dual_ratio_scan_direction_for_test(int smcp_aorn) {
    /* Keep legacy/default path on N^T (aorn=2). A^T uses reverse scan order. */
    return (smcp_aorn == 1) ? -1 : 1;
}

int dual_ratio_use_at_kernel_for_test(int smcp_aorn, int has_row_scatter) {
    return lp_policy_glpk_working_use_at_kernel(smcp_aorn, has_row_scatter);
}

int dual_ratio_use_row_kernel_for_test(int smcp_aorn,
                                       int has_row_scatter,
                                       int csr_use_scatter,
                                       int m) {
    if (!has_row_scatter) return 0;
    if (dual_ratio_use_at_kernel_for_test(smcp_aorn, has_row_scatter)) return 1;
    return csr_use_scatter && m >= 1500;
}

static inline int dual_ratio_scan_direction(const SimplexTableau *tab) {
    if (!tab || !tab->owner) return 1;
    return dual_ratio_scan_direction_for_test(tab->owner->smcp_aorn);
}

static inline int dual_ratio_use_row_kernel(const SimplexTableau *tab) {
    if (!tab || !tab->csr_rowptr || !tab->csr_colidx ||
        !tab->csr_values || !tab->csr_alpha) {
        return 0;
    }
    return dual_ratio_use_row_kernel_for_test(
        tab->owner ? tab->owner->smcp_aorn : LP_GLPK_SMCP_AORN_USE_NT,
        1,
        tab->csr_use_scatter,
        tab->m);
}

static int dual_smcp_excl_skip_var(const SimplexTableau *tab, int var) {
    int smcp_excl = 1;
    int smcp_shift = 1;
    double tol_bnd = 1e-12;
    VarStatus st;
    if (!tab || var < 0 || var >= tab->n) return 0;
    st = tab->var_status[var];
    if (tab->model && tab->free_split_col && tab->free_split_orig) {
        int mate = -1;
        if (var < tab->model->num_vars) {
            mate = tab->free_split_col[var];
        } else if (var < tab->num_structural_ext) {
            mate = tab->free_split_orig[var];
        }
        if (mate >= 0 && mate < tab->n && tab->var_status[mate] == RALPH_BASIC) {
            return 1;
        }
    }
    if (tab->owner) {
        smcp_excl = tab->owner->smcp_excl;
        smcp_shift = tab->owner->smcp_shift;
        tol_bnd = tab->owner->smcp_tol_bnd;
    }
    return lp_policy_glpk_working_exclude_nonbasic(smcp_excl,
                                                   smcp_shift,
                                                   (int)st,
                                                   tab->lb_ext[var],
                                                   tab->ub_ext[var],
                                                   tol_bnd);
}

static int dual_cadence_clamp_base_interval(int interval) {
    if (interval <= 0) interval = 50;
    if (interval < 8) interval = 8;
    if (interval > 128) interval = 128;
    return interval;
}

static int dual_cadence_clamp_rc_interval(int interval, int base_interval) {
    if (interval <= 0) interval = base_interval / 2;
    if (interval < 10) interval = 10;
    if (interval > 64) interval = 64;
    return interval;
}

void dual_cadence_intervals_for_test(int requested_base,
                                     int requested_rc,
                                     int *base_out,
                                     int *rc_out) {
    int base_interval = dual_cadence_clamp_base_interval(requested_base);
    int rc_interval = dual_cadence_clamp_rc_interval(requested_rc, base_interval);
    if (base_out) *base_out = base_interval;
    if (rc_out) *rc_out = rc_interval;
}

static int dual_refactor_base_interval_for_solver(const SimplexSolver *solver) {
    int requested = 50;
    if (solver) requested = solver->policy.dual_refactor_base_interval;
    return dual_cadence_clamp_base_interval(requested);
}

static int dual_rc_recompute_interval_for_solver(const SimplexSolver *solver,
                                                 int base_interval) {
    int requested = 20;
    if (solver) requested = solver->policy.dual_rc_recompute_interval;
    return dual_cadence_clamp_rc_interval(requested, base_interval);
}

static int dual_phase1_rescue_progress_limit(int m) {
    (void)m;
    return DUAL_PHASE1_RESCUE_PROGRESS_LIMIT;
}

int dual_phase1_rescue_progress_limit_for_test(int m) {
    return dual_phase1_rescue_progress_limit(m);
}

int dual_phase1_rescue_progress_update_for_test(int current_rows,
                                                double current_max,
                                                double current_sum,
                                                int stall_limit,
                                                int *best_rows_io,
                                                double *best_max_io,
                                                double *best_sum_io,
                                                int *stall_count_io) {
    int best_rows = best_rows_io ? *best_rows_io : -1;
    double best_max = best_max_io ? *best_max_io : 0.0;
    double best_sum = best_sum_io ? *best_sum_io : 0.0;
    int stall_count = stall_count_io ? *stall_count_io : 0;
    int improved = 0;

    if (best_rows < 0 || current_rows < best_rows) {
        improved = 1;
    } else if (current_rows == best_rows) {
        double max_tol = 1e-9 * (1.0 + fabs(best_max));
        double sum_tol = 1e-9 * (1.0 + fabs(best_sum));
        if (current_max < best_max - max_tol) {
            improved = 1;
        } else if (fabs(current_max - best_max) <= max_tol &&
                   current_sum < best_sum - sum_tol) {
            improved = 1;
        }
    }

    if (improved) {
        best_rows = current_rows;
        best_max = current_max;
        best_sum = current_sum;
        stall_count = 0;
    } else {
        stall_count++;
    }

    if (best_rows_io) *best_rows_io = best_rows;
    if (best_max_io) *best_max_io = best_max;
    if (best_sum_io) *best_sum_io = best_sum;
    if (stall_count_io) *stall_count_io = stall_count;

    return (stall_limit > 0 && stall_count >= stall_limit) ? 1 : 0;
}

static int dual_ratio_passes_theta_floor(double ratio, double theta_floor);
static int dual_ratio_candidate_value(const SimplexTableau *tab,
                                      int dir,
                                      int var,
                                      double alpha_j,
                                      double pivot_floor,
                                      double *ratio_out);

/* Test hook: adaptive ratio thresholds (orthogonal to simplex-policy tests). */
void dual_ratio_adaptive_config_for_test(int m,
                                         double lu_pivot_tol,
                                         double cond_estimate,
                                         double growth_factor,
                                         double *strict_pivot_floor_out,
                                         double *base_pivot_floor_out,
                                         double *strict_theta_floor_out,
                                         double *hard_refactor_floor_out,
                                         int *flip_round_cap_out) {
    DualRatioAdaptiveConfig cfg;
    dual_ratio_adaptive_config_core(m,
                                    lu_pivot_tol,
                                    cond_estimate,
                                    growth_factor,
                                    &cfg);
    if (strict_pivot_floor_out) *strict_pivot_floor_out = cfg.strict_pivot_floor;
    if (base_pivot_floor_out) *base_pivot_floor_out = cfg.base_pivot_floor;
    if (strict_theta_floor_out) *strict_theta_floor_out = cfg.strict_theta_floor;
    if (hard_refactor_floor_out) *hard_refactor_floor_out = cfg.hard_refactor_floor;
    if (flip_round_cap_out) *flip_round_cap_out = cfg.flip_round_cap;
}

int dual_ratio_passes_theta_floor_for_test(double ratio, double theta_floor) {
    return dual_ratio_passes_theta_floor(ratio, theta_floor);
}

int dual_ratio_candidate_value_for_test(const SimplexTableau *tab,
                                        int dir,
                                        int var,
                                        double alpha_j,
                                        double pivot_floor,
                                        double *ratio_out) {
    return dual_ratio_candidate_value(tab, dir, var, alpha_j, pivot_floor, ratio_out);
}

double dual_max_reduced_cost_violation_for_test(const SimplexTableau *tab) {
    return dual_max_reduced_cost_violation(tab);
}

static int dual_candidate_can_flip(const SimplexTableau *tab, int var) {
    if (!tab || var < 0 || var >= tab->n) return 0;
    if (tab->var_status[var] == RALPH_NONBASIC_LOWER) {
        return tab->ub_ext[var] < RALPH_INFINITY / 2.0;
    }
    if (tab->var_status[var] == RALPH_NONBASIC_UPPER) {
        return tab->lb_ext[var] > -RALPH_INFINITY / 2.0;
    }
    return 0;
}

static int dual_candidate_flip_preserves_dual_feasibility(const SimplexTableau *tab, int var) {
    if (!tab || var < 0 || var >= tab->n) return 0;
    if (!dual_candidate_can_flip(tab, var)) return 0;
    if (tab->var_status[var] == RALPH_NONBASIC_LOWER) {
        /* Opposite side is upper, which needs rc <= 0. */
        return tab->rc[var] <= RALPH_OPT_TOL;
    }
    if (tab->var_status[var] == RALPH_NONBASIC_UPPER) {
        /* Opposite side is lower, which needs rc >= 0. */
        return tab->rc[var] >= -RALPH_OPT_TOL;
    }
    return 0;
}

static int dual_apply_bound_flip_nonbasic(SimplexTableau *tab, int var) {
    if (!tab || var < 0 || var >= tab->n) return 0;
    if (tab->var_status[var] == RALPH_NONBASIC_LOWER) {
        if (tab->ub_ext[var] >= RALPH_INFINITY / 2.0) return 0;
        tab->var_status[var] = RALPH_NONBASIC_UPPER;
        tab->x[var] = tab->ub_ext[var];
        return 1;
    }
    if (tab->var_status[var] == RALPH_NONBASIC_UPPER) {
        if (tab->lb_ext[var] <= -RALPH_INFINITY / 2.0) return 0;
        tab->var_status[var] = RALPH_NONBASIC_LOWER;
        tab->x[var] = tab->lb_ext[var];
        return 1;
    }
    return 0;
}

static int dual_ratio_candidate_value(const SimplexTableau *tab,
                                      int dir,
                                      int var,
                                      double alpha_j,
                                      double pivot_floor,
                                      double *ratio_out) {
    double rc_j;
    double ratio = RALPH_INFINITY;
    VarStatus st;
    int ratio_valid = 0;

    if (!tab || !ratio_out || var < 0 || var >= tab->n) return 0;
    st = tab->var_status[var];
    if (st == RALPH_BASIC || dual_smcp_excl_skip_var(tab, var)) return 0;
    if (fabs(alpha_j) < pivot_floor) return 0;

    rc_j = tab->rc[var];
    if (dir > 0) {
        if (alpha_j < -pivot_floor && st == RALPH_NONBASIC_LOWER) {
            ratio = -rc_j / alpha_j;
            ratio_valid = 1;
        } else if (alpha_j > pivot_floor && st == RALPH_NONBASIC_UPPER) {
            ratio = -rc_j / alpha_j;
            ratio_valid = 1;
        }
    } else {
        if (alpha_j > pivot_floor && st == RALPH_NONBASIC_LOWER) {
            ratio = rc_j / alpha_j;
            ratio_valid = 1;
        } else if (alpha_j < -pivot_floor && st == RALPH_NONBASIC_UPPER) {
            ratio = rc_j / alpha_j;
            ratio_valid = 1;
        }
    }

    if (!ratio_valid) return 0;
    if (!isfinite(ratio)) return 0;
    if (ratio < 0.0 && ratio >= -RALPH_OPT_TOL) {
        ratio = 0.0;
    }
    *ratio_out = ratio;
    return 1;
}

static int dual_ratio_passes_theta_floor(double ratio, double theta_floor) {
    if (!isfinite(ratio)) return 0;
    if (ratio < 0.0) return ratio >= theta_floor;
    if (ratio <= RALPH_OPT_TOL) return 1;
    return ratio >= theta_floor;
}

static void dual_ratio_mode_flags(int mode, int *use_harris, int *prefer_flip_candidates) {
    int harris = 1;
    int prefer_flip = 0;
    if (mode == LP_DUAL_RATIO_TEST_STANDARD) {
        harris = 0;
        prefer_flip = 0;
    } else if (mode == LP_DUAL_RATIO_TEST_FLIP) {
        harris = 1;
        /* Flip mode now uses dedicated iterative bound-flip logic. Keep core
         * ratio test neutral (Harris) when that path is unavailable. */
        prefer_flip = 0;
    }
    if (use_harris) *use_harris = harris;
    if (prefer_flip_candidates) *prefer_flip_candidates = prefer_flip;
}

static int dual_ratio_test_core(SimplexTableau *tab,
                                int leaving,
                                int *entering,
                                double *theta,
                                int use_harris,
                                int prefer_flip_candidates,
                                double pivot_floor,
                                double theta_floor) {
    if (!tab || !entering || !theta) return -1;
    if (!(pivot_floor > 0.0)) pivot_floor = RALPH_PIVOT_TOL;
    int leaving_var = tab->basis[leaving];
    double x_leave = tab->x[leaving_var];

    /* Determine direction based on which bound is violated */
    int dir;  /* +1 if leaving increases, -1 if decreases */
    if (x_leave < tab->lb_ext[leaving_var] - RALPH_FEAS_TOL) {
        dir = 1;  /* Need to increase */
    } else if (x_leave > tab->ub_ext[leaving_var] + RALPH_FEAS_TOL) {
        dir = -1;  /* Need to decrease */
    } else {
        return -1;  /* Not infeasible */
    }

    /* Compute leaving row of basis inverse: e_leaving' * B^{-1} */
    {
        int rhs_idx = leaving;
        double rhs_val = 1.0;
        lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, tab->work2);
    }

    /* Find entering variable by dual ratio test */
    *entering = -1;
    *theta = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_can_flip = 0;
    const double tie_tol = 1e-12;
    int scan_dir = dual_ratio_scan_direction(tab);
    int use_row_kernel = dual_ratio_use_row_kernel(tab);
    DualWorkingExclusionCtx excl_ctx = dual_working_exclusion_ctx(tab);
    double *alpha_at = NULL;

    if (use_row_kernel) {
        alpha_at = tab->csr_alpha;
        memset(alpha_at, 0, (size_t)tab->n * sizeof(double));
        for (int i = 0; i < tab->m; i++) {
            double wi = tab->work2[i];
            if (wi == 0.0) continue;
            for (int p = tab->csr_rowptr[i]; p < tab->csr_rowptr[i + 1]; p++) {
                int col = tab->csr_colidx[p];
                alpha_at[col] += wi * tab->csr_values[p];
            }
        }
    }

    for (int t = 0; t < tab->n; t++) {
        int j = (scan_dir > 0) ? t : (tab->n - 1 - t);
        if (tab->var_status[j] == RALPH_BASIC ||
            dual_working_exclusion_skip_var(&excl_ctx, j)) {
            continue;
        }

        /* Compute alpha_j = (B^{-1} * a_j)[leaving] = alpha' * a_j using sparse dot */
        double alpha_j = use_row_kernel ? alpha_at[j]
                                        : sparse_dot_column(tab->A_ext, j, tab->work2);

        if (fabs(alpha_j) < pivot_floor) continue;

        double rc_j = tab->rc[j];
        double ratio = RALPH_INFINITY;
        int ratio_valid = 0;

        /* Dual ratio test depends on direction and variable bound status.
         *
         * For internal minimization, dual feasibility requires:
         * - At lower bound: rc >= 0
         * - At upper bound: rc <= 0
         *
         * After pivot, the leaving variable's new rc = -rc_entering / pivot.
         * For dual feasibility at the leaving var's new bound, we need this >= 0
         * (since leaving goes to lower when below its bound).
         *
         * The ratio = -rc_j / alpha_j represents the new rc for the leaving variable.
         * We select the minimum non-negative ratio.
         */
        if (dir > 0) {
            /* Leaving variable needs to increase (currently below lower bound) */
            if (alpha_j < -pivot_floor && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j from lower bound increases x_B[leaving] - good! */
                /* x_j at lower has rc_j >= 0, alpha_j < 0, so -rc_j/alpha_j >= 0 */
                ratio = -rc_j / alpha_j;
                ratio_valid = 1;
            } else if (alpha_j > pivot_floor && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j from upper bound increases x_B[leaving] - good! */
                /* x_j at upper has rc_j <= 0, alpha_j > 0, so -rc_j/alpha_j >= 0 */
                ratio = -rc_j / alpha_j;
                ratio_valid = 1;
            }
        } else {
            /* Leaving variable needs to decrease (currently above upper bound)
             *
             * For dir=-1, the ratio formula differs from dir=+1:
             * - At LOWER with alpha > 0: rc >= 0, so rc/alpha >= 0
             * - At UPPER with alpha < 0: rc <= 0, so rc/alpha >= 0
             * Using rc_j/alpha_j (not -rc_j/alpha_j) gives positive ratios.
             */
            if (alpha_j > pivot_floor && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j from lower bound decreases x_B[leaving] - good! */
                ratio = rc_j / alpha_j;
                ratio_valid = 1;
            } else if (alpha_j < -pivot_floor && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j from upper bound decreases x_B[leaving] - good! */
                ratio = rc_j / alpha_j;
                ratio_valid = 1;
            }
        }

        if (!ratio_valid || !isfinite(ratio)) continue;
        if (ratio < 0.0 && ratio >= -RALPH_OPT_TOL) {
            ratio = 0.0;
        }
        if (!dual_ratio_passes_theta_floor(ratio, theta_floor)) continue;
        int can_flip = prefer_flip_candidates ? dual_candidate_can_flip(tab, j) : 0;
        if (*entering < 0 || ratio < *theta) {
            *theta = ratio;
            *entering = j;
            best_pivot = fabs(alpha_j);
            best_can_flip = can_flip;
            continue;
        }

        if (use_harris && ratio <= *theta + HARRIS_TOL * (1.0 + fabs(*theta))) {
            if (prefer_flip_candidates && can_flip != best_can_flip) {
                if (can_flip > best_can_flip) {
                    *entering = j;
                    best_pivot = fabs(alpha_j);
                    best_can_flip = can_flip;
                }
            } else if (fabs(alpha_j) > best_pivot) {
                *entering = j;
                best_pivot = fabs(alpha_j);
            }
            continue;
        }

        if (!use_harris && fabs(ratio - *theta) <= tie_tol * (1.0 + fabs(*theta))) {
            if (prefer_flip_candidates && can_flip != best_can_flip) {
                if (can_flip > best_can_flip) {
                    *entering = j;
                    best_pivot = fabs(alpha_j);
                    best_can_flip = can_flip;
                }
            } else if (fabs(alpha_j) > best_pivot) {
                *entering = j;
                best_pivot = fabs(alpha_j);
            }
        }
    }

    return (*entering >= 0) ? 0 : -1;
}

/* Iterative dual flip mode:
 * - Run a Harris-style ratio window for the selected leaving row.
 * - Apply non-basic bound flips (no basis update) for boxed candidates in the
 *   strict interior of that window, but only when the opposite bound remains
 *   dual-feasible under current reduced costs.
 * - Return entering=-2 when at least one flip was applied; caller should skip
 *   pivot and continue from refreshed primal values.
 */
static int dual_flip_list_contains(const int *list, int count, int var) {
    for (int i = 0; i < count; i++) {
        if (list[i] == var) return 1;
    }
    return 0;
}

static int dual_ratio_test_flip_iterative(SimplexTableau *tab,
                                          int leaving,
                                          int *entering,
                                          double *theta,
                                          const DualRatioAdaptiveConfig *cfg) {
    int leaving_var;
    double x_leave;
    int dir;
    int total_flips = 0;
    int flip_round_cap = 3;
    double pivot_floor = RALPH_PIVOT_TOL;
    double theta_floor = 1e-12;
    int flip_cap;
    int max_total_flips;
    int round;
    int scan_dir = dual_ratio_scan_direction(tab);
    int use_row_kernel = dual_ratio_use_row_kernel(tab);
    DualWorkingExclusionCtx excl_ctx = dual_working_exclusion_ctx(tab);
    double *alpha_at = NULL;

    if (!tab || !entering || !theta || leaving < 0 || leaving >= tab->m) return -1;
    if (!tab->flip_list) return -1;

    if (cfg) {
        if (cfg->base_pivot_floor > 0.0) pivot_floor = cfg->base_pivot_floor;
        theta_floor = cfg->strict_theta_floor;
        if (cfg->flip_round_cap > 0) flip_round_cap = cfg->flip_round_cap;
    }

    leaving_var = tab->basis[leaving];
    x_leave = tab->x[leaving_var];
    if (x_leave < tab->lb_ext[leaving_var] - RALPH_FEAS_TOL) {
        dir = 1;
    } else if (x_leave > tab->ub_ext[leaving_var] + RALPH_FEAS_TOL) {
        dir = -1;
    } else {
        return -1;
    }

    {
        int rhs_idx = leaving;
        double rhs_val = 1.0;
        lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, tab->work2);
    }

    if (use_row_kernel) {
        alpha_at = tab->csr_alpha;
        memset(alpha_at, 0, (size_t)tab->n * sizeof(double));
        for (int i = 0; i < tab->m; i++) {
            double wi = tab->work2[i];
            if (wi == 0.0) continue;
            for (int p = tab->csr_rowptr[i]; p < tab->csr_rowptr[i + 1]; p++) {
                int col = tab->csr_colidx[p];
                alpha_at[col] += wi * tab->csr_values[p];
            }
        }
    }

    flip_cap = tab->m / 2;
    if (flip_cap < 1) flip_cap = 1;
    if (flip_cap > tab->n) flip_cap = tab->n;
    max_total_flips = flip_cap;
    tab->flip_count = 0;

    for (round = 0; round < flip_round_cap; round++) {
        int candidate_count = 0;
        double theta_min = RALPH_INFINITY;
        double theta_harris;
        int best_entering = -1;
        double best_theta = RALPH_INFINITY;
        double best_pivot = 0.0;
        int round_flip_count = 0;

        /* Pass 1: strict candidate window baseline. */
        for (int t = 0; t < tab->n; t++) {
            int j = (scan_dir > 0) ? t : (tab->n - 1 - t);
            double alpha_j;
            double ratio;
            if (tab->var_status[j] == RALPH_BASIC ||
                dual_working_exclusion_skip_var(&excl_ctx, j)) {
                continue;
            }
            alpha_j = use_row_kernel ? alpha_at[j]
                                     : sparse_dot_column(tab->A_ext, j, tab->work2);
            if (!dual_ratio_candidate_value(tab, dir, j, alpha_j, pivot_floor, &ratio)) continue;
            if (!dual_ratio_passes_theta_floor(ratio, theta_floor)) continue;
            candidate_count++;
            if (ratio < theta_min) theta_min = ratio;
        }

        if (candidate_count <= 0 || theta_min >= RALPH_INFINITY / 2.0) {
            break;
        }

        theta_harris = theta_min + HARRIS_TOL * (1.0 + fabs(theta_min));

        /* Pass 2: choose entering in Harris window; stage interior flips. */
        for (int t = 0; t < tab->n; t++) {
            int j = (scan_dir > 0) ? t : (tab->n - 1 - t);
            double alpha_j;
            double ratio;
            double abs_alpha;
            if (tab->var_status[j] == RALPH_BASIC ||
                dual_working_exclusion_skip_var(&excl_ctx, j)) {
                continue;
            }
            alpha_j = use_row_kernel ? alpha_at[j]
                                     : sparse_dot_column(tab->A_ext, j, tab->work2);
            if (!dual_ratio_candidate_value(tab, dir, j, alpha_j, pivot_floor, &ratio)) continue;
            if (!dual_ratio_passes_theta_floor(ratio, theta_floor)) continue;

            if (ratio <= theta_harris) {
                abs_alpha = fabs(alpha_j);
                if (best_entering < 0 || abs_alpha > best_pivot) {
                    best_entering = j;
                    best_theta = ratio;
                    best_pivot = abs_alpha;
                }
            }

            if (total_flips + round_flip_count < max_total_flips &&
                ratio < theta_harris &&
                dual_candidate_flip_preserves_dual_feasibility(tab, j) &&
                !dual_flip_list_contains(tab->flip_list, total_flips + round_flip_count, j)) {
                tab->flip_list[total_flips + round_flip_count] = j;
                round_flip_count++;
            }
        }

        if (round_flip_count == 0) {
            if (best_entering >= 0) {
                *entering = best_entering;
                *theta = best_theta;
                return 0;
            }
            break;
        }

        for (int i = 0; i < round_flip_count; i++) {
            int var = tab->flip_list[total_flips + i];
            int applied = dual_apply_bound_flip_nonbasic(tab, var);
            if (applied > 0) total_flips += applied;
        }
        if (total_flips <= 0) {
            break;
        }
        tab->flip_count = total_flips;
        tab->dual_cand_valid = 0;
        tableau_compute_solution(tab);

        leaving_var = tab->basis[leaving];
        x_leave = tab->x[leaving_var];
        if (!(x_leave < tab->lb_ext[leaving_var] - RALPH_FEAS_TOL ||
              x_leave > tab->ub_ext[leaving_var] + RALPH_FEAS_TOL)) {
            break;
        }

        if (total_flips >= max_total_flips || total_flips >= flip_cap) {
            break;
        }
    }

    if (total_flips > 0) {
        if (tab->owner) {
            lp_telemetry_record_dual_bound_flip_applied_iterative(tab->owner, total_flips);
        }
        *entering = -2;
        *theta = 0.0;
        return 0;
    }

    return -1;
}

int dual_ratio_test(SimplexTableau *tab, int leaving, int *entering, double *theta) {
    int mode = LP_DUAL_RATIO_TEST_HARRIS;
    int rc = -1;
    int attempt_mode;
    int use_harris = 1;
    int prefer_flip = 0;
    int strict_profile = 0;
    DualRatioAdaptiveConfig cfg;

    if (tab && tab->owner) {
        mode = tab->owner->dual_ratio_test_mode;
        strict_profile = tab->owner->glpk_strict_mode ? 1 : 0;
    }
    dual_ratio_adaptive_config_for_tableau(tab, &cfg);

    if (strict_profile) {
        if (mode == LP_DUAL_RATIO_TEST_FLIP &&
            tab && tab->owner && tab->owner->use_dual_bound_flip) {
            rc = dual_ratio_test_flip_iterative(tab, leaving, entering, theta, &cfg);
            if (rc == 0) {
                if (*entering == -2) {
                    return 0;
                }
                if (tab->owner && *theta <= 0.0) {
                    lp_telemetry_record_dual_theta_nonpositive(tab->owner);
                }
                return 0;
            }
        }
        dual_ratio_mode_flags(mode, &use_harris, &prefer_flip);
        rc = dual_ratio_test_core(tab, leaving, entering, theta,
                                  use_harris, prefer_flip,
                                  cfg.base_pivot_floor, cfg.strict_theta_floor);
        if (rc != 0) {
            if (tab && tab->owner) {
                lp_telemetry_record_dual_ratio_no_entering(tab->owner);
            }
            return -1;
        }
        if (tab && tab->owner && *theta <= 0.0) {
            lp_telemetry_record_dual_theta_nonpositive(tab->owner);
        }
        return 0;
    }

    if (mode == LP_DUAL_RATIO_TEST_FLIP &&
        tab && tab->owner && tab->owner->use_dual_bound_flip) {
        rc = dual_ratio_test_flip_iterative(tab, leaving, entering, theta, &cfg);
        if (rc == 0) {
            if (*entering == -2) {
                return 0;
            }
            if (tab->owner && *theta <= 0.0) {
                lp_telemetry_record_dual_theta_nonpositive(tab->owner);
            }
            return 0;
        }
    }

    /* Retry ladder:
     * 1) strict pivot floor and strictly-positive theta in requested mode
     * 2) fallback modes with base pivot floor and strictly-positive theta
     * 3) permissive last pass (default tolerance) to avoid false infeasibility */
    dual_ratio_mode_flags(mode, &use_harris, &prefer_flip);
    rc = dual_ratio_test_core(tab, leaving, entering, theta,
                              use_harris, prefer_flip,
                              cfg.strict_pivot_floor, cfg.strict_theta_floor);
    if (rc == 0) goto dual_ratio_done;

    for (int i = 0; i < 3; i++) {
        attempt_mode = (i == 0) ? LP_DUAL_RATIO_TEST_HARRIS :
                       (i == 1) ? LP_DUAL_RATIO_TEST_STANDARD :
                                  LP_DUAL_RATIO_TEST_FLIP;
        if (attempt_mode == mode) continue;
        dual_ratio_mode_flags(attempt_mode, &use_harris, &prefer_flip);
        rc = dual_ratio_test_core(tab, leaving, entering, theta,
                                  use_harris, prefer_flip,
                                  cfg.base_pivot_floor, cfg.strict_theta_floor);
        if (rc == 0) goto dual_ratio_done;
    }

    dual_ratio_mode_flags(mode, &use_harris, &prefer_flip);
    rc = dual_ratio_test_core(tab, leaving, entering, theta,
                              use_harris, prefer_flip,
                              cfg.base_pivot_floor, cfg.permissive_theta_floor);
    if (rc == 0) goto dual_ratio_done;

    /* Clean dual simplex must preserve dual feasibility.  The row-wise
     * Phase-1 rescue selector does not require a globally dual-feasible basis,
     * so using it here can corrupt the dual invariant and turn later ratio
     * failures into false infeasibility certificates. */
    rc = -1;

dual_ratio_done:
    if (rc != 0) {
        if (tab && tab->owner) {
            lp_telemetry_record_dual_ratio_no_entering(tab->owner);
        }
        return -1;
    }
    if (tab && tab->owner && *theta <= 0.0) {
        lp_telemetry_record_dual_theta_nonpositive(tab->owner);
    }
    return 0;
}

/* ============================================================================
 * DSE Initialization (P6)
 * ============================================================================ */

static void dse_init_exact(SimplexTableau *tab) {
    for (int k = 0; k < tab->m; k++) {
        vec_set_zero(tab->work1, tab->m);
        tab->work1[k] = 1.0;
        lu_solve_transpose(tab->lu, tab->work1, tab->tau_work);
        double norm_sq = vec_dot(tab->m, tab->tau_work, tab->tau_work);
        tab->dse_weights[k] = (norm_sq < 1e-12) ? 1.0 : norm_sq;
    }
    tab->dse_initialized = 1;
}

/* Approximate DSE init: set all weights to 1.0 (B5 fix).
 * Used after mid-loop refactorization where exact init is O(m^2).
 * The incremental weight update formula self-corrects within a few pivots. */
static void dse_init_approx(SimplexTableau *tab) {
    for (int k = 0; k < tab->m; k++) {
        tab->dse_weights[k] = 1.0;
    }
    tab->dse_initialized = 1;
}

/* ============================================================================
 * Dual Simplex Iteration
 * ============================================================================ */

static int dual_sparse_pressure_force_refactor_core(int m,
                                                    int num_updates,
                                                    int max_updates,
                                                    int spike_pool_used,
                                                    int spike_pool_capacity,
                                                    int ftran_nnz,
                                                    int btran_nnz) {
    double ftran_density;
    double btran_density;
    int aged_updates;
    int pool_pressure;

    if (m <= 0 || ftran_nnz <= 0 || btran_nnz <= 0) return 0;
    if (m < 300) return 0;

    ftran_density = (double)ftran_nnz / (double)m;
    btran_density = (double)btran_nnz / (double)m;

    if (max_updates > 0) {
        aged_updates = (3 * num_updates >= max_updates) ? 1 : 0;
    } else {
        aged_updates = (num_updates >= 40) ? 1 : 0;
    }

    pool_pressure = 0;
    if (spike_pool_capacity > 0 && spike_pool_used >= 0) {
        pool_pressure = (100 * spike_pool_used >= 80 * spike_pool_capacity) ? 1 : 0;
    }

    if (pool_pressure && (ftran_density >= 0.35 || btran_density >= 0.45)) {
        return 1;
    }
    if (aged_updates && ftran_density >= 0.55 && btran_density >= 0.65) {
        return 1;
    }
    return 0;
}

/* Test hook: solve-density reinversion pressure gate. */
int dual_sparse_pressure_force_refactor_for_test(int m,
                                                 int num_updates,
                                                 int max_updates,
                                                 int spike_pool_used,
                                                 int spike_pool_capacity,
                                                 int ftran_nnz,
                                                 int btran_nnz) {
    return dual_sparse_pressure_force_refactor_core(m,
                                                    num_updates,
                                                    max_updates,
                                                    spike_pool_used,
                                                    spike_pool_capacity,
                                                    ftran_nnz,
                                                    btran_nnz);
}

static int dual_simplex_pivot(SimplexTableau *tab,
                              int entering,
                              int leaving,
                              double theta,
                              int pivot_row_valid,
                              int pivot_alpha_valid) {
    (void)theta;  /* Step size already computed in caller */
    if (!tab || entering < 0 || entering >= tab->n || leaving < 0 || leaving >= tab->m) {
        return -1;
    }

    int leaving_var = tab->basis[leaving];
    double saved_obj = tab->obj_value;
    DualRatioAdaptiveConfig ratio_cfg;
    dual_ratio_adaptive_config_for_tableau(tab, &ratio_cfg);
    double pivot_reject_floor = ratio_cfg.base_pivot_floor;
    double hard_refactor_floor = ratio_cfg.hard_refactor_floor;
    int solve_nnz = -1;
    int ftran_nnz = -1;
    int btran_nnz = -1;
    int sparse_pressure_refactor = 0;
    int *solve_idx_buf = (tab->flip_list && tab->n >= tab->m) ? tab->flip_list : NULL;
    const int is_mip_lp = (tab->model && tab->model->num_integers > 0);
    int full_rollback_snapshot = 0;
    int force_refactor = 0;
    VarStatus entering_status_before = tab->var_status[entering];
    VarStatus leaving_status_before = tab->var_status[leaving_var];
    if (!(pivot_reject_floor > 0.0)) pivot_reject_floor = RALPH_PIVOT_TOL;
    if (!(hard_refactor_floor > 0.0)) hard_refactor_floor = 1e-4;
    if (is_mip_lp) {
        /* Keep MIP dual warm-start numerics on legacy pivot gates. */
        pivot_reject_floor = RALPH_PIVOT_TOL;
        hard_refactor_floor = 1e-4;
    }

    /* Keep pivot failures non-destructive: restore full basis/state on error.
     * Uses pre-allocated backup arrays in tableau (B2 fix: no per-pivot malloc). */
    double *x_backup = tab->dual_x_backup;
    double *rc_backup = tab->dual_rc_backup;
    int *basis_backup = tab->dual_basis_backup;
    int *basis_pos_backup = tab->dual_basis_pos_backup;
    VarStatus *status_backup = tab->dual_status_backup;

    /* Compute entering column in basis representation using sparse solve */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);
    {
        double t_ftran_ms = lp_telemetry_timer_start();
        if (is_mip_lp) {
            /* Keep MIP node LP path on the proven sparse FTRAN kernel.
             * Hyper-sparse is enabled for LP-heavy runs first. */
            lu_solve_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work3);
            ftran_nnz = 0;
            for (int k = 0; k < tab->m; k++) {
                if (fabs(tab->work3[k]) > RALPH_ZERO_TOL) ftran_nnz++;
            }
        } else {
            solve_nnz = -1;
            lu_ftran_hyper_sparse(tab->lu,
                                  col_nnz,
                                  col_idx,
                                  col_val,
                                  tab->work3,
                                  solve_idx_buf,
                                  solve_idx_buf ? &solve_nnz : NULL);
            ftran_nnz = solve_nnz;
            if (ftran_nnz < 0) {
                ftran_nnz = 0;
                for (int k = 0; k < tab->m; k++) {
                    if (fabs(tab->work3[k]) > RALPH_ZERO_TOL) ftran_nnz++;
                }
            }
        }
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
            lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
        }
    }

    double pivot = tab->work3[leaving];
    if (!isfinite(pivot) || fabs(pivot) < pivot_reject_floor) {
        if (tab->owner) {
            lp_telemetry_record_dual_pivot_reject_small(tab->owner);
        }
        goto pivot_fail_rollback;
    }

    /* Save rc_entering BEFORE updating reduced costs (needed for bound selection) */
    double rc_entering_orig = tab->rc[entering];

    /* Compute pivot row = e_leaving^T * B^{-1} via single BTRAN
     * This is MUCH more efficient than calling lu_solve_sparse for each column.
     * The pivot row gives us (B^{-1} * a_j)[leaving] for any j via a sparse dot product.
     */
    {
        double t_btran_ms = 0.0;
        if (!pivot_row_valid) {
            int rhs_idx = leaving;
            double rhs_val = 1.0;
            t_btran_ms = lp_telemetry_timer_start();
            lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, tab->work2);
        }
        btran_nnz = 0;
        for (int k = 0; k < tab->m; k++) {
            if (fabs(tab->work2[k]) > RALPH_ZERO_TOL) btran_nnz++;
        }
        if (tab->owner && !pivot_row_valid) {
            lp_telemetry_add_btran_timed(tab->owner, t_btran_ms);
        }
        if (tab->owner) {
            lp_telemetry_record_btran_nnz(tab->owner, 1, btran_nnz);
        }
    }

    if (!is_mip_lp) {
        sparse_pressure_refactor = dual_sparse_pressure_force_refactor_core(
            tab->m,
            tab->lu ? lu_get_num_updates(tab->lu) : 0,
            tab->lu ? lu_get_max_updates(tab->lu) : 0,
            (tab->lu && lu_get_use_ft_updates(tab->lu)) ? lu_get_spike_pool_used(tab->lu) : -1,
            (tab->lu && lu_get_use_ft_updates(tab->lu)) ? lu_get_spike_pool_capacity(tab->lu) : -1,
            ftran_nnz,
            btran_nnz);
    }
    force_refactor = (fabs(pivot) < hard_refactor_floor) || sparse_pressure_refactor;
    full_rollback_snapshot = is_mip_lp || force_refactor;
    if (full_rollback_snapshot) {
        memcpy(x_backup, tab->x, (size_t)tab->n * sizeof(double));
        memcpy(rc_backup, tab->rc, (size_t)tab->n * sizeof(double));
        memcpy(basis_backup, tab->basis, (size_t)tab->m * sizeof(int));
        memcpy(basis_pos_backup, tab->basis_pos, (size_t)tab->n * sizeof(int));
        memcpy(status_backup, tab->var_status, (size_t)tab->n * sizeof(VarStatus));
    }

    /* Update all reduced costs using sparse dot products:
     * rc'[j] = rc[j] - (rc_entering / pivot) * (pivot_row * a_j)
     * Also collect dual candidate list (T2.2) inline — zero extra cost.
     */
    double rc_factor = tab->rc[entering] / pivot;
    tab->dual_cand_count = 0;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) {
            tab->rc[j] = 0.0;
        } else if (j != entering) {
            /* Compute pivot_row * a_j via sparse dot product */
            double dot = pivot_alpha_valid ? tab->csr_alpha[j]
                                           : sparse_dot_column(tab->A_ext, j, tab->work2);
            tab->rc[j] -= rc_factor * dot;
            /* T2.2: Collect candidates with attractive |rc| */
            if (fabs(tab->rc[j]) > DUAL_CAND_RC_THRESH &&
                tab->dual_cand_count < tab->dual_cand_capacity) {
                tab->dual_candidates[tab->dual_cand_count++] = j;
            }
        }
    }
    tab->rc[leaving_var] = -rc_factor;
    tab->rc[entering] = 0.0;
    tab->dual_cand_valid = 1;

    /* Determine step size from infeasibility.
     * The basic variable update is: x_B = x_B - delta * d
     * where d = B^{-1} * a_entering.
     * For the leaving var: x_leave_new = x_leave - delta * pivot
     * We want x_leave_new = bound, so delta = (x_leave - bound) / pivot
     */
    double x_leave = tab->x[leaving_var];
    double step;
    if (x_leave < tab->lb_ext[leaving_var]) {
        step = (x_leave - tab->lb_ext[leaving_var]) / pivot;
    } else {
        step = (x_leave - tab->ub_ext[leaving_var]) / pivot;
    }

    double obj_delta = 0.0;

    /* Update primal solution and objective. */
    for (int k = 0; k < tab->m; k++) {
        int basic_var = tab->basis[k];
        double dx = -step * tab->work3[k];
        tab->x[basic_var] += dx;
        obj_delta += tab->c_ext[basic_var] * dx;
    }

    /* Update entering variable.
     * When entering is at lower bound and alpha < 0 (increases leaving), step > 0
     * When entering is at upper bound and alpha > 0 (increases leaving), step < 0
     * So: x_entering = bound + step (works for both cases)
     */
    double x_enter_old = tab->x[entering];
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        tab->x[entering] = tab->lb_ext[entering] + step;
    } else {
        tab->x[entering] = tab->ub_ext[entering] + step;
    }
    obj_delta += tab->c_ext[entering] * (tab->x[entering] - x_enter_old);

    /* Update basis */
    tab->basis[leaving] = entering;
    tab->basis_pos[entering] = leaving;
    tab->basis_pos[leaving_var] = -1;

    tab->var_status[entering] = RALPH_BASIC;

    /* Set leaving variable to the bound it was violating.
     * In dual simplex, the leaving variable was selected because it violated a bound:
     * - If x_leave < lb, it should go to lb
     * - If x_leave > ub, it should go to ub
     * The new var_status is determined by which bound it goes to.
     */
    if (x_leave < tab->lb_ext[leaving_var]) {
        /* Was below lower bound - go to lower bound */
        tab->var_status[leaving_var] = RALPH_NONBASIC_LOWER;
        double x_leave_after_update = tab->x[leaving_var];
        tab->x[leaving_var] = tab->lb_ext[leaving_var];
        obj_delta += tab->c_ext[leaving_var] * (tab->x[leaving_var] - x_leave_after_update);
    } else {
        /* Was above upper bound - go to upper bound */
        tab->var_status[leaving_var] = RALPH_NONBASIC_UPPER;
        double x_leave_after_update = tab->x[leaving_var];
        tab->x[leaving_var] = tab->ub_ext[leaving_var];
        obj_delta += tab->c_ext[leaving_var] * (tab->x[leaving_var] - x_leave_after_update);
    }
    tab->obj_value += obj_delta;
    (void)rc_entering_orig;  /* Suppress unused warning */

    /* DSE weight update (P6): must happen before LU update (uses old B^{-1}).
     * w_i_new = w_i - 2*(d_i/d_r)*sigma_i + (d_i/d_r)^2 * w_r
     * where sigma = B^{-1} * pi, pi = work2 (pivot row = B^{-T} * e_r) */
    if (tab->dse_initialized) {
        double w_r = tab->dse_weights[leaving];
        vec_copy_data(tab->pivot_row, tab->work2, tab->m);
        {
            double t_ftran_ms = lp_telemetry_timer_start();
            lu_solve(tab->lu, tab->pivot_row, tab->tau_work);
            if (tab->owner) {
                lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
            }
        }

        double pivot_inv = 1.0 / pivot;
        for (int k = 0; k < tab->m; k++) {
            double d_k = tab->work3[k];
            double sigma_k = tab->tau_work[k];
            double ratio_k = d_k * pivot_inv;
            double w_new = tab->dse_weights[k]
                         - 2.0 * ratio_k * sigma_k
                         + ratio_k * ratio_k * w_r;
            tab->dse_weights[k] = (w_new < 1e-8) ? 1e-8 : w_new;
        }
        double w_enter = w_r * pivot_inv * pivot_inv;
        tab->dse_weights[leaving] = (w_enter < 1e-8) ? 1e-8 : w_enter;
    }

    /* Update LU factorization */
    if (force_refactor) {
        if (tab->owner) {
            lp_telemetry_record_dual_lu_hard_trigger(tab->owner);
            if (fabs(pivot) < hard_refactor_floor) {
                lp_telemetry_set_refactor_next_reason(tab->owner, RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT);
            } else {
                lp_telemetry_set_refactor_next_reason(tab->owner, RALPH_REFACTOR_REASON_PERIODIC);
            }
        }
        double t_refactor_ms = lp_telemetry_timer_start();
        int rc_ref = tableau_refactorize(tab);
        if (tab->owner) {
            lp_telemetry_add_refactor_runtime_timed(tab->owner, t_refactor_ms);
        }
        if (rc_ref != 0) {
            goto pivot_fail_rollback;
        }
    } else {
        sparse_get_column(tab->A_ext, entering, tab->work1);
        {
            double t_lu_update_ms = lp_telemetry_timer_start();
            int rc_upd = lu_update(tab->lu, leaving, tab->work1);
            if (tab->owner) {
                lp_telemetry_add_lu_update_timed(tab->owner, t_lu_update_ms);
            }
            if (rc_upd != 0) {
                if (tab->owner) {
                    lp_telemetry_record_dual_lu_hard_trigger(tab->owner);
                    lp_telemetry_set_refactor_next_reason(tab->owner, RALPH_REFACTOR_REASON_UPDATE_RECOVERY);
                }
                if (!full_rollback_snapshot) {
                    goto pivot_fail_rollback;
                }
                double t_refactor_ms = lp_telemetry_timer_start();
                int rc_ref = tableau_refactorize(tab);
                if (tab->owner) {
                    lp_telemetry_add_refactor_runtime_timed(tab->owner, t_refactor_ms);
                }
                if (rc_ref != 0) {
                    goto pivot_fail_rollback;
                }
            }
        }
    }

    return 0;

pivot_fail_rollback:
    if (full_rollback_snapshot) {
        memcpy(tab->x, x_backup, (size_t)tab->n * sizeof(double));
        memcpy(tab->rc, rc_backup, (size_t)tab->n * sizeof(double));
        memcpy(tab->basis, basis_backup, (size_t)tab->m * sizeof(int));
        memcpy(tab->basis_pos, basis_pos_backup, (size_t)tab->n * sizeof(int));
        memcpy(tab->var_status, status_backup, (size_t)tab->n * sizeof(VarStatus));
    } else {
        tab->basis[leaving] = leaving_var;
        tab->basis_pos[leaving_var] = leaving;
        tab->basis_pos[entering] = -1;
        tab->var_status[entering] = entering_status_before;
        tab->var_status[leaving_var] = leaving_status_before;
        tab->duals_valid = 0;
        tab->rc_all_valid = 0;
        tab->dual_cand_valid = 0;
    }
    tab->obj_value = saved_obj;
    return -1;
}

/* [Phase E] dual_simplex_solve() deleted — replaced by dual_simplex_solve_v2() */

/*
 * Repair ratio test for dual Phase-1 rescue on an existing primal Phase-1
 * tableau.  Unlike clean dual simplex, this rescue path does not assume global
 * dual feasibility; it only needs a numerically valid entering column that moves
 * the selected infeasible basic variable toward its violated bound.  Keeping
 * this selector separate prevents non-dual-preserving repair pivots from being
 * used as infeasibility certificates in clean dual Phase 2.
 */
static int dual_phase1_rescue_ratio_test(SimplexTableau *tab,
                                         int leaving,
                                         int *entering,
                                         double *theta) {
    if (!tab || !entering || !theta || leaving < 0 || leaving >= tab->m) {
        return -1;
    }

    int leaving_var = tab->basis[leaving];
    double x_leave = tab->x[leaving_var];
    int dir;

    if (x_leave < tab->lb_ext[leaving_var] - RALPH_FEAS_TOL) {
        dir = 1;
    } else if (x_leave > tab->ub_ext[leaving_var] + RALPH_FEAS_TOL) {
        dir = -1;
    } else {
        return -1;
    }

    {
        int rhs_idx = leaving;
        double rhs_val = 1.0;
        lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, tab->work2);
    }

    *entering = -1;
    *theta = RALPH_INFINITY;
    int scan_dir = dual_ratio_scan_direction(tab);

    for (int t = 0; t < tab->n; t++) {
        int j = (scan_dir > 0) ? t : (tab->n - 1 - t);
        if (tab->var_status[j] == RALPH_BASIC || dual_smcp_excl_skip_var(tab, j)) {
            continue;
        }

        double alpha = sparse_dot_column(tab->A_ext, j, tab->work2);
        if (!isfinite(alpha) || fabs(alpha) < RALPH_PIVOT_TOL) {
            continue;
        }

        double rc = tab->rc[j];
        if (!isfinite(rc)) {
            continue;
        }

        double ratio = RALPH_INFINITY;

        if (dir > 0 && alpha < -RALPH_PIVOT_TOL &&
            tab->var_status[j] == RALPH_NONBASIC_LOWER) {
            ratio = -rc / alpha;
        } else if (dir > 0 && alpha > RALPH_PIVOT_TOL &&
                   tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            ratio = -rc / alpha;
        } else if (dir < 0 && alpha > RALPH_PIVOT_TOL &&
                   tab->var_status[j] == RALPH_NONBASIC_LOWER) {
            ratio = rc / alpha;
        } else if (dir < 0 && alpha < -RALPH_PIVOT_TOL &&
                   tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            ratio = rc / alpha;
        }

        if (!isfinite(ratio)) {
            continue;
        }

        if (ratio >= -RALPH_OPT_TOL && ratio < *theta) {
            *theta = ratio;
            *entering = j;
        }
    }

    return (*entering >= 0) ? 0 : -1;
}

static int phase1_rescue_has_bad_numerics(const SimplexTableau *tab) {
    if (!tab) return 1;

    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (!isfinite(tab->x[j])) {
            return 1;
        }
    }

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;
        if (!isfinite(tab->rc[j])) {
            return 1;
        }
    }

    return 0;
}

/*
 * Phase-1 rescue using dual simplex pivots on an existing tableau.
 *
 * This is intentionally limited and self-contained:
 * - no fallback to primal simplex (avoids recursion from simplex_phase1)
 * - no status mutation (caller decides terminal behavior)
 * - returns 0 only when primal feasibility is restored for current tableau
 */
int dual_simplex_phase1_rescue(SimplexSolver *solver, int max_iters) {
    if (!solver || !solver->tableau) return -1;

    SimplexTableau *tab = solver->tableau;
    if (tab->phase != 1) return -1;
    configure_dual_tableau_for_solver(solver, tab);

    if (max_iters <= 0) {
        max_iters = 3 * tab->m;
    }

    /* Start from a clean factorization when possible, but do not hard-fail
     * rescue on a single refactorization error. */
    if (tableau_refactorize(tab) != 0 && solver->verbose >= 2) {
        LP_LOG_STDERR("[dual_phase1_rescue] Initial refactorization failed, trying in-place recovery pivots\n");
    }
    phase1_rescue_compute_solution(tab);
    phase1_rescue_compute_reduced_costs(tab);

    /* Try to improve dual feasibility via bound flips first. */
    int changes = make_dual_feasible(tab,
                                     solver->model ? solver->model->obj_sense : 1,
                                     dual_allow_startup_bound_flip(solver));
    if (changes > 0) {
        phase1_rescue_compute_solution(tab);
        phase1_rescue_compute_reduced_costs(tab);
    }

    unsigned char *tried_rows = (unsigned char*)calloc((size_t)tab->m, sizeof(unsigned char));
    if (!tried_rows) {
        return 1;
    }

    const int MAX_REFACTOR_FAILURES = RALPH_PHASE1_RESCUE_MAX_REFACTOR_FAILURES;
    int refactor_failures = 0;
    int rescue_refactor_base_interval = dual_refactor_base_interval_for_solver(solver) / 2;
    int rescue_rc_recompute_interval;
    int rescue_progress_stall_limit;
    int rescue_progress_best_rows = -1;
    int rescue_progress_stall_count = 0;
    double rescue_progress_best_max = 0.0;
    double rescue_progress_best_sum = 0.0;
    if (rescue_refactor_base_interval < 8) rescue_refactor_base_interval = 8;
    rescue_rc_recompute_interval =
        dual_rc_recompute_interval_for_solver(solver, rescue_refactor_base_interval) / 2;
    if (rescue_rc_recompute_interval < 5) rescue_rc_recompute_interval = 5;
    rescue_progress_stall_limit = dual_phase1_rescue_progress_limit(tab->m);
    DualRefactorQualityState quality;
    dual_quality_init(&quality);

    for (int iter = 0; iter < max_iters; iter++) {
        if (dual_time_limit_exceeded(solver, iter)) {
            free(tried_rows);
            return 1;
        }
        phase1_rescue_compute_solution(tab);

        if (phase1_rescue_has_bad_numerics(tab)) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[dual_phase1_rescue] Non-finite x/rc at iter %d, trying refactorization repair\n", iter);
            }
            if (tableau_refactorize(tab) == 0) {
                refactor_failures = 0;
                dual_quality_on_refactor(&quality, iter);
                phase1_rescue_compute_solution(tab);
                phase1_rescue_compute_reduced_costs(tab);
                continue;
            }
            refactor_failures++;
            if (refactor_failures >= MAX_REFACTOR_FAILURES) {
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[dual_phase1_rescue] Aborting after repeated non-finite recovery failures\n");
                }
                free(tried_rows);
                return 1;
            }
            continue;
        }

        int has_infeasible = 0;
        int infeasible_rows = 0;
        double max_infeas = 0.0;
        double sum_infeas = 0.0;
        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            double infeas = 0.0;
            if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL ||
                tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
                has_infeasible = 1;
                if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL) {
                    infeas = tab->lb_ext[j] - tab->x[j];
                } else {
                    infeas = tab->x[j] - tab->ub_ext[j];
                }
                infeasible_rows++;
                sum_infeas += infeas;
                if (infeas > max_infeas) {
                    max_infeas = infeas;
                }
            }
        }
        if (!has_infeasible) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[dual_phase1_rescue] Primal feasibility restored after %d iterations\n", iter);
            }
            free(tried_rows);
            return 0;
        }
        if (dual_phase1_rescue_progress_update_for_test(
                infeasible_rows,
                max_infeas,
                sum_infeas,
                rescue_progress_stall_limit,
                &rescue_progress_best_rows,
                &rescue_progress_best_max,
                &rescue_progress_best_sum,
                &rescue_progress_stall_count)) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[dual_phase1_rescue] Aborting after %d stalled rescue iterations without primal infeasibility progress (rows=%d max=%.3e sum=%.3e)\n",
                        rescue_progress_stall_count,
                        infeasible_rows,
                        max_infeas,
                        sum_infeas);
            }
            free(tried_rows);
            return 1;
        }

        int leaving = -1;
        int entering = -1;
        double theta = RALPH_INFINITY;
        memset(tried_rows, 0, (size_t)tab->m * sizeof(unsigned char));

        /* Try multiple infeasible leaving rows to avoid getting stuck on a
         * single row with no stable entering candidate. */
        int flip_only_step = 0;
        for (int attempt = 0; attempt < tab->m; attempt++) {
            int candidate = -1;
            double max_infeas = RALPH_FEAS_TOL;

            for (int k = 0; k < tab->m; k++) {
                if (tried_rows[k]) continue;

                int j = tab->basis[k];
                double infeas = 0.0;
                if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL) {
                    infeas = tab->lb_ext[j] - tab->x[j];
                } else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
                    infeas = tab->x[j] - tab->ub_ext[j];
                }

                if (infeas > max_infeas) {
                    max_infeas = infeas;
                    candidate = k;
                }
            }

            if (candidate < 0) {
                break;
            }

            tried_rows[candidate] = 1;

            if (dual_phase1_rescue_ratio_test(tab, candidate, &entering, &theta) == 0 &&
                entering >= 0) {
                leaving = candidate;
                break;
            } else if (entering == -2) {
                flip_only_step = 1;
                break;
            }
        }

        if (flip_only_step) {
            dual_quality_record_ratio_success(&quality, -2, 0.0);
            continue;
        }

        if (leaving < 0 || entering < 0) {
            /* No valid repair pivot found for remaining infeasible rows. */
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[dual_phase1_rescue] No valid entering column for infeasible rows at iter %d\n", iter);
            }
            free(tried_rows);
            return 1;
        }

        dual_quality_record_ratio_success(&quality, entering, theta);

        if (dual_simplex_pivot(tab, entering, leaving, theta, 1, 0) != 0) {
            dual_quality_record_pivot_failure(&quality);
            if (tableau_refactorize(tab) != 0) {
                refactor_failures++;
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[dual_phase1_rescue] Pivot/refactor recovery failed at iter %d (count=%d)\n",
                            iter, refactor_failures);
                }
                if (refactor_failures >= MAX_REFACTOR_FAILURES) {
                    free(tried_rows);
                    return 1;
                }
            } else {
                refactor_failures = 0;
                dual_quality_on_refactor(&quality, iter);
            }
            phase1_rescue_compute_solution(tab);
            phase1_rescue_compute_reduced_costs(tab);
            continue;
        }

        /* Keep numerics under control during rescue. */
        int need_refactor = dual_governor_refactor_decision(solver,
                                                            tab,
                                                            &quality,
                                                            iter,
                                                            rescue_refactor_base_interval,
                                                            0,
                                                            0,
                                                            0);
        if (need_refactor) {
            if (tableau_refactorize(tab) != 0) {
                refactor_failures++;
                if (solver->verbose >= 2) {
                    LP_LOG_STDERR("[dual_phase1_rescue] Periodic refactor failed at iter %d (count=%d)\n",
                            iter, refactor_failures);
                }
                if (refactor_failures >= MAX_REFACTOR_FAILURES) {
                    free(tried_rows);
                    return 1;
                }
            } else {
                refactor_failures = 0;
                dual_quality_on_refactor(&quality, iter);
            }
            phase1_rescue_compute_solution(tab);
            phase1_rescue_compute_reduced_costs(tab);
        } else if (iter > 0 && iter % rescue_rc_recompute_interval == 0) {
            phase1_rescue_compute_reduced_costs(tab);
        }
    }

    free(tried_rows);
    return 1;
}

/* ============================================================================
 * True Dual Phase 1 - Initialize for Dual Simplex from Scratch
 * ============================================================================ */

/*
 * Initialize tableau for dual simplex by achieving dual feasibility.
 *
 * The reduced costs stored in tab->rc are for the INTERNAL minimization problem.
 * For dual feasibility of the internal minimization:
 *   - Variables at lower bound need rc >= 0
 *   - Variables at upper bound need rc <= 0
 *
 * Strategy: Flip non-basic variables to the bound that satisfies dual feasibility.
 * After flipping, basic variable values are recomputed and may become infeasible,
 * which dual Phase 2 will fix.
 */
int make_dual_feasible(SimplexTableau *tab, int obj_sense, int allow_bound_flip) {
    (void)obj_sense;  /* Not needed - rc is already for internal minimization */

    /* Compute reduced costs with current basis */
    tableau_compute_reduced_costs(tab);

    int changes = 0;
    if (!allow_bound_flip) return changes;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        double rc = tab->rc[j];
        double lb = tab->lb_ext[j];
        double ub = tab->ub_ext[j];

        /*
         * For internal minimization:
         * - At lower bound: need rc >= 0 (else variable wants to increase)
         * - At upper bound: need rc <= 0 (else variable wants to decrease)
         */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            /* Dual infeasible at lower bound, try to flip to upper */
            if (ub < RALPH_INFINITY/2) {
                tab->x[j] = ub;
                tab->var_status[j] = RALPH_NONBASIC_UPPER;
                changes++;
            }
            /* else: can't flip, will need Phase 1 pivots to fix */
        }
        else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            /* Dual infeasible at upper bound, try to flip to lower */
            if (lb > -RALPH_INFINITY/2) {
                tab->x[j] = lb;
                tab->var_status[j] = RALPH_NONBASIC_LOWER;
                changes++;
            }
            /* else: can't flip, will need Phase 1 pivots to fix */
        }
        /* Free non-basic variables (RALPH_NONBASIC_FREE) with rc != 0 are dual
         * infeasible but cannot be fixed by bound flipping.  They require basis
         * pivots to enter the basis (where rc becomes irrelevant).  Dual Phase 1
         * handles these residual infeasibilities. */
    }

    if (changes > 0 && tab->owner) {
        lp_telemetry_record_dual_bound_flip_applied_startup(tab->owner, changes);
    }

    return changes;
}

/*
 * Bound Perturbation for Degeneracy Prevention
 *
 * Adds small perturbations to upper bounds to break degeneracy and prevent
 * cycling. Uses pseudo-random perturbations based on variable index to ensure
 * reproducibility. Perturbations are removed before returning the final solution.
 *
 * Uses per-tableau storage in work4 array instead of static storage to be
 * safe for concurrent use and multiple tableaux.
 */
#define PERTURB_BASE 1e-4  /* Larger perturbation to break cycles more aggressively */
#define PERTURB_MULT 7  /* Prime for pseudo-randomness */

static void apply_bound_perturbation(SimplexTableau *tab) {
    if (!dual_smcp_shift_allows_perturb(tab)) return;
    /* Allocate backup storage and save original bounds only on FIRST call.
     * Re-perturbation (for cycling) adds more perturbation but must
     * NOT overwrite the backup — remove_bound_perturbation must always restore
     * to the original (unperturbed) bounds. */
    int is_mip = (tab->model && tab->model->num_integers > 0);
    int fresh = 0;
    if (!tab->perturb_backup) {
        tab->perturb_backup = (double*)calloc(tab->n, sizeof(double));
        tab->perturb_backup_lb = (double*)calloc(tab->n, sizeof(double));
        if (!tab->perturb_backup || !tab->perturb_backup_lb) {
            SAFE_FREE(tab->perturb_backup);
            SAFE_FREE(tab->perturb_backup_lb);
            return;
        }
        fresh = 1;
    }

    /* Progressive scaling: scale > 1.0 for re-perturbation attempts to break
     * different cycling patterns. Default perturb_scale = 1.0 (set in tableau_create_ex). */
    double scale = (tab->perturb_scale > 0.0) ? tab->perturb_scale : 1.0;
    double base = PERTURB_BASE * scale;

    for (int j = 0; j < tab->n; j++) {
        if (fresh) {
            tab->perturb_backup[j] = tab->ub_ext[j];
            tab->perturb_backup_lb[j] = tab->lb_ext[j];
        }

        /* Perturb finite upper bounds (widen interval) */
        if (tab->ub_ext[j] < RALPH_INFINITY / 2) {
            double eps = base * (1.0 + fabs(tab->ub_ext[j]));
            tab->ub_ext[j] += eps * (1.0 + (j * PERTURB_MULT) % 13);
        }

        /* W5: Perturb finite lower bounds (widen interval, different prime
         * pattern to avoid correlation with UB perturbation).
         * Skip for MIP LP relaxations — LB perturbation weakens the relaxation
         * and causes suboptimal branching/cuts. Only apply for pure LP solves. */
        if (!is_mip && tab->lb_ext[j] > -RALPH_INFINITY / 2) {
            double eps = base * (1.0 + fabs(tab->lb_ext[j]));
            tab->lb_ext[j] -= eps * (1.0 + (j * 11) % 17);
        }

        if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            tab->x[j] = tab->ub_ext[j];
        } else if (tab->var_status[j] == RALPH_NONBASIC_LOWER) {
            tab->x[j] = tab->lb_ext[j];
        }
    }
}

static void remove_bound_perturbation(SimplexTableau *tab) {
    if (!tab->perturb_backup) return;

    for (int j = 0; j < tab->n; j++) {
        tab->ub_ext[j] = tab->perturb_backup[j];

        /* W5: Restore lower bounds too */
        if (tab->perturb_backup_lb) {
            tab->lb_ext[j] = tab->perturb_backup_lb[j];
        }

        /* Snap non-basic variables to their original bounds */
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            tab->x[j] = tab->perturb_backup[j];
        } else if (tab->var_status[j] == RALPH_NONBASIC_LOWER && tab->perturb_backup_lb) {
            tab->x[j] = tab->perturb_backup_lb[j];
        }
    }
}

static void dual_perturb_state_activate(SimplexTableau *tab,
                                        int *state_io,
                                        int reapply) {
    int state = (state_io ? *state_io : LP_GLPK_PERTURB_STATE_OFF);
    int next = state;
    int event = reapply ? LP_GLPK_PERTURB_EVENT_REAPPLY
                        : LP_GLPK_PERTURB_EVENT_ENABLE;
    (void)lp_policy_glpk_perturb_next_state(state, event, &next);
    apply_bound_perturbation(tab);
    if (tab && tab->perturb_backup) {
        state = next;
    } else {
        state = LP_GLPK_PERTURB_STATE_OFF;
    }
    if (state_io) *state_io = state;
}

static void dual_perturb_state_begin_cleanup(SimplexTableau *tab, int *state_io) {
    int state = (state_io ? *state_io : LP_GLPK_PERTURB_STATE_OFF);
    int next = state;
    if (tab && tab->perturb_backup) {
        if (lp_policy_glpk_perturb_next_state(state,
                                              LP_GLPK_PERTURB_EVENT_BEGIN_CLEANUP,
                                              &next)) {
            state = next;
        }
        remove_bound_perturbation(tab);
    } else {
        state = LP_GLPK_PERTURB_STATE_OFF;
    }
    if (state_io) *state_io = state;
}

static void dual_perturb_state_disable(SimplexTableau *tab, int *state_io) {
    int state = (state_io ? *state_io : LP_GLPK_PERTURB_STATE_OFF);
    int next = state;
    if (tab && tab->perturb_backup) {
        remove_bound_perturbation(tab);
    }
    if (lp_policy_glpk_perturb_next_state(state,
                                          LP_GLPK_PERTURB_EVENT_DISABLE,
                                          &next)) {
        state = next;
    } else {
        state = LP_GLPK_PERTURB_STATE_OFF;
    }
    if (state_io) *state_io = state;
}

/* Clear stale perturbation backup before warm-starting v2.
 * After branching, bounds change between nodes. apply_bound_perturbation()
 * saves the FIRST bounds to backup — if bounds changed, the backup is stale.
 * Freeing it ensures the next v2 call saves the correct (updated) bounds. */
void dual_v2_clear_perturbation(SimplexTableau *tab) {
    if (!tab) return;
    SAFE_FREE(tab->perturb_backup);
    SAFE_FREE(tab->perturb_backup_lb);
}

/* ============================================================================
 * Clean Dual Phase 2 (T1.3) — No primal fallbacks
 *
 * Takes a dual-feasible tableau, runs dual simplex to primal feasibility.
 * Returns: 0=OPTIMAL, 1=INFEASIBLE, -1=FAILED (caller decides fallback)
 * ============================================================================ */

int dual_simplex_solve_v2(SimplexSolver *solver) {
    if (!solver || !solver->tableau) return -1;
    solver->current_phase = SIMPLEX_PHASE_1;
    if (!lp_basis_governor_mode_is_valid(solver->policy.basis_governor_mode)) {
        solver->policy.basis_governor_mode = LP_BASIS_GOV_MODE_OFF;
    }
    lp_basis_governor_set_mode(&solver->policy.basis_governor,
                               solver->policy.basis_governor_mode);

    SimplexTableau *tab = solver->tableau;
    configure_dual_tableau_for_solver(solver, tab);
    int n_orig = solver->model->num_vars;

    /* Apply bound perturbation for cycling prevention.
     * Cost is O(n) — cheap even for warm starts. */
    int perturb_state = LP_GLPK_PERTURB_STATE_OFF;
    dual_perturb_state_activate(tab, &perturb_state, 0);

    int use_dse = solver->use_dual_steepest_edge;

    /* Initialize DSE weights (exact for standalone solve).
     * For warm starts, caller should set dse_initialized=0 to force reinit,
     * or leave it if weights are still approximately valid from previous solve. */
    if (use_dse && !tab->dse_initialized) {
        dse_init_exact(tab);
    }

    /* Stalling/degeneracy tracking */
    int degenerate_count = 0;
    const int DEGEN_PERTURB_THRESHOLD = 15;
    int stall_count = 0;
    const int STALL_THRESHOLD = 50;
    int perturb_attempts = 0;
    const int MAX_PERTURB_ATTEMPTS = 20;
    int dual_recovery_used = 0;
    int lu_hard_start = solver->telemetry.perf_dual_lu_hard_trigger;
    int dual_refactor_base_interval = dual_refactor_base_interval_for_solver(solver);
    int dual_rc_recompute_interval =
        dual_rc_recompute_interval_for_solver(solver, dual_refactor_base_interval);
    DualRefactorQualityState quality;
    dual_quality_init(&quality);

    /* Compute primal solution once before entering the main loop.
     * After this, dual_simplex_pivot() maintains x incrementally via
     * x_B -= step * d.  Full recomputation only after refactorization. */
    tableau_compute_solution(tab);
    double last_obj = tab->obj_value;

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        solver->iterations = iter;
        if (dual_run_user_callbacks(solver, tab, iter, 0, 1) != 0) {
            dual_perturb_state_disable(tab, &perturb_state);
            solver->status = RALPH_STATUS_TIME_LIMIT;
            solver->iterations = iter;
            return -1;
        }

        /* T3.1: Objective limit early-exit (internal minimization space) */
        if (solver->objective_limit < RALPH_INFINITY &&
            tab->obj_value >= solver->objective_limit) {
            dual_perturb_state_disable(tab, &perturb_state);
            tableau_compute_solution(tab);
            solver->status = RALPH_STATUS_OBJ_LIMIT;
            solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
            dual_run_user_callbacks(solver, tab, iter, 1, 0);
            return 0;
        }

        /* Find leaving variable: DSE scoring or most-infeasible */
        int leaving = -1;

        if (use_dse && tab->dse_initialized) {
            double best_score = 0.0;
            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                double infeas = 0.0;
                if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL)
                    infeas = tab->lb_ext[j] - tab->x[j];
                else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL)
                    infeas = tab->x[j] - tab->ub_ext[j];
                if (infeas <= RALPH_FEAS_TOL) continue;

                double w = tab->dse_weights[k];
                double score = (infeas * infeas) / w;
                if (score > best_score) {
                    best_score = score;
                    leaving = k;
                }
            }
        } else {
            double max_infeas = RALPH_FEAS_TOL;
            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                double infeas = 0.0;
                if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL)
                    infeas = tab->lb_ext[j] - tab->x[j];
                else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL)
                    infeas = tab->x[j] - tab->ub_ext[j];
                if (infeas > max_infeas) {
                    max_infeas = infeas;
                    leaving = k;
                }
            }
        }

        if (leaving < 0) {
            /* Primal feasible under (possibly perturbed) bounds.
             * Remove perturbation and check if still feasible. */
            if (solver->verbose >= 2) {
                LP_LOG_STDOUT("[dual_v2] leaving<0 at iter %d, perturb_backup=%s\n",
                       iter, tab->perturb_backup ? "yes" : "no");
            }
            if (tab->perturb_backup) {
                dual_perturb_state_begin_cleanup(tab, &perturb_state);
                /* Snap non-basic variables to restored (unperturbed) bounds.
                 * remove_bound_perturbation restores lb_ext/ub_ext but x[j]
                 * still holds the perturbed value (e.g. 1+ε instead of 1).
                 * Without this, obj_value is computed from stale x values. */
                for (int j = 0; j < tab->n; j++) {
                    if (tab->var_status[j] == RALPH_NONBASIC_UPPER)
                        tab->x[j] = tab->ub_ext[j];
                    else if (tab->var_status[j] == RALPH_NONBASIC_LOWER)
                        tab->x[j] = tab->lb_ext[j];
                }
                /* Refactorize for accurate solution after perturbation removal.
                 * Eta-file drift at high condition numbers causes the basis
                 * to appear feasible when it isn't. */
                {
                    double t_refactor_ms = lp_telemetry_timer_start();
                    int rc_ref = tableau_refactorize(tab);
                    lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
                    if (rc_ref == 0) {
                        dual_quality_on_refactor(&quality, iter);
                    }
                }
                tab->dse_initialized = 0;
                if (use_dse) dse_init_approx(tab);
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);

                /* Cleanup: continue dual pivots without perturbation
                 * until primal feasibility is restored under original bounds.
                 * This is the standard "unshift" procedure (CLP, GLOP). */
                int cleanup_iters = 0;
                const int MAX_CLEANUP = 200;
                while (cleanup_iters < MAX_CLEANUP) {
                    /* Find a primal-infeasible basic variable */
                    int cl_leaving = -1;
                    if (use_dse && tab->dse_initialized) {
                        double best_score = 0.0;
                        for (int k = 0; k < tab->m; k++) {
                            int j = tab->basis[k];
                            double infeas = 0.0;
                            if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL)
                                infeas = tab->lb_ext[j] - tab->x[j];
                            else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL)
                                infeas = tab->x[j] - tab->ub_ext[j];
                            if (infeas <= RALPH_FEAS_TOL) continue;
                            double w = tab->dse_weights[k];
                            double score = (infeas * infeas) / w;
                            if (score > best_score) {
                                best_score = score;
                                cl_leaving = k;
                            }
                        }
                    } else {
                        double max_infeas = RALPH_FEAS_TOL;
                        for (int k = 0; k < tab->m; k++) {
                            int j = tab->basis[k];
                            double infeas = 0.0;
                            if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL)
                                infeas = tab->lb_ext[j] - tab->x[j];
                            else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL)
                                infeas = tab->x[j] - tab->ub_ext[j];
                            if (infeas > max_infeas) {
                                max_infeas = infeas;
                                cl_leaving = k;
                            }
                        }
                    }

                    if (cl_leaving < 0) {
                        if (solver->verbose >= 2) {
                            /* Double-check: scan for any violations */
                            double max_v = 0.0;
                            for (int k = 0; k < tab->m; k++) {
                                int j = tab->basis[k];
                                double v = 0.0;
                                if (tab->x[j] < tab->lb_ext[j] - 1e-12)
                                    v = tab->lb_ext[j] - tab->x[j];
                                else if (tab->x[j] > tab->ub_ext[j] + 1e-12)
                                    v = tab->x[j] - tab->ub_ext[j];
                                if (v > max_v) max_v = v;
                            }
                            LP_LOG_STDOUT("[dual_v2] Cleanup: no leaving found, max_basic_infeas=%.6e\n", max_v);
                        }
                        break;  /* Truly feasible now */
                    }

                    int cl_entering;
                    double cl_theta;
                    {
                        double t_ratio_ms = lp_telemetry_timer_start();
                        int rc_ratio = dual_ratio_test(tab, cl_leaving, &cl_entering, &cl_theta);
                        lp_telemetry_record_ratio_timed(solver, 0, t_ratio_ms);
                        if (rc_ratio != 0) {
                            dual_quality_record_ratio_failure(&quality);
                            /* Infeasible after unshift — should not happen, bail */
                            break;
                        }
                    }
                    if (cl_entering == -2) {
                        dual_quality_record_ratio_success(&quality, -2, 0.0);
                        cleanup_iters++;
                        solver->iterations++;
                        continue;
                    }
                    dual_quality_record_ratio_success(&quality, cl_entering, cl_theta);
                    {
                        double t_pivot_ms = lp_telemetry_timer_start();
                        int rc_pivot = dual_simplex_pivot(tab, cl_entering, cl_leaving, cl_theta,
                                                          1, dual_ratio_use_row_kernel(tab));
                        lp_telemetry_record_pivot_timed(solver, 0, t_pivot_ms);
                        if (rc_pivot != 0) {
                            dual_quality_record_pivot_failure(&quality);
                            double t_refactor_ms = lp_telemetry_timer_start();
                            int rc_ref = tableau_refactorize(tab);
                            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
                            if (rc_ref != 0) break;
                            dual_quality_on_refactor(&quality, iter + cleanup_iters);
                            tab->dse_initialized = 0;
                            if (use_dse) dse_init_approx(tab);
                            tableau_compute_solution(tab);
                            tableau_compute_reduced_costs(tab);
                        }
                    }
                    cleanup_iters++;
                    solver->iterations++;

                    /* Governed refactorization during unshift cleanup. */
                    int need_cleanup_refactor = dual_governor_refactor_decision(
                        solver,
                        tab,
                        &quality,
                        iter + cleanup_iters,
                        dual_refactor_base_interval,
                        0,
                        0,
                        0);
                    if (need_cleanup_refactor) {
                        double t_refactor_ms = lp_telemetry_timer_start();
                        int rc_ref = tableau_refactorize(tab);
                        lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
                        if (rc_ref != 0) break;
                        dual_quality_on_refactor(&quality, iter + cleanup_iters);
                        tab->dse_initialized = 0;
                        if (use_dse) dse_init_approx(tab);
                        tableau_compute_solution(tab);
                        tableau_compute_reduced_costs(tab);
                    }
                }

                if (solver->verbose && cleanup_iters > 0) {
                    LP_LOG_STDOUT("[dual_v2] Unshift cleanup: %d pivots\n", cleanup_iters);
                }

                /* Recompute solution after cleanup */
                tableau_compute_solution(tab);
            }

            /* Fresh reduced cost computation + dual feasibility check.
             * After unshift cleanup or direct termination, verify rc signs
             * to catch suboptimal termination (stale rc from perturbed pivots). */
            tableau_compute_reduced_costs(tab);
            {
                double max_dual_viol = dual_max_reduced_cost_violation(tab);
                if (max_dual_viol > 1e-4) {
                    if (solver->verbose) {
                        LP_LOG_STDOUT("[dual_v2] Suboptimal: max dual violation %.2e after unshift\n",
                               max_dual_viol);
                    }
                    dual_perturb_state_disable(tab, &perturb_state);
                    solver->status = RALPH_STATUS_ERROR;
                    return -1;  /* Trigger primal fallback */
                }
            }

            solver->status = RALPH_STATUS_OPTIMAL;
            solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;

            if (!solver->solution)
                solver->solution = (double*)calloc(n_orig, sizeof(double));
            if (solver->solution) {
                for (int j = 0; j < n_orig; j++)
                    solver->solution[j] = tab->x[j];
            }
            dual_perturb_state_disable(tab, &perturb_state);
            dual_run_user_callbacks(solver, tab, iter, 1, 0);
            return 0;
        }

        /* Dual ratio test */
        int entering;
        double theta;

        {
            double t_ratio_ms = lp_telemetry_timer_start();
            int rc_ratio = dual_ratio_test(tab, leaving, &entering, &theta);
            lp_telemetry_record_ratio_timed(solver, 0, t_ratio_ms);
            if (rc_ratio != 0) {
                dual_quality_record_ratio_failure(&quality);
                if (dual_try_one_shot_recovery(solver,
                                               tab,
                                               use_dse,
                                               lu_hard_start,
                                               &dual_recovery_used,
                                               iter,
                                               "ratio_no_entering")) {
                    continue;
                }
                tableau_compute_reduced_costs(tab);
                if (dual_max_reduced_cost_violation(tab) > RALPH_OPT_TOL) {
                    dual_perturb_state_disable(tab, &perturb_state);
                    solver->status = RALPH_STATUS_ERROR;
                    return -1;
                }
                /* No entering variable — problem is infeasible */
                dual_perturb_state_disable(tab, &perturb_state);
                extract_farkas_ray_dual(solver);
                solver->status = RALPH_STATUS_INFEASIBLE;
                dual_run_user_callbacks(solver, tab, iter, 1, 0);
                return 1;
            }
        }

        if (entering == -2) {
            dual_quality_record_ratio_success(&quality, -2, 0.0);
            continue;
        }
        dual_quality_record_ratio_success(&quality, entering, theta);

        /* Perform dual pivot */
        {
            double t_pivot_ms = lp_telemetry_timer_start();
            int rc_pivot = dual_simplex_pivot(tab, entering, leaving, theta,
                                              1, dual_ratio_use_row_kernel(tab));
            lp_telemetry_record_pivot_timed(solver, 0, t_pivot_ms);
            if (rc_pivot != 0) {
                dual_quality_record_pivot_failure(&quality);
                double t_refactor_ms = lp_telemetry_timer_start();
                int rc_ref = tableau_refactorize(tab);
                lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
                if (rc_ref != 0) {
                    dual_perturb_state_disable(tab, &perturb_state);
                    solver->status = RALPH_STATUS_ERROR;
                    return -1;  /* FAILED */
                }
                dual_quality_on_refactor(&quality, iter);
                tab->dse_initialized = 0;
                if (use_dse) dse_init_approx(tab);
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                continue;
            }
        }
        /* Degeneracy detection */
        if (fabs(theta) < RALPH_FEAS_TOL) {
            degenerate_count++;
            if (degenerate_count >= DEGEN_PERTURB_THRESHOLD) {
                dual_perturb_state_activate(tab, &perturb_state, 0);
                degenerate_count = 0;
            }
        } else {
            degenerate_count = 0;
        }

        /* Stalling detection */
        double obj_tol = 1e-4 * (1.0 + fabs(last_obj));
        double obj_change = fabs(tab->obj_value - last_obj);
        if (obj_change < obj_tol) {
            stall_count++;
            if (stall_count >= STALL_THRESHOLD) {
                perturb_attempts++;
                if (perturb_attempts <= MAX_PERTURB_ATTEMPTS) {
                    dual_perturb_state_disable(tab, &perturb_state);
                    tab->perturb_scale = 1.0 + 2.0 * perturb_attempts;
                    dual_perturb_state_activate(tab, &perturb_state, 1);
                    stall_count = 0;
                    if (solver->verbose) {
                        LP_LOG_STDOUT("[dual_v2] Iter %d: stalled, re-perturbing (attempt %d, scale %.1f)\n",
                               iter, perturb_attempts, tab->perturb_scale);
                    }
                } else {
                    /* Exhausted perturbation attempts — FAILED */
                    if (solver->verbose) {
                        LP_LOG_STDOUT("[dual_v2] Iter %d: stalled after %d perturb attempts, giving up\n",
                               iter, perturb_attempts);
                    }
                    dual_perturb_state_disable(tab, &perturb_state);
                    solver->status = RALPH_STATUS_ERROR;
                    return -1;
                }
            }
        } else {
            stall_count = 0;
            perturb_attempts = 0;
            last_obj = tab->obj_value;
        }

        int need_refactor = dual_governor_refactor_decision(solver,
                                                            tab,
                                                            &quality,
                                                            iter,
                                                            dual_refactor_base_interval,
                                                            degenerate_count,
                                                            stall_count,
                                                            perturb_attempts);
        if (need_refactor) {
            double t_refactor_ms = lp_telemetry_timer_start();
            int rc_ref = tableau_refactorize(tab);
            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            if (rc_ref != 0) {
                dual_perturb_state_disable(tab, &perturb_state);
                solver->status = RALPH_STATUS_ERROR;
                return -1;
            }
            dual_quality_on_refactor(&quality, iter);
            tab->dse_initialized = 0;
            if (use_dse) dse_init_approx(tab);
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        } else if (iter > 0 && iter % dual_rc_recompute_interval == 0) {
            tableau_compute_reduced_costs(tab);
        }

        if (solver->verbose && iter % 100 == 0) {
            LP_LOG_STDOUT("[dual_v2] Iter %d: obj=%.6f\n", iter, tab->obj_value);
        }
    }

    /* Exceeded max iterations — FAILED */
    dual_perturb_state_disable(tab, &perturb_state);
    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    solver->iterations = solver->max_iterations;
    dual_run_user_callbacks(solver, tab, solver->iterations, 1, 0);
    return -1;
}

/* ============================================================================
 * Dual Phase 1 (T3.5) — Achieve dual feasibility via auxiliary objective
 *
 * When make_dual_feasible() can't fix all dual infeasibilities (free variables,
 * no finite upper bound), use auxiliary objective pivots (Koberstein 2005):
 *   1. Save original c_ext[]
 *   2. Set c_aux[j] = -sign(rc[j]) for dual-infeasible non-basics, 0 otherwise
 *   3. Run dual pivots on auxiliary objective
 *   4. After each pivot, check if ORIGINAL objective is now dual feasible
 *   5. If yes: stop, restore c_ext, done. If auxiliary terminates: failed.
 * ============================================================================ */

/* Helper: count dual infeasibilities for original objective with current basis */
static int count_orig_dual_infeas(SimplexTableau *tab, const double *c_orig) {
    int m = tab->m;
    int n = tab->n;

    /* Compute y = c_orig_B' * B^{-1} using BTRAN */
    double *cb = tab->work1;
    double *y_orig = tab->work3;
    memset(cb, 0, m * sizeof(double));
    for (int k = 0; k < m; k++)
        cb[k] = c_orig[tab->basis[k]];
    lu_solve_transpose(tab->lu, cb, y_orig);

    int count = 0;
    for (int j = 0; j < n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        /* rc_orig[j] = c_orig[j] - y_orig' * a_j */
        double rc_j = c_orig[j] - sparse_dot_column(tab->A_ext, j, y_orig);

        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc_j < -RALPH_OPT_TOL)
            count++;
        else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc_j > RALPH_OPT_TOL)
            count++;
        else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc_j) > RALPH_OPT_TOL)
            count++;
    }
    return count;
}

int dual_phase1(SimplexSolver *solver) {
    if (!solver || !solver->tableau) return -1;

    SimplexTableau *tab = solver->tableau;
    configure_dual_tableau_for_solver(solver, tab);
    int n = tab->n;

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_phase1] Starting auxiliary-objective dual Phase 1...\n");
    }

    /* Save original objective */
    double *c_saved = (double*)malloc(n * sizeof(double));
    if (!c_saved) return -1;
    memcpy(c_saved, tab->c_ext, n * sizeof(double));

    /* Count dual infeasibilities and set auxiliary objective */
    int infeas_count = 0;
    tableau_compute_reduced_costs(tab);

    for (int j = 0; j < n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) {
            tab->c_ext[j] = 0.0;
            continue;
        }

        double rc_j = tab->rc[j];

        int dual_infeas = 0;
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc_j < -RALPH_OPT_TOL)
            dual_infeas = 1;
        else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc_j > RALPH_OPT_TOL)
            dual_infeas = 1;
        else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc_j) > RALPH_OPT_TOL)
            dual_infeas = 1;

        if (dual_infeas) {
            tab->c_ext[j] = (rc_j > 0) ? -1.0 : 1.0;
            infeas_count++;
        } else {
            tab->c_ext[j] = 0.0;
        }
    }

    if (infeas_count == 0) {
        memcpy(tab->c_ext, c_saved, n * sizeof(double));
        free(c_saved);
        return 0;
    }

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_phase1] %d dual infeasibilities, running auxiliary pivots...\n",
               infeas_count);
    }

    /* Recompute reduced costs with auxiliary objective */
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    tableau_compute_reduced_costs(tab);

    /* Apply bound perturbation for cycling prevention */
    int perturb_state = LP_GLPK_PERTURB_STATE_OFF;
    dual_perturb_state_activate(tab, &perturb_state, 0);

    int use_dse = solver->use_dual_steepest_edge;
    if (use_dse) dse_init_exact(tab);

    int max_phase1_iters = 200 * tab->m;
    int dual_refactor_base_interval = dual_refactor_base_interval_for_solver(solver);
    DualRefactorQualityState quality;
    dual_quality_init(&quality);

    for (int iter = 0; iter < max_phase1_iters; iter++) {
        if (dual_time_limit_exceeded(solver, iter)) {
            dual_perturb_state_disable(tab, &perturb_state);
            memcpy(tab->c_ext, c_saved, n * sizeof(double));
            free(c_saved);
            return -1;
        }
        tableau_compute_solution(tab);

        /* Find leaving variable (most infeasible basic) */
        int leaving = -1;
        if (use_dse && tab->dse_initialized) {
            double best_score = 0.0;
            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                double infeas = 0.0;
                if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL)
                    infeas = tab->lb_ext[j] - tab->x[j];
                else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL)
                    infeas = tab->x[j] - tab->ub_ext[j];
                if (infeas <= RALPH_FEAS_TOL) continue;
                double w = tab->dse_weights[k];
                double score = (infeas * infeas) / w;
                if (score > best_score) { best_score = score; leaving = k; }
            }
        } else {
            double max_infeas = RALPH_FEAS_TOL;
            for (int k = 0; k < tab->m; k++) {
                int j = tab->basis[k];
                double infeas = 0.0;
                if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL)
                    infeas = tab->lb_ext[j] - tab->x[j];
                else if (tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL)
                    infeas = tab->x[j] - tab->ub_ext[j];
                if (infeas > max_infeas) { max_infeas = infeas; leaving = k; }
            }
        }

        if (leaving < 0) {
            /* Auxiliary is primal+dual feasible — check original dual feasibility */
            break;
        }

        /* Dual ratio test + pivot on auxiliary */
        int entering;
        double theta;
        if (dual_ratio_test(tab, leaving, &entering, &theta) != 0) {
            dual_quality_record_ratio_failure(&quality);
            break;  /* Infeasible for auxiliary — can't continue */
        }
        if (entering == -2) {
            dual_quality_record_ratio_success(&quality, -2, 0.0);
            continue;
        }
        dual_quality_record_ratio_success(&quality, entering, theta);

        if (dual_simplex_pivot(tab, entering, leaving, theta,
                               1, dual_ratio_use_row_kernel(tab)) != 0) {
            dual_quality_record_pivot_failure(&quality);
            if (tableau_refactorize(tab) != 0) break;
            dual_quality_on_refactor(&quality, iter);
            tab->dse_initialized = 0;
            if (use_dse) dse_init_approx(tab);
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
            continue;
        }

        /* Governed periodic refactorization. */
        if (dual_governor_refactor_decision(solver,
                                            tab,
                                            &quality,
                                            iter,
                                            dual_refactor_base_interval,
                                            0,
                                            0,
                                            0)) {
            if (tableau_refactorize(tab) != 0) break;
            dual_quality_on_refactor(&quality, iter);
            tab->dse_initialized = 0;
            if (use_dse) dse_init_approx(tab);
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        }

        /* Check original dual feasibility after each pivot */
        if (iter % 5 == 0 || iter > max_phase1_iters - 10) {
            int orig_infeas = count_orig_dual_infeas(tab, c_saved);
            if (orig_infeas == 0) {
                if (solver->verbose) {
                    LP_LOG_STDOUT("[dual_phase1] Original dual feasibility achieved at iter %d\n", iter);
                }
                break;
            }
        }
    }

    /* Remove perturbation */
    dual_perturb_state_disable(tab, &perturb_state);

    /* Restore original objective */
    memcpy(tab->c_ext, c_saved, n * sizeof(double));
    free(c_saved);

    /* Recompute everything with original objective */
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    /* Final check: try make_dual_feasible on the new basis */
    make_dual_feasible(tab, solver->model->obj_sense,
                       dual_allow_startup_bound_flip(solver));
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    /* Verify dual feasibility */
    int still_infeasible = 0;
    for (int j = 0; j < n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;
        double rc_j = tab->rc[j];
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc_j < -RALPH_OPT_TOL) {
            still_infeasible = 1; break;
        }
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc_j > RALPH_OPT_TOL) {
            still_infeasible = 1; break;
        }
        if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc_j) > RALPH_OPT_TOL) {
            still_infeasible = 1; break;
        }
    }

    if (still_infeasible) {
        if (solver->verbose) {
            LP_LOG_STDOUT("[dual_phase1] Failed to achieve dual feasibility\n");
        }
        return -1;
    }

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_phase1] Dual feasibility achieved\n");
    }
    return 0;
}

/* ============================================================================
 * Clean Dual Simplex from Scratch (T1.3)
 *
 * Creates tableau without Big-M artificials (slacks only), then:
 * 1. make_dual_feasible() — flip non-basics to achieve dual feasibility
 * 2. dual_phase1() — auxiliary pivots if flipping wasn't enough
 * 3. dual_simplex_solve_v2() — clean Phase 2 to achieve primal feasibility
 *
 * Returns 0 on success (OPTIMAL, INFEASIBLE, OBJ_LIMIT), -1 on failure.
 * No primal fallbacks — caller decides.
 * ============================================================================ */

int dual_simplex_solve_from_scratch_v2(SimplexSolver *solver) {
    if (!solver || !solver->model) return -1;
    if (!lp_basis_governor_mode_is_valid(solver->policy.basis_governor_mode)) {
        solver->policy.basis_governor_mode = LP_BASIS_GOV_MODE_OFF;
    }
    lp_basis_governor_set_mode(&solver->policy.basis_governor,
                               solver->policy.basis_governor_mode);
    if (solver->progress_start_ms <= 0.0) {
        solver->progress_start_ms = lp_telemetry_now_ms();
    }

    clock_t start = clock();

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_v2_scratch] Starting from scratch...\n");
    }

    /* Create dual tableau if needed (no artificials — one aux per constraint) */
    if (!solver->tableau) {
        solver->tableau = tableau_create_dual(solver->model);
        if (!solver->tableau) {
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }
    }

    SimplexTableau *tab = solver->tableau;
    configure_dual_tableau_for_solver(solver, tab);

    /* Note: crash is NOT used for dual from-scratch.  The all-auxiliary basis
     * gives y=0, rc=c — ideal for make_dual_feasible + dual_phase1.  Crashing
     * structural vars into the basis contaminates y with non-zero costs. */

    /* Factorize initial basis (slacks/surplus) */
    {
        double t_refactor_ms = lp_telemetry_timer_start();
        int rc_ref = tableau_refactorize(tab);
        lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
        if (rc_ref != 0) {
            solver->status = RALPH_STATUS_ERROR;
            return -1;
        }
    }

    /* Compute initial solution and reduced costs */
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_v2_scratch] Initial: obj=%.2f\n",
               tab->obj_value * solver->model->obj_sense);
    }

    /* Step 1: Flip non-basic bounds to achieve dual feasibility */
    int changes = make_dual_feasible(tab, solver->model->obj_sense,
                                     dual_allow_startup_bound_flip(solver));

    if (changes > 0) {
        tableau_compute_solution(tab);
        tableau_compute_reduced_costs(tab);
    }

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_v2_scratch] Bound flips: %d\n", changes);
    }

    /* Check if dual feasible */
    int dual_infeasible = 0;
    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;
        double rc = tab->rc[j];
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER && rc < -RALPH_OPT_TOL) {
            dual_infeasible = 1; break;
        }
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER && rc > RALPH_OPT_TOL) {
            dual_infeasible = 1; break;
        }
        if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(rc) > RALPH_OPT_TOL) {
            dual_infeasible = 1; break;
        }
    }

    /* Step 2: If still dual infeasible, run dual Phase 1 */
    if (dual_infeasible) {
        if (solver->verbose) {
            LP_LOG_STDOUT("[dual_v2_scratch] Dual infeasible after flips, running Phase 1...\n");
        }

        int p1rc = dual_phase1(solver);
        if (p1rc != 0) {
            if (solver->verbose) {
                LP_LOG_STDOUT("[dual_v2_scratch] Dual Phase 1 failed\n");
            }
            solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
            return -1;
        }
    }

    /* Check if already primal feasible (optimal) */
    int primal_infeasible = 0;
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL ||
            tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
            primal_infeasible = 1;
            break;
        }
    }

    if (!primal_infeasible) {
        solver->status = RALPH_STATUS_OPTIMAL;
        solver->obj_value = tab->obj_value * solver->model->obj_sense + solver->model->obj_offset;
        solver->iterations = 0;

        int n_orig = solver->model->num_vars;
        if (!solver->solution)
            solver->solution = (double*)calloc(n_orig, sizeof(double));
        if (solver->solution) {
            for (int j = 0; j < n_orig; j++)
                solver->solution[j] = tab->x[j];
        }
        if (!solver->dual_solution)
            solver->dual_solution = (double*)calloc(solver->model->num_cons, sizeof(double));
        if (solver->dual_solution) {
            for (int i = 0; i < solver->model->num_cons; i++)
                solver->dual_solution[i] = tab->y[i] * solver->model->obj_sense;
        }
        if (!solver->reduced_costs)
            solver->reduced_costs = (double*)calloc(solver->model->num_vars, sizeof(double));
        if (solver->reduced_costs) {
            for (int j = 0; j < solver->model->num_vars; j++)
                solver->reduced_costs[j] = tab->rc[j] * solver->model->obj_sense;
        }

        solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
        return 0;
    }

    if (solver->verbose) {
        LP_LOG_STDOUT("[dual_v2_scratch] Running dual Phase 2...\n");
    }

    /* Step 3: Run clean dual Phase 2 */
    int rc = dual_simplex_solve_v2(solver);

    if (rc == 0) {
        /* OPTIMAL — copy dual solution and reduced costs */
        if (!solver->dual_solution)
            solver->dual_solution = (double*)calloc(solver->model->num_cons, sizeof(double));
        if (solver->dual_solution) {
            for (int i = 0; i < solver->model->num_cons; i++)
                solver->dual_solution[i] = tab->y[i] * solver->model->obj_sense;
        }
        if (!solver->reduced_costs)
            solver->reduced_costs = (double*)calloc(solver->model->num_vars, sizeof(double));
        if (solver->reduced_costs) {
            for (int j = 0; j < solver->model->num_vars; j++)
                solver->reduced_costs[j] = tab->rc[j] * solver->model->obj_sense;
        }
        solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
        return 0;
    } else if (rc == 1) {
        /* INFEASIBLE */
        solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
        return 0;
    }

    /* FAILED */
    solver->solve_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    return -1;
}
