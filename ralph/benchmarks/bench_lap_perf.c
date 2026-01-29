/*
 * bench_lap_perf.c - Performance benchmarks for LAP solver optimizations
 *
 * Measures JVC solver performance across problem sizes and compares with LP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "lap.h"

/* High-resolution timing */
#ifdef _WIN32
#include <windows.h>
static double get_time_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / freq.QuadPart * 1000.0;
}
#else
#include <sys/time.h>
static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
#endif

/* Problem generators */
static void generate_random(int n, double *cost, unsigned int seed) {
    srand(seed);
    for (int i = 0; i < n * n; i++) {
        cost[i] = (rand() % 10000) / 100.0;
    }
}

static void generate_geometric(int n, double *cost, unsigned int seed) {
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

/* Benchmark runner */
typedef struct {
    double min_ms;
    double max_ms;
    double avg_ms;
    double total_cost;
    int valid;
} BenchResult;

static BenchResult bench_jvc(int n, double *cost, int warmup, int trials) {
    BenchResult result = {1e9, 0, 0, 0, 1};
    int *row_sol = malloc(n * sizeof(int));
    double cost_val;

    /* Warmup */
    for (int i = 0; i < warmup; i++) {
        ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE, row_sol, NULL, NULL, NULL, NULL);
    }

    /* Timed runs */
    double total = 0;
    for (int i = 0; i < trials; i++) {
        double start = get_time_ms();
        RalphLapStatus status = ralph_lap_solve(n, cost, RALPH_LAP_MINIMIZE,
                                                 row_sol, NULL, NULL, NULL, &cost_val);
        double elapsed = get_time_ms() - start;

        if (status != RALPH_LAP_SUCCESS) {
            result.valid = 0;
            break;
        }

        total += elapsed;
        if (elapsed < result.min_ms) result.min_ms = elapsed;
        if (elapsed > result.max_ms) result.max_ms = elapsed;
    }

    result.avg_ms = total / trials;
    result.total_cost = cost_val;

    /* Verify solution */
    if (result.valid) {
        result.valid = ralph_lap_verify(n, cost, row_sol, NULL);
    }

    free(row_sol);
    return result;
}

static BenchResult bench_lp(int n, double *cost, int trials) {
    BenchResult result = {1e9, 0, 0, 0, 1};
    int *row_sol = malloc(n * sizeof(int));
    double cost_val;

    /* LP is slow, fewer trials */
    double total = 0;
    for (int i = 0; i < trials; i++) {
        double start = get_time_ms();
        RalphLapStatus status = ralph_lap_solve_lp(n, cost, RALPH_LAP_MINIMIZE,
                                                    row_sol, &cost_val);
        double elapsed = get_time_ms() - start;

        if (status != RALPH_LAP_SUCCESS) {
            result.valid = 0;
            break;
        }

        total += elapsed;
        if (elapsed < result.min_ms) result.min_ms = elapsed;
        if (elapsed > result.max_ms) result.max_ms = elapsed;
    }

    result.avg_ms = total / trials;
    result.total_cost = cost_val;

    free(row_sol);
    return result;
}

/* Print results */
static void print_header(void) {
    printf("┌────────┬─────────────────────────────────────┬─────────────────────┬──────────┐\n");
    printf("│  Size  │         JVC (ms)                    │      LP (ms)        │ Speedup  │\n");
    printf("│        │   min      avg      max    valid    │   avg      valid    │          │\n");
    printf("├────────┼─────────────────────────────────────┼─────────────────────┼──────────┤\n");
}

static void print_row(int n, BenchResult jvc, BenchResult lp, int skip_lp) {
    if (skip_lp) {
        printf("│ %6d │ %7.3f %8.3f %8.3f   %s   │       N/A           │    N/A   │\n",
               n, jvc.min_ms, jvc.avg_ms, jvc.max_ms, jvc.valid ? "✓" : "✗");
    } else {
        double speedup = lp.avg_ms / jvc.avg_ms;
        printf("│ %6d │ %7.3f %8.3f %8.3f   %s   │ %9.2f   %s   │ %6.1fx  │\n",
               n, jvc.min_ms, jvc.avg_ms, jvc.max_ms, jvc.valid ? "✓" : "✗",
               lp.avg_ms, lp.valid ? "✓" : "✗", speedup);
    }
}

