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
#include "ralph.h"

static unsigned int seed;
static double randf(double lo, double hi) {
    seed = seed * 1103515245 + 12345;
    return lo + (seed % 10000) / 10000.0 * (hi - lo);
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

    /* Write LP file for GLPK */
    char filename[64];
    snprintf(filename, sizeof(filename), "/tmp/ralph_bench_%dx%d.lp", n, m);
    FILE *f = fopen(filename, "w");
    fprintf(f, "Maximize\n obj: ");
    for (int j = 0; j < n; j++) {
        if (j > 0) fprintf(f, " + ");
        fprintf(f, "%.10f x%d", costs[j], j);
    }
    fprintf(f, "\nSubject To\n");
    for (int i = 0; i < m; i++) {
        fprintf(f, " c%d: ", i);
        for (int k = 0; k < con_nnz[i]; k++) {
            if (k > 0) fprintf(f, " + ");
            fprintf(f, "%.10f x%d", con_val[i][k], con_idx[i][k]);
        }
        fprintf(f, " <= %.10f\n", rhs[i]);
    }
    fprintf(f, "Bounds\n");
    for (int j = 0; j < n; j++) fprintf(f, " 0 <= x%d <= 100\n", j);
    fprintf(f, "End\n");
    fclose(f);

    /* Solve with Ralph */
    RalphModel *model = ralph_create();
    ralph_set_int_param(model, "verbose", 0);
    ralph_set_obj_sense(model, RALPH_MAXIMIZE);
    for (int j = 0; j < n; j++) ralph_add_var(model, 0.0, 100.0, costs[j], 'C');
    for (int i = 0; i < m; i++) {
        ralph_add_constraint(model, con_nnz[i], con_idx[i], con_val[i], 'L', rhs[i]);
    }

    clock_t start = clock();
    ralph_optimize(model);
    double ralph_time = (double)(clock() - start) / CLOCKS_PER_SEC;
    double ralph_obj = ralph_get_objval(model);
    const char *ralph_status = ralph_status_string(ralph_get_status(model));
    ralph_free(model);

    /* Solve with GLPK - capture timing from output */
    char cmd[256], solfile[64];
    snprintf(solfile, sizeof(solfile), "/tmp/ralph_bench_%dx%d.sol", n, m);
    snprintf(cmd, sizeof(cmd), "glpsol --lp %s -o %s 2>&1", filename, solfile);

    FILE *glpk_pipe = popen(cmd, "r");
    double glpk_time = 0;
    int ret = 0;
    if (glpk_pipe) {
        char line[256];
        while (fgets(line, sizeof(line), glpk_pipe)) {
            if (strstr(line, "Time used:")) {
                sscanf(line, "Time used: %lf", &glpk_time);
            }
        }
        ret = pclose(glpk_pipe);
    }

    /* Parse GLPK result */
    double glpk_obj = 0;
    char glpk_status[32] = "UNKNOWN";
    if (ret == 0) {
        f = fopen(solfile, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "Status:")) {
                    sscanf(line, "Status: %31s", glpk_status);
                }
                if (strstr(line, "Objective:")) {
                    char *p = strstr(line, "=");
                    if (p) glpk_obj = atof(p + 1);
                }
            }
            fclose(f);
        }
    }

    /* Report results */
    double diff_pct = (glpk_obj != 0) ? 100.0 * (ralph_obj - glpk_obj) / glpk_obj : 0;
    char match[16];
    if (fabs(diff_pct) < 0.01) {
        snprintf(match, sizeof(match), "EXACT");
    } else {
        snprintf(match, sizeof(match), "%.2f%%", diff_pct);
    }

    printf("  Ralph: %8.4fs  %-8s  obj=%12.2f\n", ralph_time, ralph_status, ralph_obj);
    printf("  GLPK:  %8.4fs  %-8s  obj=%12.2f\n", glpk_time, glpk_status, glpk_obj);
    printf("  Match: %s\n", match);

    /* Cleanup */
    free(costs); free(rhs); free(con_nnz);
    for (int i = 0; i < m; i++) { free(con_idx[i]); free(con_val[i]); }
    free(con_idx); free(con_val);
}

int main(int argc, char **argv) {
    printf("Ralph vs GLPK Comparison\n");
    printf("========================\n\n");

    /* Check if GLPK is available */
    if (system("which glpsol >/dev/null 2>&1") != 0) {
        printf("Error: glpsol not found. Install GLPK to run this benchmark.\n");
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

    return 0;
}
