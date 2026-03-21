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
int simplex_prepare_primal_tableau(SimplexSolver *solver, int allow_crash);
int simplex_resolve_prepared_primal_tableau(SimplexSolver *solver);
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
 * Regression for the exact-cover propagation + row-branching path.
 * The SPP plugins should handle this benchmark family directly, while still
 * preserving warm node LP reuse underneath the generic MIP controller.
 */
int test_set_partitioning_spp_branching_regression(void) {
    printf("\n=== test_set_partitioning_spp_branching_regression ===\n");

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
        printf("FAIL: Could not create SPP branching regression model\n");
        return 1;
    }
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    covers = (int*)calloc((size_t)num_elements * (size_t)num_subsets, sizeof(int));
    indices = (int*)malloc((size_t)num_subsets * sizeof(int));
    values = (double*)malloc((size_t)num_subsets * sizeof(double));
    if (!covers || !indices || !values) {
        printf("FAIL: Could not allocate SPP branching regression buffers\n");
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
        printf("FAIL: SPP branching MIP solver unavailable\n");
        failed = 1;
        goto cleanup;
    }

    if (mip->spp_prop_calls <= 0) {
        printf("FAIL: Expected SPP propagation to run, got %d calls\n",
               mip->spp_prop_calls);
        failed = 1;
        goto cleanup;
    }
    if (mip->spp_branch_uses <= 0) {
        printf("FAIL: Expected SPP row branching to be used, got %d\n",
               mip->spp_branch_uses);
        failed = 1;
        goto cleanup;
    }
    if (mip->node_lp_warm_solves <= 0) {
        printf("FAIL: Expected warm node LP solves under SPP branching, got %d\n",
               mip->node_lp_warm_solves);
        failed = 1;
        goto cleanup;
    }

    printf("PASS: prop_calls=%d fixings=%d branch_uses=%d warm_solves=%d\n",
           mip->spp_prop_calls, mip->spp_prop_fixings,
           mip->spp_branch_uses, mip->node_lp_warm_solves);

cleanup:
    free(covers);
    free(indices);
    free(values);
    ralph_test_free(model);
    return failed;
}

