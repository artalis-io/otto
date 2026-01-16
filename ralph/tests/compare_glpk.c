/*
 * Ralph vs GLPK Quick Comparison
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "ralph.h"
#include "lp.h"

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

/* Generate LP file for GLPK and equivalent Ralph model */
static RalphModel* generate_lp(int n, int m, double density, const char *filename) {
    FILE *f = fopen(filename, "w");
    RalphModel *model = ralph_create();

    fprintf(f, "Minimize\n obj: ");

    /* Objective - use bounded variables to ensure finite optimum */
    double *c = malloc(n * sizeof(double));
    for (int j = 0; j < n; j++) {
        c[j] = randf(-10, 10);
        ralph_add_var(model, 0.0, 100.0, c[j], 'C');  /* Bounded: 0 <= x <= 100 */
        if (j > 0) {
            if (c[j] >= 0) fprintf(f, " + %.4f x%d", c[j], j);
            else fprintf(f, " - %.4f x%d", -c[j], j);
        } else {
            fprintf(f, "%.4f x%d", c[j], j);
        }
    }
    fprintf(f, "\n\nSubject To\n");

    /* Constraints */
    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));

    for (int i = 0; i < m; i++) {
        int nnz = 0;
        fprintf(f, " c%d: ", i);
        int first = 1;

        for (int j = 0; j < n; j++) {
            if (randf(0,1) < density) {
                double v = randf(-5, 5);
                idx[nnz] = j;
                val[nnz] = v;
                nnz++;

                if (first) {
                    fprintf(f, "%.4f x%d", v, j);
                    first = 0;
                } else {
                    if (v >= 0) fprintf(f, " + %.4f x%d", v, j);
                    else fprintf(f, " - %.4f x%d", -v, j);
                }
            }
        }

        if (nnz == 0) {
            idx[0] = 0;
            val[0] = 1.0;
            nnz = 1;
            fprintf(f, "1.0 x0");
        }

        double rhs = randf(0, 50);
        fprintf(f, " <= %.4f\n", rhs);
        ralph_add_constraint(model, nnz, idx, val, 'L', rhs);
    }

    fprintf(f, "\nBounds\n");
    for (int j = 0; j < n; j++) {
        fprintf(f, " 0 <= x%d <= 100\n", j);
    }
    fprintf(f, "\nEnd\n");

    fclose(f);
    free(c); free(idx); free(val);
    return model;
}

static void run_comparison(int n, int m, double density) {
    printf("\n── %d vars × %d cons (%.0f%% dense) ──\n", n, m, density*100);

    seed = 12345;  /* Reset for reproducibility */

    /* Generate problem */
    RalphModel *model = generate_lp(n, m, density, "/tmp/test.lp");

    /* Time Ralph */
    double t0 = get_time();
    ralph_optimize(model);
    double ralph_time = get_time() - t0;
    int status = ralph_get_status(model);
    double ralph_obj = ralph_get_objval(model);
    int ralph_iters = 0;
    ralph_get_int_param(model, "iterations", &ralph_iters);
    ralph_free(model);

    /* Time GLPK */
    t0 = get_time();
    system("glpsol --lp /tmp/test.lp -o /tmp/glpk.sol 2>/dev/null >/dev/null");
    double glpk_time = get_time() - t0;

    /* Parse GLPK objective */
    double glpk_obj = 0;
    char glpk_status[32] = "UNKNOWN";
    FILE *f = fopen("/tmp/glpk.sol", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Status:", 7) == 0) {
                sscanf(line, "Status: %31s", glpk_status);
            }
            /* Format: "Objective:  obj = -22.5 (MINimum)" */
            char *p = strstr(line, "obj =");
            if (p) {
                sscanf(p, "obj = %lf", &glpk_obj);
            }
        }
        fclose(f);
    }

    printf("  Ralph: %.4fs, obj=%.4f, iters=%d, status=%d\n",
           ralph_time, ralph_obj, ralph_iters, status);
    printf("  GLPK:  %.4fs, obj=%.4f\n", glpk_time, glpk_obj);
    printf("  Speedup: %.2fx %s\n",
           glpk_time > 0.0001 ? glpk_time / ralph_time : 0,
           ralph_time < glpk_time ? "(Ralph faster)" : "(GLPK faster)");
}

int main() {
    printf("╔═══════════════════════════════════════════╗\n");
    printf("║     Ralph vs GLPK 5.0 Comparison          ║\n");
    printf("╚═══════════════════════════════════════════╝\n");

    run_comparison(20, 10, 0.5);
    run_comparison(50, 25, 0.3);
    run_comparison(100, 50, 0.2);
    run_comparison(200, 100, 0.15);
    run_comparison(500, 200, 0.1);

    printf("\n");
    return 0;
}
