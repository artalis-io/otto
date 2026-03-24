#ifndef RALPH_MIP_LP_ADAPTER_H
#define RALPH_MIP_LP_ADAPTER_H

#include <math.h>
#include "mip.h"

typedef enum {
    MIP_LP_DUAL_FAIL_NONE = 0,
    MIP_LP_DUAL_FAIL_ERROR = 1,
    MIP_LP_DUAL_FAIL_ITERATION_LIMIT = 2,
    MIP_LP_DUAL_FAIL_TIME_LIMIT = 3,
    MIP_LP_DUAL_FAIL_OTHER = 4
} MIPLPDualFailReason;

/* Stage a warm basis on an LP solver for one-shot cold-start application. */
static inline int mip_lp_stage_warm_basis(SimplexSolver *lp, int m, int n,
                                          const int *basis, const VarStatus *var_status) {
    if (!lp) return -1;
    return simplex_set_warm_basis(lp, m, n, basis, var_status);
}

/* Apply a warm basis to an existing tableau and restore a consistent LP state. */
static inline int mip_lp_restore_warm_basis(SimplexSolver *lp, int m, int n,
                                            const int *basis, const VarStatus *var_status);

/* Refresh solver-visible primal/dual/objective arrays from the live tableau. */
static inline int mip_lp_sync_solver_outputs(SimplexSolver *lp) {
    if (!lp || !lp->tableau || !lp->model) return -1;

    SimplexTableau *tab = lp->tableau;
    int n_orig = lp->model->num_vars;
    int m_cons = lp->model->num_cons;

    if (!lp->solution) {
        lp->solution = (double*)calloc((size_t)n_orig, sizeof(double));
        if (!lp->solution) return -1;
    }
    if (!lp->dual_solution) {
        lp->dual_solution = (double*)calloc((size_t)m_cons, sizeof(double));
        if (!lp->dual_solution) return -1;
    }
    if (!lp->reduced_costs) {
        lp->reduced_costs = (double*)calloc((size_t)n_orig, sizeof(double));
        if (!lp->reduced_costs) return -1;
    }

    lp->obj_value = tab->obj_value * lp->model->obj_sense + lp->model->obj_offset;

    for (int j = 0; j < n_orig; j++) {
        double x = tab->x ? tab->x[j] : 0.0;
        double rc = tab->rc ? tab->rc[j] : 0.0;
        if (lp->is_scaled && lp->col_scale) {
            x *= lp->col_scale[j];
            if (fabs(lp->col_scale[j]) > RALPH_ZERO_TOL) {
                rc /= lp->col_scale[j];
            }
        }
        lp->solution[j] = x;
        lp->reduced_costs[j] = rc * lp->model->obj_sense;
    }

    for (int i = 0; i < m_cons; i++) {
        double y = tab->y ? tab->y[i] : 0.0;
        if (lp->is_scaled && lp->row_scale) {
            y *= lp->row_scale[i];
        }
        lp->dual_solution[i] = y * lp->model->obj_sense;
    }

    return 0;
}

/* Update structural (original-variable) bounds in the active tableau. */
static inline int mip_lp_apply_structural_bounds(SimplexTableau *tab, int num_struct_vars,
                                                 const double *lb, const double *ub) {
    if (!tab || !lb || !ub) return -1;
    return tableau_apply_structural_bounds(tab, num_struct_vars, lb, ub);
}

/* Prepare tableau state for robust dual reoptimization. */
static inline void mip_lp_prepare_dual_reopt(SimplexTableau *tab) {
    if (!tab) return;
    dual_v2_clear_perturbation(tab);
    tab->dse_initialized = 0;
}

/* Recompute primal solution and reduced costs for current tableau state. */
static inline int mip_lp_recompute(SimplexTableau *tab) {
    if (!tab) return -1;
    if (tableau_compute_solution(tab) != 0) return -1;
    if (tableau_compute_reduced_costs(tab) != 0) return -1;
    return 0;
}

