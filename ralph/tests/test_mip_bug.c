/*
 * Test for MIP solver bug where INFEASIBLE is returned for feasible problems.
 *
 * Build: make test_mip_bug
 * Run: ./test_mip_bug
 */

#include <stdio.h>
#include <stdlib.h>
#include "ralph_test_mod_api.h"
#include "lp.h"
#include "mip.h"

/* Forward declaration */
SimplexSolver* simplex_create(LPModel *model);
int simplex_solve(SimplexSolver *solver);
void simplex_free(SimplexSolver *solver);

/*
 * Simple test problem:
 * minimize 5*z[0] + 5*z[1] + 5*z[2] + theta
 * subject to:
 *   z[0] + z[1] + z[2] >= 0  (trivially satisfied)
 *   3*z[0] + 2*z[1] + z[2] >= 1
 *   10*z[0] >= 1  (forces z[0] >= 0.1, but z[0] is binary so z[0] = 1)
 *
 * z[0], z[1], z[2] are binary [0,1]
 * theta is continuous [-1e9, 1e9]
 *
 * Expected solution: z[0]=1, z[1]=z[2]=0, theta=arbitrary but >=0
 * Optimal objective: 5
 */
int test_basic_mip(void) {
    printf("=== test_basic_mip ===\n");

    LPModel *m = lp_model_create();
    m->obj_sense = 1;  /* Minimize */

    /* Add binary variables z[0], z[1], z[2] with cost 5 each */
    lp_model_add_var(m, 0, 1, 5.0, 'B');
    lp_model_add_var(m, 0, 1, 5.0, 'B');
    lp_model_add_var(m, 0, 1, 5.0, 'B');
    /* Add continuous theta with cost 1 */
    lp_model_add_var(m, -1e9, 1e9, 1.0, 'C');

    printf("After adding vars: num_integers=%d, num_binary=%d\n",
           m->num_integers, m->num_binary);

    /* Constraint 1: z[0] + z[1] + z[2] >= 0 */
    int ind1[] = {0, 1, 2};
    double val1[] = {1.0, 1.0, 1.0};
    lp_model_add_constraint(m, 3, ind1, val1, 'G', 0.0);

    /* Constraint 2: 3*z[0] + 2*z[1] + z[2] >= 1 */
    int ind2[] = {0, 1, 2};
    double val2[] = {3.0, 2.0, 1.0};
    lp_model_add_constraint(m, 3, ind2, val2, 'G', 1.0);

    /* Constraint 3: 10*z[0] >= 1 */
    int ind3[] = {0};
    double val3[] = {10.0};
    lp_model_add_constraint(m, 1, ind3, val3, 'G', 1.0);

    /* Finalize to build sparse matrix */
    lp_model_finalize(m);

    printf("Model: %d vars, %d cons, %d integers, %d binary\n",
           m->num_vars, m->num_cons, m->num_integers, m->num_binary);
    printf("var_type: %c %c %c %c\n",
           m->var_type[0], m->var_type[1], m->var_type[2], m->var_type[3]);

    /* First test: LP relaxation should work */
    printf("\n--- Testing LP relaxation ---\n");
    SimplexSolver *lp = simplex_create(m);
    if (!lp) {
        printf("FAIL: Could not create simplex solver\n");
        lp_model_free(m);
        return 1;
    }
    simplex_solve(lp);
    printf("LP: status=%d, obj=%.4f\n", lp->status, lp->obj_value);
    if (lp->status != 1) {
        printf("FAIL: LP relaxation not optimal\n");
        simplex_free(lp);
        lp_model_free(m);
        return 1;
    }
    if (lp->solution) {
        printf("LP solution: z=[%.4f, %.4f, %.4f] theta=%.4f\n",
               lp->solution[0], lp->solution[1], lp->solution[2], lp->solution[3]);
    }
    simplex_free(lp);

    /* Second test: MIP solver */
    printf("\n--- Testing MIP solver (direct mip_create/mip_solve) ---\n");
    MIPSolver *mip = mip_create(m, 0, 1024);
    if (!mip) {
        printf("FAIL: Could not create MIP solver\n");
        lp_model_free(m);
        return 1;
    }
    mip->verbose = 0;

    int ret = mip_solve(mip);
    printf("MIP: ret=%d, status=%d (%s)\n", ret, mip->status,
           mip->status == 1 ? "OPTIMAL" :
           mip->status == 2 ? "INFEASIBLE" :
           mip->status == 3 ? "UNBOUNDED" : "OTHER");

    if (mip->has_incumbent) {
        printf("MIP solution: obj=%.4f z=[%.4f, %.4f, %.4f] theta=%.4f\n",
               mip->best_obj,
               mip->best_solution[0], mip->best_solution[1],
               mip->best_solution[2], mip->best_solution[3]);
    } else {
        printf("No incumbent found\n");
    }

    int ok = (mip->status == 1 && mip->has_incumbent);
    mip_free(mip);
    lp_model_free(m);

    if (!ok) {
        printf("FAIL: MIP should have found optimal solution\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}

/*
 * Test using RalphModel wrapper to see if it works correctly.
 */
int test_ralph_wrapper(void) {
    printf("\n=== test_ralph_wrapper ===\n");

    RalphModel *m = ralph_test_create();
    ralph_test_set_obj_sense(m, RALPH_MINIMIZE);

    ralph_test_add_var(m, 0, 1, 5.0, RALPH_BINARY);
    ralph_test_add_var(m, 0, 1, 5.0, RALPH_BINARY);
    ralph_test_add_var(m, 0, 1, 5.0, RALPH_BINARY);
    ralph_test_add_var(m, -1e9, 1e9, 1.0, RALPH_CONTINUOUS);

    int ind1[] = {0, 1, 2};
    double val1[] = {1.0, 1.0, 1.0};
    ralph_test_add_constraint(m, 3, ind1, val1, RALPH_GREATER_EQUAL, 0.0);

    int ind2[] = {0, 1, 2};
    double val2[] = {3.0, 2.0, 1.0};
    ralph_test_add_constraint(m, 3, ind2, val2, RALPH_GREATER_EQUAL, 1.0);

    int ind3[] = {0};
    double val3[] = {10.0};
    ralph_test_add_constraint(m, 1, ind3, val3, RALPH_GREATER_EQUAL, 1.0);

    printf("Model: %d vars, %d cons, %d integers\n",
           ralph_test_get_num_vars(m), ralph_test_get_num_cons(m), ralph_test_get_num_integers(m));

    ralph_test_set_int_param(m, "verbose", 0);

    int ret = ralph_test_optimize(m);
    RalphStatus status = ralph_test_get_status(m);

    printf("Ralph: ret=%d, status=%d (%s)\n", ret, status,
           status == RALPH_STATUS_OPTIMAL ? "OPTIMAL" :
           status == RALPH_STATUS_INFEASIBLE ? "INFEASIBLE" : "OTHER");

    if (status == RALPH_STATUS_OPTIMAL) {
        double x[4];
        ralph_test_get_solution(m, x);
        printf("Solution: z=[%.4f, %.4f, %.4f] theta=%.4f\n",
               x[0], x[1], x[2], x[3]);
        printf("Objective: %.4f\n", ralph_test_get_objval(m));
    }

    int ok = (status == RALPH_STATUS_OPTIMAL);
    ralph_test_free(m);

    if (!ok) {
        printf("FAIL: Ralph should have found optimal solution\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}

int main(void) {
    int failures = 0;

    failures += test_basic_mip();
    failures += test_ralph_wrapper();

    printf("\n=== Summary ===\n");
    if (failures == 0) {
        printf("All tests passed\n");
    } else {
        printf("%d test(s) failed\n", failures);
    }

    return failures;
}
