/*
 * Test for MIP solver bug where INFEASIBLE is returned for feasible problems.
 *
 * Build: make test_mip_bug
 * Run: ./test_mip_bug
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "lp.h"
#include "mip.h"

/* Forward declaration */
SimplexSolver* simplex_create(LPModel *model);
int simplex_solve(SimplexSolver *solver);
void simplex_free(SimplexSolver *solver);

typedef struct {
    int num_vars;
    int num_cons;
    int num_integers;
    double *obj;
    double *lb;
    double *ub;
    char *vtype;
    int *con_row;
    int *con_col;
    double *con_val;
    int nnz;
    int nnz_alloc;
    double *rhs;
    char *sense_con;
} RegressionMIPProblem;

static unsigned int reg_seed = 42;

static void reg_seed_random(unsigned int seed)
{
    reg_seed = seed;
}

static double reg_rand_double(double min, double max)
{
    reg_seed = reg_seed * 1103515245 + 12345;
    return min + ((double)(reg_seed % 100000) / 100000.0) * (max - min);
}

static int reg_rand_int(int min, int max)
{
    reg_seed = reg_seed * 1103515245 + 12345;
    return min + (reg_seed % (unsigned int)(max - min + 1));
}

static RegressionMIPProblem *reg_mip_create(int num_vars, int num_cons, int est_nnz)
{
    RegressionMIPProblem *prob =
        (RegressionMIPProblem*)calloc(1, sizeof(RegressionMIPProblem));
    if (!prob) return NULL;

    prob->num_vars = num_vars;
    prob->num_cons = num_cons;
    prob->obj = (double*)calloc((size_t)num_vars, sizeof(double));
    prob->lb = (double*)calloc((size_t)num_vars, sizeof(double));
    prob->ub = (double*)calloc((size_t)num_vars, sizeof(double));
    prob->vtype = (char*)calloc((size_t)num_vars, sizeof(char));
    prob->rhs = (double*)calloc((size_t)num_cons, sizeof(double));
    prob->sense_con = (char*)calloc((size_t)num_cons, sizeof(char));
    prob->nnz_alloc = est_nnz;
    prob->con_row = (int*)malloc((size_t)est_nnz * sizeof(int));
    prob->con_col = (int*)malloc((size_t)est_nnz * sizeof(int));
    prob->con_val = (double*)malloc((size_t)est_nnz * sizeof(double));

    if (!prob->obj || !prob->lb || !prob->ub || !prob->vtype ||
        !prob->rhs || !prob->sense_con || !prob->con_row ||
        !prob->con_col || !prob->con_val) {
        free(prob->obj);
        free(prob->lb);
        free(prob->ub);
        free(prob->vtype);
        free(prob->rhs);
        free(prob->sense_con);
        free(prob->con_row);
        free(prob->con_col);
        free(prob->con_val);
        free(prob);
        return NULL;
    }

    for (int j = 0; j < num_vars; j++) {
        prob->lb[j] = 0.0;
        prob->ub[j] = RALPH_INFINITY;
        prob->vtype[j] = 'C';
    }
    for (int i = 0; i < num_cons; i++) {
        prob->sense_con[i] = 'L';
    }

    return prob;
}

static void reg_mip_add_coef(RegressionMIPProblem *prob, int row, int col, double val)
{
    if (prob->nnz >= prob->nnz_alloc) {
        prob->nnz_alloc *= 2;
        prob->con_row = (int*)realloc(prob->con_row,
                                      (size_t)prob->nnz_alloc * sizeof(int));
        prob->con_col = (int*)realloc(prob->con_col,
                                      (size_t)prob->nnz_alloc * sizeof(int));
        prob->con_val = (double*)realloc(prob->con_val,
                                         (size_t)prob->nnz_alloc * sizeof(double));
    }
    prob->con_row[prob->nnz] = row;
    prob->con_col[prob->nnz] = col;
    prob->con_val[prob->nnz] = val;
    prob->nnz++;
}

