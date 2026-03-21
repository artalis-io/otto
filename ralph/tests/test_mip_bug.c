/*
 * Test for MIP solver bug where INFEASIBLE is returned for feasible problems.
 *
 * Build: make test_mip_bug
 * Run: ./test_mip_bug
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "lp.h"
#include "mip.h"

/* Forward declaration */
SimplexSolver* simplex_create(LPModel *model);
int simplex_solve(SimplexSolver *solver);
void simplex_free(SimplexSolver *solver);
MIPSolver* ralph_get_mip_solver(const RalphModel *model);

static unsigned int g_sp_seed;

static double sp_rand_double(double min, double max) {
    g_sp_seed = g_sp_seed * 1103515245u + 12345u;
    return min + ((double)(g_sp_seed % 100000u) / 100000.0) * (max - min);
}

static int sp_rand_int(int min, int max) {
    g_sp_seed = g_sp_seed * 1103515245u + 12345u;
    return min + (int)(g_sp_seed % (unsigned int)(max - min + 1));
}

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

/*
 * Focused regression for the quick benchmark mismatch:
 * SetPartitioning(10 elements, 30 subsets, density 0.35, seed 42)
 * should solve to the exact-cover optimum 12.40383.
 */
int test_set_partitioning_benchmark_objective_regression(void) {
    printf("\n=== test_set_partitioning_benchmark_objective_regression ===\n");

    const int num_elements = 10;
    const int num_subsets = 30;
    const double density = 0.35;
    const double expected_obj = 12.40383;
    int failed = 0;

    RalphModel *model = NULL;
    double *costs = NULL;
    int *covers = NULL;
    int *indices = NULL;
    double *values = NULL;
    double *x = NULL;

    g_sp_seed = 42;
    model = ralph_test_create();
    if (!model) {
        printf("FAIL: Could not create benchmark regression model\n");
        return 1;
    }
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    costs = (double*)malloc((size_t)num_subsets * sizeof(double));
    covers = (int*)calloc((size_t)num_elements * (size_t)num_subsets, sizeof(int));
    indices = (int*)malloc((size_t)num_subsets * sizeof(int));
    values = (double*)malloc((size_t)num_subsets * sizeof(double));
    x = (double*)calloc((size_t)num_subsets, sizeof(double));
    if (!costs || !covers || !indices || !values || !x) {
        printf("FAIL: Could not allocate benchmark regression buffers\n");
        failed = 1;
        goto cleanup;
    }

    for (int j = 0; j < num_subsets; j++) {
        costs[j] = sp_rand_double(1.0, 10.0);
        ralph_test_add_var(model, 0.0, 1.0, costs[j], RALPH_BINARY);
        values[j] = 1.0;
    }

    for (int i = 0; i < num_elements; i++) {
        int nnz = 0;
        for (int j = 0; j < num_subsets; j++) {
            if (sp_rand_double(0.0, 1.0) < density) {
                indices[nnz] = j;
                covers[i * num_subsets + j] += 1;
                nnz++;
            }
        }
        while (nnz < 2) {
            int j = sp_rand_int(0, num_subsets - 1);
            indices[nnz] = j;
            covers[i * num_subsets + j] += 1;
            nnz++;
        }
        ralph_test_add_constraint(model, nnz, indices, values, RALPH_EQUAL, 1.0);
    }

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "presolve", 1);
    ralph_test_set_dbl_param(model, "time_limit", 60.0);
    ralph_test_set_int_param(model, "max_nodes", 100000);
    ralph_test_optimize(model);

    if (ralph_test_get_status(model) != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Expected OPTIMAL, got status=%d\n", (int)ralph_test_get_status(model));
        failed = 1;
        goto cleanup;
    }

    if (fabs(ralph_test_get_objval(model) - expected_obj) > 1e-5) {
        printf("FAIL: Expected objective %.5f, got %.10f\n",
               expected_obj, ralph_test_get_objval(model));
        failed = 1;
        goto cleanup;
    }

    if (ralph_test_get_solution(model, x) != 0) {
        printf("FAIL: Could not retrieve benchmark regression solution\n");
        failed = 1;
        goto cleanup;
    }

    for (int j = 0; j < num_subsets; j++) {
        if (x[j] < -1e-9 || x[j] > 1.0 + 1e-9) {
            printf("FAIL: Solution violates binary bounds at var %d (%.10f)\n", j, x[j]);
            failed = 1;
            goto cleanup;
        }
        if (fabs(x[j] - round(x[j])) > 1e-9) {
            printf("FAIL: Solution violates integrality at var %d (%.10f)\n", j, x[j]);
            failed = 1;
            goto cleanup;
        }
    }

    for (int i = 0; i < num_elements; i++) {
        double sum = 0.0;
        for (int j = 0; j < num_subsets; j++) {
            sum += (double)covers[i * num_subsets + j] * x[j];
        }
        if (fabs(sum - 1.0) > 1e-9) {
            printf("FAIL: Element %d is not exactly covered once (sum=%.10f)\n", i, sum);
            failed = 1;
            goto cleanup;
        }
    }

    printf("PASS: objective %.5f with valid exact-cover binary solution\n",
           ralph_test_get_objval(model));

