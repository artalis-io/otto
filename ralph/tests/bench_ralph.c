/*
 * Ralph LP/MIP Solver - Performance Benchmark
 *
 * Tests solver performance on LP problems of various sizes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"

/* Simple random number generator */
static unsigned int bench_seed = 12345;

static double rand_double(double min, double max) {
    bench_seed = bench_seed * 1103515245 + 12345;
    double r = (double)(bench_seed % 10000) / 10000.0;
    return min + r * (max - min);
}

static int rand_int(int min, int max) {
    bench_seed = bench_seed * 1103515245 + 12345;
    return min + (bench_seed % (max - min + 1));
}

/*
 * Generate a random LP problem:
 *   min c'x
 *   s.t. Ax <= b
 *        x >= 0
 *
 * Matrix A has approximately 'density' fraction of non-zeros.
 */
static RalphModel* generate_random_lp(int num_vars, int num_cons, double density) {
    RalphModel *model = ralph_create();
    if (!model) return NULL;

    /* Add variables with random objective coefficients */
    for (int j = 0; j < num_vars; j++) {
        double obj = rand_double(-10.0, 10.0);
        ralph_add_var(model, 0.0, RALPH_INFINITY, obj, 'C');
    }

    /* Add constraints with random coefficients */
    int *indices = (int*)malloc(num_vars * sizeof(int));
    double *values = (double*)malloc(num_vars * sizeof(double));

    for (int i = 0; i < num_cons; i++) {
        int nnz = 0;

        /* Generate sparse row */
        for (int j = 0; j < num_vars; j++) {
            if (rand_double(0, 1) < density) {
                indices[nnz] = j;
                values[nnz] = rand_double(-5.0, 5.0);
                nnz++;
            }
        }

        /* Ensure at least one non-zero per row */
        if (nnz == 0) {
            indices[0] = rand_int(0, num_vars - 1);
            values[0] = rand_double(-5.0, 5.0);
            nnz = 1;
        }

        /* RHS chosen to make problem likely feasible */
        double rhs = rand_double(0, 50.0);
        ralph_add_constraint(model, nnz, indices, values, 'L', rhs);
    }

    free(indices);
    free(values);

    return model;
}

/*
 * Generate a random MIP (mixed-integer) problem
 */
static RalphModel* generate_random_mip(int num_vars, int num_cons, double density, double int_frac) {
    RalphModel *model = ralph_create();
    if (!model) return NULL;

    /* Add variables - some integer, some continuous */
    for (int j = 0; j < num_vars; j++) {
        double obj = rand_double(-10.0, 10.0);
        char type = (rand_double(0, 1) < int_frac) ? 'I' : 'C';
        double ub = (type == 'I') ? 10.0 : RALPH_INFINITY;
        ralph_add_var(model, 0.0, ub, obj, type);
    }

    /* Add constraints */
    int *indices = (int*)malloc(num_vars * sizeof(int));
    double *values = (double*)malloc(num_vars * sizeof(double));

    for (int i = 0; i < num_cons; i++) {
        int nnz = 0;

        for (int j = 0; j < num_vars; j++) {
            if (rand_double(0, 1) < density) {
                indices[nnz] = j;
                values[nnz] = rand_double(-5.0, 5.0);
                nnz++;
            }
        }

        if (nnz == 0) {
            indices[0] = rand_int(0, num_vars - 1);
            values[0] = rand_double(-5.0, 5.0);
            nnz = 1;
        }

        double rhs = rand_double(0, 50.0);
        ralph_add_constraint(model, nnz, indices, values, 'L', rhs);
    }

    free(indices);
    free(values);

    return model;
}

/*
 * Run LP benchmark
 */
