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
#include "simplex_phase1_decision.h"
#include "simplex_phase2_zones.h"

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

/* Phase 1 trace + stagnation moved to simplex_phase1_trace.c (R3.9) */



/* PHASE2_DEGEN_ESCAPE_* constants now in simplex_internal.h (R4.1) */
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
    if (tab->model && tab->free_split_col && tab->free_split_orig) {
        int mate = -1;
        if (j < tab->model->num_vars) {
            mate = tab->free_split_col[j];
        } else if (j < tab->num_structural_ext) {
            mate = tab->free_split_orig[j];
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
    return simplex_smcp_working_excl_should_skip_for_test(smcp_excl,
                                                          smcp_shift,
                                                          (int)tab->var_status[j],
                                                          tab->lb_ext[j],
                                                          tab->ub_ext[j],
                                                          tol_bnd);
}





static int solution_refine_iteration_budget(double max_residual, double feas_tol) {
    if (!isfinite(max_residual) || !isfinite(feas_tol) || feas_tol <= 0.0) return 0;
    if (max_residual <= feas_tol) return 0;
    if (max_residual <= 10.0 * feas_tol) return 1;
    if (max_residual <= 100.0 * feas_tol) return 2;
    return 5;
}

static int simplex_dense_spike_min_updates_override(const SimplexTableau *tab) {
    if (!tab) return 0;
    return lp_refactor_policy_dense_spike_min_updates_override(tab->phase,
                                                               tab->m);
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

/* Phase 1 decision functions moved to simplex_phase1_decision.c (R3.8) */


/* FNV-1a style mixer for deterministic trace signatures. */




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

/* Scaling + verification moved to simplex_scaling.c (R3.9) */


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
        /* int arrays: free split mappings (n each) */
        2 * (size_t)n * sizeof(int) +
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
        /* int array: artificial_vars and byte bitmap for two-phase */
        (size_t)num_artificial * sizeof(int) +
        (size_t)n * sizeof(unsigned char) +
        /* int array: redundant_rows for two-phase (m) */
        (size_t)m * sizeof(int) +
        /* double array: dse_weights for dual steepest edge (m) */
        (size_t)m * sizeof(double) +
        /* int array: flip_list for bound flipping (n) */
        (size_t)n * sizeof(int) +
        /* int arrays: heap, heap_pos for heap pricing (n each) */
        2 * (size_t)n * sizeof(int) +
        /* primal pivot rollback: basic x(m dbl) */
        (size_t)m * sizeof(double) +
        /* dual pivot backup: x(n dbl), rc(n dbl), basis(m int), basis_pos(n int), status(n VarStatus) */
        2 * (size_t)n * sizeof(double) + (size_t)m * sizeof(int) + (size_t)n * sizeof(int) + (size_t)n * sizeof(VarStatus) +
        /* Alignment padding (38 allocations * 8 bytes) */
        304;

    /* Create arena */
    tab->arena = sh_arena_create(arena_size);
    if (!tab->arena) {
        return -1;
    }

    /* Allocate all arrays from arena (calloc zeros memory) */
    tab->c_ext = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->lb_ext = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->ub_ext = (double*)sh_arena_calloc(tab->arena, n, sizeof(double));
    tab->free_split_col = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    tab->free_split_orig = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    if (tab->free_split_col) memset(tab->free_split_col, -1, n * sizeof(int));
    if (tab->free_split_orig) memset(tab->free_split_orig, -1, n * sizeof(int));

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
    tab->is_artificial_var = (unsigned char*)sh_arena_calloc(tab->arena, n, sizeof(unsigned char));
    tab->artificial_basic_count = 0;

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
    tab->primal_basic_x_backup = (double*)sh_arena_alloc(tab->arena, m * sizeof(double));
    tab->dual_x_backup = (double*)sh_arena_alloc(tab->arena, n * sizeof(double));
    tab->dual_rc_backup = (double*)sh_arena_alloc(tab->arena, n * sizeof(double));
    tab->dual_basis_backup = (int*)sh_arena_alloc(tab->arena, m * sizeof(int));
    tab->dual_basis_pos_backup = (int*)sh_arena_alloc(tab->arena, n * sizeof(int));
    tab->dual_status_backup = (VarStatus*)sh_arena_alloc(tab->arena, n * sizeof(VarStatus));

    /* Single check for all allocations */
    if (!tab->c_ext || !tab->lb_ext || !tab->ub_ext ||
        !tab->free_split_col || !tab->free_split_orig ||
        !tab->basis || !tab->nonbasis || !tab->var_status || !tab->basis_pos ||
        !tab->basis_col_cache || !tab->basis_col_nnz_cache ||
        !tab->x || !tab->y || !tab->rc ||
        !tab->work1 || !tab->work2 || !tab->work3 || !tab->work4 || !tab->rhs || !tab->row_sign ||
        !tab->pivot_row || !tab->tau_work || !tab->se_weights ||
        !tab->cb_sparse_idx || !tab->cb_sparse_val ||
        !tab->aux_row || !tab->aux_coef || !tab->partial_candidates || !tab->dual_candidates ||
        !tab->c_original || !tab->is_artificial_var || (num_artificial > 0 && !tab->artificial_vars) ||
        !tab->redundant_rows || !tab->dse_weights || !tab->flip_list ||
        !tab->heap || !tab->heap_pos ||
        !tab->primal_basic_x_backup ||
        !tab->dual_x_backup || !tab->dual_rc_backup ||
        !tab->dual_basis_backup || !tab->dual_basis_pos_backup || !tab->dual_status_backup) {
        return -1;
    }
    return 0;
}

static void tableau_refresh_artificial_basic_count(SimplexTableau *tab) {
    int count = 0;
    if (!tab || !tab->artificial_vars || !tab->var_status) return;
    for (int k = 0; k < tab->num_artificial; k++) {
        int art_j = tab->artificial_vars[k];
        if (art_j >= 0 && art_j < tab->n &&
            tab->var_status[art_j] == RALPH_BASIC) {
            count++;
        }
    }
    tab->artificial_basic_count = count;
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

    int num_free_splits = 0; /* One generated negative-part column per unrestricted var */
    for (int j = 0; j < model->num_vars; j++) {
        if (model->lb[j] <= -0.5 * RALPH_INFINITY &&
            model->ub[j] >= 0.5 * RALPH_INFINITY) {
            num_free_splits++;
        }
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

    tab->num_structural_ext = model->num_vars + num_free_splits;
    tab->n = tab->num_structural_ext + num_aux_vars;
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

    int next_split_col = model->num_vars;

    /* Copy structural variable data. Unrestricted variables are represented
     * internally as x = x_pos - x_neg with both generated columns >= 0. This
     * keeps the primal tableau finite without changing the public model. */
    for (int j = 0; j < model->num_vars; j++) {
        double orig_cost = model->c[j] * model->obj_sense;  /* Convert to minimization */
        int is_free = (model->lb[j] <= -0.5 * RALPH_INFINITY &&
                       model->ub[j] >= 0.5 * RALPH_INFINITY);
        tab->c_original[j] = orig_cost;
        if (use_two_phase) {
            /* Phase 1 objective: structural variables have zero cost */
            tab->c_ext[j] = 0.0;
        } else {
            tab->c_ext[j] = orig_cost;
        }
        tab->lb_ext[j] = is_free ? 0.0 : model->lb[j];
        tab->ub_ext[j] = is_free ? RALPH_INFINITY : model->ub[j];
        if (is_free) {
            int split_col = next_split_col++;
            tab->free_split_col[j] = split_col;
            tab->free_split_orig[split_col] = j;
            tab->c_original[split_col] = -orig_cost;
            tab->c_ext[split_col] = use_two_phase ? 0.0 : -orig_cost;
            tab->lb_ext[split_col] = 0.0;
            tab->ub_ext[split_col] = RALPH_INFINITY;
        }
    }

    /* Build extended constraint matrix with slacks/surplus/artificial */
    SparseTriplets *trips = triplets_create(tab->m, tab->n,
                                            model->A->nnz * (num_free_splits > 0 ? 2 : 1) + num_aux_vars);
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
            int split_col = tab->free_split_col[j];
            if (split_col >= 0) {
                triplets_add(trips, row, split_col, -val);
            }
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
            double xj0 = (tab->free_split_col[j] >= 0) ? 0.0 : model->lb[j];
            for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
                int row = model->A->rowidx[p];
                double val = model->A->values[p] * norm_sign[row];
                ax_initial[row] += val * xj0;
            }
        }
    }

    /* Add auxiliary variables and record their mapping to constraints.
     * Artificial variable costs are 1.0 (Phase 1 objective).
     * For dual_mode: no artificials — one aux per constraint with zero cost. */
    double artificial_cost = 1.0;
    int aux_idx = tab->num_structural_ext;
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

    for (int k = 0; k < tab->num_artificial; k++) {
        int art_j = tab->artificial_vars[k];
        if (art_j >= 0 && art_j < tab->n) {
            tab->is_artificial_var[art_j] = 1;
        }
    }

    /* Store original costs for auxiliary variables (for Phase 2 transition).
     * Slack/surplus have zero cost, artificials have cost 1.0 (Phase 1 objective). */
    for (int j = tab->num_structural_ext; j < tab->n; j++) {
        /* Check if this is an artificial variable */
        int is_artificial = tab->is_artificial_var[j] ? 1 : 0;
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
        if (tab->is_artificial_var[bv]) {
            tab->artificial_basic_count++;
        }
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
        for (int j = 0; j < tab->num_structural_ext; j++) {
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
    tab->free_split_col = NULL;
    tab->free_split_orig = NULL;
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
    tab->primal_basic_x_backup = NULL;
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
    int num_struct = tab->num_structural_ext;
    int repairs = 0;
    const int MAX_REPAIRS = 100;
    const int sparse_only_repair = (m >= 700);

    /* Build a copy of the basis for analysis */
    SparseMatrix *B = build_basis_matrix(tab);
    if (!B) return -1;

    if (sparse_only_repair) {
        goto crash_basis_repair;
    }

    /* Strategy 1: Try swapping structural variables with any non-basic slack */
    for (int attempt = 0; attempt < MAX_REPAIRS && repairs < MAX_REPAIRS; attempt++) {
        /* Try factorization */
        int status = sparse_only_repair
            ? lu_factorize_sparse_no_dense(tab->lu, B)
            : lu_factorize(tab->lu, B);
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
                int test_status = sparse_only_repair
                    ? lu_factorize_sparse_no_dense(tab->lu, B)
                    : lu_factorize(tab->lu, B);
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
crash_basis_repair:
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

    int status = sparse_only_repair
        ? lu_factorize_sparse_no_dense(tab->lu, B)
        : lu_factorize(tab->lu, B);

    return status;
}

static void column_to_dense(const SimplexTableau *tab, int col, double *out) {
    vec_set_zero(out, tab->m);
    if (!tab || !tab->A_ext || col < 0 || col >= tab->A_ext->ncols) return;
    for (int p = tab->A_ext->colptr[col]; p < tab->A_ext->colptr[col + 1]; p++) {
        out[tab->A_ext->rowidx[p]] = tab->A_ext->values[p];
    }
}

static int transition_column_independent(double *q,
                                         int m,
                                         int rank,
                                         const double *col,
                                         double *work) {
    double norm0 = 0.0;
    for (int i = 0; i < m; i++) {
        work[i] = col[i];
        norm0 += work[i] * work[i];
    }
    norm0 = sqrt(norm0);
    if (norm0 <= RALPH_PIVOT_TOL) return 0;

    for (int r = 0; r < rank; r++) {
        double dot = 0.0;
        double *qr = q + (size_t)r * (size_t)m;
        for (int i = 0; i < m; i++) dot += work[i] * qr[i];
        for (int i = 0; i < m; i++) work[i] -= dot * qr[i];
    }

    double norm = 0.0;
    for (int i = 0; i < m; i++) norm += work[i] * work[i];
    norm = sqrt(norm);
    if (norm <= fmax(1e-10, 1e-9 * norm0)) {
        return 0;
    }

    double *qnew = q + (size_t)rank * (size_t)m;
    for (int i = 0; i < m; i++) {
        qnew[i] = work[i] / norm;
    }
    return 1;
}

static int transition_rebasis_without_artificials(SimplexSolver *solver) {
    SimplexTableau *tab = solver ? solver->tableau : NULL;
    if (!tab || !tab->A_ext || tab->num_artificial <= 0) return -1;

    int m = tab->m;
    int n = tab->n;
    int *saved_basis = (int*)malloc((size_t)m * sizeof(int));
    int *saved_basis_pos = (int*)malloc((size_t)n * sizeof(int));
    VarStatus *saved_status = (VarStatus*)malloc((size_t)n * sizeof(VarStatus));
    double *saved_x = (double*)malloc((size_t)n * sizeof(double));
    int *selected = (int*)malloc((size_t)m * sizeof(int));
    int *used = (int*)calloc((size_t)n, sizeof(int));
    double *q = (double*)calloc((size_t)m * (size_t)m, sizeof(double));
    double *col = (double*)malloc((size_t)m * sizeof(double));
    double *work = (double*)malloc((size_t)m * sizeof(double));
    if (!saved_basis || !saved_basis_pos || !saved_status || !saved_x ||
        !selected || !used || !q || !col || !work) {
        free(saved_basis); free(saved_basis_pos); free(saved_status); free(saved_x);
        free(selected); free(used); free(q); free(col); free(work);
        return -1;
    }

    memcpy(saved_basis, tab->basis, (size_t)m * sizeof(int));
    memcpy(saved_basis_pos, tab->basis_pos, (size_t)n * sizeof(int));
    memcpy(saved_status, tab->var_status, (size_t)n * sizeof(VarStatus));
    memcpy(saved_x, tab->x, (size_t)n * sizeof(double));

    int rank = 0;
    for (int k = 0; k < m; k++) {
        int j = saved_basis[k];
        if (is_artificial_var(tab, j)) continue;
        column_to_dense(tab, j, col);
        if (transition_column_independent(q, m, rank, col, work)) {
            selected[rank++] = j;
            used[j] = 1;
        }
    }

    for (int j = 0; j < n && rank < m; j++) {
        if (used[j]) continue;
        if (is_artificial_var(tab, j)) continue;
        if (tab->var_status[j] == RALPH_FIXED) continue;
        column_to_dense(tab, j, col);
        if (transition_column_independent(q, m, rank, col, work)) {
            selected[rank++] = j;
            used[j] = 1;
        }
    }

    if (rank < m) {
        if (solver && solver->trace_phase1) {
            LP_LOG_STDERR("[phase1_trace] transition_rebasis rank_short rank=%d m=%d\n", rank, m);
        }
        goto fail;
    }

    for (int j = 0; j < n; j++) {
        tab->basis_pos[j] = -1;
        if (is_artificial_var(tab, j)) {
            tab->var_status[j] = RALPH_FIXED;
            tab->x[j] = 0.0;
        } else if (used[j]) {
            tab->var_status[j] = RALPH_BASIC;
        } else {
            if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                tab->x[j] = tab->ub_ext[j];
            } else {
                tab->var_status[j] = RALPH_NONBASIC_LOWER;
                tab->x[j] = tab->lb_ext[j];
            }
        }
    }
    for (int k = 0; k < m; k++) {
        int j = selected[k];
        tab->basis[k] = j;
        tab->basis_pos[j] = k;
        tab->var_status[j] = RALPH_BASIC;
    }
    tab->basis_cache_valid = 0;
    tab->basis_cache_total_nnz = 0;

    if (tableau_refactorize_with_reason(tab, RALPH_REFACTOR_REASON_PHASE_TRANSITION) != 0) {
        goto fail;
    }
    tableau_compute_solution(tab);

    double max_basic_violation = 0.0;
    int worst_basic = -1;
    for (int k = 0; k < m; k++) {
        int j = tab->basis[k];
        double viol = 0.0;
        if (tab->x[j] < tab->lb_ext[j]) {
            viol = tab->lb_ext[j] - tab->x[j];
        } else if (tab->x[j] > tab->ub_ext[j]) {
            viol = tab->x[j] - tab->ub_ext[j];
        }
        if (viol > max_basic_violation) {
            max_basic_violation = viol;
            worst_basic = j;
        }
        if (viol > 1e-4) {
            if (solver && solver->trace_phase1) {
                LP_LOG_STDERR("[phase1_trace] transition_rebasis infeasible max_basic=%.17g worst=%d rank=%d\n",
                        max_basic_violation, worst_basic, rank);
            }
            goto fail;
        }
    }
    for (int k = 0; k < tab->num_artificial; k++) {
        int j = tab->artificial_vars[k];
        if (tab->var_status[j] == RALPH_BASIC || fabs(tab->x[j]) > RALPH_FEAS_TOL) {
            if (solver && solver->trace_phase1) {
                LP_LOG_STDERR("[phase1_trace] transition_rebasis artificial_alive var=%d x=%.17g basic=%d rank=%d\n",
                        j, tab->x[j], tab->var_status[j] == RALPH_BASIC, rank);
            }
            goto fail;
        }
    }

    if (solver && solver->trace_phase1) {
        LP_LOG_STDERR("[phase1_trace] transition_rebasis accepted rank=%d max_basic=%.17g\n",
                rank, max_basic_violation);
    }

    free(saved_basis); free(saved_basis_pos); free(saved_status); free(saved_x);
    free(selected); free(used); free(q); free(col); free(work);
    return 0;

fail:
    memcpy(tab->basis, saved_basis, (size_t)m * sizeof(int));
    memcpy(tab->basis_pos, saved_basis_pos, (size_t)n * sizeof(int));
    memcpy(tab->var_status, saved_status, (size_t)n * sizeof(VarStatus));
    memcpy(tab->x, saved_x, (size_t)n * sizeof(double));
    tab->basis_cache_valid = 0;
    tab->basis_cache_total_nnz = 0;
    free(saved_basis); free(saved_basis_pos); free(saved_status); free(saved_x);
    free(selected); free(used); free(q); free(col); free(work);
    return -1;
}

int tableau_refactorize(SimplexTableau *tab) {
    double t_refactor_ms = lp_telemetry_timer_start();
    SimplexSolver *owner = tab ? tab->owner : NULL;
    int reason = RALPH_REFACTOR_REASON_OTHER;
    int updates_before = (tab && tab->lu) ? lu_get_num_updates(tab->lu) : 0;
    lp_telemetry_begin_refactor(owner, &reason);

    /* Legacy zeroed-row Phase 2 bases need artificial identity columns first.
     * With original rows preserved, do not reorder around unproven redundant
     * rows. */
    if (tab->phase == 2 && tab->num_redundant > 0 &&
        tab->redundant_rows_zeroed && tab->num_artificial > 0) {
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
     * Phase 2: only permit redundant-row regularization if those rows have
     * actually been zeroed into explicit artificial identities.  When Phase 2
     * preserves original rows, regularizing those rows changes the linear
     * system being solved and can certify a solution that violates the
     * original constraints. */
    {
        int allow = 0;
        int reg_limit = 0;
        if (tab->use_two_phase && ((tab->phase == 1 &&
             tab->num_redundant > 0) ||
            (tab->phase == 2 &&
             tab->num_redundant > 0 &&
             tab->redundant_rows_zeroed))) {
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

    /* The old zeroed-row Phase 2 path used a relaxed tolerance for redundant
     * row identities.  With original rows preserved, use the normal tolerance:
     * accepting tiny pivots here risks solving a numerically different basis. */
    double saved_tol = lu_get_pivot_tol(tab->lu);
    if (tab->phase == 2 && tab->num_redundant > 0 && tab->redundant_rows_zeroed) {
        lu_set_pivot_tol(tab->lu, 1e-15);
    }

    int status = lu_factorize(tab->lu, B);

    lu_set_pivot_tol(tab->lu, saved_tol);

    if (status != 0) {
        /* Factorization failed - try to repair the basis */
        int original_lu_failure_reason =
            tab->lu ? (int)lu_get_last_failure_reason(tab->lu) : (int)LU_FAIL_NONE;
        int original_sparse_numeric_failure_reason =
            tab->lu ? tab->lu->telemetry.sparse_numeric_last_failure_reason
                    : (int)LU_SPARSE_NUMERIC_FAIL_NONE;
        status = repair_singular_basis(tab);
        lp_telemetry_record_refactor_repair_outcome(owner,
                                                    tab ? tab->phase : 0,
                                                    original_lu_failure_reason,
                                                    original_sparse_numeric_failure_reason,
                                                    status);
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
    double obj_delta = 0.0;
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

    double *x_basic_backup = tab->primal_basic_x_backup;
    if (leaving_pos != -2 && !x_basic_backup) return -1;

    /* Update entering variable */
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        x[entering] += theta;
    } else {
        x[entering] -= theta;
    }
    obj_delta += tab->c_ext[entering] * (x[entering] - x_enter_old);

    /* Update basic variables and accumulate ||d_entering||^2 in one pass. */
    for (int k = 0; k < tab->m; k++) {
        double dk = work2[k];
        int basic_var = basis[k];
        double dx = -step * dk;
        if (leaving_pos != -2) {
            x_basic_backup[k] = x[basic_var];
        }
        x[basic_var] += dx;
        obj_delta += tab->c_ext[basic_var] * dx;
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
        obj_delta += tab->c_ext[entering] * (tab->x[entering] - x_enter_old -
                                             step);
        tab->obj_value += obj_delta;
        /* Status changed → heap score changed; re-sift to correct position */
        if (tab->pricing_strategy == 4) heap_update(tab, entering);
        return 0;
    }

    /* Normal pivot: swap entering and leaving */
    int leaving = basis[leaving_pos];
    double x_leave_old = x[leaving];
    VarStatus entering_old_status = tab->var_status[entering];
    int entering_is_artificial = tab->is_artificial_var && tab->is_artificial_var[entering];
    int leaving_is_artificial = tab->is_artificial_var && tab->is_artificial_var[leaving];
    int artificial_basic_after_pivot =
        tab->artificial_basic_count + entering_is_artificial - leaving_is_artificial;

    /* Update basis */
    basis[leaving_pos] = entering;
    tab->basis_pos[entering] = leaving_pos;
    tab->basis_pos[leaving] = -1;

    /* Update variable status */
    tab->var_status[entering] = RALPH_BASIC;

    /* Leaving goes to appropriate bound */
    if (tab->work2[leaving_pos] * dir > 0) {
        tab->var_status[leaving] = RALPH_NONBASIC_LOWER;
        obj_delta += tab->c_ext[leaving] * (tab->lb_ext[leaving] - x[leaving]);
        x[leaving] = tab->lb_ext[leaving];
    } else {
        tab->var_status[leaving] = RALPH_NONBASIC_UPPER;
        obj_delta += tab->c_ext[leaving] * (tab->ub_ext[leaving] - x[leaving]);
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

    double *pivot_row = tab->pivot_row;
    double *tau_helper = tab->tau_work;
    int artificials_in_basis =
        (tab->pricing_strategy == 2 && artificial_basic_after_pivot > 0);
    int use_true_se =
        (tab->pricing_strategy == 1 || tab->pricing_strategy == 5) || artificials_in_basis;
    int use_lazy_rc_update = (tab->pricing_strategy == 3);

    if (tab->rc_all_valid && !use_lazy_rc_update) {
        /* Compute pivot row for incremental reduced cost updates using the old
         * basis: pivot_row = e_r^T * B^{-1}. */
        int rhs_idx = leaving_pos;
        double rhs_val = 1.0;
        double t_btran_ms = lp_telemetry_timer_start();
        lu_solve_transpose_sparse(tab->lu, 1, &rhs_idx, &rhs_val, pivot_row);
        if (tab->owner) {
            lp_telemetry_add_btran_timed(tab->owner, t_btran_ms);
        }

        /* Weight update strategy:
         * - SE (pricing_strategy==1): always use exact tau BTRAN
         * - Devex (pricing_strategy==2): use exact tau BTRAN while artificials
         *   remain in the basis (Phase 1), then switch to cheap Devex formula. */
        if (use_true_se && fabs(pivot_sq) > RALPH_ZERO_TOL) {
            t_btran_ms = lp_telemetry_timer_start();
            lu_solve_transpose(tab->lu, tab->work2, tau_helper);
            if (tab->owner) {
                lp_telemetry_add_btran_timed(tab->owner, t_btran_ms);
            }
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
    int artificial_count_refreshed = 0;
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
                lu_set_dense_spike_min_updates_override(
                    tab->lu,
                    simplex_dense_spike_min_updates_override(tab));
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
                    tableau_refresh_artificial_basic_count(tab);
                    artificial_basic_after_pivot = tab->artificial_basic_count;
                    artificial_count_refreshed = 1;
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
    if (!artificial_count_refreshed) {
        tab->artificial_basic_count = artificial_basic_after_pivot;
    }
    tab->obj_value += obj_delta;

    /* In lazy RC mode, partial pricing recomputes requested reduced costs from
     * fresh duals on demand.  No incremental RC or steepest-edge update will be
     * applied below, so avoid the pivot-row BTRAN entirely. */
    if (!tab->rc_all_valid || use_lazy_rc_update) {
        tab->duals_valid = 0;
        tab->rc_all_valid = 0;
        tab->rc[entering] = 0.0;  /* Basic variables have rc = 0 */
        return 0;
    }

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
    for (int k = 0; k < tab->m; k++) {
        tab->x[tab->basis[k]] = x_basic_backup[k];
    }
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
    solver->use_dual_steepest_edge = 0;
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
void extract_unbounded_ray(SimplexSolver *solver, int entering, double dir) {
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
static double simplex_phase1_artificial_sum(const SimplexTableau *tab) {
    double art_sum = 0.0;

    if (!tab || !tab->x || !tab->artificial_vars) return RALPH_INFINITY;
    for (int k = 0; k < tab->num_artificial; k++) {
        int j = tab->artificial_vars[k];
        if (j < 0 || j >= tab->n) return RALPH_INFINITY;
        art_sum += fabs(tab->x[j]);
    }
    return art_sum;
}

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
    double art_sum = simplex_phase1_artificial_sum(tab);

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
        if (solver->trace_phase1 && (iter == 0 || (iter % 1000) == 0)) {
            double trace_art_sum = 0.0;
            double trace_art_max = 0.0;
            int trace_art_basic = 0;
            for (int kk = 0; kk < tab->num_artificial; kk++) {
                int aj = tab->artificial_vars[kk];
                double abs_x = fabs(tab->x[aj]);
                trace_art_sum += abs_x;
                if (abs_x > trace_art_max) trace_art_max = abs_x;
                if (tab->var_status[aj] == RALPH_BASIC) trace_art_basic++;
            }
            LP_LOG_STDERR("[phase1_trace] iter=%d obj=%.17g art_sum=%.17g art_max=%.17g art_basic=%d pivots=%d no_pivot=%d refactors=%d\n",
                          iter,
                          tab->obj_value,
                          trace_art_sum,
                          trace_art_max,
                          trace_art_basic,
                          solver->telemetry.perf_phase1_pivot_calls,
                          solver->telemetry.perf_phase1_no_pivot_events,
                          solver->telemetry.perf_phase1_refactor_calls);
        }
        if (lp_run_user_callbacks(solver, tab, RALPH_LP_PROGRESS_PHASE_1, iter, 0, 1) != 0) {
            primal_remove_perturbation(tab);
            if (tableau_refactorize_with_reason(
                    tab, RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP) == 0) {
                tab->phase1_compute_solution_context =
                    LP_PHASE1_COMPUTE_CTX_NO_ENTERING_CLEANUP;
                tab->phase1_compute_rc_context =
                    LP_PHASE1_COMPUTE_CTX_NO_ENTERING_CLEANUP;
                tableau_compute_solution(tab);
                tableau_compute_reduced_costs(tab);
                if (simplex_phase1_artificial_sum(tab) <= RALPH_FEAS_TOL) {
                    solver->iterations = iter;
                    phase1_trace_emit_summary(solver, RALPH_STATUS_OPTIMAL);
                    return 0;
                }
            }
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
    int art_stuck_zero_replacement = 0;

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
            for (int j = 0; j < tab->num_structural_ext && !found_replacement; j++) {
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
                for (int j = tab->num_structural_ext; j < tab->n; j++) {
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
                /* The replacement search uses a BTRAN row to compute
                 * coefficients. simplex_pivot expects work2 to hold the
                 * FTRAN direction B^{-1} A[:,best_j]. */
                sparse_get_column(tab->A_ext, best_j, tab->work1);
                lu_solve(tab->lu, tab->work1, tab->work2);

                /* Pivot with zero theta since artificial is at zero value. */
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
                if (fabs(best_coef) <= RALPH_PIVOT_TOL) {
                    art_stuck_zero_replacement++;
                }
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

    /* If every stuck artificial row has a zero tableau coefficient against
     * all eligible non-artificial columns, a full non-artificial basis is
     * structurally unavailable.  The dense rebasis scan can only rediscover
     * that rank deficiency, so keep the existing stuck-artificial Phase 2
     * path and preserve the original rows below. */
    if (art_stuck > 0 && art_stuck_zero_replacement < art_stuck) {
        if (transition_rebasis_without_artificials(solver) == 0) {
            art_stuck = 0;
            art_in_basis = 0;
            memset(tab->redundant_rows, 0, tab->m * sizeof(int));
            tab->num_redundant = 0;
            if (solver->verbose) {
                LP_LOG_STDERR("[simplex_transition] Rebuilt full-rank Phase 2 basis without artificial columns\n");
            }
        }
    }

    /* Handle all artificial variables for Phase 2:
     * - Non-basic: fix at [0,0] (FIXED status)
     * - Basic (stuck): set cost=0, bounds=[0,0]. The following Phase 2
     *   feasibility check rejects the transition if the preserved original
     *   rows require any stuck artificial to become nonzero. */
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

    /* Preserve original constraint rows through Phase 2. A stuck artificial
     * at zero is not, by itself, a certificate that the corresponding row is
     * linearly redundant; zeroing such rows can change the feasible region. */
    tab->redundant_rows_zeroed = 0;

    /* Refactorize basis for Phase 2 with original rows preserved. */
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
int phase2_confirm_optimality(SimplexSolver *solver, int iter, int *entering_out) {
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
     * state after the transition. */
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

    /* Initialize iteration state */
    P2IterState st;
    memset(&st, 0, sizeof(st));

    /* D4: Apply proactive perturbation when arriving from dual fallback. */
    if (solver->from_dual_fallback && !tab->use_two_phase) {
        primal_apply_perturbation(tab);
        st.perturbation_active = 1;
    }

    /* Compute initial solution for Phase 2 */
    tableau_compute_solution(tab);
    if (tab->use_two_phase && tab->num_artificial > 0) {
        double art_max = 0.0;
        for (int k = 0; k < tab->num_artificial; k++) {
            int art_j = tab->artificial_vars[k];
            double ax = fabs(tab->x[art_j]);
            if (ax > art_max) art_max = ax;
        }
        if (art_max > RALPH_FEAS_TOL) {
            solver->status = RALPH_STATUS_ERROR;
            solver->iterations = 0;
            return -1;
        }
    }

    st.last_obj = tab->obj_value;
    st.last_entering = -1;
    st.last_leaving = -1;
    st.bland_start_iters = tab->use_two_phase ? 20 : 0;

    /* Compute initial reduced costs. */
    if (solver->pricing_strategy == 3 && !tab->use_two_phase) {
        tableau_compute_duals(tab);
    } else {
        tableau_compute_reduced_costs(tab);
        if (solver->pricing_strategy == 4) heap_build(tab);
    }
    if (st.perturbation_active && solver->telemetry_enabled) {
        solver->telemetry.perf_phase2_perturb_applied++;
    }
    st.phase2_hot_ms_prev = phase_hotpath_ms(solver, 2);

    for (int iter = 0; iter < solver->max_iterations; iter++) {
        tab->iterations = iter;

        /* User callbacks / time limit (stays in orchestrator) */
        if (lp_run_user_callbacks(solver, tab, RALPH_LP_PROGRESS_PHASE_2, iter, 0, 1) != 0) {
            primal_remove_perturbation(tab);
            solver->status = RALPH_STATUS_TIME_LIMIT;
            solver->iterations = iter;
            return -1;
        }

        /* Zone 1: cooldown tick + objective limit */
        P2ZoneResult zr = p2_zone_pre_iter(solver, tab, &st, iter);
        if (zr == P2_ZONE_RETURN_OPTIMAL) return 0;
        if (zr == P2_ZONE_RETURN_FAIL)    return -1;

        /* Zone 2: pricing */
        zr = p2_zone_pricing(solver, tab, &st, iter);
        if (zr == P2_ZONE_CONTINUE)       continue;
        if (zr == P2_ZONE_RETURN_OPTIMAL) return 0;
        if (zr == P2_ZONE_RETURN_FAIL)    return -1;

        /* Zone 3: ratio test */
        zr = p2_zone_ratio(solver, tab, &st, iter);
        if (zr == P2_ZONE_CONTINUE)       continue;
        if (zr == P2_ZONE_RETURN_OPTIMAL) return 0;
        if (zr == P2_ZONE_RETURN_FAIL)    return -1;

        /* Zone 4: pre-pivot (direction telemetry, degeneracy tracking) */
        p2_zone_pre_pivot(solver, tab, &st, iter);

        /* Zone 5: pivot */
        zr = p2_zone_pivot(solver, tab, &st, iter);
        if (zr == P2_ZONE_CONTINUE)    continue;
        if (zr == P2_ZONE_RETURN_FAIL) return -1;

        /* Zone 6: post-pivot (refactor policy, recompute, stall detection) */
        zr = p2_zone_post_pivot(solver, tab, &st, iter);
        if (zr == P2_ZONE_CONTINUE)    continue;
        if (zr == P2_ZONE_RETURN_FAIL) return -1;
    }

    primal_remove_perturbation(tab);
    solver->status = RALPH_STATUS_ITERATION_LIMIT;
    solver->iterations = solver->max_iterations;
    return -1;
}

/* Triangular crash basis moved to simplex_crash.c (R3.9) */


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
    dst->num_structural_ext = src->num_structural_ext;
    dst->num_aux = src->num_aux + extra_aux;
    dst->num_equalities = src->num_equalities + extra_eq;
    dst->use_two_phase = (src->use_two_phase || extra_art > 0) ? 1 : 0;

    if (tableau_alloc_arrays(dst, dst->num_aux, src->num_artificial + extra_art) != 0) {
        tableau_free(dst);
        return NULL;
    }

    memcpy(dst->lb_ext, src->lb_ext, (size_t)src->n * sizeof(double));
    memcpy(dst->ub_ext, src->ub_ext, (size_t)src->n * sizeof(double));
    memcpy(dst->free_split_col, src->free_split_col, (size_t)src->n * sizeof(int));
    memcpy(dst->free_split_orig, src->free_split_orig, (size_t)src->n * sizeof(int));
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
            int split_col = dst->free_split_col[col];
            if (split_col >= 0 &&
                triplets_add(trips, row_idx, split_col, -value) != 0) {
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
    memset(dst->is_artificial_var, 0, (size_t)dst->n * sizeof(unsigned char));
    for (int k = 0; k < dst->num_artificial; k++) {
        int art_j = dst->artificial_vars[k];
        if (art_j >= 0 && art_j < dst->n) {
            dst->is_artificial_var[art_j] = 1;
        }
    }

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
    tableau_refresh_artificial_basic_count(dst);
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

static int simplex_should_use_dense_small_phase1_partial(const SimplexSolver *solver,
                                                         const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Dense, small Phase-1 tableaus make full Devex scans disproportionately
     * expensive while partial pricing still sees enough of the entering set.
     * Keep this conservative: larger or sparser NETLIB cases showed new
     * failures when partial Phase 1 was applied globally. */
    return (m <= 220 && density >= 0.05);
}

static int simplex_should_use_sparse_midrow_phase1_partial(const SimplexSolver *solver,
                                                           const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Sparse mid-row Phase-1 tableaus pay a high O(n) Devex scan/update cost
     * while each pivot touches relatively little matrix structure. Partial
     * pricing is allowed only in this moderate row band; higher-row NETLIB
     * cases need stronger full pricing to avoid Phase-1 stalls. */
    return (m >= 520 && m <= 680 &&
            n >= 1000 && n <= 1400 &&
            width_ratio >= 1.7 &&
            density >= 0.004 && density <= 0.010);
}

static int simplex_should_use_sparse_bridge_phase1_partial(const SimplexSolver *solver,
                                                           const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Mid/large sparse Phase-1 tableaus in this bridge band spend heavily on
     * exact Devex/steepest-edge maintenance while partial pricing still keeps
     * enough entering coverage. Keep the band away from nearby lower-width
     * presolved cases and wider NETLIB models that regress under partial. */
    return (m >= 780 && m <= 1350 &&
            n >= 1550 && n <= 1750 &&
            width_ratio >= 1.2 && width_ratio <= 2.1 &&
            density >= 0.003 && density <= 0.010);
}

static int simplex_should_use_mid_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                        const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Mid-row sparse Phase-1 tableaus can spend more maintaining Devex weights
     * than they gain from the stronger pivot score. Keep the selector bounded
     * to the observed shape class; smaller near-square cases regress under
     * Dantzig, and very wide cases have different numerical behavior. */
    return (m >= 350 && m <= 600 &&
            n >= 800 && n <= 1200 &&
            width_ratio >= 1.5 &&
            density >= 0.01 && density <= 0.03);
}

static int simplex_should_use_very_sparse_large_dantzig(const SimplexSolver *solver,
                                                        const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Very large, very sparse tableaus spend heavily on Devex bookkeeping.
     * Dantzig gives up some pivot quality but cuts enough pricing/update work
     * to win on this structural class. */
    return (m >= 1500 && n >= 8000 && density <= 0.0015);
}

static int simplex_should_use_large_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Large, very sparse Phase-1 tableaus with only moderate width can spend
     * heavily on Devex maintenance before feasibility.  Dantzig is limited to
     * this high-row sparse band; lower-row sparse cases such as bnl1 still need
     * Devex to avoid Phase-1 stalls. */
    return (m >= 1800 && m <= 2800 &&
            n >= 2500 && n <= 4500 &&
            width_ratio >= 1.2 && width_ratio <= 2.0 &&
            density >= 0.001 && density <= 0.003);
}

static int simplex_should_use_large_moderate_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                                   const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;
    if (!tab->use_two_phase || tab->num_artificial <= 0) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    double width_ratio = (double)n / (double)m;
    /* Very large, moderately sparse Phase-1 tableaus can stall under Devex
     * before feasibility.  Keep this Phase-1-only selector away from smaller
     * pilot-class LPs and much sparser fit-class LPs. */
    return (m >= 3000 && m <= 3300 &&
            n >= 9000 && n <= 10000 &&
            width_ratio >= 2.8 && width_ratio <= 3.2 &&
            density >= 0.004 && density <= 0.006);
}

static int simplex_should_use_mid_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                         const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Sparse mid-size cases in this band get cheaper Phase 1 and Phase 2
     * progress from Dantzig. Nearby lower-row shapes such as bnl1/pilot4
     * regress, so keep the row and density band tight. */
    return (m >= 790 && m <= 900 &&
            n >= 1400 && n <= 1600 &&
            density >= 0.007 && density <= 0.009);
}

static int simplex_should_use_sparse_grow_phase12_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    return (m >= 350 && m <= 500 &&
            n >= 900 && n <= 1100 &&
            density >= 0.018 && density <= 0.022);
}

static int simplex_should_use_scsd_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    return (m >= 350 && m <= 450 &&
            n >= 2400 && n <= 3000 &&
            density >= 0.006 && density <= 0.009);
}

static int simplex_should_use_dense_lowrow_phase2_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab) {
    if (!solver || !solver->model || !solver->model->A || !tab) return 0;

    int m = solver->model->num_cons;
    int n = solver->model->num_vars;
    int nnz = solver->model->A->nnz;
    if (m <= 0 || n <= 0 || nnz <= 0) return 0;

    double density = (double)nnz / ((double)m * (double)n);
    /* Very low-row dense NETLIB LPs spend more in Devex weight maintenance than
     * they recover from better pricing. Keep this selector narrow; blanket
     * Dantzig pricing regresses wider sparse models such as fit1p. */
    return (m <= 30 && n >= 1000 && density >= 0.30);
}

static int simplex_should_skip_auto_dual_startup(const SimplexSolver *solver) {
    if (!solver || !solver->model) return 0;

    int m = solver->model->num_cons;
    if (m <= 0) return 0;

    /* In auto mode the scratch dual solve is speculative: if it does not return
     * a verified optimum, the solver pays the full dual startup cost and then
     * runs the primal path anyway.  On NETLIB-scale models up through roughly
     * 1500 rows that failed-dual toll dominates, while the larger sparse cases
     * still have enough Phase 2 work for a dual attempt to be useful.  Explicit
     * dual mode keeps the dual path; this only governs method=2 dispatch. */
    return (m <= 1500);
}

static int simplex_finish_prepared_primal_solve(SimplexSolver *solver, clock_t start) {
    if (!solver || !solver->tableau) return -1;

    SimplexTableau *tab = solver->tableau;

    /* T3.4: Override pricing strategy for Phase 1 if configured.
     * Two-phase simplex requires full pricing during Phase 1 — partial pricing
     * can miss improving directions for artificial variables. Default to Devex
     * for Phase 1 when partial/heap pricing is selected. */
    int saved_pricing = solver->pricing_strategy;
    int saved_phase1_pricing = solver->phase1_pricing;
    int saved_tab_pricing = tab->pricing_strategy;
    int saved_tab_se = tab->use_steepest_edge;
    if (solver->phase1_pricing >= 0) {
        solver->pricing_strategy = solver->phase1_pricing;
        tab->pricing_strategy = solver->phase1_pricing;
        tab->use_steepest_edge = (solver->phase1_pricing == 1 || solver->phase1_pricing == 2
                                  || solver->phase1_pricing == 5);
    } else if (simplex_should_use_dense_small_phase1_partial(solver, tab)) {
        solver->pricing_strategy = 3;
        tab->pricing_strategy = 3;
        tab->use_steepest_edge = 0;
    } else if (simplex_should_use_sparse_midrow_phase1_partial(solver, tab)) {
        solver->pricing_strategy = 3;
        tab->pricing_strategy = 3;
        tab->use_steepest_edge = 0;
    } else if (simplex_should_use_sparse_bridge_phase1_partial(solver, tab)) {
        solver->phase1_pricing = 3;
        solver->pricing_strategy = 3;
        tab->pricing_strategy = 3;
        tab->use_steepest_edge = 0;
    } else if (simplex_should_use_mid_sparse_phase1_dantzig(solver, tab)) {
        solver->pricing_strategy = 0;
        tab->pricing_strategy = 0;
        tab->use_steepest_edge = 0;
    } else if (simplex_should_use_very_sparse_large_dantzig(solver, tab)) {
        solver->pricing_strategy = 0;
        tab->pricing_strategy = 0;
        tab->use_steepest_edge = 0;
    } else if (simplex_should_use_large_sparse_phase1_dantzig(solver, tab)) {
        solver->phase1_pricing = 0;
        solver->pricing_strategy = 0;
        tab->pricing_strategy = 0;
        tab->use_steepest_edge = 0;
    } else if (simplex_should_use_large_moderate_sparse_phase1_dantzig(solver, tab)) {
        solver->pricing_strategy = 0;
        tab->pricing_strategy = 0;
        tab->use_steepest_edge = 0;
    } else if (simplex_should_use_mid_sparse_phase12_dantzig(solver, tab) ||
               simplex_should_use_sparse_grow_phase12_dantzig(solver, tab) ||
               simplex_should_use_scsd_sparse_phase12_dantzig(solver, tab)) {
        solver->pricing_strategy = 0;
        tab->pricing_strategy = 0;
        tab->use_steepest_edge = 0;
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
            solver->phase1_pricing = saved_phase1_pricing;
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
    solver->phase1_pricing = saved_phase1_pricing;
    solver->pricing_strategy = saved_pricing;
    tab->pricing_strategy = saved_tab_pricing;
    tab->use_steepest_edge = saved_tab_se;
    if (solver->verbose) LP_LOG_STDOUT("[simplex_solve] Phase 1 complete\n");

    if (saved_pricing == 2 &&
        (simplex_should_use_dense_lowrow_phase2_dantzig(solver, tab) ||
         simplex_should_use_very_sparse_large_dantzig(solver, tab) ||
         simplex_should_use_mid_sparse_phase12_dantzig(solver, tab) ||
         simplex_should_use_sparse_grow_phase12_dantzig(solver, tab) ||
         simplex_should_use_scsd_sparse_phase12_dantzig(solver, tab))) {
        solver->pricing_strategy = 0;
        tab->pricing_strategy = 0;
        tab->use_steepest_edge = 0;
    }

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
        int two_phase_status = solver->status;
        int two_phase_iterations = solver->iterations;
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
        if (dual_result != 0 &&
            solver->status == RALPH_STATUS_UNKNOWN &&
            two_phase_status != RALPH_STATUS_UNKNOWN) {
            solver->status = two_phase_status;
            solver->iterations = two_phase_iterations;
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
            double xj = tab->x[j];
            int split_col = tab->free_split_col ? tab->free_split_col[j] : -1;
            if (split_col >= 0) {
                xj -= tab->x[split_col];
            }
            true_obj += tab->c_ext[j] * xj;
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
                int split_col = tab->free_split_col ? tab->free_split_col[j] : -1;
                if (split_col >= 0) {
                    solver->solution[j] -= tab->x[split_col];
                }
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

    if (solver->status == RALPH_STATUS_OPTIMAL && solver->solution) {
        double true_obj = 0.0;
        for (int j = 0; j < solver->model->num_vars; j++) {
            true_obj += solver->model->c[j] * solver->solution[j];
        }
        solver->obj_value = true_obj + solver->model->obj_offset;
    }

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
    int skip_auto_dual_startup =
        (solver->method == 2 && simplex_should_skip_auto_dual_startup(solver));
    if (solver->method == 1 || (solver->method == 2 && !skip_auto_dual_startup)) {
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
            if (solver->status == RALPH_STATUS_OPTIMAL && solver->solution) {
                double true_obj = 0.0;
                for (int j = 0; j < solver->model->num_vars; j++) {
                    true_obj += solver->model->c[j] * solver->solution[j];
                }
                solver->obj_value = true_obj * solver->model->obj_sense + solver->model->obj_offset;
            }

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
        if (skip_auto_dual_startup && solver->verbose) {
            LP_LOG_STDOUT("[simplex_solve] Skipping speculative dual startup in auto mode\n");
        }
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