cleanup:
    free(costs);
    free(covers);
    free(indices);
    free(values);
    free(x);
    ralph_test_free(model);
    return failed;
}

/*
 * Reliability/strong-branch regression for equality-heavy set partitioning.
 * After failed probes, the solver should recover a reusable node LP state
 * and reach at least some warm node solves on this benchmark family.
 */
int test_set_partitioning_reliability_recovery_regression(void) {
    printf("\n=== test_set_partitioning_reliability_recovery_regression ===\n");

    const int num_elements = 10;
    const int num_subsets = 30;
    const double density = 0.35;
    int failed = 0;

    RalphModel *model = NULL;
    int *covers = NULL;
    int *indices = NULL;
    double *values = NULL;

    g_sp_seed = 42;
    model = ralph_test_create();
    if (!model) {
        printf("FAIL: Could not create reliability recovery model\n");
        return 1;
    }
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    covers = (int*)calloc((size_t)num_elements * (size_t)num_subsets, sizeof(int));
    indices = (int*)malloc((size_t)num_subsets * sizeof(int));
    values = (double*)malloc((size_t)num_subsets * sizeof(double));
    if (!covers || !indices || !values) {
        printf("FAIL: Could not allocate reliability recovery buffers\n");
        failed = 1;
        goto cleanup;
    }

    for (int j = 0; j < num_subsets; j++) {
        ralph_test_add_var(model, 0.0, 1.0, sp_rand_double(1.0, 10.0), RALPH_BINARY);
        values[j] = 1.0;
    }

    for (int i = 0; i < num_elements; i++) {
        int nnz = 0;
        for (int j = 0; j < num_subsets; j++) {
            if (sp_rand_double(0.0, 1.0) < density) {
                indices[nnz] = j;
                covers[i * num_subsets + j] += 1;
                nnz++;
            }
        }
        while (nnz < 2) {
            int j = sp_rand_int(0, num_subsets - 1);
            indices[nnz] = j;
            covers[i * num_subsets + j] += 1;
            nnz++;
        }
        ralph_test_add_constraint(model, nnz, indices, values, RALPH_EQUAL, 1.0);
    }

    ralph_test_set_int_param(model, "verbose", 0);
    ralph_test_set_int_param(model, "presolve", 1);
    ralph_test_set_int_param(model, "var_select", 3);  /* reliability */
    ralph_test_set_dbl_param(model, "time_limit", 60.0);
    ralph_test_set_int_param(model, "max_nodes", 100000);
    ralph_test_optimize(model);

    if (ralph_test_get_status(model) != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Expected OPTIMAL, got status=%d\n", (int)ralph_test_get_status(model));
        failed = 1;
        goto cleanup;
    }

    MIPSolver *mip = ralph_get_mip_solver(model);
    if (!mip) {
        printf("FAIL: Reliability recovery MIP solver unavailable\n");
        failed = 1;
        goto cleanup;
    }

    if (mip->strong_branch_failures <= 0) {
        printf("FAIL: Expected at least one failed strong-branch probe, got %d\n",
               mip->strong_branch_failures);
        failed = 1;
        goto cleanup;
    }
    if (mip->strong_branch_recoveries != mip->strong_branch_failures) {
        printf("FAIL: Expected all failed probes recovered (%d/%d)\n",
               mip->strong_branch_recoveries, mip->strong_branch_failures);
        failed = 1;
        goto cleanup;
    }
    if (mip->node_lp_state_restore_success <= 0) {
        printf("FAIL: Expected reusable post-probe node-state restores, got %d\n",
               mip->node_lp_state_restore_success);
        failed = 1;
        goto cleanup;
    }
    if (mip->node_lp_state_restore_failures != 0) {
        printf("FAIL: Expected zero post-probe restore failures, got %d\n",
               mip->node_lp_state_restore_failures);
        failed = 1;
        goto cleanup;
    }
    if (mip->node_lp_warm_solves <= 0) {
        printf("FAIL: Expected warm node LP solves after recovery, got %d\n",
               mip->node_lp_warm_solves);
        failed = 1;
        goto cleanup;
    }
    {
        int max_allowed_probes = MIP_RELIABILITY_ARTIFICIAL_ROOT_MAX_STRONG +
                                 2 * MIP_RELIABILITY_ARTIFICIAL_SHALLOW_MAX_STRONG;
        if (mip->strong_branch_probes > max_allowed_probes) {
            printf("FAIL: Expected artificial-column probing cap <= %d, got %d\n",
                   max_allowed_probes, mip->strong_branch_probes);
            failed = 1;
            goto cleanup;
        }
    }

    printf("PASS: recoveries=%d state_restores=%d warm_solves=%d\n",
           mip->strong_branch_recoveries, mip->node_lp_state_restore_success,
           mip->node_lp_warm_solves);

cleanup:
    free(covers);
    free(indices);
    free(values);
    ralph_test_free(model);
    return failed;
}

int main(void) {
    int failures = 0;

    failures += test_basic_mip();
    failures += test_ralph_wrapper();
    failures += test_set_partitioning_benchmark_objective_regression();
    failures += test_set_partitioning_reliability_recovery_regression();

    printf("\n=== Summary ===\n");
    if (failures == 0) {
        printf("All tests passed\n");
    } else {
        printf("%d test(s) failed\n", failures);
    }

    return failures;
}
