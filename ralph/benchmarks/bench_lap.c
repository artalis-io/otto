/*
 * bench_lap.c - Benchmarks for Linear Assignment Problem solver
 *
 * Compares JVC algorithm performance against LP solver on various
 * problem sizes and structures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "lap.h"
#include "ralph_test_mod_api.h"

/* ============================================================================
 * Timing utilities
 * ============================================================================ */

typedef struct {
    clock_t start;
    double elapsed_ms;
} Timer;

static void timer_start(Timer *t) {
    t->start = clock();
}

static void timer_stop(Timer *t) {
    t->elapsed_ms = (double)(clock() - t->start) / CLOCKS_PER_SEC * 1000.0;
}

/* ============================================================================
 * Problem generators
 * ============================================================================ */

/* Uniform random costs */
static void generate_random(int n, double *cost, int seed) {
    srand(seed);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 10000) / 100.0;  /* 0.00 to 99.99 */
    }
}

/* Sparse problem (many infinite costs) */
static void generate_sparse(int n, double *cost, double density, int seed) {
    srand(seed);
    for (int i = 0; i < n * n; i++) {
        if ((double)rand() / RAND_MAX < density) {
            cost[i] = (rand() % 10000) / 100.0;
        } else {
            cost[i] = RALPH_LAP_INFINITY;
        }
    }
    /* Ensure feasibility: add diagonal as fallback */
    for (int i = 0; i < n; i++) {
        if (cost[i * n + i] >= RALPH_LAP_INFINITY) {
            cost[i * n + i] = (rand() % 10000) / 100.0;
        }
    }
}

/* Geometric distance (TSP-like) */
static void generate_geometric(int n, double *cost, int seed) {
    srand(seed);
    double *x = malloc(n * sizeof(double));
    double *y = malloc(n * sizeof(double));

    for (int i = 0; i < n; i++) {
        x[i] = (double)rand() / RAND_MAX * 100.0;
        y[i] = (double)rand() / RAND_MAX * 100.0;
    }

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double dx = x[i] - x[j];
            double dy = y[i] - y[j];
            cost[i * n + j] = sqrt(dx * dx + dy * dy);
        }
    }

    free(x);
    free(y);
}

/* Structured problem (blocks with different cost ranges) */
static void generate_structured(int n, double *cost, int seed) {
    srand(seed);
    int block_size = n / 4;
    if (block_size < 1) block_size = 1;

    for (int i = 0; i < n; i++) {
        int block_i = i / block_size;
        for (int j = 0; j < n; j++) {
            int block_j = j / block_size;
            double base = (block_i == block_j) ? 10.0 : 100.0;
            cost[i * n + j] = base + (rand() % 100) / 10.0;
        }
    }
}

/* ============================================================================
 * Benchmark runner
 * ============================================================================ */

typedef struct {
    const char *name;
    void (*generator)(int n, double *cost, int seed);
    int use_density;      /* For sparse generator */
    double density;
} ProblemType;

static void run_benchmark(const char *name, int n, double *cost, int warmup, int trials) {
    int *row_sol = malloc(n * sizeof(int));
    double jvc_cost, lp_cost;
    Timer timer;

    /* Warmup */
    for (int i = 0; i < warmup; i++) {
        ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL);
    }

    /* JVC benchmark */
    double jvc_total = 0.0;
    for (int i = 0; i < trials; i++) {
        timer_start(&timer);
        ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &jvc_cost);
        timer_stop(&timer);
        jvc_total += timer.elapsed_ms;
    }
    double jvc_avg = jvc_total / trials;

    /* LP benchmark (skip for large problems) */
    double lp_avg = -1.0;
    if (n <= 50) {  /* LP is too slow for larger problems */
        double lp_total = 0.0;
        for (int i = 0; i < trials; i++) {
            timer_start(&timer);
            ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, row_sol, &lp_cost);
            timer_stop(&timer);
            lp_total += timer.elapsed_ms;
        }
        lp_avg = lp_total / trials;
    }

    /* Print results */
    if (lp_avg > 0) {
        printf("  %-20s %6d  %10.3f  %10.3f  %10.1fx  %.2f vs %.2f\n",
               name, n, jvc_avg, lp_avg, lp_avg / jvc_avg, jvc_cost, lp_cost);
    } else {
        printf("  %-20s %6d  %10.3f  %10s  %10s  %.2f\n",
               name, n, jvc_avg, "N/A", "N/A", jvc_cost);
    }

    free(row_sol);
}