static void benchmark_lp(int num_vars, int num_cons, double density, int num_trials) {
    printf("\n┌─────────────────────────────────────────────────────────┐\n");
    printf("│ LP Benchmark: %d vars × %d cons (%.0f%% dense)           \n",
           num_vars, num_cons, density * 100);
    printf("└─────────────────────────────────────────────────────────┘\n");

    double total_time = 0.0;
    int total_iters = 0;
    int solved = 0;
    int optimal = 0;

    for (int trial = 0; trial < num_trials; trial++) {
        bench_seed = 12345 + trial * 1000;  /* Different seed per trial */

        RalphModel *model = generate_random_lp(num_vars, num_cons, density);
        if (!model) {
            printf("  Trial %d: Failed to create model\n", trial + 1);
            continue;
        }

        clock_t start = clock();
        int status = ralph_optimize(model);
        clock_t end = clock();

        double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
        total_time += elapsed;

        int iters = 0;
        ralph_get_int_param(model, "iterations", &iters);
        total_iters += iters;

        solved++;
        if (status == RALPH_STATUS_OPTIMAL) {
            optimal++;
        }

        ralph_free(model);
    }

    if (solved > 0) {
        printf("  Trials:     %d/%d solved, %d optimal\n", solved, num_trials, optimal);
        printf("  Avg time:   %.4f sec\n", total_time / solved);
        printf("  Avg iters:  %d\n", total_iters / solved);
        printf("  Total time: %.4f sec\n", total_time);
    }
}

/*
 * Run MIP benchmark
 */
static void benchmark_mip(int num_vars, int num_cons, double density, double int_frac, int num_trials) {
    printf("\n┌─────────────────────────────────────────────────────────┐\n");
    printf("│ MIP Benchmark: %d vars × %d cons (%.0f%% int, %.0f%% dense)\n",
           num_vars, num_cons, int_frac * 100, density * 100);
    printf("└─────────────────────────────────────────────────────────┘\n");

    double total_time = 0.0;
    int solved = 0;
    int optimal = 0;

    for (int trial = 0; trial < num_trials; trial++) {
        bench_seed = 54321 + trial * 1000;

        RalphModel *model = generate_random_mip(num_vars, num_cons, density, int_frac);
        if (!model) {
            printf("  Trial %d: Failed to create model\n", trial + 1);
            continue;
        }

        /* Set time limit for MIP */
        ralph_set_dbl_param(model, "time_limit", 30.0);

        clock_t start = clock();
        int status = ralph_optimize(model);
        clock_t end = clock();

        double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
        total_time += elapsed;

        solved++;
        if (status == RALPH_STATUS_OPTIMAL) {
            optimal++;
        }

        ralph_free(model);
    }

    if (solved > 0) {
        printf("  Trials:     %d/%d solved, %d optimal\n", solved, num_trials, optimal);
        printf("  Avg time:   %.4f sec\n", total_time / solved);
        printf("  Total time: %.4f sec\n", total_time);
    }
}

/*
 * Benchmark specific problem: Diet problem scaled up
 */
