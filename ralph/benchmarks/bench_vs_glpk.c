/*
 * Ralph vs GLPK Benchmark
 *
 * Compares Ralph LP solver performance against GLPK on identical problems.
 * Both solvers solve the same randomly generated LPs and measure:
 * - Total solve time
 * - Iteration count
 * - Per-iteration cost
 * - Solution quality (objective value match)
 *
 * Build:
 *   make bench-glpk
 *
 * Run:
 *   ./bench_vs_glpk [--quick] [--size N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* Ralph headers */
#include "ralph_test_mod_api.h"

static int glpsol_available(void) {
    int rc = system("which glpsol >/dev/null 2>&1");
    return rc == 0 ? 1 : 0;
}

/* ============================================================================
 * Random number generator (deterministic for reproducibility)
 * ============================================================================ */

static unsigned int g_seed = 42;

static void seed_random(unsigned int seed) {
    g_seed = seed;
}

static double rand_double(double min, double max) {
    g_seed = g_seed * 1103515245 + 12345;
    double r = (double)(g_seed % 100000) / 100000.0;
    return min + r * (max - min);
}

static int rand_int(int min, int max) {
    g_seed = g_seed * 1103515245 + 12345;
    return min + (g_seed % (max - min + 1));
}

/* ============================================================================
 * Problem generation
 * ============================================================================ */

typedef struct {
    int num_vars;
    int num_cons;
    double density;

    /* Problem data (stored for both solvers) */
    double *obj;        /* Objective coefficients [num_vars] */
    double *lb;         /* Variable lower bounds [num_vars] */
    double *ub;         /* Variable upper bounds [num_vars] */

    /* Constraints in triplet format */
    int *con_row;       /* Row indices */
    int *con_col;       /* Column indices */
    double *con_val;    /* Values */
    int nnz;            /* Total non-zeros */

    double *rhs;        /* RHS values [num_cons] */
    char *sense;        /* Constraint sense [num_cons] */
} TestProblem;

static TestProblem* generate_problem(int num_vars, int num_cons, double density,
                                      unsigned int seed) {
    seed_random(seed);

    TestProblem *prob = (TestProblem*)calloc(1, sizeof(TestProblem));
    if (!prob) return NULL;

    prob->num_vars = num_vars;
    prob->num_cons = num_cons;
    prob->density = density;

    /* Allocate arrays */
    prob->obj = (double*)malloc(num_vars * sizeof(double));
    prob->lb = (double*)malloc(num_vars * sizeof(double));
    prob->ub = (double*)malloc(num_vars * sizeof(double));
    prob->rhs = (double*)malloc(num_cons * sizeof(double));
    prob->sense = (char*)malloc(num_cons * sizeof(char));

    /* Estimate max non-zeros */
    int max_nnz = (int)(num_vars * num_cons * density * 1.5) + num_cons;
    prob->con_row = (int*)malloc(max_nnz * sizeof(int));
    prob->con_col = (int*)malloc(max_nnz * sizeof(int));
    prob->con_val = (double*)malloc(max_nnz * sizeof(double));

    if (!prob->obj || !prob->lb || !prob->ub || !prob->rhs || !prob->sense ||
        !prob->con_row || !prob->con_col || !prob->con_val) {
        /* Cleanup on failure */
        free(prob->obj); free(prob->lb); free(prob->ub);
        free(prob->rhs); free(prob->sense);
        free(prob->con_row); free(prob->con_col); free(prob->con_val);
        free(prob);
        return NULL;
    }

    /* Generate objective and bounds
     * Use NEGATIVE coefficients for minimization (so x=0 is NOT optimal)
     * This ensures the solver actually needs to do iterations */
    for (int j = 0; j < num_vars; j++) {
        prob->obj[j] = rand_double(-10.0, -1.0);  /* Negative costs: minimize = maximize negative */
        prob->lb[j] = 0.0;
        prob->ub[j] = 100.0;  /* Bounded variables ensure feasibility */
    }

    /* Generate constraints with positive coefficients
     * This ensures x=0 is always feasible, and the problem is bounded */
    prob->nnz = 0;
    for (int i = 0; i < num_cons; i++) {
        int row_nnz = 0;
        double row_sum = 0.0;

        for (int j = 0; j < num_vars; j++) {
            if (rand_double(0, 1) < density) {
                prob->con_row[prob->nnz] = i;
                prob->con_col[prob->nnz] = j;
                double val = rand_double(1.0, 5.0);  /* Positive coefficients */
                prob->con_val[prob->nnz] = val;
                row_sum += val * 50.0;  /* Contribution if x[j] = 50 (midpoint) */
                prob->nnz++;
                row_nnz++;
            }
        }

        /* Ensure at least one non-zero per row */
        if (row_nnz == 0) {
            int j = rand_int(0, num_vars - 1);
            prob->con_row[prob->nnz] = i;
            prob->con_col[prob->nnz] = j;
            double val = rand_double(1.0, 5.0);
            prob->con_val[prob->nnz] = val;
            row_sum = val * 50.0;
            prob->nnz++;
        }

        /* RHS: ensure problem has non-trivial feasible region
         * Allow roughly half the variables to be at their midpoint */
        prob->rhs[i] = row_sum * rand_double(0.5, 1.5);
        prob->sense[i] = 'L';  /* All <= constraints */
    }

    return prob;
}

