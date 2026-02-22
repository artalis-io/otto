#ifndef RALPH_MIP_LP_ADAPTER_H
#define RALPH_MIP_LP_ADAPTER_H

#include "mip.h"

/* Stage a warm basis on an LP solver for one-shot cold-start application. */
static inline int mip_lp_stage_warm_basis(SimplexSolver *lp, int m, int n,
                                          const int *basis, const VarStatus *var_status) {
    if (!lp) return -1;
    return simplex_set_warm_basis(lp, m, n, basis, var_status);
}

/* Apply a warm basis to an existing tableau and restore a consistent LP state. */
static inline int mip_lp_restore_warm_basis(SimplexSolver *lp, int m, int n,
                                            const int *basis, const VarStatus *var_status);

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
    return mip_lp_refactor_and_recompute(lp->tableau);
}

#endif /* RALPH_MIP_LP_ADAPTER_H */
