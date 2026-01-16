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
#include "lp.h"

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

/* Generate bounded LP and write to file */
static RalphModel* generate_lp(int n, int m, double density, const char *fname) {
    FILE *f = fopen(fname, "w");
    RalphModel *model = ralph_create();
    ralph_set_int_param(model, "max_iterations", 100000);

    fprintf(f, "Minimize\n obj:");
    double *c = malloc(n * sizeof(double));
    for (int j = 0; j < n; j++) {
        c[j] = randf(1, 10);  /* Positive costs for bounded optimum */
        ralph_add_var(model, 0.0, 100.0, c[j], 'C');
        fprintf(f, " + %.4f x%d", c[j], j);
    }
    fprintf(f, "\n\nSubject To\n");

    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));
    for (int i = 0; i < m; i++) {
        int nnz = 0;
        fprintf(f, " c%d:", i);
        for (int j = 0; j < n; j++) {
            if (randf(0,1) < density) {
                double v = randf(1, 5);  /* Positive coefficients */
                idx[nnz] = j;
                val[nnz] = v;
                fprintf(f, " + %.4f x%d", v, j);
                nnz++;
            }
        }
        if (nnz == 0) { idx[0] = 0; val[0] = 1.0; nnz = 1; fprintf(f, " + 1.0 x0"); }
        double rhs = randf(50, 200);
        fprintf(f, " >= %.4f\n", rhs);
        ralph_add_constraint(model, nnz, idx, val, 'G', rhs);
    }

    fprintf(f, "\nBounds\n");
    for (int j = 0; j < n; j++) fprintf(f, " 0 <= x%d <= 100\n", j);
    fprintf(f, "\nEnd\n");

    fclose(f);
    free(c); free(idx); free(val);
    return model;
}

static void benchmark(int n, int m, double density) {
    printf("\n%d×%d (%.0f%% dense):\n", n, m, density*100);
    seed = 42;

    RalphModel *model = generate_lp(n, m, density, "/tmp/test.lp");

    double t0 = get_time();
    ralph_optimize(model);
    double ralph_time = get_time() - t0;
    int status = ralph_get_status(model);
    double ralph_obj = ralph_get_objval(model);

    t0 = get_time();
    system("glpsol --lp /tmp/test.lp -o /tmp/glpk.sol 2>/dev/null >/dev/null");
    double glpk_time = get_time() - t0;

    double glpk_obj = 0;
    FILE *f = fopen("/tmp/glpk.sol", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *p = strstr(line, "obj =");
            if (p) { sscanf(p, "obj = %lf", &glpk_obj); break; }
        }
        fclose(f);
    }

    printf("  Ralph: %.4fs, obj=%.2f, status=%s\n",
           ralph_time, ralph_obj, ralph_status_string(status));
    printf("  GLPK:  %.4fs, obj=%.2f\n", glpk_time, glpk_obj);

    if (status == RALPH_STATUS_OPTIMAL && fabs(ralph_obj - glpk_obj) < 0.01) {
        double speedup = glpk_time / ralph_time;
        printf("  ✓ Match! Speedup: %.2fx %s\n", speedup,
               speedup > 1 ? "(Ralph faster)" : "(GLPK faster)");
    } else if (status == RALPH_STATUS_OPTIMAL) {
        printf("  ✗ Objectives differ by %.2f\n", fabs(ralph_obj - glpk_obj));
    } else {
        printf("  ✗ Ralph failed: %s\n", ralph_status_string(status));
    }

    ralph_free(model);
}

int main() {
    printf("═══════════════════════════════════════\n");
    printf("   Ralph vs GLPK 5.0 Quick Benchmark\n");
    printf("═══════════════════════════════════════\n");

    benchmark(20, 10, 0.5);
    benchmark(50, 25, 0.3);
    benchmark(100, 50, 0.2);
    benchmark(200, 100, 0.15);

    printf("\n");
    return 0;
}