/* ============================================================================
 * Size scaling benchmark
 * ============================================================================ */

static void bench_size_scaling(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Size Scaling Benchmark (Random Costs)                                    ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("  %-20s %6s  %10s  %10s  %10s  %s\n",
           "Problem", "Size", "JVC (ms)", "LP (ms)", "Speedup", "Cost Check");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int sizes[] = {5, 10, 20, 30, 50, 75, 100, 150, 200, 300, 500};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));
        generate_random(n, cost, 42);

        int trials = (n <= 50) ? 10 : (n <= 100) ? 5 : 3;
        int warmup = (n <= 100) ? 2 : 1;

        run_benchmark("Random", n, cost, warmup, trials);
        free(cost);
    }
}

/* ============================================================================
 * Problem type comparison
 * ============================================================================ */

static void bench_problem_types(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Problem Type Comparison (n=50)                                           ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("  %-20s %6s  %10s  %10s  %10s  %s\n",
           "Type", "Size", "JVC (ms)", "LP (ms)", "Speedup", "Costs");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int n = 50;
    int trials = 10;
    int warmup = 2;

    /* Random */
    {
        double *cost = malloc(n * n * sizeof(double));
        generate_random(n, cost, 123);
        run_benchmark("Random", n, cost, warmup, trials);
        free(cost);
    }

    /* Geometric */
    {
        double *cost = malloc(n * n * sizeof(double));
        generate_geometric(n, cost, 123);
        run_benchmark("Geometric", n, cost, warmup, trials);
        free(cost);
    }

    /* Structured */
    {
        double *cost = malloc(n * n * sizeof(double));
        generate_structured(n, cost, 123);
        run_benchmark("Structured", n, cost, warmup, trials);
        free(cost);
    }

    /* Sparse 50% */
    {
        double *cost = malloc(n * n * sizeof(double));
        generate_sparse(n, cost, 0.5, 123);
        run_benchmark("Sparse (50%)", n, cost, warmup, trials);
        free(cost);
    }

    /* Sparse 20% */
    {
        double *cost = malloc(n * n * sizeof(double));
        generate_sparse(n, cost, 0.2, 123);
        run_benchmark("Sparse (20%)", n, cost, warmup, trials);
        free(cost);
    }
}

/* ============================================================================
 * Sparse vs Dense benchmark at various sparsity levels
 * ============================================================================ */

