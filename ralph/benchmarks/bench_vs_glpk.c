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
#include "ralph.h"

/* GLPK header */
#include <glpk.h>

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

static SolveResult solve_with_ralph(TestProblem *prob) {
    SolveResult result = {0};

    RalphModel *model = ralph_create();
    if (!model) {
        result.status = 3;
        return result;
    }

    /* Suppress output */
    ralph_set_int_param(model, "verbose", 0);
    ralph_set_int_param(model, "max_iterations", 100000);

    /* Add variables */
    for (int j = 0; j < prob->num_vars; j++) {
        ralph_add_var(model, prob->lb[j], prob->ub[j], prob->obj[j], 'C');
    }

    /* Build constraint arrays per row */
    int *row_start = (int*)calloc(prob->num_cons + 1, sizeof(int));
    for (int k = 0; k < prob->nnz; k++) {
        row_start[prob->con_row[k] + 1]++;
    }
    for (int i = 0; i < prob->num_cons; i++) {
        row_start[i + 1] += row_start[i];
    }

    int *row_idx = (int*)malloc(prob->nnz * sizeof(int));
    double *row_val = (double*)malloc(prob->nnz * sizeof(double));
    int *row_pos = (int*)malloc(prob->num_cons * sizeof(int));
    memcpy(row_pos, row_start, prob->num_cons * sizeof(int));

    for (int k = 0; k < prob->nnz; k++) {
        int i = prob->con_row[k];
        int pos = row_pos[i]++;
        row_idx[pos] = prob->con_col[k];
        row_val[pos] = prob->con_val[k];
    }

    /* Add constraints */
    for (int i = 0; i < prob->num_cons; i++) {
        int nnz = row_start[i + 1] - row_start[i];
        ralph_add_constraint(model, nnz, &row_idx[row_start[i]],
                            &row_val[row_start[i]], prob->sense[i], prob->rhs[i]);
    }

    free(row_start);
    free(row_idx);
    free(row_val);
    free(row_pos);

    /* Solve and time */
    clock_t start = clock();
    int status = ralph_optimize(model);
    clock_t end = clock();

    result.solve_time = (double)(end - start) / CLOCKS_PER_SEC;

    /* Get results */
    result.iterations = ralph_get_iterations(model);
    result.objective = ralph_get_objval(model);

    /* Map Ralph status codes:
     * RALPH_STATUS_OPTIMAL = 1
     * RALPH_STATUS_INFEASIBLE = 2
     * RALPH_STATUS_UNBOUNDED = 3
     */
    if (status == RALPH_STATUS_OPTIMAL) {
        result.status = 0;  /* Optimal */
    } else if (status == RALPH_STATUS_INFEASIBLE) {
        result.status = 1;  /* Infeasible */
    } else if (status == RALPH_STATUS_UNBOUNDED) {
        result.status = 2;  /* Unbounded */
    } else {
        result.status = status;  /* Keep original for debugging */
    }

    ralph_free(model);
    return result;
}

/* ============================================================================
 * GLPK solver
 * ============================================================================ */

static SolveResult solve_with_glpk(TestProblem *prob) {
    SolveResult result = {0};

    glp_prob *lp = glp_create_prob();
    if (!lp) {
        result.status = 3;
        return result;
    }

    glp_set_obj_dir(lp, GLP_MIN);

    /* Add rows (constraints) */
    glp_add_rows(lp, prob->num_cons);
    for (int i = 0; i < prob->num_cons; i++) {
        if (prob->sense[i] == 'L') {
            glp_set_row_bnds(lp, i + 1, GLP_UP, 0.0, prob->rhs[i]);
        } else if (prob->sense[i] == 'G') {
            glp_set_row_bnds(lp, i + 1, GLP_LO, prob->rhs[i], 0.0);
        } else {
            glp_set_row_bnds(lp, i + 1, GLP_FX, prob->rhs[i], prob->rhs[i]);
        }
    }

    /* Add columns (variables) */
    glp_add_cols(lp, prob->num_vars);
    for (int j = 0; j < prob->num_vars; j++) {
        /* Set double bounds: lb <= x <= ub */
        glp_set_col_bnds(lp, j + 1, GLP_DB, prob->lb[j], prob->ub[j]);
        glp_set_obj_coef(lp, j + 1, prob->obj[j]);
    }

    /* Load constraint matrix (GLPK uses 1-based indexing) */
    int *ia = (int*)malloc((prob->nnz + 1) * sizeof(int));
    int *ja = (int*)malloc((prob->nnz + 1) * sizeof(int));
    double *ar = (double*)malloc((prob->nnz + 1) * sizeof(double));

    for (int k = 0; k < prob->nnz; k++) {
        ia[k + 1] = prob->con_row[k] + 1;
        ja[k + 1] = prob->con_col[k] + 1;
        ar[k + 1] = prob->con_val[k];
    }

    glp_load_matrix(lp, prob->nnz, ia, ja, ar);

    free(ia);
    free(ja);
    free(ar);

    /* Set solver parameters */
    glp_smcp parm;
    glp_init_smcp(&parm);
    parm.msg_lev = GLP_MSG_OFF;  /* Suppress output */
    parm.meth = GLP_PRIMAL;      /* Use primal simplex for fair comparison */
    parm.pricing = GLP_PT_STD;   /* Standard pricing (Dantzig) */
    parm.it_lim = 100000;

    /* Solve and time */
    clock_t start = clock();
    (void)glp_simplex(lp, &parm);
    clock_t end = clock();

    result.solve_time = (double)(end - start) / CLOCKS_PER_SEC;

    /* Get results */
    result.iterations = glp_get_it_cnt(lp);

    int glp_status = glp_get_status(lp);
    if (glp_status == GLP_OPT) {
        result.status = 0;
        result.objective = glp_get_obj_val(lp);
    } else if (glp_status == GLP_INFEAS || glp_status == GLP_NOFEAS) {
        result.status = 1;
    } else if (glp_status == GLP_UNBND) {
        result.status = 2;
    } else {
        result.status = 3;
    }

    glp_delete_prob(lp);
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
    printf("Comparing Ralph %s against GLPK %d.%d\n",
           ralph_version(), GLP_MAJOR_VERSION, GLP_MINOR_VERSION);
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

    return 0;
}