/* Run dual simplex with an optional pivot budget. */
static inline int mip_lp_dual_reopt(SimplexSolver *lp, int iter_budget, int *rc_out) {
    if (!lp) return -1;
    if (lp->tableau) mip_lp_prepare_dual_reopt(lp->tableau);

    int save_max_iter = lp->max_iterations;
    if (iter_budget > 0) lp->max_iterations = iter_budget;
    int rc = dual_simplex_solve_v2(lp);
    lp->max_iterations = save_max_iter;
    if (rc_out) *rc_out = rc;
    return rc;
}

/* Reoptimize an already-prepared tableau via primal simplex without rebuilding it. */
static inline int mip_lp_primal_reopt(SimplexSolver *lp) {
    if (!lp || !lp->tableau) return -1;

    int save_method = lp->method;
    lp->method = 0;
    int rc = simplex_resolve_prepared_primal_tableau(lp);
    lp->method = save_method;
    return rc;
}

static inline MIPLPDualFailReason mip_lp_dual_fail_reason(const SimplexSolver *lp) {
    if (!lp) return MIP_LP_DUAL_FAIL_OTHER;
    switch (lp->status) {
        case RALPH_STATUS_ERROR:
            return MIP_LP_DUAL_FAIL_ERROR;
        case RALPH_STATUS_ITERATION_LIMIT:
            return MIP_LP_DUAL_FAIL_ITERATION_LIMIT;
        case RALPH_STATUS_TIME_LIMIT:
            return MIP_LP_DUAL_FAIL_TIME_LIMIT;
        default:
            return MIP_LP_DUAL_FAIL_OTHER;
    }
}

static inline int mip_lp_needs_dual_repair(const SimplexSolver *lp) {
    return lp && lp->tableau &&
           (lp->tableau->phase == 1 ||
            lp->tableau->num_artificial > 0 ||
            lp->tableau->use_two_phase);
}

/* Refactorize the current basis and recompute solution/reduced costs. */
static inline int mip_lp_refactor_and_recompute(SimplexTableau *tab) {
    if (!tab) return -1;
    if (tableau_refactorize(tab) != 0) return -1;
    mip_lp_prepare_dual_reopt(tab);
    return mip_lp_recompute(tab);
}

/* Cold-start solve via primal simplex. */
static inline int mip_lp_cold_start_primal(SimplexSolver *lp, int restore_method) {
    if (!lp) return -1;

    int save_method = lp->method;
    if (lp->tableau) {
        tableau_free(lp->tableau);
        lp->tableau = NULL;
    }

    lp->method = 0;
    simplex_solve(lp);
    lp->method = (restore_method >= 0) ? restore_method : save_method;

    return (lp->status == RALPH_STATUS_OPTIMAL ||
            lp->status == RALPH_STATUS_INFEASIBLE ||
            lp->status == RALPH_STATUS_OBJ_LIMIT) ? 0 : -1;
}

/* Ensure LP state is usable (solution+tableau), cold-starting if needed. */
static inline int mip_lp_recover_state(SimplexSolver *lp) {
    if (!lp) return -1;
    if (lp->tableau && lp->solution) return 0;
    if (mip_lp_cold_start_primal(lp, 2) != 0) return -1;
    return (lp->tableau && lp->solution) ? 0 : -1;
}

static inline int mip_lp_restore_warm_basis(SimplexSolver *lp, int m, int n,
                                            const int *basis, const VarStatus *var_status) {
    if (!lp || !lp->tableau || !basis || !var_status) return -1;
    if (tableau_apply_warm_basis(lp->tableau, m, n, basis, var_status) != 0) return -1;
    if (mip_lp_refactor_and_recompute(lp->tableau) != 0) return -1;
    return mip_lp_sync_solver_outputs(lp);
}

#endif /* RALPH_MIP_LP_ADAPTER_H */