static void bench_sparse_vs_dense(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Sparse vs Dense Comparison at Various Sparsity Levels                    ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("  %-8s %8s  %12s  %12s  %10s  %8s\n",
           "Size", "Density", "Dense (ms)", "Sparse (ms)", "Speedup", "Status");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int sizes[] = {100, 200, 500, 1000};
    double densities[] = {1.0, 0.5, 0.3, 0.2, 0.1, 0.05};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    int num_densities = sizeof(densities) / sizeof(densities[0]);

    for (int s = 0; s < num_sizes; s++) {
        int n = sizes[s];
        int trials = (n <= 200) ? 5 : 3;

        for (int d = 0; d < num_densities; d++) {
            double density = densities[d];

            /* Skip very sparse on small problems (not interesting) */
            if (n <= 100 && density < 0.1) continue;

            /* Allocate arrays - use full n*n for worst case */
            double *dense_cost = malloc(n * n * sizeof(double));
            int max_nnz = n * n;  /* Worst case: fully dense */
            int *row_ptr = malloc((n + 1) * sizeof(int));
            int *col_idx = malloc(max_nnz * sizeof(int));
            double *values = malloc(max_nnz * sizeof(double));

            /* Generate sparse problem with guaranteed feasibility */
            srand(42 + n + (int)(density * 100));
            int nnz = 0;
            row_ptr[0] = 0;

            for (int i = 0; i < n * n; i++) {
                dense_cost[i] = RALPH_LAP_INFINITY;
            }

            for (int i = 0; i < n; i++) {
                /* Always include diagonal for feasibility */
                col_idx[nnz] = i;
                values[nnz] = (rand() % 10000) / 100.0 + 50.0;
                dense_cost[i * n + i] = values[nnz];
                nnz++;

                /* Add random edges based on density */
                for (int j = 0; j < n; j++) {
                    if (j != i && (double)rand() / RAND_MAX < density) {
                        col_idx[nnz] = j;
                        values[nnz] = (rand() % 10000) / 100.0;
                        dense_cost[i * n + j] = values[nnz];
                        nnz++;
                    }
                }
                row_ptr[i + 1] = nnz;
            }

            double actual_density = (double)nnz / (n * n);

            /* Allocate solution arrays */
            int *dense_sol = malloc(n * sizeof(int));
            int *sparse_sol = malloc(n * sizeof(int));
            double dense_cost_val, sparse_cost_val;
            Timer timer;

            /* Benchmark dense solver */
            double dense_total = 0;
            for (int t = 0; t < trials; t++) {
                timer_start(&timer);
                ralph_lap_solve(n, dense_cost, RALPH_LAP_MINIMIZE,
                               dense_sol, NULL, NULL, NULL, &dense_cost_val);
                timer_stop(&timer);
                dense_total += timer.elapsed_ms;
            }
            double dense_avg = dense_total / trials;

            /* Benchmark sparse solver */
            double sparse_total = 0;
            RalphLapStatus sparse_status = RALPH_LAP_SUCCESS;
            for (int t = 0; t < trials; t++) {
                timer_start(&timer);
                sparse_status = ralph_lap_solve_sparse(n, nnz, row_ptr, col_idx, values,
                                                        RALPH_LAP_MINIMIZE,
                                                        sparse_sol, NULL, &sparse_cost_val);
                timer_stop(&timer);
                sparse_total += timer.elapsed_ms;
                if (sparse_status != RALPH_LAP_SUCCESS) break;
            }
            double sparse_avg = sparse_total / trials;

            /* Calculate speedup */
            double speedup = (sparse_status == RALPH_LAP_SUCCESS) ? dense_avg / sparse_avg : 0;

            /* Verify costs match */
            const char *status = "OK";
            if (sparse_status != RALPH_LAP_SUCCESS) {
                status = "FAIL";
            } else if (fabs(dense_cost_val - sparse_cost_val) > 1e-4) {
                status = "MISMATCH";
            }

            printf("  %6d   %5.0f%%    %10.3f    %10.3f    %8.2fx    %s\n",
                   n, actual_density * 100, dense_avg, sparse_avg, speedup, status);

            free(dense_cost);
            free(row_ptr);
            free(col_idx);
            free(values);
            free(dense_sol);
            free(sparse_sol);
        }

        if (s < num_sizes - 1) {
            printf("  ────────────────────────────────────────────────────────────────────────\n");
        }
    }
}

/* ============================================================================
 * Sparse scaling benchmark (native sparse algorithm only)
 * ============================================================================ */

static void bench_sparse_scaling(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Sparse LAP Scaling (Native Sparse JVC Algorithm)                         ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("  %-8s %8s  %10s  %12s  %12s\n",
           "Size", "Density", "nnz", "Time (ms)", "Cost");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    /* Test with 10% density - where sparse shines */
    double density = 0.10;
    int sizes[] = {500, 1000, 2000, 3000, 5000};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int n = sizes[s];

        /* Allocate sparse arrays - use n*(density*n + 2) for safety */
        int edges_per_row = (int)(n * density) + 2;
        int max_nnz = n * edges_per_row;
        int *row_ptr = malloc((n + 1) * sizeof(int));
        int *col_idx = malloc(max_nnz * sizeof(int));
        double *values = malloc(max_nnz * sizeof(double));

        /* Generate sparse problem */
        srand(42 + n);
        int nnz = 0;
        row_ptr[0] = 0;

        for (int i = 0; i < n; i++) {
            /* Always include diagonal */
            col_idx[nnz] = i;
            values[nnz] = (rand() % 10000) / 100.0 + 50.0;
            nnz++;

            /* Add random edges */
            for (int j = 0; j < n; j++) {
                if (j != i && (double)rand() / RAND_MAX < density) {
                    col_idx[nnz] = j;
                    values[nnz] = (rand() % 10000) / 100.0;
                    nnz++;
                }
            }
            row_ptr[i + 1] = nnz;
        }

        int *row_sol = malloc(n * sizeof(int));
        double total_cost;
        Timer timer;

        int trials = (n <= 1000) ? 3 : 1;
        double total_time = 0;
        RalphLapStatus status = RALPH_LAP_SUCCESS;

        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            status = ralph_lap_solve_sparse(n, nnz, row_ptr, col_idx, values,
                                             RALPH_LAP_MINIMIZE,
                                             row_sol, NULL, &total_cost);
            timer_stop(&timer);
            total_time += timer.elapsed_ms;
            if (status != RALPH_LAP_SUCCESS) break;
        }

        double avg_time = total_time / trials;
        double actual_density = (double)nnz / (n * n);

        if (status == RALPH_LAP_SUCCESS) {
            printf("  %6d   %5.1f%%   %9d    %10.2f    %10.2f\n",
                   n, actual_density * 100, nnz, avg_time, total_cost);
        } else {
            printf("  %6d   %5.1f%%   %9d    %10s    %10s\n",
                   n, actual_density * 100, nnz, "FAILED", "N/A");
        }

        free(row_ptr);
        free(col_idx);
        free(values);
        free(row_sol);
    }
}

