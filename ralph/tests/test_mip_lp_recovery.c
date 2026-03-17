/*
 * Ralph - Phase 4 MIP/LP Recovery Contract Tests
 *
 * Focused tests for:
 * 1) strong-branch probe safety (basis/bounds/state restored after probing)
 * 2) root LP recovery contract used by cut-loop fallback
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "lp.h"
#include "mip.h"

/* Internal test-only hook from ralph.c */
LPModel* ralph_get_lp_model(const RalphModel *model);

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

#define ASSERT_NEAR(a, b, tol, msg) \
    ASSERT(fabs((a) - (b)) <= (tol), msg)

static RalphModel* build_fractional_probe_model(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    /* max x + y
     * s.t. x + y <= 1.5
     *      x, y binary
     * Root LP has a fractional optimum. */
    int x = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    int y = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }
    {
        int idx[] = {x, y};
        double val[] = {1.0, 1.0};
        if (ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 1.5) != 0) {
            ralph_test_free(model);
            return NULL;
        }
    }
    return model;
}

static RalphModel* build_root_recovery_model(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    int x = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    int y = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }
    {
        int idx[] = {x, y};
        double val[] = {1.0, 1.0};
        if (ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 1.0) != 0) {
            ralph_test_free(model);
            return NULL;
        }
    }
    return model;
}

static int force_fixed_branch_var_cb(void *user_data,
                                     const double *x_relaxation,
                                     int num_vars,
                                     const int *is_integer,
                                     const double *lb,
                                     const double *ub) {
    (void)x_relaxation;
    (void)num_vars;
    (void)is_integer;
    (void)lb;
    (void)ub;
    if (!user_data) return -1;
    return *(const int*)user_data;
}

static void reset_reliability_counts(MIPSolver *mip) {
    if (!mip) return;
    for (int k = 0; k < mip->num_integers; k++) {
        int j = mip->integer_vars[k];
        mip->pseudo_count_down[j] = 0;
        mip->pseudo_count_up[j] = 0;
    }
}

static void force_fractional_integer_solution(MIPSolver *mip, double value) {
    if (!mip || !mip->lp_solver || !mip->lp_solver->solution) return;
    for (int k = 0; k < mip->num_integers; k++) {
        int j = mip->integer_vars[k];
        mip->lp_solver->solution[j] = value;
    }
}

static void test_strong_branch_probe_contract(void) {
    RalphModel *model = build_fractional_probe_model();
    ASSERT(model != NULL, "Probe model created");
    if (!model) return;

    LPModel *lp_model = ralph_get_lp_model(model);
    ASSERT(lp_model != NULL, "Internal LP model handle available");
    if (!lp_model) {
        ralph_test_free(model);
        return;
    }

    MIPSolver *mip = mip_create(lp_model, 0, 128);
    ASSERT(mip != NULL, "MIP solver created");
    if (!mip) {
        ralph_test_free(model);
        return;
    }
    mip->verbose = 0;

    ASSERT(mip_recover_root_relaxation(mip) == 0, "Root LP recovery builds initial LP state");
    ASSERT(mip->lp_solver && mip->lp_solver->tableau && mip->lp_solver->solution,
           "LP tableau+solution available for probing");
    if (!mip->lp_solver || !mip->lp_solver->tableau || !mip->lp_solver->solution) {
        mip_free(mip);
        ralph_test_free(model);
        return;
    }

    SimplexTableau *tab = mip->lp_solver->tableau;
    int m = tab->m;
    int n = tab->n;
    int num_struct = mip->working_model->num_vars;

    int probe_var = -1;
    double probe_val = 0.0;
    for (int k = 0; k < mip->num_integers; k++) {
        int j = mip->integer_vars[k];
        double v = mip->lp_solver->solution[j];
        double frac = v - floor(v);
        if (frac < 0.0) frac += 1.0;
        if (frac > RALPH_INT_TOL && frac < 1.0 - RALPH_INT_TOL) {
            probe_var = j;
            probe_val = v;
            break;
        }
    }
    if (probe_var < 0 && mip->num_integers > 0) {
        probe_var = mip->integer_vars[0];
        probe_val = 0.5;  /* Deterministic fallback split for contract testing. */
    }
    ASSERT(probe_var >= 0, "Found probing variable for strong-branch contract test");
    if (probe_var < 0) {
        mip_free(mip);
        ralph_test_free(model);
        return;
    }

    int *basis_before = (int*)malloc((size_t)m * sizeof(int));
    VarStatus *status_before = (VarStatus*)malloc((size_t)n * sizeof(VarStatus));
    double *lb_before = (double*)malloc((size_t)num_struct * sizeof(double));
    double *ub_before = (double*)malloc((size_t)num_struct * sizeof(double));
    ASSERT(basis_before && status_before && lb_before && ub_before,
           "Allocated probe snapshots");
    if (!basis_before || !status_before || !lb_before || !ub_before) {
        free(basis_before);
        free(status_before);
        free(lb_before);
        free(ub_before);
        mip_free(mip);
        ralph_test_free(model);
        return;
    }

    memcpy(basis_before, tab->basis, (size_t)m * sizeof(int));
    memcpy(status_before, tab->var_status, (size_t)n * sizeof(VarStatus));
    memcpy(lb_before, tab->lb_ext, (size_t)num_struct * sizeof(double));
    memcpy(ub_before, tab->ub_ext, (size_t)num_struct * sizeof(double));

    double down_obj = RALPH_INFINITY;
    double up_obj = RALPH_INFINITY;
    ASSERT(strong_branch(mip, probe_var, probe_val, &down_obj, &up_obj,
                         MIP_RELIABILITY_PIVOT_BUDGET) == 0,
           "Strong-branch probe succeeds");
    ASSERT(mip->lp_solver && mip->lp_solver->tableau && mip->lp_solver->solution,
           "Strong-branch leaves LP state usable");

    tab = mip->lp_solver->tableau;
    ASSERT(tab->m == m && tab->n == n, "Tableau dimensions unchanged after probing");
    ASSERT(memcmp(tab->basis, basis_before, (size_t)m * sizeof(int)) == 0,
           "Basis restored after probing");
    ASSERT(memcmp(tab->var_status, status_before, (size_t)n * sizeof(VarStatus)) == 0,
           "Var status restored after probing");
    ASSERT(memcmp(tab->lb_ext, lb_before, (size_t)num_struct * sizeof(double)) == 0,
           "Bounds restored after probing (lb)");
    ASSERT(memcmp(tab->ub_ext, ub_before, (size_t)num_struct * sizeof(double)) == 0,
           "Bounds restored after probing (ub)");
    ASSERT(mip->strong_branch_probes > 0, "Probe telemetry increments");
    ASSERT(mip->strong_branch_failures == 0, "Probe succeeded without failure");
    ASSERT(down_obj < RALPH_INFINITY / 2 || up_obj < RALPH_INFINITY / 2,
           "At least one probe branch returned finite LP bound");

    free(basis_before);
    free(status_before);
    free(lb_before);
    free(ub_before);
    mip_free(mip);
    ralph_test_free(model);
}