static void free_problem(TestProblem *prob) {
    if (!prob) return;
    free(prob->obj);
    free(prob->lb);
    free(prob->ub);
    free(prob->rhs);
    free(prob->sense);
    free(prob->con_row);
    free(prob->con_col);
    free(prob->con_val);
    free(prob);
}

/* ============================================================================
 * Ralph solver
 * ============================================================================ */

typedef struct {
    double solve_time;
    int iterations;
    double objective;
    int status;  /* 0 = optimal, 1 = infeasible, 2 = unbounded, 3 = error */
} SolveResult;

static RalphModel* build_ralph_model_from_problem(const TestProblem *prob) {
    RalphModel *model = ralph_test_create();
    int *row_start = NULL;
    int *row_idx = NULL;
    double *row_val = NULL;
    int *row_pos = NULL;
    if (!model) return NULL;

    /* Add variables */
    for (int j = 0; j < prob->num_vars; j++) {
        ralph_test_add_var(model, prob->lb[j], prob->ub[j], prob->obj[j], 'C');
    }

    /* Build constraint arrays per row */
    row_start = (int*)calloc((size_t)prob->num_cons + 1, sizeof(int));
    row_idx = (int*)malloc((size_t)prob->nnz * sizeof(int));
    row_val = (double*)malloc((size_t)prob->nnz * sizeof(double));
    row_pos = (int*)malloc((size_t)prob->num_cons * sizeof(int));
    if (!row_start || !row_idx || !row_val || !row_pos) {
        free(row_start);
        free(row_idx);
        free(row_val);
        free(row_pos);
        ralph_test_free(model);
        return NULL;
    }

    for (int k = 0; k < prob->nnz; k++) {
        row_start[prob->con_row[k] + 1]++;
    }
    for (int i = 0; i < prob->num_cons; i++) {
        row_start[i + 1] += row_start[i];
    }

    memcpy(row_pos, row_start, (size_t)prob->num_cons * sizeof(int));

    for (int k = 0; k < prob->nnz; k++) {
        int i = prob->con_row[k];
        int pos = row_pos[i]++;
        row_idx[pos] = prob->con_col[k];
        row_val[pos] = prob->con_val[k];
    }

    /* Add constraints */
    for (int i = 0; i < prob->num_cons; i++) {
        int nnz = row_start[i + 1] - row_start[i];
        ralph_test_add_constraint(model, nnz, &row_idx[row_start[i]],
                            &row_val[row_start[i]], prob->sense[i], prob->rhs[i]);
    }

    free(row_start);
    free(row_idx);
    free(row_val);
    free(row_pos);
    return model;
}

static void map_ralph_status(SolveResult *result, RalphStatus status) {
    if (!result) return;
    switch (status) {
        case RALPH_STATUS_OPTIMAL:
        case RALPH_STATUS_IMPRECISE:
        case RALPH_STATUS_OBJ_LIMIT:
            result->status = 0;
            break;
        case RALPH_STATUS_INFEASIBLE:
            result->status = 1;
            break;
        case RALPH_STATUS_UNBOUNDED:
        case RALPH_STATUS_INF_OR_UNBD:
            result->status = 2;
            break;
        default:
            result->status = 3;
            break;
    }
}