/* ============================================================================
 * Large problem benchmark (JVC only)
 * ============================================================================ */

static void bench_large_problems(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Large Problem Benchmark (JVC Only)                                       ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("  %-20s %6s  %12s  %12s\n", "Problem", "Size", "Time (ms)", "Cost");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int sizes[] = {500, 750, 1000, 1500, 2000};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < num_sizes; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));
        int *row_sol = malloc(n * sizeof(int));
        double total_cost;

        generate_random(n, cost, 42);

        Timer timer;
        timer_start(&timer);
        RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                                row_sol, NULL, NULL, NULL, &total_cost);
        timer_stop(&timer);

        if (status == RALPH_LAP_SUCCESS) {
            printf("  %-20s %6d  %12.2f  %12.2f\n",
                   "Random", n, timer.elapsed_ms, total_cost);
        } else {
            printf("  %-20s %6d  %12s  %12s\n",
                   "Random", n, "FAILED", "N/A");
        }

        free(cost);
        free(row_sol);
    }
}

/* ============================================================================
 * Verification benchmark
 * ============================================================================ */

static void bench_correctness(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Correctness Verification (JVC vs LP on multiple random instances)        ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");

    int sizes[] = {10, 20, 30, 40};
    int instances_per_size = 20;
    int total = 0;
    int passed = 0;

    for (int s = 0; s < 4; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));
        int *jvc_sol = malloc(n * sizeof(int));
        int *lp_sol = malloc(n * sizeof(int));
        int size_passed = 0;

        for (int inst = 0; inst < instances_per_size; inst++) {
            generate_random(n, cost, 1000 + s * 100 + inst);

            double jvc_cost, lp_cost;
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, jvc_sol, NULL, NULL, NULL, &jvc_cost);
            ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, lp_sol, &lp_cost);

            total++;
            if (fabs(jvc_cost - lp_cost) < 1e-4) {
                passed++;
                size_passed++;
            }
        }

        printf("  Size %3d: %2d/%2d instances matched\n", n, size_passed, instances_per_size);

        free(cost);
        free(jvc_sol);
        free(lp_sol);
    }

    printf("  ────────────────────────────────────────────────────────────────────────\n");
    printf("  Total: %d/%d (%.1f%%)\n", passed, total, 100.0 * passed / total);
}

/* ============================================================================
 * Epsilon scaling benchmark
 * ============================================================================ */

