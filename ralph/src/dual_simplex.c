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
#include "lp_log.h"

/* Forward declarations */
SimplexTableau* tableau_create(LPModel *model);
SimplexTableau* tableau_create_dual(LPModel *model);
int tableau_refactorize(SimplexTableau *tab);
int tableau_compute_solution(SimplexTableau *tab);
int tableau_compute_reduced_costs(SimplexTableau *tab);
void tableau_free(SimplexTableau *tab);

/* Dual candidate-list pricing constants (T2.2) */
#define DUAL_CAND_CAPACITY    200    /* Max candidates in dual hot set */
#define DUAL_CAND_RC_THRESH   1e-4   /* |rc| threshold for candidate inclusion */

/* Bound perturbation for degeneracy prevention (defined below) */
static void apply_bound_perturbation(SimplexTableau *tab);
static void remove_bound_perturbation(SimplexTableau *tab);

/* Forward declaration for dual feasibility function (non-static for simplex.c access) */
int make_dual_feasible(SimplexTableau *tab, int obj_sense, int allow_bound_flip);

static void configure_dual_tableau_for_solver(SimplexSolver *solver, SimplexTableau *tab) {
    if (!solver || !tab) return;
    tab->owner = solver;

    if (!tab->lu) return;

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

    if (solver->lu_backend_policy == LP_LU_BACKEND_POLICY_CBG) {
        tab->lu->mkz_enabled = 0;
        tab->lu->sn_enabled = 0;
    } else if (solver->lu_backend_policy == LP_LU_BACKEND_POLICY_CGR) {
        tab->lu->mkz_enabled = 1;
        tab->lu->sn_enabled = 0;
    } else {
        tab->lu->sn_enabled = enable_supernode ? 1 : 0;
    }

    if (solver->lu_update_limit_override > 0) {
        tab->lu->max_updates = solver->lu_update_limit_override;
    }
    if (solver->lu_pivot_tol_override > 0.0) {
        tab->lu->pivot_tol = solver->lu_pivot_tol_override;
    }
    if (solver->lu_growth_guard_override > 0.0) {
        tab->lu->growth_refactor_threshold = solver->lu_growth_guard_override;
    } else if (tab->lu->growth_refactor_threshold <= 0.0) {
        tab->lu->growth_refactor_threshold = RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
    }
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

    /* Allocate if needed */
    if (!solver->farkas_ray) {
        solver->farkas_ray = (double*)calloc(m, sizeof(double));
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

/* ============================================================================
 * Dual Ratio Test
 * ============================================================================ */

/* Harris tolerance allows slightly suboptimal ratios if they provide
 * numerically more stable pivot elements. */
#define HARRIS_TOL 1e-6

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

static void dual_ratio_mode_flags(int mode, int *use_harris, int *prefer_flip_candidates) {
    int harris = 1;
    int prefer_flip = 0;
    if (mode == LP_DUAL_RATIO_TEST_STANDARD) {
        harris = 0;
        prefer_flip = 0;
    } else if (mode == LP_DUAL_RATIO_TEST_FLIP) {
        harris = 1;
        prefer_flip = 1;
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
    vec_set_zero(tab->work1, tab->m);
    tab->work1[leaving] = 1.0;
    lu_solve_transpose(tab->lu, tab->work1, tab->work2);  /* alpha = B^{-T} * e_leaving */

    /* Find entering variable by dual ratio test */
    *entering = -1;
    *theta = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_can_flip = 0;
    const double tie_tol = 1e-12;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        /* Compute alpha_j = (B^{-1} * a_j)[leaving] = alpha' * a_j using sparse dot */
        double alpha_j = sparse_dot_column(tab->A_ext, j, tab->work2);

        if (fabs(alpha_j) < pivot_floor) continue;

        double rc_j = tab->rc[j];
        double ratio = RALPH_INFINITY;

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
            if (alpha_j < -RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j from lower bound increases x_B[leaving] - good! */
                /* x_j at lower has rc_j >= 0, alpha_j < 0, so -rc_j/alpha_j >= 0 */
                ratio = -rc_j / alpha_j;
            } else if (alpha_j > RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j from upper bound increases x_B[leaving] - good! */
                /* x_j at upper has rc_j <= 0, alpha_j > 0, so -rc_j/alpha_j >= 0 */
                ratio = -rc_j / alpha_j;
            }
        } else {
            /* Leaving variable needs to decrease (currently above upper bound)
             *
             * For dir=-1, the ratio formula differs from dir=+1:
             * - At LOWER with alpha > 0: rc >= 0, so rc/alpha >= 0
             * - At UPPER with alpha < 0: rc <= 0, so rc/alpha >= 0
             * Using rc_j/alpha_j (not -rc_j/alpha_j) gives positive ratios.
             */
            if (alpha_j > RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_LOWER) {
                /* Increasing x_j from lower bound decreases x_B[leaving] - good! */
                ratio = rc_j / alpha_j;
            } else if (alpha_j < -RALPH_PIVOT_TOL && tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* Decreasing x_j from upper bound decreases x_B[leaving] - good! */
                ratio = rc_j / alpha_j;
            }
        }

        if (ratio < theta_floor) continue;

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

int dual_ratio_test(SimplexTableau *tab, int leaving, int *entering, double *theta) {
    int mode = LP_DUAL_RATIO_TEST_HARRIS;
    int rc = -1;
    int attempt_mode;
    int use_harris = 1;
    int prefer_flip = 0;

    if (tab && tab->owner) {
        mode = tab->owner->dual_ratio_test_mode;
    }

    /* Retry ladder:
     * 1) strict pivot floor and strictly-positive theta in requested mode
     * 2) fallback modes with base pivot floor and strictly-positive theta
     * 3) permissive last pass (default tolerance) to avoid false infeasibility */
    dual_ratio_mode_flags(mode, &use_harris, &prefer_flip);
    rc = dual_ratio_test_core(tab, leaving, entering, theta,
                              use_harris, prefer_flip,
                              10.0 * RALPH_PIVOT_TOL, 1e-12);
    if (rc == 0) goto dual_ratio_done;

    for (int i = 0; i < 3; i++) {
        attempt_mode = (i == 0) ? LP_DUAL_RATIO_TEST_HARRIS :
                       (i == 1) ? LP_DUAL_RATIO_TEST_STANDARD :
                                  LP_DUAL_RATIO_TEST_FLIP;
        if (attempt_mode == mode) continue;
        dual_ratio_mode_flags(attempt_mode, &use_harris, &prefer_flip);
        rc = dual_ratio_test_core(tab, leaving, entering, theta,
                                  use_harris, prefer_flip,
                                  RALPH_PIVOT_TOL, 1e-12);
        if (rc == 0) goto dual_ratio_done;
    }

    dual_ratio_mode_flags(mode, &use_harris, &prefer_flip);
    rc = dual_ratio_test_core(tab, leaving, entering, theta,
                              use_harris, prefer_flip,
                              RALPH_PIVOT_TOL, -RALPH_OPT_TOL);

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

static int dual_simplex_pivot(SimplexTableau *tab, int entering, int leaving, double theta) {
    (void)theta;  /* Step size already computed in caller */
    if (!tab || entering < 0 || entering >= tab->n || leaving < 0 || leaving >= tab->m) {
        return -1;
    }

    int leaving_var = tab->basis[leaving];
    double saved_obj = tab->obj_value;

    /* Keep pivot failures non-destructive: restore full basis/state on error.
     * Uses pre-allocated backup arrays in tableau (B2 fix: no per-pivot malloc). */
    double *x_backup = tab->dual_x_backup;
    double *rc_backup = tab->dual_rc_backup;
    int *basis_backup = tab->dual_basis_backup;
    int *basis_pos_backup = tab->dual_basis_pos_backup;
    VarStatus *status_backup = tab->dual_status_backup;

    memcpy(x_backup, tab->x, (size_t)tab->n * sizeof(double));
    memcpy(rc_backup, tab->rc, (size_t)tab->n * sizeof(double));
    memcpy(basis_backup, tab->basis, (size_t)tab->m * sizeof(int));
    memcpy(basis_pos_backup, tab->basis_pos, (size_t)tab->n * sizeof(int));
    memcpy(status_backup, tab->var_status, (size_t)tab->n * sizeof(VarStatus));

    /* Compute entering column in basis representation using sparse solve */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);
    {
        double t_ftran_ms = lp_telemetry_timer_start();
        lu_solve_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work3);  /* d = B^{-1} * a_entering */
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }

    double pivot = tab->work3[leaving];
    if (!isfinite(pivot) || fabs(pivot) < RALPH_PIVOT_TOL) {
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
    vec_set_zero(tab->work1, tab->m);
    tab->work1[leaving] = 1.0;
    {
        double t_btran_ms = lp_telemetry_timer_start();
        lu_solve_transpose(tab->lu, tab->work1, tab->work2);  /* work2 = pivot_row */
        if (tab->owner) {
            lp_telemetry_add_btran_timed(tab->owner, t_btran_ms);
        }
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
            double dot = sparse_dot_column(tab->A_ext, j, tab->work2);
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

    /* Update primal solution */
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] -= step * tab->work3[k];
    }

    /* Update entering variable.
     * When entering is at lower bound and alpha < 0 (increases leaving), step > 0
     * When entering is at upper bound and alpha > 0 (increases leaving), step < 0
     * So: x_entering = bound + step (works for both cases)
     */
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        tab->x[entering] = tab->lb_ext[entering] + step;
    } else {
        tab->x[entering] = tab->ub_ext[entering] + step;
    }

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
        tab->x[leaving_var] = tab->lb_ext[leaving_var];
    } else {
        /* Was above upper bound - go to upper bound */
        tab->var_status[leaving_var] = RALPH_NONBASIC_UPPER;
        tab->x[leaving_var] = tab->ub_ext[leaving_var];
    }
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
    const int force_refactor = fabs(pivot) < 1e-4;
    if (force_refactor) {
        if (tab->owner) {
            lp_telemetry_record_dual_lu_hard_trigger(tab->owner);
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

    /* Recompute objective value */
    tab->obj_value = 0.0;
    for (int j = 0; j < tab->n; j++) {
        tab->obj_value += tab->c_ext[j] * tab->x[j];
    }

    return 0;

pivot_fail_rollback:
    memcpy(tab->x, x_backup, (size_t)tab->n * sizeof(double));
    memcpy(tab->rc, rc_backup, (size_t)tab->n * sizeof(double));
    memcpy(tab->basis, basis_backup, (size_t)tab->m * sizeof(int));
    memcpy(tab->basis_pos, basis_pos_backup, (size_t)tab->n * sizeof(int));
    memcpy(tab->var_status, status_backup, (size_t)tab->n * sizeof(VarStatus));
    tab->obj_value = saved_obj;
    return -1;
}

/* [Phase E] dual_simplex_solve() deleted — replaced by dual_simplex_solve_v2() */

/*
 * Fallback ratio test used by Phase-1 rescue when strict dual-ratio selection
 * fails for a leaving row. This mirrors the robust row-wise dual-pivot logic
 * used in primal Phase-1 recovery and does not require global dual feasibility.
 */
static int phase1_rescue_ratio_test(SimplexTableau *tab, int leaving,
                                    int *entering, double *theta) {
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

    vec_set_zero(tab->work1, tab->m);
    tab->work1[leaving] = 1.0;
    lu_solve_transpose(tab->lu, tab->work1, tab->work2);

    *entering = -1;
    *theta = RALPH_INFINITY;

    for (int j = 0; j < tab->n; j++) {
        if (tab->var_status[j] == RALPH_BASIC || tab->var_status[j] == RALPH_FIXED) {
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

        if (dir > 0 && alpha > RALPH_PIVOT_TOL &&
            tab->var_status[j] == RALPH_NONBASIC_LOWER) {
            ratio = -rc / alpha;
        } else if (dir > 0 && alpha < -RALPH_PIVOT_TOL &&
                   tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            ratio = rc / (-alpha);
        } else if (dir < 0 && alpha < -RALPH_PIVOT_TOL &&
                   tab->var_status[j] == RALPH_NONBASIC_LOWER) {
            ratio = -rc / (-alpha);
        } else if (dir < 0 && alpha > RALPH_PIVOT_TOL &&
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
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    /* Try to improve dual feasibility via bound flips first. */
    int changes = make_dual_feasible(tab,
                                     solver->model ? solver->model->obj_sense : 1,
                                     solver->use_dual_bound_flip);
    if (changes > 0) {
        tableau_compute_solution(tab);
        tableau_compute_reduced_costs(tab);
    }

    unsigned char *tried_rows = (unsigned char*)calloc((size_t)tab->m, sizeof(unsigned char));
    if (!tried_rows) {
        return 1;
    }

    const int MAX_REFACTOR_FAILURES = RALPH_PHASE1_RESCUE_MAX_REFACTOR_FAILURES;
    int refactor_failures = 0;

    for (int iter = 0; iter < max_iters; iter++) {
        if (dual_time_limit_exceeded(solver, iter)) {
            free(tried_rows);
            return 1;
        }
        tableau_compute_solution(tab);

        if (phase1_rescue_has_bad_numerics(tab)) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[dual_phase1_rescue] Non-finite x/rc at iter %d, trying refactorization repair\n", iter);
            }
            if (tableau_refactorize(tab) == 0) {
                refactor_failures = 0;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
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
        for (int k = 0; k < tab->m; k++) {
            int j = tab->basis[k];
            if (tab->x[j] < tab->lb_ext[j] - RALPH_FEAS_TOL ||
                tab->x[j] > tab->ub_ext[j] + RALPH_FEAS_TOL) {
                has_infeasible = 1;
                break;
            }
        }
        if (!has_infeasible) {
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[dual_phase1_rescue] Primal feasibility restored after %d iterations\n", iter);
            }
            free(tried_rows);
            return 0;
        }

        int leaving = -1;
        int entering = -1;
        double theta = RALPH_INFINITY;
        memset(tried_rows, 0, (size_t)tab->m * sizeof(unsigned char));

        /* Try multiple infeasible leaving rows to avoid getting stuck on a
         * single row with no stable entering candidate. */
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

            if (dual_ratio_test(tab, candidate, &entering, &theta) == 0 && entering >= 0) {
                leaving = candidate;
                break;
            }

            if (phase1_rescue_ratio_test(tab, candidate, &entering, &theta) == 0 && entering >= 0) {
                leaving = candidate;
                break;
            }
        }

        if (leaving < 0 || entering < 0) {
            /* No valid repair pivot found for remaining infeasible rows. */
            if (solver->verbose >= 2) {
                LP_LOG_STDERR("[dual_phase1_rescue] No valid entering column for infeasible rows at iter %d\n", iter);
            }
            free(tried_rows);
            return 1;
        }

        if (dual_simplex_pivot(tab, entering, leaving, theta) != 0) {
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
            }
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
            continue;
        }

        /* Keep numerics under control during rescue. */
        int lu_refactor_needed = lu_needs_refactorization(tab->lu);
        int periodic_refactor = (iter > 0 && iter % 25 == 0);
        int need_refactor = lu_refactor_needed || periodic_refactor;
        {
            int shadow_refactor = lp_basis_governor_shadow_decide(
                LP_BASIS_GOV_PHASE_DUAL,
                lu_refactor_needed,
                periodic_refactor);
            int governed_refactor = lp_basis_governor_decide_refactor(
                &solver->policy.basis_governor,
                LP_BASIS_GOV_PHASE_DUAL,
                lu_refactor_needed,
                periodic_refactor,
                need_refactor);
            if (solver->telemetry_enabled) {
                lp_basis_governor_observe_refactor(
                    &solver->policy.basis_governor,
                    LP_BASIS_GOV_PHASE_DUAL,
                    shadow_refactor,
                    governed_refactor);
            }
            need_refactor = governed_refactor;
        }
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
            }
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        } else if (iter > 0 && iter % 10 == 0) {
            tableau_compute_reduced_costs(tab);
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
        lp_telemetry_record_dual_bound_flip_applied(tab->owner, changes);
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
    apply_bound_perturbation(tab);

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

    /* Compute primal solution once before entering the main loop.
     * After this, dual_simplex_pivot() maintains x incrementally via
     * x_B -= step * d.  Full recomputation only after refactorization. */
    tableau_compute_solution(tab);
    double last_obj = tab->obj_value;

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        solver->iterations = iter;
        if (dual_run_user_callbacks(solver, tab, iter, 0, 1) != 0) {
            remove_bound_perturbation(tab);
            solver->status = RALPH_STATUS_TIME_LIMIT;
            solver->iterations = iter;
            return -1;
        }

        /* T3.1: Objective limit early-exit (internal minimization space) */
        if (solver->objective_limit < RALPH_INFINITY &&
            tab->obj_value >= solver->objective_limit) {
            remove_bound_perturbation(tab);
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
                remove_bound_perturbation(tab);
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
                    tableau_refactorize(tab);
                    lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
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
                        if (rc_ratio != 0) {
                            rc_ratio = phase1_rescue_ratio_test(tab, cl_leaving, &cl_entering, &cl_theta);
                        }
                        lp_telemetry_record_ratio_timed(solver, 0, t_ratio_ms);
                        if (rc_ratio != 0) {
                            /* Infeasible after unshift — should not happen, bail */
                            break;
                        }
                    }
                    {
                        double t_pivot_ms = lp_telemetry_timer_start();
                        int rc_pivot = dual_simplex_pivot(tab, cl_entering, cl_leaving, cl_theta);
                        lp_telemetry_record_pivot_timed(solver, 0, t_pivot_ms);
                        if (rc_pivot != 0) {
                            double t_refactor_ms = lp_telemetry_timer_start();
                            int rc_ref = tableau_refactorize(tab);
                            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
                            if (rc_ref != 0) break;
                            tab->dse_initialized = 0;
                            if (use_dse) dse_init_approx(tab);
                            tableau_compute_solution(tab);
                            tableau_compute_reduced_costs(tab);
                        }
                    }
                    cleanup_iters++;
                    solver->iterations++;

                    /* Periodic refactorization during cleanup */
                    if (cleanup_iters % 50 == 0) {
                        double t_refactor_ms = lp_telemetry_timer_start();
                        tableau_refactorize(tab);
                        lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
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
                double max_dual_viol = 0.0;
                for (int j = 0; j < tab->n; j++) {
                    if (tab->var_status[j] == RALPH_BASIC) continue;
                    double viol = 0.0;
                    if (tab->var_status[j] == RALPH_NONBASIC_LOWER && tab->rc[j] < -RALPH_OPT_TOL)
                        viol = -tab->rc[j];
                    else if (tab->var_status[j] == RALPH_NONBASIC_UPPER && tab->rc[j] > RALPH_OPT_TOL)
                        viol = tab->rc[j];
                    else if (tab->var_status[j] == RALPH_NONBASIC_FREE && fabs(tab->rc[j]) > RALPH_OPT_TOL)
                        viol = fabs(tab->rc[j]);
                    if (viol > max_dual_viol) max_dual_viol = viol;
                }
                if (max_dual_viol > 1e-4) {
                    if (solver->verbose) {
                        LP_LOG_STDOUT("[dual_v2] Suboptimal: max dual violation %.2e after unshift\n",
                               max_dual_viol);
                    }
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
            dual_run_user_callbacks(solver, tab, iter, 1, 0);
            return 0;
        }

        /* Dual ratio test */
        int entering;
        double theta;

        {
            double t_ratio_ms = lp_telemetry_timer_start();
            int rc_ratio = dual_ratio_test(tab, leaving, &entering, &theta);
            if (rc_ratio != 0) {
                rc_ratio = phase1_rescue_ratio_test(tab, leaving, &entering, &theta);
            }
            lp_telemetry_record_ratio_timed(solver, 0, t_ratio_ms);
            if (rc_ratio != 0) {
                /* No entering variable — problem is infeasible */
                remove_bound_perturbation(tab);
                extract_farkas_ray_dual(solver);
                solver->status = RALPH_STATUS_INFEASIBLE;
                dual_run_user_callbacks(solver, tab, iter, 1, 0);
                return 1;
            }
        }

        /* Perform dual pivot */
        {
            double t_pivot_ms = lp_telemetry_timer_start();
            int rc_pivot = dual_simplex_pivot(tab, entering, leaving, theta);
            lp_telemetry_record_pivot_timed(solver, 0, t_pivot_ms);
            if (rc_pivot != 0) {
                double t_refactor_ms = lp_telemetry_timer_start();
                int rc_ref = tableau_refactorize(tab);
                lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
                if (rc_ref != 0) {
                    remove_bound_perturbation(tab);
                    return -1;  /* FAILED */
                }
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
                apply_bound_perturbation(tab);
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
                    remove_bound_perturbation(tab);
                    tab->perturb_scale = 1.0 + 2.0 * perturb_attempts;
                    apply_bound_perturbation(tab);
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
                    remove_bound_perturbation(tab);
                    return -1;
                }
            }
        } else {
            stall_count = 0;
            last_obj = tab->obj_value;
        }

        /* Periodic refactorization */
        int lu_refactor_needed = lu_needs_refactorization(tab->lu);
        int periodic_refactor = (iter > 0 && iter % 50 == 0);
        int need_refactor = lu_refactor_needed || periodic_refactor;
        {
            int shadow_refactor = lp_basis_governor_shadow_decide(
                LP_BASIS_GOV_PHASE_DUAL,
                lu_refactor_needed,
                periodic_refactor);
            int governed_refactor = lp_basis_governor_decide_refactor(
                &solver->policy.basis_governor,
                LP_BASIS_GOV_PHASE_DUAL,
                lu_refactor_needed,
                periodic_refactor,
                need_refactor);
            if (solver->telemetry_enabled) {
                lp_basis_governor_observe_refactor(
                    &solver->policy.basis_governor,
                    LP_BASIS_GOV_PHASE_DUAL,
                    shadow_refactor,
                    governed_refactor);
            }
            need_refactor = governed_refactor;
        }
        if (need_refactor) {
            double t_refactor_ms = lp_telemetry_timer_start();
            int rc_ref = tableau_refactorize(tab);
            lp_telemetry_add_refactor_runtime_timed(solver, t_refactor_ms);
            if (rc_ref != 0) {
                remove_bound_perturbation(tab);
                return -1;
            }
            tab->dse_initialized = 0;
            if (use_dse) dse_init_approx(tab);
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        } else if (iter > 0 && iter % 20 == 0) {
            tableau_compute_reduced_costs(tab);
        }

        if (solver->verbose && iter % 100 == 0) {
            LP_LOG_STDOUT("[dual_v2] Iter %d: obj=%.6f\n", iter, tab->obj_value);
        }
    }

    /* Exceeded max iterations — FAILED */
    remove_bound_perturbation(tab);
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
    apply_bound_perturbation(tab);

    int use_dse = solver->use_dual_steepest_edge;
    if (use_dse) dse_init_exact(tab);

    int max_phase1_iters = 200 * tab->m;
    int succeeded = 0;

    for (int iter = 0; iter < max_phase1_iters; iter++) {
        if (dual_time_limit_exceeded(solver, iter)) {
            remove_bound_perturbation(tab);
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
            if (phase1_rescue_ratio_test(tab, leaving, &entering, &theta) != 0) {
                break;  /* Infeasible for auxiliary — can't continue */
            }
        }

        if (dual_simplex_pivot(tab, entering, leaving, theta) != 0) {
            if (tableau_refactorize(tab) != 0) break;
            tab->dse_initialized = 0;
            if (use_dse) dse_init_approx(tab);
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
            continue;
        }

        /* Periodic refactorization */
        if (lu_needs_refactorization(tab->lu) || (iter > 0 && iter % 50 == 0)) {
            if (tableau_refactorize(tab) != 0) break;
            tab->dse_initialized = 0;
            if (use_dse) dse_init_approx(tab);
            tableau_compute_solution(tab);
            tableau_compute_reduced_costs(tab);
        }

        /* Check original dual feasibility after each pivot */
        if (iter % 5 == 0 || iter > max_phase1_iters - 10) {
            int orig_infeas = count_orig_dual_infeas(tab, c_saved);
            if (orig_infeas == 0) {
                succeeded = 1;
                if (solver->verbose) {
                    LP_LOG_STDOUT("[dual_phase1] Original dual feasibility achieved at iter %d\n", iter);
                }
                break;
            }
        }
    }

    /* Remove perturbation */
    remove_bound_perturbation(tab);

    /* Restore original objective */
    memcpy(tab->c_ext, c_saved, n * sizeof(double));
    free(c_saved);

    /* Recompute everything with original objective */
    tab->duals_valid = 0;
    tab->rc_all_valid = 0;
    tableau_compute_solution(tab);
    tableau_compute_reduced_costs(tab);

    /* Final check: try make_dual_feasible on the new basis */
    make_dual_feasible(tab, solver->model->obj_sense, solver->use_dual_bound_flip);
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

    if (still_infeasible && !succeeded) {
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
    int changes = make_dual_feasible(tab, solver->model->obj_sense, solver->use_dual_bound_flip);

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