int test_spp_root_cuts_disabled_by_default_regression(void) {
    printf("\n=== test_spp_root_cuts_disabled_by_default_regression ===\n");

    const int num_elements = 20;
    const int num_subsets = 60;
    const double density = 0.30;
    int failed = 0;

    RalphModel *model = NULL;
    int *covers = NULL;
    int *indices = NULL;
    double *costs = NULL;
    double *values = NULL;

    g_sp_seed = 123;
    model = ralph_test_create();
    if (!model) {
        printf("FAIL: Could not create SPP root-cut policy regression model\n");
        return 1;
    }
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    costs = (double *)malloc((size_t)num_subsets * sizeof(double));
    covers = (int *)calloc((size_t)num_elements * (size_t)num_subsets, sizeof(int));
    indices = (int *)malloc((size_t)num_subsets * sizeof(int));
    values = (double *)malloc((size_t)num_subsets * sizeof(double));
    if (!costs || !covers || !indices || !values) {
        printf("FAIL: Could not allocate SPP root-cut policy regression buffers\n");
        failed = 1;
        goto cleanup;
    }

    for (int j = 0; j < num_subsets; j++) {
        costs[j] = sp_rand_double(1.0, 10.0);
        values[j] = 1.0;
        ralph_test_add_var(model, 0.0, 1.0, costs[j], RALPH_BINARY);
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

    MIPSolver *mip = ralph_get_mip_solver(model);
    if (!mip) {
        printf("FAIL: SPP root-cut policy MIP solver unavailable\n");
        failed = 1;
        goto cleanup;
    }
    if (mip->max_cut_rounds != RALPH_DEFAULT_MAX_CUT_ROUNDS) {
        printf("FAIL: Expected default cut rounds=%d, got %d\n",
               RALPH_DEFAULT_MAX_CUT_ROUNDS, mip->max_cut_rounds);
        failed = 1;
        goto cleanup;
    }
    if (mip->root_cut_skip_disabled != 0) {
        printf("FAIL: Expected root cuts not disabled by default, skip_disabled=%d\n",
               mip->root_cut_skip_disabled);
        failed = 1;
        goto cleanup;
    }
    if (mip->root_cut_skip_spp_disabled <= 0) {
        printf("FAIL: Expected SPP root cuts disabled by default, skip_spp_disabled=%d\n",
               mip->root_cut_skip_spp_disabled);
        failed = 1;
        goto cleanup;
    }
    if (mip->root_cut_rounds != 0) {
        printf("FAIL: Expected zero SPP root cut rounds by default, got %d\n",
               mip->root_cut_rounds);
        failed = 1;
        goto cleanup;
    }

    printf("PASS: max_cut_rounds=%d root_cut_rounds=%d skip_spp_disabled=%d\n",
           mip->max_cut_rounds, mip->root_cut_rounds, mip->root_cut_skip_spp_disabled);

cleanup:
    free(covers);
    free(indices);
    free(costs);
    free(values);
    ralph_test_free(model);
    return failed;
}

int test_prepared_two_phase_primal_pipeline_regression(void) {
    printf("\n=== test_prepared_two_phase_primal_pipeline_regression ===\n");

    int failed = 0;
    LPModel *model = lp_model_create();
    SimplexSolver *cold = NULL;
    SimplexSolver *prepared = NULL;

    if (!model) {
        printf("FAIL: Could not create prepared-two-phase regression model\n");
        return 1;
    }

    model->obj_sense = 1;
    lp_model_add_var(model, 0.0, RALPH_INFINITY, 1.0, 'C');
    lp_model_add_var(model, 0.0, RALPH_INFINITY, 2.0, 'C');

    {
        int eq_ind[] = {0, 1};
        double eq_val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, eq_ind, eq_val, 'E', 1.0);
    }
    {
        int g_ind[] = {0};
        double g_val[] = {1.0};
        lp_model_add_constraint(model, 1, g_ind, g_val, 'G', 0.2);
    }
    {
        int g_ind[] = {1};
        double g_val[] = {1.0};
        lp_model_add_constraint(model, 1, g_ind, g_val, 'G', 0.3);
    }

    if (lp_model_finalize(model) != 0) {
        printf("FAIL: Could not finalize prepared-two-phase regression model\n");
        failed = 1;
        goto cleanup;
    }

    cold = simplex_create(model);
    prepared = simplex_create(model);
    if (!cold || !prepared) {
        printf("FAIL: Could not create regression simplex solvers\n");
        failed = 1;
        goto cleanup;
    }

    cold->verbose = 0;
    cold->method = 0;
    prepared->verbose = 0;
    prepared->method = 0;

    if (simplex_solve(cold) != 0 || cold->status != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Cold simplex solve failed, status=%d\n", cold ? cold->status : -1);
        failed = 1;
        goto cleanup;
    }

    if (simplex_prepare_primal_tableau(prepared, 0) != 0) {
        printf("FAIL: Could not prepare primal tableau for regression\n");
        failed = 1;
        goto cleanup;
    }
    if (!prepared->tableau || !prepared->tableau->use_two_phase) {
        printf("FAIL: Expected prepared regression tableau to use two-phase\n");
        failed = 1;
        goto cleanup;
    }
    if (simplex_resolve_prepared_primal_tableau(prepared) != 0 ||
        prepared->status != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Prepared-tableau primal pipeline failed, status=%d\n",
               prepared ? prepared->status : -1);
        failed = 1;
        goto cleanup;
    }

    if (fabs(prepared->obj_value - cold->obj_value) > 1e-8) {
        printf("FAIL: Prepared objective %.10f != cold objective %.10f\n",
               prepared->obj_value, cold->obj_value);
        failed = 1;
        goto cleanup;
    }
    if (!prepared->solution || !cold->solution) {
        printf("FAIL: Missing solution vectors after prepared-two-phase regression\n");
        failed = 1;
        goto cleanup;
    }
    for (int j = 0; j < model->num_vars; j++) {
        if (fabs(prepared->solution[j] - cold->solution[j]) > 1e-8) {
            printf("FAIL: Prepared solution mismatch at var %d (%.10f vs %.10f)\n",
                   j, prepared->solution[j], cold->solution[j]);
            failed = 1;
            goto cleanup;
        }
    }

    printf("PASS: prepared two-phase pipeline matched cold solve objective %.10f\n",
           prepared->obj_value);