static void bench_epsilon_scaling(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Epsilon Scaling Benchmark (Standard vs ε-Scaling Auction)                ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("  %6s  %10s  %10s  %10s  %8s  %8s\n",
           "Size", "Std (ms)", "Eps (ms)", "Speedup", "Std Cost", "Eps Cost");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int sizes[] = {100, 200, 500, 1000};
    int num_trials = 5;

    for (int s = 0; s < 4; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));
        int *row_sol = malloc(n * sizeof(int));

        double time_std = 0, time_eps = 0;
        double cost_std = 0, cost_eps = 0;

        for (int t = 0; t < num_trials; t++) {
            generate_random(n, cost, 5000 + s * 100 + t);

            /* Standard mode */
            ralph_lap_set_epsilon_scaling(0);
            Timer timer;
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &cost_std);
            timer_stop(&timer);
            time_std += timer.elapsed_ms;

            /* Epsilon scaling mode */
            ralph_lap_set_epsilon_scaling(1);
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &cost_eps);
            timer_stop(&timer);
            time_eps += timer.elapsed_ms;
        }

        ralph_lap_set_epsilon_scaling(0);  /* Reset */

        time_std /= num_trials;
        time_eps /= num_trials;
        double speedup = time_std / time_eps;
        const char *match = (fabs(cost_std - cost_eps) < 0.01) ? "OK" : "DIFF";

        printf("  %6d  %10.3f  %10.3f  %10.2fx  %8.2f  %8.2f  %s\n",
               n, time_std, time_eps, speedup, cost_std, cost_eps, match);

        free(cost);
        free(row_sol);
    }

    /* Test on tied-cost problem (where epsilon helps) */
    printf("\n  Tied-cost problem (all costs equal except small perturbations):\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    for (int s = 0; s < 3; s++) {
        int n = (s == 0) ? 100 : (s == 1) ? 200 : 500;
        double *cost = malloc(n * n * sizeof(double));
        int *row_sol = malloc(n * sizeof(int));

        /* All costs equal (degenerate) */
        for (int i = 0; i < n * n; i++) {
            cost[i] = 100.0;
        }
        /* Small perturbations on diagonal */
        for (int i = 0; i < n; i++) {
            cost[i * n + i] = 99.0 + 0.001 * i;
        }

        double time_std = 0, time_eps = 0;
        double cost_std = 0, cost_eps = 0;

        for (int t = 0; t < num_trials; t++) {
            /* Standard mode */
            ralph_lap_set_epsilon_scaling(0);
            Timer timer;
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &cost_std);
            timer_stop(&timer);
            time_std += timer.elapsed_ms;

            /* Epsilon scaling mode */
            ralph_lap_set_epsilon_scaling(1);
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &cost_eps);
            timer_stop(&timer);
            time_eps += timer.elapsed_ms;
        }

        ralph_lap_set_epsilon_scaling(0);

        time_std /= num_trials;
        time_eps /= num_trials;
        double speedup = time_std / time_eps;

        printf("  %6d  %10.3f  %10.3f  %10.2fx  %8.2f  %8.2f\n",
               n, time_std, time_eps, speedup, cost_std, cost_eps);

        free(cost);
        free(row_sol);
    }
}

/* ============================================================================
 * Warm start benchmark
 * ============================================================================ */

