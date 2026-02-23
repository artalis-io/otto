/*
 * Ralph LP Solver - GLPK Comparison Benchmark
 *
 * Compares Ralph's solutions and timing against GLPK on identical problems.
 * Requires glpsol (GLPK command-line solver) to be installed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "ralph_test_mod_api.h"

static unsigned int seed;
static double randf(double lo, double hi) {
    seed = seed * 1103515245 + 12345;
    return lo + (seed % 10000) / 10000.0 * (hi - lo);
}

static int glpsol_available(void) {
    int rc = system("which glpsol >/dev/null 2>&1");
    return rc == 0 ? 1 : 0;
}

static RalphModel* build_model(int n, int m, const double *costs, const double *rhs,
                               int **con_idx, double **con_val, const int *con_nnz) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
    ralph_set_int_param_id(model, RALPH_PARAM_VERBOSE, 0);
    ralph_set_int_param_id(model, RALPH_PARAM_MAX_ITERATIONS, 100000);
    for (int j = 0; j < n; j++) ralph_test_add_var(model, 0.0, 100.0, costs[j], 'C');
    for (int i = 0; i < m; i++) {
        ralph_test_add_constraint(model, con_nnz[i], con_idx[i], con_val[i], 'L', rhs[i]);
    }
    return model;
}

void run_comparison(int n, int m, double density, unsigned int s) {
    seed = s;

    /* Generate problem data */
    double *costs = malloc(n * sizeof(double));
    double *rhs = malloc(m * sizeof(double));
    int **con_idx = malloc(m * sizeof(int*));
    double **con_val = malloc(m * sizeof(double*));
    int *con_nnz = malloc(m * sizeof(int));

    for (int j = 0; j < n; j++) costs[j] = randf(1, 10);
    for (int i = 0; i < m; i++) {
        con_idx[i] = malloc(n * sizeof(int));
        con_val[i] = malloc(n * sizeof(double));
        int nnz = 0;
        for (int j = 0; j < n; j++) {
            if (randf(0, 1) < density) {
                con_idx[i][nnz] = j;
                con_val[i][nnz] = randf(1, 5);
                nnz++;
            }
        }
        if (nnz == 0) { con_idx[i][0] = 0; con_val[i][0] = 1.0; nnz = 1; }
        rhs[i] = randf(100, 500);
        con_nnz[i] = nnz;
    }

    /* Solve with Ralph internal primal simplex */
    RalphModel *model = build_model(n, m, costs, rhs, con_idx, con_val, con_nnz);
    if (!model) {
        fprintf(stderr, "Failed to build internal model\n");
        goto cleanup;
    }
    ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX);

    clock_t start = clock();
    (void)ralph_test_optimize_lp(model);
    double ralph_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    double ralph_obj = ralph_test_get_objval(model);
    const char *ralph_status = ralph_test_status_string(ralph_test_get_status(model));
    ralph_test_free(model);

    /* Solve with GLPK via Ralph out-of-process external adapter */
    model = build_model(n, m, costs, rhs, con_idx, con_val, con_nnz);
    if (!model) {
        fprintf(stderr, "Failed to build external model\n");
        goto cleanup;
    }
    ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                           (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL);
    start = clock();
    (void)ralph_test_optimize_lp(model);
    double glpk_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    double glpk_obj = ralph_test_get_objval(model);
    const char *glpk_status = ralph_test_status_string(ralph_test_get_status(model));
    ralph_test_free(model);

    /* Report results */
    double diff_pct = (glpk_obj != 0) ? 100.0 * (ralph_obj - glpk_obj) / glpk_obj : 0;
    char match[16];
    if (fabs(diff_pct) < 0.01) {
        snprintf(match, sizeof(match), "EXACT");
    } else {
        snprintf(match, sizeof(match), "%.2f%%", diff_pct);
    }

    printf("  Ralph: %8.4fs  %-8s  obj=%12.2f\n", ralph_time, ralph_status, ralph_obj);
    printf("  GLPK* (oop): %8.4fs  %-8s  obj=%12.2f\n", glpk_time, glpk_status, glpk_obj);
    printf("  Match: %s\n", match);

cleanup:
    free(costs); free(rhs); free(con_nnz);
    for (int i = 0; i < m; i++) { free(con_idx[i]); free(con_val[i]); }
    free(con_idx); free(con_val);
}

int main(int argc, char **argv) {
    printf("Ralph vs GLPK Comparison\n");
    printf("========================\n\n");

    if (!glpsol_available()) {
        printf("Error: glpsol not found. Install GLPK to run this benchmark.\n");
        return 1;
    }
    ralph_unregister_all_lp_external_adapters();
    if (ralph_register_lp_external_glpk_oop(NULL) != 0) {
        printf("Error: failed to register GLPK out-of-process adapter.\n");
        return 1;
    }

    int sizes[][2] = {{20, 10}, {50, 25}, {100, 50}, {200, 100}, {500, 250}};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    /* Allow limiting sizes via command line */
    int max_size = 500;
    if (argc > 1) {
        max_size = atoi(argv[1]);
    }

    for (int i = 0; i < num_sizes; i++) {
        int n = sizes[i][0];
        int m = sizes[i][1];

        if (n > max_size) break;

        printf("%dx%d:\n", n, m);
        run_comparison(n, m, 0.3, 42);
        printf("\n");
    }

    ralph_unregister_all_lp_external_adapters();
    return 0;
}
