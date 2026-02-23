/*
 * Ralph vs GLPK Quick Comparison
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "ralph_test_mod_api.h"

static double get_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static unsigned int seed = 12345;
static double randf(double lo, double hi) {
    seed = seed * 1103515245 + 12345;
    return lo + (seed % 10000) / 10000.0 * (hi - lo);
}

static int glpsol_available(void) {
    int rc = system("which glpsol >/dev/null 2>&1");
    return rc == 0 ? 1 : 0;
}

/* Generate LP model */
static RalphModel* generate_lp(int n, int m, double density) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;
    ralph_set_int_param_id(model, RALPH_PARAM_VERBOSE, 0);
    ralph_set_int_param_id(model, RALPH_PARAM_MAX_ITERATIONS, 100000);

    /* Objective */
    double *c = malloc(n * sizeof(double));
    for (int j = 0; j < n; j++) {
        c[j] = randf(-10, 10);
        ralph_test_add_var(model, 0.0, 100.0, c[j], 'C');
    }

    /* Constraints */
    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));
    if (!c || !idx || !val) {
        free(c);
        free(idx);
        free(val);
        ralph_test_free(model);
        return NULL;
    }

    for (int i = 0; i < m; i++) {
        int nnz = 0;

        for (int j = 0; j < n; j++) {
            if (randf(0,1) < density) {
                double v = randf(-5, 5);
                idx[nnz] = j;
                val[nnz] = v;
                nnz++;
            }
        }

        if (nnz == 0) {
            idx[0] = 0;
            val[0] = 1.0;
            nnz = 1;
        }

        double rhs = randf(0, 50);
        ralph_test_add_constraint(model, nnz, idx, val, 'L', rhs);
    }

    free(c); free(idx); free(val);
    return model;
}

static void run_comparison(int n, int m, double density) {
    printf("\n── %d vars × %d cons (%.0f%% dense) ──\n", n, m, density*100);

    seed = 12345;

    /* Internal Ralph solve */
    RalphModel *model = generate_lp(n, m, density);
    if (!model) {
        printf("  Build error (internal)\n");
        return;
    }
    ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX);

    double t0 = get_time();
    (void)ralph_test_optimize_lp(model);
    double ralph_time = get_time() - t0;
    int status = ralph_test_get_status(model);
    double ralph_obj = ralph_test_get_objval(model);
    int ralph_iters = ralph_test_get_iterations(model);
    ralph_test_free(model);

    /* GLPK solve routed via Ralph external adapter */
    seed = 12345;
    model = generate_lp(n, m, density);
    if (!model) {
        printf("  Build error (external)\n");
        return;
    }
    ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                           (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL);
    t0 = get_time();
    (void)ralph_test_optimize_lp(model);
    double glpk_time = get_time() - t0;
    double glpk_obj = ralph_test_get_objval(model);
    const char *glpk_status = ralph_test_status_string(ralph_test_get_status(model));
    ralph_test_free(model);

    printf("  Ralph: %.4fs, obj=%.4f, iters=%d, status=%d\n",
           ralph_time, ralph_obj, ralph_iters, status);
    printf("  GLPK* (oop): %.4fs, obj=%.4f, status=%s\n", glpk_time, glpk_obj, glpk_status);
    printf("  Speedup: %.2fx %s\n",
           glpk_time > 0.0001 ? glpk_time / ralph_time : 0,
           ralph_time < glpk_time ? "(Ralph faster)" : "(GLPK faster)");
}

int main() {
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║     Ralph vs GLPK 5.0 Comparison          ║\n");
    printf("╚═══════════════════════════════════════════╝\n");

    if (!glpsol_available()) {
        printf("Error: glpsol not found. Install GLPK to run this comparison.\n");
        return 1;
    }
    ralph_unregister_all_lp_external_adapters();
    if (ralph_register_lp_external_glpk_oop(NULL) != 0) {
        printf("Error: failed to register GLPK out-of-process adapter.\n");
        return 1;
    }

    run_comparison(20, 10, 0.5);
    run_comparison(50, 25, 0.3);
    run_comparison(100, 50, 0.2);
    run_comparison(200, 100, 0.15);
    run_comparison(500, 200, 0.1);

    printf("\n");
    ralph_unregister_all_lp_external_adapters();
    return 0;
}