static void bench_warm_start(void) {
    printf("\n╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Warm Start Benchmark (Cold Start vs Warm Start)                          ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("  %6s  %10s  %10s  %10s  %8s\n",
           "Size", "Cold (ms)", "Warm (ms)", "Speedup", "Status");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int sizes[] = {50, 100, 200, 500};
    int num_problems = 10;  /* Solve sequence of similar problems */

    for (int s = 0; s < 4; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));
        int *row_sol = malloc(n * sizeof(int));

        /* Generate base problem */
        srand(42);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 1000) / 10.0;
        }

        RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
        double cold_total = 0, warm_total = 0;
        double cold_cost = 0, warm_cost = 0;
        int all_match = 1;

        /* Cold start benchmark */
        for (int p = 0; p < num_problems; p++) {
            /* Small perturbation (10% of cells) */
            for (int i = 0; i < n * n / 10; i++) {
                int idx = rand() % (n * n);
                cost[idx] = (rand() % 1000) / 10.0;
            }

            Timer timer;
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &cold_cost);
            timer_stop(&timer);
            cold_total += timer.elapsed_ms;
        }

        /* Reset for warm start benchmark */
        srand(42);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 1000) / 10.0;
        }
        ralph_lap_warm_start_clear(ws);

        /* Warm start benchmark */
        for (int p = 0; p < num_problems; p++) {
            /* Same perturbations */
            for (int i = 0; i < n * n / 10; i++) {
                int idx = rand() % (n * n);
                cost[idx] = (rand() % 1000) / 10.0;
            }

            Timer timer;
            timer_start(&timer);
            ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &warm_cost, ws, 1);
            timer_stop(&timer);
            warm_total += timer.elapsed_ms;

            /* Verify optimality against cold start for this specific problem */
            double verify_cost;
            int *verify_sol = malloc(n * sizeof(int));
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, verify_sol, NULL, NULL, NULL, &verify_cost);
            if (fabs(warm_cost - verify_cost) > 1e-4) {
                all_match = 0;
            }
            free(verify_sol);
        }

        double speedup = cold_total / warm_total;
        const char *status = all_match ? "OK" : "MISMATCH";

        printf("  %6d  %10.3f  %10.3f  %10.2fx  %8s\n",
               n, cold_total / num_problems, warm_total / num_problems, speedup, status);

        free(cost);
        free(row_sol);
        ralph_lap_workspace_free(ws);
    }

    /* Test with very similar problems (only 1% perturbation) */
    printf("\n  Very similar problems (1%% perturbation):\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    for (int s = 0; s < 3; s++) {
        int n = (s == 0) ? 100 : (s == 1) ? 200 : 500;
        double *cost = malloc(n * n * sizeof(double));
        int *row_sol = malloc(n * sizeof(int));

        /* Generate base problem */
        srand(123);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 1000) / 10.0;
        }

        RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
        double cold_total = 0, warm_total = 0;

        /* Cold start */
        for (int p = 0; p < num_problems; p++) {
            /* 1% perturbation */
            for (int i = 0; i < n * n / 100; i++) {
                int idx = rand() % (n * n);
                cost[idx] = (rand() % 1000) / 10.0;
            }

            Timer timer;
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL);
            timer_stop(&timer);
            cold_total += timer.elapsed_ms;
        }

        /* Reset */
        srand(123);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 1000) / 10.0;
        }
        ralph_lap_warm_start_clear(ws);

        /* Warm start */
        for (int p = 0; p < num_problems; p++) {
            for (int i = 0; i < n * n / 100; i++) {
                int idx = rand() % (n * n);
                cost[idx] = (rand() % 1000) / 10.0;
            }

            Timer timer;
            timer_start(&timer);
            ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL, ws, 1);
            timer_stop(&timer);
            warm_total += timer.elapsed_ms;
        }

        double speedup = cold_total / warm_total;

        printf("  %6d  %10.3f  %10.3f  %10.2fx\n",
               n, cold_total / num_problems, warm_total / num_problems, speedup);

        free(cost);
        free(row_sol);
        ralph_lap_workspace_free(ws);
    }

    /* Test with identical problems (best case for warm start) */
    printf("\n  Identical problems (best case for warm start):\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    for (int s = 0; s < 3; s++) {
        int n = (s == 0) ? 100 : (s == 1) ? 200 : 500;
        double *cost = malloc(n * n * sizeof(double));
        int *row_sol = malloc(n * sizeof(int));

        /* Generate fixed problem */
        srand(456);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 1000) / 10.0;
        }

        RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
        Timer timer;

        /* Cold start (single solve) */
        timer_start(&timer);
        ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL);
        timer_stop(&timer);
        double cold_time = timer.elapsed_ms;

        /* Warm start (repeated solves) */
        double warm_total = 0;
        for (int p = 0; p < num_problems; p++) {
            timer_start(&timer);
            ralph_lap_solve_warm(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL, ws, 1);
            timer_stop(&timer);
            warm_total += timer.elapsed_ms;
        }
        double warm_avg = warm_total / num_problems;
        double speedup = cold_time / warm_avg;

        printf("  %6d  %10.3f  %10.3f  %10.2fx\n", n, cold_time, warm_avg, speedup);

        free(cost);
        free(row_sol);
        ralph_lap_workspace_free(ws);
    }
}

/* ============================================================================
 * Benchmark: Callback solver (O(n) memory)
 * ============================================================================ */

/* Callback context */
typedef struct {
    int n;
    const double *cost;
} CallbackContext;

static double bench_cost_callback(int i, int j, void *user_data) {
    CallbackContext *ctx = (CallbackContext *)user_data;
    return ctx->cost[i * ctx->n + j];
}

static void bench_callback(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Callback Solver Benchmark (Dense vs Callback)                           ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");
    printf("    Size   Dense (ms)   Callback (ms)     Ratio\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int sizes[] = {50, 100, 200, 500, 1000};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    Timer timer;

    for (int s = 0; s < num_sizes; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));
        int *row_sol = malloc(n * sizeof(int));

        generate_random(n, cost, 12345);
        CallbackContext ctx = { n, cost };

        int trials = (n <= 200) ? 20 : (n <= 500) ? 10 : 5;

        /* Benchmark dense */
        timer_start(&timer);
        for (int t = 0; t < trials; t++) {
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL);
        }
        timer_stop(&timer);
        double dense_time = timer.elapsed_ms / trials;

        /* Benchmark callback */
        timer_start(&timer);
        for (int t = 0; t < trials; t++) {
            ralph_lap_solve_callback(n, bench_cost_callback, &ctx, RALPH_LAP_MINIMIZE,
                                      row_sol, NULL, NULL, NULL, NULL);
        }
        timer_stop(&timer);
        double callback_time = timer.elapsed_ms / trials;

        double ratio = callback_time / dense_time;
        printf("  %6d  %10.3f    %10.3f     %6.2fx\n", n, dense_time, callback_time, ratio);

        free(cost);
        free(row_sol);
    }

    printf("\n  Note: Callback overhead due to function call per cost access.\n");
    printf("        Memory: O(n) for callback vs O(n²) for dense.\n");
}

