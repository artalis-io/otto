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
#include "ralph.h"
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
    RalphModel *model = ralph_create();
    if (!model) return NULL;
    ralph_set_obj_sense(model, RALPH_MAXIMIZE);

    /* max x + y
     * s.t. x + y <= 1.5
     *      x, y binary
     * Root LP has a fractional optimum. */
    int x = ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    int y = ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    if (x != 0 || y != 1) {
        ralph_free(model);
        return NULL;
    }
    {
        int idx[] = {x, y};
        double val[] = {1.0, 1.0};
        if (ralph_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 1.5) != 0) {
            ralph_free(model);
            return NULL;
        }
    }
    return model;
}

static RalphModel* build_root_recovery_model(void) {
    RalphModel *model = ralph_create();
    if (!model) return NULL;
    ralph_set_obj_sense(model, RALPH_MAXIMIZE);

    int x = ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    int y = ralph_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    if (x != 0 || y != 1) {
        ralph_free(model);
        return NULL;
    }
    {
        int idx[] = {x, y};
        double val[] = {1.0, 1.0};
        if (ralph_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 1.0) != 0) {
            ralph_free(model);
            return NULL;
        }
    }
    return model;
}

static void test_strong_branch_probe_contract(void) {
    RalphModel *model = build_fractional_probe_model();
    ASSERT(model != NULL, "Probe model created");
    if (!model) return;

    LPModel *lp_model = ralph_get_lp_model(model);
    ASSERT(lp_model != NULL, "Internal LP model handle available");
    if (!lp_model) {
        ralph_free(model);
        return;
    }

    MIPSolver *mip = mip_create(lp_model, 0, 128);
    ASSERT(mip != NULL, "MIP solver created");
    if (!mip) {
        ralph_free(model);
        return;
    }
    mip->verbose = 0;

    ASSERT(mip_recover_root_relaxation(mip) == 0, "Root LP recovery builds initial LP state");
    ASSERT(mip->lp_solver && mip->lp_solver->tableau && mip->lp_solver->solution,
           "LP tableau+solution available for probing");
    if (!mip->lp_solver || !mip->lp_solver->tableau || !mip->lp_solver->solution) {
        mip_free(mip);
        ralph_free(model);
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
        ralph_free(model);
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
        ralph_free(model);
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
    ralph_free(model);
}

static void test_root_lp_recovery_contract(void) {
    RalphModel *model = build_root_recovery_model();
    ASSERT(model != NULL, "Recovery model created");
    if (!model) return;

    LPModel *lp_model = ralph_get_lp_model(model);
    ASSERT(lp_model != NULL, "Internal LP model handle available");
    if (!lp_model) {
        ralph_free(model);
        return;
    }

    MIPSolver *mip = mip_create(lp_model, 0, 128);
    ASSERT(mip != NULL, "MIP solver created");
    if (!mip) {
        ralph_free(model);
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
    ralph_free(model);
}

int main(void) {
    test_strong_branch_probe_contract();
    test_root_lp_recovery_contract();

    printf("MIP LP recovery tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
