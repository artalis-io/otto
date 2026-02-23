/*
 * Quick Ralph vs GLPK benchmark
 * Tests bounded LP problems of various sizes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "ralph.h"

static double get_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static unsigned int seed;
static double randf(double lo, double hi) {
    seed = seed * 1103515245 + 12345;
    return lo + (seed % 10000) / 10000.0 * (hi - lo);
}

static int glpsol_available(void) {
    int rc = system("which glpsol >/dev/null 2>&1");
    return rc == 0 ? 1 : 0;
}

/* Generate bounded LP model */
static RalphModel* generate_lp(int n, int m, double density) {
    RalphModel *model = ralph_create();
    if (!model) return NULL;
    ralph_set_int_param_id(model, RALPH_PARAM_VERBOSE, 0);
    ralph_set_int_param_id(model, RALPH_PARAM_MAX_ITERATIONS, 100000);

    double *c = malloc(n * sizeof(double));
    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));
    if (!c || !idx || !val) {
        free(c);
        free(idx);
        free(val);
        ralph_free(model);
        return NULL;
    }
    for (int j = 0; j < n; j++) {
        c[j] = randf(1, 10);  /* Positive costs for bounded optimum */
        ralph_add_var(model, 0.0, 100.0, c[j], 'C');
    }

    for (int i = 0; i < m; i++) {
        int nnz = 0;
        for (int j = 0; j < n; j++) {
            if (randf(0,1) < density) {
                double v = randf(1, 5);  /* Positive coefficients */
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
        double rhs = randf(50, 200);
        ralph_add_constraint(model, nnz, idx, val, 'G', rhs);
    }

    free(c); free(idx); free(val);
    return model;
}

static void benchmark(int n, int m, double density) {
    printf("\n%d×%d (%.0f%% dense):\n", n, m, density*100);
    seed = 42;

    RalphModel *model = generate_lp(n, m, density);
    if (!model) {
        printf("  Build error (internal)\n");
        return;
    }
    ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX);

    double t0 = get_time();
    (void)ralph_optimize_lp(model);
    double ralph_time = get_time() - t0;
    int status = ralph_get_status(model);
    double ralph_obj = ralph_get_objval(model);
    ralph_free(model);

    seed = 42;
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
    (void)ralph_optimize_lp(model);
    double glpk_time = get_time() - t0;
    double glpk_obj = ralph_get_objval(model);
    const char *glpk_status = ralph_status_string(ralph_get_status(model));
    ralph_free(model);

    printf("  Ralph: %.4fs, obj=%.2f, status=%s\n",
           ralph_time, ralph_obj, ralph_status_string(status));
    printf("  GLPK* (oop): %.4fs, obj=%.2f, status=%s\n",
           glpk_time, glpk_obj, glpk_status);

    if (status == RALPH_STATUS_OPTIMAL && fabs(ralph_obj - glpk_obj) < 0.01) {
        double speedup = glpk_time / ralph_time;
        printf("  ✓ Match! Speedup: %.2fx %s\n", speedup,
               speedup > 1 ? "(Ralph faster)" : "(GLPK faster)");
    } else if (status == RALPH_STATUS_OPTIMAL) {
        printf("  ✗ Objectives differ by %.2f\n", fabs(ralph_obj - glpk_obj));
    } else {
        printf("  ✗ Ralph failed: %s\n", ralph_status_string(status));
    }

}

int main() {
    printf("═══════════════════════════════════════\n");
    printf("   Ralph vs GLPK 5.0 Quick Benchmark\n");
    printf("═══════════════════════════════════════\n");

    if (!glpsol_available()) {
        printf("Error: glpsol not found. Install GLPK to run this benchmark.\n");
        return 1;
    }
    ralph_unregister_all_lp_external_adapters();
    if (ralph_register_lp_external_glpk_oop(NULL) != 0) {
        printf("Error: failed to register GLPK out-of-process adapter.\n");
        return 1;
    }

    benchmark(20, 10, 0.5);
    benchmark(50, 25, 0.3);
    benchmark(100, 50, 0.2);
    benchmark(200, 100, 0.15);

    printf("\n");
    ralph_unregister_all_lp_external_adapters();
    return 0;
}