static void test_branch_selector_skips_fixed_integer_vars(void) {
    RalphModel *model = build_fractional_probe_model();
    ASSERT(model != NULL, "Selector model created");
    if (!model) return;

    LPModel *lp_model = ralph_get_lp_model(model);
    ASSERT(lp_model != NULL, "Selector internal LP model handle available");
    if (!lp_model) {
        ralph_test_free(model);
        return;
    }

    MIPSolver *mip = mip_create(lp_model, 0, 128);
    ASSERT(mip != NULL, "Selector MIP solver created");
    if (!mip) {
        ralph_test_free(model);
        return;
    }
    mip->verbose = 0;

    ASSERT(mip_recover_root_relaxation(mip) == 0, "Selector root LP recovery succeeds");
    ASSERT(mip->lp_solver && mip->lp_solver->solution, "Selector LP solution available");
    if (!mip->lp_solver || !mip->lp_solver->solution) {
        mip_free(mip);
        ralph_test_free(model);
        return;
    }

    const int fixed_var = 0;
    const int free_var = 1;
    ASSERT(mip->working_model && mip->working_model->num_vars >= 2,
           "Selector working model has expected size");
    if (!mip->working_model || mip->working_model->num_vars < 2) {
        mip_free(mip);
        ralph_test_free(model);
        return;
    }

    /* Simulate current-node bounds where var 0 is fixed and var 1 remains branchable. */
    mip->working_model->lb[fixed_var] = 0.0;
    mip->working_model->ub[fixed_var] = 0.0;
    mip->working_model->lb[free_var] = 0.0;
    mip->working_model->ub[free_var] = 1.0;

    /* Priority path: fixed var has higher priority but must still be skipped. */
    if (!mip->branch_priorities) {
        mip->branch_priorities = (int*)calloc((size_t)mip->working_model->num_vars, sizeof(int));
    }
    ASSERT(mip->branch_priorities != NULL, "Selector branch priorities allocated");
    if (!mip->branch_priorities) {
        mip_free(mip);
        ralph_test_free(model);
        return;
    }
    mip->branch_priorities[fixed_var] = 100;
    mip->branch_priorities[free_var] = 1;

    /* Deterministic reliability path: skip strong branching in this unit test. */
    mip->pseudo_count_down[fixed_var] = MIP_RELIABILITY_THRESHOLD;
    mip->pseudo_count_up[fixed_var] = MIP_RELIABILITY_THRESHOLD;
    mip->pseudo_count_down[free_var] = MIP_RELIABILITY_THRESHOLD;
    mip->pseudo_count_up[free_var] = MIP_RELIABILITY_THRESHOLD;

    /* Stale/contradictory LP state: fixed var appears fractional in solution.
     * Selector must still pick the branchable free var. */
    mip->lp_solver->solution[fixed_var] = 0.5;
    mip->lp_solver->solution[free_var] = 0.5;

    int branch_var = -1;
    mip->var_select = VAR_SELECT_MAX_INFEAS;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
           "MAX_INFEAS selector succeeds");
    ASSERT(branch_var == free_var, "MAX_INFEAS selector skips fixed var");

    branch_var = -1;
    mip->var_select = VAR_SELECT_PSEUDO_COST;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
           "PSEUDO_COST selector succeeds");
    ASSERT(branch_var == free_var, "PSEUDO_COST selector skips fixed var");

    branch_var = -1;
    mip->var_select = VAR_SELECT_RELIABILITY;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
           "RELIABILITY selector succeeds");
    ASSERT(branch_var == free_var, "RELIABILITY selector skips fixed var");

    /* Callback attempts to force branching on fixed var; selector must reject it
     * and fall back to a valid branchable fractional variable. */
    {
        int forced = fixed_var;
        RalphBranchCallback cb;
        memset(&cb, 0, sizeof(cb));
        cb.select_branch_var = force_fixed_branch_var_cb;
        cb.user_data = &forced;
        mip->branch_callback = cb;
        mip->has_branch_callback = 1;

        branch_var = -1;
        mip->var_select = VAR_SELECT_MAX_INFEAS;
        ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
               "Callback fallback selector succeeds");
        ASSERT(branch_var == free_var, "Callback-forced fixed var is rejected");

        mip->has_branch_callback = 0;
        memset(&mip->branch_callback, 0, sizeof(mip->branch_callback));
    }

    /* If only the fixed variable appears fractional, selector must report none. */
    mip->lp_solver->solution[fixed_var] = 0.5;
    mip->lp_solver->solution[free_var] = 1.0;

    branch_var = -1;
    mip->var_select = VAR_SELECT_MAX_INFEAS;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == -1,
           "Selector returns no variable when only fixed var is fractional");

    mip_free(mip);
    ralph_test_free(model);
}