static SolveResult solve_with_ralph(TestProblem *prob) {
    SolveResult result = {0};
    RalphModel *model = build_ralph_model_from_problem(prob);
    if (!model) {
        result.status = 3;
        return result;
    }

    ralph_set_int_param_id(model, RALPH_PARAM_VERBOSE, 0);
    ralph_set_int_param_id(model, RALPH_PARAM_MAX_ITERATIONS, 100000);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX);

    /* Solve and time */
    clock_t start = clock();
    (void)ralph_test_optimize_lp(model);
    clock_t end = clock();

    result.solve_time = (double)(end - start) / CLOCKS_PER_SEC;
    result.iterations = ralph_test_get_iterations(model);
    result.objective = ralph_test_get_objval(model);
    map_ralph_status(&result, ralph_test_get_status(model));

    ralph_test_free(model);
    return result;
}

/* ============================================================================
 * GLPK solver
 * ============================================================================ */

static SolveResult solve_with_glpk(TestProblem *prob) {
    SolveResult result = {0};
    RalphModel *model = build_ralph_model_from_problem(prob);
    if (!model) {
        result.status = 3;
        return result;
    }

    ralph_set_int_param_id(model, RALPH_PARAM_VERBOSE, 0);
    ralph_set_int_param_id(model, RALPH_PARAM_MAX_ITERATIONS, 100000);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                           (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1);
    ralph_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                           (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL);

    /* Solve and time */
    clock_t start = clock();
    (void)ralph_test_optimize_lp(model);
    clock_t end = clock();

    result.solve_time = (double)(end - start) / CLOCKS_PER_SEC;
    result.iterations = ralph_test_get_iterations(model);
    result.objective = ralph_test_get_objval(model);
    map_ralph_status(&result, ralph_test_get_status(model));
    ralph_test_free(model);
    return result;
}

/* ============================================================================
 * Benchmark runner
 * ============================================================================ */

typedef struct {
    int num_vars;
    int num_cons;
    double density;
    int num_trials;
} BenchConfig;

