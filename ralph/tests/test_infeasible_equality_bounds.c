/*
 * test_infeasible_equality_bounds.c
 *
 * Regression test for artalis-io/otto#96: the base simplex (presolve off) used
 * to report UNBOUNDED (or ERROR) for infeasible LPs whose infeasibility comes
 * from an equality row that forces a variable outside its bounds.
 *
 * Root cause: the equality branch of tableau construction always gave the
 * artificial a +1 coefficient, so on a normalized row whose structural activity
 * at the starting point exceeds rhs the artificial started negative (below its
 * own lower bound of 0). The phase-1 objective (minimize +1*a) then drove it
 * further from zero, hiding the infeasibility; the solve could fall through to a
 * bogus UNBOUNDED. The fix chooses the artificial's coefficient sign so it
 * starts non-negative (mirroring the <= / >= branches).
 *
 * These cases lock in the correct verdict on both the base simplex and the
 * presolve path. Part of `make -C ralph test`.
 */

#include <stdio.h>
#include "ralph_lp.h"

static int expect_status(const char *name,
                         void (*build)(RalphLPModel *),
                         RalphLPStatus want,
                         int presolve) {
    RalphLPModel *m = ralph_lp_create();
    build(m);
    ralph_lp_set_int_param(m, "presolve", presolve);
    ralph_lp_optimize(m);
    RalphLPStatus got = ralph_lp_get_status(m);
    double obj = ralph_lp_get_objval(m);
    int ok = (got == want);
    printf("  [%s] presolve=%d: %s obj=%g  %s\n",
           name, presolve, ralph_lp_status_string(got), obj,
           ok ? "OK" : "FAIL");
    if (!ok)
        printf("        expected %s\n", ralph_lp_status_string(want));
    ralph_lp_free(m);
    return ok;
}

/* -3 x0 = -1, x0 in [1,6]: forces x0 = 1/3 below its lower bound. INFEASIBLE. */
static void build_single_equality(RalphLPModel *m) {
    ralph_lp_add_var(m, 1, 6, -5.0, RALPH_LP_VAR_CONTINUOUS);
    int idx[] = {0};
    double val[] = {-3};
    ralph_lp_add_constraint(m, 1, idx, val, RALPH_LP_SENSE_EQUAL, -1);
}

/*
 * The #96 reproducer (reaches Phase 2 and used to report UNBOUNDED):
 *   min -5 x0 - 4 x1 - 5 x2 - x3
 *     x2 + 5 x3 <= 3
 *    -5 x0 + 4 x1 >= -2
 *     5 x0 - 2 x1  = -6
 *    -3 x2        = -1
 *   -2 <= x0 <= +inf, 0 <= x1 <= +inf, 1 <= x2 <= 6, 0 <= x3 <= 6
 * Infeasible: the last equality forces x2 = 1/3 but x2 >= 1.
 */
static void build_hash96(RalphLPModel *m) {
    ralph_lp_add_var(m, -2, RALPH_LP_INFINITY, -5.0, RALPH_LP_VAR_CONTINUOUS);
    ralph_lp_add_var(m, 0, RALPH_LP_INFINITY, -4.0, RALPH_LP_VAR_CONTINUOUS);
    ralph_lp_add_var(m, 1, 6, -5.0, RALPH_LP_VAR_CONTINUOUS);
    ralph_lp_add_var(m, 0, 6, -1.0, RALPH_LP_VAR_CONTINUOUS);
    int i0[] = {2, 3}; double v0[] = {1, 5};   ralph_lp_add_constraint(m, 2, i0, v0, RALPH_LP_SENSE_LESS_EQUAL, 3);
    int i1[] = {0, 1}; double v1[] = {-5, 4};  ralph_lp_add_constraint(m, 2, i1, v1, RALPH_LP_SENSE_GREATER_EQUAL, -2);
    int i2[] = {0, 1}; double v2[] = {5, -2};  ralph_lp_add_constraint(m, 2, i2, v2, RALPH_LP_SENSE_EQUAL, -6);
    int i3[] = {2};    double v3[] = {-3};     ralph_lp_add_constraint(m, 1, i3, v3, RALPH_LP_SENSE_EQUAL, -1);
}

/*
 * Feasible equality whose activity at the all-lower-bound start exceeds rhs, so
 * the fix's coef=-1 path is exercised on a solvable model (guards against the
 * fix breaking the common case):
 *   min x0,  x0 - x1 = 0,  x0 in [2,5], x1 in [0,5]
 * At the start (x0=2, x1=0) activity 2 > rhs 0; feasible optimum x0=x1=2.
 */
static void build_feasible_activity_gt_rhs(RalphLPModel *m) {
    ralph_lp_add_var(m, 2, 5, 1.0, RALPH_LP_VAR_CONTINUOUS);
    ralph_lp_add_var(m, 0, 5, 0.0, RALPH_LP_VAR_CONTINUOUS);
    int idx[] = {0, 1};
    double val[] = {1, -1};
    ralph_lp_add_constraint(m, 2, idx, val, RALPH_LP_SENSE_EQUAL, 0);
}

int main(void) {
    printf("Ralph infeasible-equality-bounds regression (otto#96)\n");
    printf("=====================================================\n");

    int fails = 0;
    for (int p = 0; p <= 1; p++) {
        fails += !expect_status("single-equality", build_single_equality,
                                RALPH_LP_STATUS_INFEASIBLE, p);
        fails += !expect_status("hash96", build_hash96,
                                RALPH_LP_STATUS_INFEASIBLE, p);
        fails += !expect_status("feasible-activity>rhs", build_feasible_activity_gt_rhs,
                                RALPH_LP_STATUS_OPTIMAL, p);
    }

    printf("-----------------------------------------------------\n");
    if (fails == 0) {
        printf("RESULT: all equality-bound verdicts correct on both paths. PASS\n");
        return 0;
    }
    printf("RESULT: %d case(s) wrong. FAIL\n", fails);
    return 1;
}