static void test_reliability_no_incumbent_probe_throttle(void) {
    RalphModel *model = build_fractional_probe_model();
    ASSERT(model != NULL, "Throttle model created");
    if (!model) return;

    LPModel *lp_model = ralph_get_lp_model(model);
    ASSERT(lp_model != NULL, "Throttle internal LP model handle available");
    if (!lp_model) {
        ralph_test_free(model);
        return;
    }

    MIPSolver *mip = mip_create(lp_model, 0, 128);
    ASSERT(mip != NULL, "Throttle MIP solver created");
    if (!mip) {
        ralph_test_free(model);
        return;
    }
    mip->verbose = 0;
    mip->var_select = VAR_SELECT_RELIABILITY;

    ASSERT(mip_recover_root_relaxation(mip) == 0, "Throttle root LP recovery succeeds");
    ASSERT(mip->lp_solver && mip->lp_solver->solution, "Throttle LP solution available");
    if (!mip->lp_solver || !mip->lp_solver->solution) {
        mip_free(mip);
        ralph_test_free(model);
        return;
    }

    /* Early search (no incumbent, shallow tree): reliability should probe. */
    reset_reliability_counts(mip);
    force_fractional_integer_solution(mip, 0.5);
    mip->has_incumbent = 0;
    mip->nodes_explored = 0;
    int probes_before = mip->strong_branch_probes;
    int branch_var = -1;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
           "Reliability selector succeeds in early search");
    ASSERT(branch_var >= 0, "Reliability selector returns a branch variable");
    ASSERT(mip->strong_branch_probes > probes_before,
           "Reliability probing active in early no-incumbent search");
    ASSERT(mip->strong_branch_probes - probes_before <= MIP_RELIABILITY_MAX_STRONG,
           "Early no-incumbent probing respects per-node probe cap");

    /* Deep no-incumbent search: strong probing should be throttled off. */
    ASSERT(mip_recover_root_relaxation(mip) == 0, "Throttle LP recovery before deep-search check succeeds");
    reset_reliability_counts(mip);
    force_fractional_integer_solution(mip, 0.5);
    mip->has_incumbent = 0;
    mip->nodes_explored = MIP_RELIABILITY_NO_INCUMBENT_DISABLE_AFTER + 8;
    probes_before = mip->strong_branch_probes;
    branch_var = -1;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
           "Reliability selector succeeds in deep no-incumbent search");
    ASSERT(branch_var >= 0, "Deep no-incumbent selector returns branch variable");
    ASSERT(mip->strong_branch_probes == probes_before,
           "Deep no-incumbent reliability skips strong probes");

    /* Early post-incumbent search keeps only a small bootstrap probe budget. */
    ASSERT(mip_recover_root_relaxation(mip) == 0, "Throttle LP recovery before post-incumbent bootstrap succeeds");
    reset_reliability_counts(mip);
    force_fractional_integer_solution(mip, 0.5);
    mip->has_incumbent = 1;
    mip->nodes_explored = 0;
    probes_before = mip->strong_branch_probes;
    branch_var = -1;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
           "Reliability selector succeeds in early post-incumbent search");
    ASSERT(branch_var >= 0, "Early post-incumbent selector returns branch variable");
    ASSERT(mip->strong_branch_probes > probes_before,
           "Early post-incumbent reliability still allows bootstrap probing");
    ASSERT(mip->strong_branch_probes - probes_before <= MIP_RELIABILITY_POST_INCUMBENT_MAX_STRONG,
           "Early post-incumbent probing uses reduced probe cap");

    /* Once enough nodes are explored with an incumbent, stop strong probing. */
    ASSERT(mip_recover_root_relaxation(mip) == 0, "Throttle LP recovery before late post-incumbent check succeeds");
    reset_reliability_counts(mip);
    force_fractional_integer_solution(mip, 0.5);
    mip->has_incumbent = 1;
    mip->nodes_explored = MIP_RELIABILITY_POST_INCUMBENT_PROBE_NODES + 8;
    probes_before = mip->strong_branch_probes;
    branch_var = -1;
    ASSERT(select_branch_variable(mip, mip->lp_solver->solution, &branch_var) == 0,
           "Reliability selector succeeds in late post-incumbent search");
    ASSERT(branch_var >= 0, "Late post-incumbent selector returns branch variable");
    ASSERT(mip->strong_branch_probes == probes_before,
           "Late post-incumbent reliability skips strong probes");

    mip_free(mip);
    ralph_test_free(model);
}