static void run_benchmark(BenchConfig config) {
    printf("\n");
    printf("================================================================================\n");
    printf("  Problem: %d vars x %d cons, %.0f%% density, %d trials\n",
           config.num_vars, config.num_cons, config.density * 100, config.num_trials);
    printf("================================================================================\n\n");

    double ralph_total_time = 0, glpk_total_time = 0;
    int ralph_total_iters = 0, glpk_total_iters = 0;
    int ralph_optimal = 0, glpk_optimal = 0;
    double obj_diff_sum = 0;
    int comparable = 0;

    printf("  %-8s  %-12s %-8s %-12s %-8s %-12s\n",
           "Trial", "Ralph(s)", "Iters", "GLPK(s)", "Iters", "Obj Match");
    printf("  %-8s  %-12s %-8s %-12s %-8s %-12s\n",
           "-----", "--------", "-----", "-------", "-----", "---------");

    for (int t = 0; t < config.num_trials; t++) {
        unsigned int seed = 42 + t * 1000;

        TestProblem *prob = generate_problem(config.num_vars, config.num_cons,
                                             config.density, seed);
        if (!prob) {
            printf("  Trial %d: Failed to generate problem\n", t + 1);
            continue;
        }

        SolveResult ralph_result = solve_with_ralph(prob);
        SolveResult glpk_result = solve_with_glpk(prob);

        /* Accumulate stats */
        ralph_total_time += ralph_result.solve_time;
        glpk_total_time += glpk_result.solve_time;
        ralph_total_iters += ralph_result.iterations;
        glpk_total_iters += glpk_result.iterations;

        if (ralph_result.status == 0) ralph_optimal++;
        if (glpk_result.status == 0) glpk_optimal++;

        /* Check objective match */
        const char *match_str = "-";
        char match_buf[32];
        if (ralph_result.status == 0 && glpk_result.status == 0) {
            double diff = fabs(ralph_result.objective - glpk_result.objective);
            double scale = fmax(1.0, fabs(glpk_result.objective));
            if (diff / scale < 1e-4) {
                match_str = "OK";
            } else {
                snprintf(match_buf, sizeof(match_buf), "DIFF(%.1f)", diff);
                match_str = match_buf;
                obj_diff_sum += diff;
            }
            comparable++;
        } else {
            /* Show status codes if not both optimal */
            snprintf(match_buf, sizeof(match_buf), "R%d/G%d",
                     ralph_result.status, glpk_result.status);
            match_str = match_buf;
        }

        printf("  %-8d  %-12.4f %-8d %-12.4f %-8d %-12s\n",
               t + 1, ralph_result.solve_time, ralph_result.iterations,
               glpk_result.solve_time, glpk_result.iterations, match_str);

        free_problem(prob);
    }

    /* Summary statistics */
    printf("\n");
    printf("  Summary:\n");
    printf("  -------\n");

    double ralph_avg_time = ralph_total_time / config.num_trials;
    double glpk_avg_time = glpk_total_time / config.num_trials;
    double ralph_avg_iters = (double)ralph_total_iters / config.num_trials;
    double glpk_avg_iters = (double)glpk_total_iters / config.num_trials;

    printf("  Ralph:  Avg time = %.4fs, Avg iters = %.0f, Optimal = %d/%d\n",
           ralph_avg_time, ralph_avg_iters, ralph_optimal, config.num_trials);
    printf("  GLPK:   Avg time = %.4fs, Avg iters = %.0f, Optimal = %d/%d\n",
           glpk_avg_time, glpk_avg_iters, glpk_optimal, config.num_trials);

    if (glpk_avg_time > 0) {
        printf("\n  Speedup: GLPK is %.1fx faster than Ralph\n",
               ralph_avg_time / glpk_avg_time);
    }

    if (ralph_avg_iters > 0 && glpk_avg_iters > 0) {
        double ralph_per_iter = (ralph_avg_time / ralph_avg_iters) * 1000.0;
        double glpk_per_iter = (glpk_avg_time / glpk_avg_iters) * 1000.0;
        printf("  Per-iteration: Ralph = %.3f ms, GLPK = %.3f ms (%.1fx)\n",
               ralph_per_iter, glpk_per_iter, ralph_per_iter / glpk_per_iter);
    }

    if (ralph_avg_iters > 0 && glpk_avg_iters > 0) {
        printf("  Iteration ratio: Ralph/GLPK = %.2f\n",
               ralph_avg_iters / glpk_avg_iters);
    }

    if (comparable > 0 && obj_diff_sum > 0) {
        printf("  Objective match issues in %d trials\n",
               (int)(obj_diff_sum > 0 ? 1 : 0));
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_header(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                    Ralph vs GLPK Benchmark Suite                             ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    printf("Comparing Ralph %s against GLPK (out-of-process adapter via glpsol)\n",
           ralph_test_version());
    printf("\n");
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  --quick       Run quick benchmarks (smaller sizes)\n");
    printf("  --size N      Run only size N (e.g., --size 500)\n");
    printf("  --all         Run all sizes including very large\n");
    printf("  --help        Show this help\n");
}

int main(int argc, char **argv) {
    print_header();

    if (!glpsol_available()) {
        fprintf(stderr, "Error: glpsol not found in PATH\n");
        return 1;
    }
    ralph_unregister_all_lp_external_adapters();
    if (ralph_register_lp_external_glpk_oop(NULL) != 0) {
        fprintf(stderr, "Error: failed to register GLPK out-of-process adapter\n");
        return 1;
    }

    /* Parse command line */
    int quick_mode = 0;
    int single_size = 0;
    int all_sizes = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            quick_mode = 1;
        } else if (strcmp(argv[i], "--all") == 0) {
            all_sizes = 1;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            single_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Define benchmark configurations */
    BenchConfig configs[] = {
        /* Small problems */
        { 50,   25,  0.30, 5 },
        { 100,  50,  0.25, 5 },

        /* Medium problems */
        { 200,  100, 0.20, 5 },
        { 500,  250, 0.15, 3 },

        /* Large problems */
        { 1000, 500, 0.10, 2 },

        /* Very large problems */
        { 2000, 1000, 0.05, 1 },
    };
    int num_configs = sizeof(configs) / sizeof(configs[0]);

    if (quick_mode) {
        num_configs = 2;  /* Only small problems */
    }

    if (!all_sizes && !quick_mode) {
        num_configs = 5;  /* Skip very large */
    }

    /* Run benchmarks */
    for (int i = 0; i < num_configs; i++) {
        if (single_size > 0 && configs[i].num_vars != single_size) {
            continue;
        }
        run_benchmark(configs[i]);
    }

    /* Final summary */
    printf("\n");
    printf("================================================================================\n");
    printf("  Benchmark Complete\n");
    printf("================================================================================\n\n");

    ralph_unregister_all_lp_external_adapters();
    return 0;
}