static void benchmark_diet_scaled(int scale) {
    printf("\n┌─────────────────────────────────────────────────────────┐\n");
    printf("│ Diet Problem (scaled %dx)                                \n", scale);
    printf("└─────────────────────────────────────────────────────────┘\n");

    RalphModel *model = ralph_create();

    /* Foods: bread, milk, cheese, potato, fish, yogurt (repeated) */
    int num_foods = 6 * scale;
    double base_costs[] = {2.0, 3.5, 8.0, 1.5, 11.0, 1.0};
    double base_protein[] = {4.0, 8.0, 7.0, 1.3, 8.0, 9.2};
    double base_fat[] = {1.0, 5.0, 9.0, 0.1, 7.0, 1.0};
    double base_carbs[] = {15.0, 11.7, 0.4, 22.6, 0.0, 17.0};
    double base_calories[] = {90, 120, 106, 97, 130, 180};

    for (int i = 0; i < num_foods; i++) {
        int base = i % 6;
        double cost = base_costs[base] * (1.0 + 0.1 * (i / 6));
        ralph_add_var(model, 0.0, RALPH_INFINITY, cost, 'C');
    }

    /* Nutrient constraints */
    int *indices = (int*)malloc(num_foods * sizeof(int));
    double *values = (double*)malloc(num_foods * sizeof(double));

    /* Protein >= 50 * scale */
    for (int i = 0; i < num_foods; i++) {
        indices[i] = i;
        values[i] = base_protein[i % 6];
    }
    ralph_add_constraint(model, num_foods, indices, values, 'G', 50.0 * scale);

    /* Fat <= 60 * scale */
    for (int i = 0; i < num_foods; i++) {
        values[i] = base_fat[i % 6];
    }
    ralph_add_constraint(model, num_foods, indices, values, 'L', 60.0 * scale);

    /* Carbs >= 200 * scale */
    for (int i = 0; i < num_foods; i++) {
        values[i] = base_carbs[i % 6];
    }
    ralph_add_constraint(model, num_foods, indices, values, 'G', 200.0 * scale);

    /* Calories in range */
    for (int i = 0; i < num_foods; i++) {
        values[i] = base_calories[i % 6];
    }
    ralph_add_constraint(model, num_foods, indices, values, 'G', 1800.0 * scale);
    ralph_add_constraint(model, num_foods, indices, values, 'L', 2500.0 * scale);

    free(indices);
    free(values);

    clock_t start = clock();
    int status = ralph_optimize(model);
    clock_t end = clock();

    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    int iters = 0;
    ralph_get_int_param(model, "iterations", &iters);

    printf("  Variables:  %d\n", num_foods);
    printf("  Constraints: 5\n");
    printf("  Status:     %s\n", ralph_status_string(status));
    if (status == RALPH_STATUS_OPTIMAL) {
        double obj = 0;
        obj = ralph_get_objval(model);
        printf("  Objective:  %.2f\n", obj);
    }
    printf("  Iterations: %d\n", iters);
    printf("  Time:       %.4f sec\n", elapsed);

    ralph_free(model);
}

/*
 * Main benchmark driver
 */
int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║       Ralph LP/MIP Solver - Performance Benchmark        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    printf("\nRalph version: %s\n", ralph_version());

    int quick_mode = (argc > 1 && strcmp(argv[1], "--quick") == 0);

    if (quick_mode) {
        printf("\n[Quick mode: reduced problem sizes]\n");
    }

    /* LP Benchmarks */
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("                    LP BENCHMARKS\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* Small LP */
    benchmark_lp(50, 20, 0.3, 5);

    /* Medium LP */
    benchmark_lp(100, 50, 0.2, 5);

    if (!quick_mode) {
        /* Larger LP */
        benchmark_lp(200, 100, 0.15, 3);

        /* Large LP */
        benchmark_lp(500, 200, 0.1, 3);

        /* Very large LP */
        benchmark_lp(1000, 500, 0.05, 2);
    }

    /* Diet problem scaling */
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("                    DIET PROBLEM SCALING\n");
    printf("═══════════════════════════════════════════════════════════\n");

    benchmark_diet_scaled(1);
    benchmark_diet_scaled(10);
    if (!quick_mode) {
        benchmark_diet_scaled(100);
        benchmark_diet_scaled(500);
    }

    /* MIP Benchmarks */
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("                    MIP BENCHMARKS\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* Small MIP */
    benchmark_mip(20, 10, 0.4, 0.5, 3);

    /* Medium MIP */
    benchmark_mip(50, 25, 0.3, 0.3, 3);

    /* Larger MIPs */
    benchmark_mip(100, 50, 0.2, 0.2, 2);

    if (!quick_mode) {
        benchmark_mip(200, 100, 0.15, 0.3, 2);
        benchmark_mip(300, 150, 0.10, 0.3, 2);
    }

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("                    BENCHMARK COMPLETE\n");
    printf("═══════════════════════════════════════════════════════════\n\n");

    return 0;
}