static void print_footer(void) {
    printf("└────────┴─────────────────────────────────────┴─────────────────────┴──────────┘\n");
}

/* Main benchmark */
int main(int argc, char *argv[]) {
    int quick_mode = (argc > 1 && strcmp(argv[1], "--quick") == 0);

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                     Ralph LAP Performance Benchmark                            ║\n");
    printf("║                     JVC Algorithm vs LP Solver                                 ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");

    /* Size scaling benchmark */
    printf("=== Random Cost Matrix (seed=42) ===\n\n");
    print_header();

    int sizes[] = {50, 100, 200, 300, 500, 750, 1000, 1500, 2000};
    int num_sizes = quick_mode ? 5 : 9;
    int lp_max_size = 50;  /* LP is too slow beyond this */

    for (int s = 0; s < num_sizes; s++) {
        int n = sizes[s];
        double *cost = malloc(n * n * sizeof(double));
        generate_random(n, cost, 42);

        int warmup = (n <= 500) ? 3 : 1;
        int trials = (n <= 200) ? 10 : (n <= 500) ? 5 : 3;

        BenchResult jvc = bench_jvc(n, cost, warmup, trials);

        int skip_lp = (n > lp_max_size);
        BenchResult lp = {0};
        if (!skip_lp) {
            lp = bench_lp(n, cost, 3);
        }

        print_row(n, jvc, lp, skip_lp);

        free(cost);
    }

    print_footer();

    /* Problem type comparison */
    printf("\n=== Problem Type Comparison (n=200) ===\n\n");
    printf("┌──────────────────┬─────────────────────────────────────┬──────────┐\n");
    printf("│  Problem Type    │         JVC (ms)                    │  Cost    │\n");
    printf("│                  │   min      avg      max    valid    │          │\n");
    printf("├──────────────────┼─────────────────────────────────────┼──────────┤\n");

    int n = 200;
    double *cost = malloc(n * n * sizeof(double));

    /* Random */
    generate_random(n, cost, 123);
    BenchResult r = bench_jvc(n, cost, 3, 10);
    printf("│ Random           │ %7.3f %8.3f %8.3f   %s   │ %8.2f │\n",
           r.min_ms, r.avg_ms, r.max_ms, r.valid ? "✓" : "✗", r.total_cost);

    /* Geometric */
    generate_geometric(n, cost, 123);
    r = bench_jvc(n, cost, 3, 10);
    printf("│ Geometric        │ %7.3f %8.3f %8.3f   %s   │ %8.2f │\n",
           r.min_ms, r.avg_ms, r.max_ms, r.valid ? "✓" : "✗", r.total_cost);

    printf("└──────────────────┴─────────────────────────────────────┴──────────┘\n");

    free(cost);

    /* Large problem scaling */
    if (!quick_mode) {
        printf("\n=== Large Problem Scaling (JVC only) ===\n\n");
        printf("┌────────┬─────────────────────────────────────┬──────────────┐\n");
        printf("│  Size  │         JVC (ms)                    │  Throughput  │\n");
        printf("│   n    │   min      avg      max    valid    │  (Mops/sec)  │\n");
        printf("├────────┼─────────────────────────────────────┼──────────────┤\n");

        int large_sizes[] = {1000, 2000, 3000, 4000, 5000};
        for (int s = 0; s < 5; s++) {
            int n = large_sizes[s];
            double *cost = malloc(n * n * sizeof(double));
            generate_random(n, cost, 42);

            BenchResult r = bench_jvc(n, cost, 1, 3);
            double ops = (double)n * n * n;  /* O(n³) operations */
            double mops = ops / (r.avg_ms * 1000.0);  /* Million ops per second */

            printf("│ %6d │ %7.2f %8.2f %8.2f   %s   │ %10.1f   │\n",
                   n, r.min_ms, r.avg_ms, r.max_ms, r.valid ? "✓" : "✗", mops);

            free(cost);
        }

        printf("└────────┴─────────────────────────────────────┴──────────────┘\n");
    }

    printf("\n");
    return 0;
}