/* ============================================================================
 * Benchmark: MIP with LAP structure (LAP-based vs Simplex-based LP relaxation)
 * ============================================================================ */

/* Create an assignment MIP: min sum c[i,j]*x[i,j] s.t. assignment constraints */
static RalphModel *create_assignment_mip(int n, const double *cost) {
    RalphModel *model = ralph_test_create();
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add n*n binary variables with costs */
    for (int i = 0; i < n * n; i++) {
        ralph_test_add_var(model, 0.0, 1.0, cost[i], RALPH_BINARY);
    }

    /* Row constraints: sum_j x[i,j] = 1 for each row i */
    int *idx = malloc(n * sizeof(int));
    double *val = malloc(n * sizeof(double));
    for (int j = 0; j < n; j++) val[j] = 1.0;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) idx[j] = i * n + j;
        ralph_test_add_constraint(model, n, idx, val, RALPH_EQUAL, 1.0);
    }

    /* Column constraints: sum_i x[i,j] = 1 for each column j */
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) idx[i] = i * n + j;
        ralph_test_add_constraint(model, n, idx, val, RALPH_EQUAL, 1.0);
    }

    free(idx);
    free(val);
    return model;
}

static void bench_mip_lap(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  MIP with LAP Structure (LAP-based vs Simplex-based LP relaxation)       ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════════╣\n");

    /* Part 1: Full MIP benchmark (both methods solve in 1 node for pure assignments) */
    printf("\n  Part 1: Assignment MIP (both methods solve optimally at root - no branching)\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");
    printf("    Size    LAP-MIP (ms)   Simplex-MIP (ms)   Status    Objective\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int sizes[] = {5, 10, 15, 20, 25};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    Timer timer;

    for (int s = 0; s < num_sizes; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));

        /* Generate random cost matrix */
        srand(42 + n);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 10000) / 100.0;
        }

        int trials = (n <= 15) ? 10 : 5;
        double lap_total = 0, simplex_total = 0;
        double lap_obj = 0, simplex_obj = 0;

        /* Benchmark with LAP-based LP relaxation */
        for (int t = 0; t < trials; t++) {
            RalphModel *model = create_assignment_mip(n, cost);
            ralph_test_set_int_param(model, "detect_special", 1);
            ralph_test_set_int_param(model, "verbose", 0);

            timer_start(&timer);
            ralph_test_optimize(model);
            timer_stop(&timer);

            lap_total += timer.elapsed_ms;
            lap_obj = ralph_test_get_objval(model);
            ralph_test_free(model);
        }

        /* Benchmark with simplex-based LP relaxation */
        for (int t = 0; t < trials; t++) {
            RalphModel *model = create_assignment_mip(n, cost);
            ralph_test_set_int_param(model, "detect_special", 0);
            ralph_test_set_int_param(model, "verbose", 0);

            timer_start(&timer);
            ralph_test_optimize(model);
            timer_stop(&timer);

            simplex_total += timer.elapsed_ms;
            simplex_obj = ralph_test_get_objval(model);
            ralph_test_free(model);
        }

        double lap_avg = lap_total / trials;
        double simplex_avg = simplex_total / trials;

        const char *status = (fabs(lap_obj - simplex_obj) < 1e-4) ? "OK" : "DIFF";

        printf("  %4dx%-4d   %10.3f       %10.3f        %s      %.2f\n",
               n, n, lap_avg, simplex_avg, status, lap_obj);

        free(cost);
    }

    /* Part 2: Pure LAP vs LP relaxation (solving the LP, not MIP) */
    printf("\n  Part 2: LP Relaxation Only (JVC vs Simplex without MIP overhead)\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");
    printf("    Size    JVC (ms)     LP-Simplex (ms)    Speedup    Objectives\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    /* Only test sizes where LP simplex works reliably (n <= 25) */
    int lp_sizes[] = {5, 10, 15, 20, 25};
    int num_lp_sizes = sizeof(lp_sizes) / sizeof(lp_sizes[0]);

    for (int s = 0; s < num_lp_sizes; s++) {
        int n = lp_sizes[s];
        double *cost = malloc(n * n * sizeof(double));

        srand(42 + n);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 10000) / 100.0;
        }

        int *row_sol = malloc(n * sizeof(int));
        int trials = 10;
        double jvc_total = 0, lp_total = 0;
        double jvc_obj = 0, lp_obj = 0;

        /* Benchmark JVC */
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &jvc_obj);
            timer_stop(&timer);
            jvc_total += timer.elapsed_ms;
        }

        /* Benchmark LP */
        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE, row_sol, &lp_obj);
            timer_stop(&timer);
            lp_total += timer.elapsed_ms;
        }

        double jvc_avg = jvc_total / trials;
        double lp_avg = lp_total / trials;
        double speedup = lp_avg / jvc_avg;

        const char *match = (fabs(jvc_obj - lp_obj) < 1e-4) ? "match" : "DIFF";

        printf("  %4dx%-4d   %10.4f      %10.3f       %6.0fx   %.2f (%s)\n",
               n, n, jvc_avg, lp_avg, speedup, jvc_obj, match);

        free(cost);
        free(row_sol);
    }

    /* Part 3: JVC scaling to larger sizes (where LP fails) */
    printf("\n  Part 3: JVC Only (larger sizes where LP simplex has numerical issues)\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");
    printf("    Size    JVC (ms)     Objective\n");
    printf("  ────────────────────────────────────────────────────────────────────────\n");

    int large_sizes[] = {50, 100, 200, 500};
    int num_large = sizeof(large_sizes) / sizeof(large_sizes[0]);

    for (int s = 0; s < num_large; s++) {
        int n = large_sizes[s];
        double *cost = malloc(n * n * sizeof(double));

        srand(42 + n);
        for (int i = 0; i < n * n; i++) {
            cost[i] = (rand() % 10000) / 100.0;
        }

        int *row_sol = malloc(n * sizeof(int));
        int trials = (n <= 200) ? 5 : 3;
        double jvc_total = 0;
        double jvc_obj = 0;

        for (int t = 0; t < trials; t++) {
            timer_start(&timer);
            ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, &jvc_obj);
            timer_stop(&timer);
            jvc_total += timer.elapsed_ms;
        }

        printf("  %4dx%-4d   %10.3f     %.2f\n", n, n, jvc_total / trials, jvc_obj);

        free(cost);
        free(row_sol);
    }

    printf("\n  Note: Assignment LP relaxations are naturally integral (total unimodularity),\n");
    printf("        so MIP solves in 1 node. LAP-based MIP shows benefit when there are\n");
    printf("        additional constraints that break integrality.\n");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                                                                          ║\n");
    printf("║   Ralph LAP Solver Benchmarks                                             ║\n");
    printf("║   JVC (Jonker-Volgenant-Castanon) Algorithm                               ║\n");
    printf("║                                                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════╝\n");

    int run_all = (argc < 2);
    int run_size = run_all || (argc > 1 && strcmp(argv[1], "size") == 0);
    int run_types = run_all || (argc > 1 && strcmp(argv[1], "types") == 0);
    int run_large = run_all || (argc > 1 && strcmp(argv[1], "large") == 0);
    int run_verify = run_all || (argc > 1 && strcmp(argv[1], "verify") == 0);
    int run_sparse = run_all || (argc > 1 && strcmp(argv[1], "sparse") == 0);
    int run_epsilon = run_all || (argc > 1 && strcmp(argv[1], "epsilon") == 0);
    int run_warm = run_all || (argc > 1 && strcmp(argv[1], "warm") == 0);
    int run_callback = run_all || (argc > 1 && strcmp(argv[1], "callback") == 0);
    int run_mip = run_all || (argc > 1 && strcmp(argv[1], "mip") == 0);

    if (run_size) bench_size_scaling();
    if (run_types) bench_problem_types();
    if (run_sparse) {
        bench_sparse_vs_dense();
        bench_sparse_scaling();
    }
    if (run_large) bench_large_problems();
    if (run_epsilon) bench_epsilon_scaling();
    if (run_warm) bench_warm_start();
    if (run_callback) bench_callback();
    if (run_mip) bench_mip_lap();
    if (run_verify) bench_correctness();

    printf("\n");
    return 0;
}
