/*
 * Ralph LP Solver - Size Scaling Benchmark
 *
 * Tests solver performance on increasingly large LP problems.
 * Problems are generated with random coefficients and <= constraints
 * (maximization) to ensure feasibility.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ralph.h"

static unsigned int seed;
static double randf(double lo, double hi) {
    seed = seed * 1103515245 + 12345;
    return lo + (seed % 10000) / 10000.0 * (hi - lo);
}

double solve_problem(int n, int m, double density, unsigned int s,
                     double *obj_out, int *status_out) {
    seed = s;

    RalphModel *model = ralph_create();
    ralph_set_int_param(model, "verbose", 0);
    ralph_set_int_param(model, "max_iterations", 100000);
    ralph_set_obj_sense(model, RALPH_MAXIMIZE);

    /* Add variables with random costs */
    for (int j = 0; j < n; j++) {
        ralph_add_var(model, 0.0, 100.0, randf(1, 10), 'C');
    }

    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));

    /* Add <= constraints - always feasible with x=0 */
    for (int i = 0; i < m; i++) {
        int nnz = 0;
        for (int j = 0; j < n; j++) {
            if (randf(0, 1) < density) {
                idx[nnz] = j;
                val[nnz] = randf(1, 5);
                nnz++;
            }
        }
        if (nnz == 0) { idx[0] = 0; val[0] = 1.0; nnz = 1; }
        double rhs = randf(100, 500);
        ralph_add_constraint(model, nnz, idx, val, 'L', rhs);
    }

    free(idx);
    free(val);

    clock_t start = clock();
    ralph_optimize(model);
    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;

    *status_out = ralph_get_status(model);
    *obj_out = ralph_get_objval(model);
    ralph_free(model);
    return elapsed;
}

int main(int argc, char **argv) {
    printf("Ralph LP Solver - Size Scaling Benchmark\n");
    printf("=========================================\n\n");

    /* Default sizes, can be overridden with command line */
    int sizes[][2] = {
        {20, 10}, {50, 25}, {100, 50}, {200, 100},
        {500, 250}, {1000, 500}
    };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    /* Allow limiting sizes via command line */
    int max_size = 1000;
    if (argc > 1) {
        max_size = atoi(argv[1]);
    }

    printf("%-12s %-12s %-12s %-12s\n", "Size (nxm)", "Time(s)", "Status", "Objective");
    printf("%-12s %-12s %-12s %-12s\n", "----------", "-------", "------", "---------");

    for (int i = 0; i < num_sizes; i++) {
        int n = sizes[i][0];
        int m = sizes[i][1];

        if (n > max_size) break;

        double obj;
        int status;
        double solve_time = solve_problem(n, m, 0.3, 42, &obj, &status);

        char size_str[32];
        snprintf(size_str, sizeof(size_str), "%dx%d", n, m);

        printf("%-12s %-12.4f %-12s %-12.2f\n",
               size_str, solve_time, ralph_status_string(status), obj);
        fflush(stdout);
    }

    printf("\n");
    return 0;
}