cleanup:
    simplex_free(prepared);
    simplex_free(cold);
    lp_model_free(model);
    return failed;
}

int test_augmented_prepared_two_phase_pipeline_regression(void) {
    printf("\n=== test_augmented_prepared_two_phase_pipeline_regression ===\n");

    int failed = 0;
    LPModel *base_model = lp_model_create();
    LPModel *aug_model = lp_model_create();
    SimplexSolver *base_solver = NULL;
    SimplexSolver *aug_solver = NULL;
    int *warm_basis = NULL;
    VarStatus *warm_status = NULL;

    if (!base_model || !aug_model) {
        printf("FAIL: Could not create augmented prepared-two-phase regression models\n");
        failed = 1;
        goto cleanup;
    }

    base_model->obj_sense = 1;
    aug_model->obj_sense = 1;
    for (int i = 0; i < 2; i++) {
        double cost = (i == 0) ? 1.0 : 2.0;
        lp_model_add_var(base_model, 0.0, RALPH_INFINITY, cost, 'C');
        lp_model_add_var(aug_model, 0.0, RALPH_INFINITY, cost, 'C');
    }

    {
        int eq_ind[] = {0, 1};
        double eq_val[] = {1.0, 1.0};
        lp_model_add_constraint(base_model, 2, eq_ind, eq_val, 'E', 1.0);
        lp_model_add_constraint(aug_model, 2, eq_ind, eq_val, 'E', 1.0);
    }
    {
        int g_ind[] = {0};
        double g_val[] = {1.0};
        lp_model_add_constraint(base_model, 1, g_ind, g_val, 'G', 0.2);
        lp_model_add_constraint(aug_model, 1, g_ind, g_val, 'G', 0.2);
    }
    {
        int g_ind[] = {1};
        double g_val[] = {1.0};
        lp_model_add_constraint(base_model, 1, g_ind, g_val, 'G', 0.3);
        lp_model_add_constraint(aug_model, 1, g_ind, g_val, 'G', 0.3);
    }
    {
        int g_ind[] = {1};
        double g_val[] = {1.0};
        lp_model_add_constraint(aug_model, 1, g_ind, g_val, 'G', 0.4);
    }

    if (lp_model_finalize(base_model) != 0 || lp_model_finalize(aug_model) != 0) {
        printf("FAIL: Could not finalize augmented regression models\n");
        failed = 1;
        goto cleanup;
    }

    base_solver = simplex_create(base_model);
    aug_solver = simplex_create(aug_model);
    if (!base_solver || !aug_solver) {
        printf("FAIL: Could not create augmented regression solvers\n");
        failed = 1;
        goto cleanup;
    }

    base_solver->verbose = 0;
    base_solver->method = 0;
    aug_solver->verbose = 0;
    aug_solver->method = 0;

    if (simplex_solve(base_solver) != 0 || base_solver->status != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Base regression solve failed, status=%d\n",
               base_solver ? base_solver->status : -1);
        failed = 1;
        goto cleanup;
    }
    if (simplex_solve(aug_solver) != 0 || aug_solver->status != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Augmented cold solve failed, status=%d\n",
               aug_solver ? aug_solver->status : -1);
        failed = 1;
        goto cleanup;
    }

    if (simplex_prepare_primal_tableau(base_solver, 0) != 0) {
        printf("FAIL: Could not prepare base tableau for augmentation regression\n");
        failed = 1;
        goto cleanup;
    }
    if (!base_solver->tableau || !base_solver->tableau->use_two_phase) {
        printf("FAIL: Expected base tableau to use two-phase before augmentation\n");
        failed = 1;
        goto cleanup;
    }

    warm_basis = (int*)malloc((size_t)(base_solver->tableau->m + 1) * sizeof(int));
    warm_status = (VarStatus*)malloc((size_t)(base_solver->tableau->n + 2) * sizeof(VarStatus));
    if (!warm_basis || !warm_status) {
        printf("FAIL: Could not allocate augmented warm basis buffers\n");
        failed = 1;
        goto cleanup;
    }

    memcpy(warm_basis, base_solver->tableau->basis,
           (size_t)base_solver->tableau->m * sizeof(int));
    memcpy(warm_status, base_solver->tableau->var_status,
           (size_t)base_solver->tableau->n * sizeof(VarStatus));
    warm_basis[base_solver->tableau->m] = base_solver->tableau->n + 1;
    warm_status[base_solver->tableau->n] = RALPH_NONBASIC_LOWER;
    warm_status[base_solver->tableau->n + 1] = RALPH_BASIC;

    {
        int idx[] = {1};
        double val[] = {1.0};
        LPAugmentRow row = {
            .nnz = 1,
            .indices = idx,
            .values = val,
            .sense = 'G',
            .rhs = 0.4
        };

        if (simplex_prepare_augmented_primal_tableau(base_solver,
                                                     &row,
                                                     1,
                                                     base_solver->tableau->m + 1,
                                                     base_solver->tableau->n + 2,
                                                     warm_basis,
                                                     warm_status) != 0) {
            printf("FAIL: Could not prepare augmented two-phase tableau\n");
            failed = 1;
            goto cleanup;
        }
    }

    if (simplex_resolve_prepared_primal_tableau(base_solver) != 0 ||
        base_solver->status != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: Augmented prepared-tableau solve failed, status=%d\n",
               base_solver ? base_solver->status : -1);
        failed = 1;
        goto cleanup;
    }

    if (fabs(base_solver->obj_value - aug_solver->obj_value) > 1e-8) {
        printf("FAIL: Augmented prepared objective %.10f != cold objective %.10f\n",
               base_solver->obj_value, aug_solver->obj_value);
        failed = 1;
        goto cleanup;
    }
    if (!base_solver->solution || !aug_solver->solution) {
        printf("FAIL: Missing solutions after augmented prepared regression\n");
        failed = 1;
        goto cleanup;
    }
    for (int j = 0; j < aug_model->num_vars; j++) {
        if (fabs(base_solver->solution[j] - aug_solver->solution[j]) > 1e-8) {
            printf("FAIL: Augmented prepared solution mismatch at var %d (%.10f vs %.10f)\n",
                   j, base_solver->solution[j], aug_solver->solution[j]);
            failed = 1;
            goto cleanup;
        }
    }

    printf("PASS: augmented prepared two-phase pipeline matched cold objective %.10f\n",
           base_solver->obj_value);

cleanup:
    free(warm_basis);
    free(warm_status);
    simplex_free(aug_solver);
    simplex_free(base_solver);
    lp_model_free(aug_model);
    lp_model_free(base_model);
    return failed;
}

int main(void) {
    int failures = 0;

    failures += test_basic_mip();
    failures += test_ralph_wrapper();
    failures += test_set_partitioning_benchmark_objective_regression();
    failures += test_set_partitioning_spp_branching_regression();
    failures += test_spp_root_cuts_disabled_by_default_regression();
    failures += test_prepared_two_phase_primal_pipeline_regression();
    failures += test_augmented_prepared_two_phase_pipeline_regression();

    printf("\n=== Summary ===\n");
    if (failures == 0) {
        printf("All tests passed\n");
    } else {
        printf("%d test(s) failed\n", failures);
    }

    return failures;
}