static void reg_mip_free(RegressionMIPProblem *prob)
{
    if (!prob) return;
    free(prob->obj);
    free(prob->lb);
    free(prob->ub);
    free(prob->vtype);
    free(prob->rhs);
    free(prob->sense_con);
    free(prob->con_row);
    free(prob->con_col);
    free(prob->con_val);
    free(prob);
}

static int validate_integer_solution_against_model(RalphModel *m, const RegressionMIPProblem *prob,
                                                   double *out_obj)
{
    double *x = NULL;
    double obj = 0.0;

    if (!m || !prob) return 0;

    x = (double*)calloc((size_t)prob->num_vars, sizeof(double));
    if (!x) return 0;
    if (ralph_test_get_solution(m, x) != 0) {
        free(x);
        return 0;
    }

    for (int j = 0; j < prob->num_vars; j++) {
        double v = x[j];
        if (v < -1e-9 || v > 1.0 + 1e-9) {
            printf("FAIL: solution violates binary bounds at x[%d]=%.10f\n", j, v);
            free(x);
            return 0;
        }
        if (fabs(v - round(v)) > 1e-9) {
            printf("FAIL: solution violates integrality at x[%d]=%.10f\n", j, v);
            free(x);
            return 0;
        }
        obj += prob->obj[j] * v;
    }

    for (int i = 0; i < prob->num_cons; i++) {
        double lhs = 0.0;
        for (int p = 0; p < prob->nnz; p++) {
            if (prob->con_row[p] != i) continue;
            lhs += prob->con_val[p] * x[prob->con_col[p]];
        }
        if (fabs(lhs - prob->rhs[i]) > 1e-9) {
            printf("FAIL: equality row %d violated: lhs=%.10f rhs=%.10f\n",
                   i, lhs, prob->rhs[i]);
            free(x);
            return 0;
        }
    }

    if (out_obj) *out_obj = obj;
    free(x);
    return 1;
}

static RegressionMIPProblem *generate_set_partitioning_regression(
    int num_elements, int num_subsets, double density, unsigned int seed)
{
    RegressionMIPProblem *prob;

    reg_seed_random(seed);
    prob = reg_mip_create(num_subsets, num_elements,
                          (int)(num_elements * num_subsets * density * 1.5));
    if (!prob) return NULL;

    prob->num_integers = num_subsets;
    for (int j = 0; j < num_subsets; j++) {
        prob->obj[j] = reg_rand_double(1.0, 10.0);
        prob->lb[j] = 0.0;
        prob->ub[j] = 1.0;
        prob->vtype[j] = 'B';
    }

    for (int i = 0; i < num_elements; i++) {
        int covers = 0;
        for (int j = 0; j < num_subsets; j++) {
            if (reg_rand_double(0, 1) < density) {
                reg_mip_add_coef(prob, i, j, 1.0);
                covers++;
            }
        }
        while (covers < 2) {
            int j = reg_rand_int(0, num_subsets - 1);
            reg_mip_add_coef(prob, i, j, 1.0);
            covers++;
        }
        prob->rhs[i] = 1.0;
        prob->sense_con[i] = 'E';
    }

    return prob;
}