static void test_root_lp_recovery_contract(void) {
    RalphModel *model = build_root_recovery_model();
    ASSERT(model != NULL, "Recovery model created");
    if (!model) return;

    LPModel *lp_model = ralph_get_lp_model(model);
    ASSERT(lp_model != NULL, "Internal LP model handle available");
    if (!lp_model) {
        ralph_test_free(model);
        return;
    }

    MIPSolver *mip = mip_create(lp_model, 0, 128);
    ASSERT(mip != NULL, "MIP solver created");
    if (!mip) {
        ralph_test_free(model);
        return;
    }
    mip->verbose = 0;

    ASSERT(mip_recover_root_relaxation(mip) == 0, "Initial root LP recovery succeeds");
    ASSERT(mip->lp_solver && mip->lp_solver->status == RALPH_STATUS_OPTIMAL,
           "Initial recovered root LP is OPTIMAL");
    ASSERT(isfinite(mip->lp_solver->obj_value), "Initial recovered root objective is finite");
    double recovered_obj_1 = mip->lp_solver->obj_value;

    /* Corrupt working model bounds, then require full root recovery. */
    mip->working_model->lb[0] = 2.0;
    mip->working_model->ub[0] = 1.0;

    ASSERT(mip_recover_root_relaxation(mip) == 0, "Root LP recovery repairs corrupted working model");
    ASSERT(mip->lp_solver && mip->lp_solver->status == RALPH_STATUS_OPTIMAL,
           "Recovered root LP after corruption is OPTIMAL");
    ASSERT(isfinite(mip->lp_solver->obj_value), "Recovered root objective remains finite");
    ASSERT_NEAR(mip->lp_solver->obj_value, recovered_obj_1, 1e-9,
                "Recovered root objective remains stable");
    ASSERT_NEAR(mip->working_model->lb[0], mip->original_model->lb[0], 1e-12,
                "Recovered working-model lb matches original");
    ASSERT_NEAR(mip->working_model->ub[0], mip->original_model->ub[0], 1e-12,
                "Recovered working-model ub matches original");
    ASSERT(mip->cut_recovery_attempts >= 2, "Cut-loop recovery attempts counted");
    ASSERT(mip->cut_recovery_success >= 2, "Cut-loop recovery successes counted");
    ASSERT(mip->cut_recovery_failures == 0, "No cut-loop recovery failures");

    mip_free(mip);
    ralph_test_free(model);
}

int main(void) {
    test_strong_branch_probe_contract();
    test_branch_selector_skips_fixed_integer_vars();
    test_reliability_no_incumbent_probe_throttle();
    test_root_lp_recovery_contract();

    printf("MIP LP recovery tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