static RalphModel *build_regression_model(const RegressionMIPProblem *prob)
{
    RalphModel *m = ralph_test_create();
    if (!m) return NULL;

    ralph_test_set_obj_sense(m, RALPH_MINIMIZE);
    for (int j = 0; j < prob->num_vars; j++) {
        RalphVarType type = RALPH_CONTINUOUS;
        if (prob->vtype[j] == 'B') type = RALPH_BINARY;
        else if (prob->vtype[j] == 'I') type = RALPH_INTEGER;
        ralph_test_add_var(m, prob->lb[j], prob->ub[j], prob->obj[j], type);
    }

    for (int i = 0; i < prob->num_cons; i++) {
        int count = 0;
        for (int k = 0; k < prob->nnz; k++) {
            if (prob->con_row[k] == i) count++;
        }
        if (count > 0) {
            int *ind = (int*)malloc((size_t)count * sizeof(int));
            double *val = (double*)malloc((size_t)count * sizeof(double));
            int pos = 0;
            for (int k = 0; k < prob->nnz; k++) {
                if (prob->con_row[k] == i) {
                    ind[pos] = prob->con_col[k];
                    val[pos] = prob->con_val[k];
                    pos++;
                }
            }
            ralph_test_add_constraint(m, count, ind, val,
                                      prob->sense_con[i] == 'E'
                                          ? RALPH_EQUAL
                                          : (prob->sense_con[i] == 'G'
                                                 ? RALPH_GREATER_EQUAL
                                                 : RALPH_LESS_EQUAL),
                                      prob->rhs[i]);
            free(ind);
            free(val);
        }
    }

    ralph_test_set_int_param(m, "verbose", 0);
    return m;
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

static int test_set_partitioning_small_glpk_regression(void)
{
    RegressionMIPProblem *prob =
        generate_set_partitioning_regression(10, 30, 0.35, 42);
    RalphModel *m;
    double obj;
    int failed = 0;

    printf("\n=== test_set_partitioning_small_glpk_regression ===\n");
    if (!prob) {
        printf("FAIL: could not generate regression problem\n");
        return 1;
    }

    m = build_regression_model(prob);
    if (!m) {
        printf("FAIL: could not build Ralph model\n");
        reg_mip_free(prob);
        return 1;
    }

    (void)ralph_test_optimize(m);
    if (ralph_test_get_status(m) != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: expected OPTIMAL, got status=%d\n", ralph_test_get_status(m));
        failed = 1;
    } else {
        double manual_obj = 0.0;
        obj = ralph_test_get_objval(m);
        if (!validate_integer_solution_against_model(m, prob, &manual_obj)) {
            failed = 1;
        }
        if (fabs(obj - manual_obj) > 1e-6) {
            printf("FAIL: API objective %.10f does not match returned solution %.10f\n",
                   obj, manual_obj);
            failed = 1;
        }
        if (!isfinite(ralph_test_get_best_bound(m))) {
            printf("FAIL: best bound must be finite\n");
            failed = 1;
        }
        printf("Objective: %.4f (GLPK benchmark reference ~12.40)\n", obj);
        if (fabs(obj - 12.40) > 0.05) {
            printf("FAIL: objective deviates from GLPK reference\n");
            failed = 1;
        } else {
            printf("PASS\n");
        }
    }

    ralph_test_free(m);
    reg_mip_free(prob);
    return failed;
}

static int test_set_partitioning_medium_glpk_regression(void)
{
    RegressionMIPProblem *prob =
        generate_set_partitioning_regression(20, 60, 0.30, 123);
    RalphModel *m;
    int failed = 0;

    printf("\n=== test_set_partitioning_medium_glpk_regression ===\n");
    if (!prob) {
        printf("FAIL: could not generate regression problem\n");
        return 1;
    }

    m = build_regression_model(prob);
    if (!m) {
        printf("FAIL: could not build Ralph model\n");
        reg_mip_free(prob);
        return 1;
    }

    (void)ralph_test_optimize(m);
    if (ralph_test_get_status(m) != RALPH_STATUS_INFEASIBLE) {
        printf("FAIL: expected INFEASIBLE like GLPK benchmark, got status=%d obj=%.4f\n",
               ralph_test_get_status(m), ralph_test_get_objval(m));
        failed = 1;
    } else {
        printf("PASS\n");
    }

    ralph_test_free(m);
    reg_mip_free(prob);
    return failed;
}

static int test_raw_lp_glpk_regression(const char *name,
                                       const char *path,
                                       double expected_obj)
{
    RalphModel *m = ralph_test_create();
    int failed = 0;

    printf("\n=== %s ===\n", name);
    if (!m) {
        printf("FAIL: could not create Ralph model\n");
        return 1;
    }
    if (ralph_test_read_lp(m, path) != 0) {
        printf("FAIL: could not read LP fixture %s\n", path);
        ralph_test_free(m);
        return 1;
    }

    ralph_test_set_int_param(m, "verbose", 0);
    (void)ralph_test_optimize(m);
    if (ralph_test_get_status(m) != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: expected OPTIMAL, got status=%d\n", ralph_test_get_status(m));
        failed = 1;
    } else {
        double obj = ralph_test_get_objval(m);
        int nodes = ralph_test_get_node_count(m);
        printf("Objective: %.6f (GLPK reference %.6f), nodes=%d\n",
               obj, expected_obj, nodes);
        if (fabs(obj - expected_obj) > 1e-4) {
            printf("FAIL: objective deviates from GLPK reference\n");
            failed = 1;
        } else {
            printf("PASS\n");
        }
        if (!isfinite(ralph_test_get_best_bound(m))) {
            printf("FAIL: best bound must be finite\n");
            failed = 1;
        }
    }

    ralph_test_free(m);
    return failed;
}

static int test_raw_lp_glpk_regression_with_gomory(const char *name,
                                                   const char *path,
                                                   double expected_obj)
{
    RalphModel *m = ralph_test_create();
    int failed = 0;

    printf("\n=== %s ===\n", name);
    if (!m) {
        printf("FAIL: could not create Ralph model\n");
        return 1;
    }
    if (ralph_test_read_lp(m, path) != 0) {
        printf("FAIL: could not read LP fixture %s\n", path);
        ralph_test_free(m);
        return 1;
    }

    ralph_test_set_int_param(m, "verbose", 0);
    ralph_test_set_int_param(m, "max_cut_rounds", 3);
    ralph_test_set_int_param(m, "root_cut_mask", MIP_ROOT_CUT_GOMORY_MASK);
    (void)ralph_test_optimize(m);
    if (ralph_test_get_status(m) != RALPH_STATUS_OPTIMAL) {
        printf("FAIL: expected OPTIMAL, got status=%d\n", ralph_test_get_status(m));
        failed = 1;
    } else {
        double obj = ralph_test_get_objval(m);
        int nodes = ralph_test_get_node_count(m);
        printf("Objective with Gomory root cuts: %.6f (GLPK reference %.6f), nodes=%d\n",
               obj, expected_obj, nodes);
        if (fabs(obj - expected_obj) > 1e-4) {
            printf("FAIL: objective deviates from GLPK reference with Gomory enabled\n");
            failed = 1;
        } else {
            printf("PASS\n");
        }
        if (!isfinite(ralph_test_get_best_bound(m))) {
            printf("FAIL: best bound must be finite\n");
            failed = 1;
        }
    }

    ralph_test_free(m);
    return failed;
}

int main(void) {
    int failures = 0;

    failures += test_basic_mip();
    failures += test_ralph_wrapper();
    failures += test_set_partitioning_small_glpk_regression();
    failures += test_set_partitioning_medium_glpk_regression();
    failures += test_raw_lp_glpk_regression("test_fuelwise_milp15_12348_raw_lp",
                                            "tests/data/fuelwise_milp15_12348.lp",
                                            111.0805589);
    failures += test_raw_lp_glpk_regression("test_fuelwise_milp15_12349_raw_lp",
                                            "tests/data/fuelwise_milp15_12349.lp",
                                            115.2437986);
    failures += test_raw_lp_glpk_regression("test_fuelwise_milp100_12347_raw_lp",
                                            "tests/data/fuelwise_milp100_12347.lp",
                                            1473.224345);
    failures += test_raw_lp_glpk_regression_with_gomory(
        "test_fuelwise_milp15_12348_raw_lp_gomory",
        "tests/data/fuelwise_milp15_12348.lp",
        111.0805589);
    failures += test_raw_lp_glpk_regression_with_gomory(
        "test_fuelwise_milp15_12349_raw_lp_gomory",
        "tests/data/fuelwise_milp15_12349.lp",
        115.2437986);
    failures += test_raw_lp_glpk_regression_with_gomory(
        "test_fuelwise_milp100_12347_raw_lp_gomory",
        "tests/data/fuelwise_milp100_12347.lp",
        1473.224345);

    printf("\n=== Summary ===\n");
    if (failures == 0) {
        printf("All tests passed\n");
    } else {
        printf("%d test(s) failed\n", failures);
    }

    return failures;
}
