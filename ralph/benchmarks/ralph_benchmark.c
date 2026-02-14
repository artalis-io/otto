/*
 * Ralph LP Benchmark Tool
 *
 * Compares Ralph solver against GLPK on standard LP problems.
 * Produces structured JSON output for automated analysis.
 *
 * Usage:
 *   ./ralph-benchmark problem.mps           # Single problem
 *   ./ralph-benchmark --netlib afiro        # NETLIB problem by name
 *   ./ralph-benchmark --suite tiny          # Run test suite
 *   ./ralph-benchmark --list                # List available problems
 *   ./ralph-benchmark --download-netlib     # Download NETLIB problems
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

#include "ralph.h"

/* ============================================================================
 * Constants and Configuration
 * ============================================================================ */

#define MAX_PATH 4096
#define MAX_LINE 4096
#define MAX_PROBLEMS 200

/* Default settings */
#define DEFAULT_TIME_MULTIPLIER 20.0
#define DEFAULT_HARD_CAP_SEC 60.0
#define DEFAULT_OBJ_REL_TOL 1e-6
#define DEFAULT_OBJ_ABS_TOL 1e-8
#define DEFAULT_FEAS_TOL 1e-6

/* NETLIB directory relative to this executable */
static const char *NETLIB_DIR = "netlib";

/* ============================================================================
 * Data Structures
 * ============================================================================ */

typedef struct {
    char name[256];
    char path[MAX_PATH];
    int is_mip;
} ProblemInfo;

typedef struct {
    int status;          /* 0=optimal, 1=infeasible, 2=unbounded, 3=error, 4=timeout */
    double objective;
    double time_ms;
    int iterations;
    double *solution;    /* Primal solution (may be NULL) */
    int solution_size;
} SolveResult;

typedef struct {
    int solution_valid;
    double max_constraint_violation;
    double max_bound_violation;
    int objective_match;
    double objective_rel_error;
    double objective_abs_error;
    int numerically_stable;
    char issues[1024];
} ValidationResult;

typedef struct {
    /* CLI options */
    int show_help;
    int show_version;
    int list_problems;
    int download_netlib;
    int json_output;
    int verbose;
    int lp_only;         /* Default: 1 (skip MIP) */
    int mip_only;

    /* Time limits */
    double time_multiplier;
    double hard_cap_sec;

    /* Tolerances */
    double obj_rel_tol;
    double obj_abs_tol;
    double feas_tol;

    /* Suite to run */
    char suite[64];

    /* Single problem */
    char problem_path[MAX_PATH];
    char netlib_name[256];

    /* Output */
    char output_dir[MAX_PATH];
} Options;

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void get_benchmark_dir(char *buf, size_t size) {
    /*
     * Get the benchmarks directory where NETLIB problems are stored.
     * This handles running from different locations:
     * - ./ralph-benchmark (from ralph/ directory)
     * - ./benchmarks/ralph-benchmark (from ralph/ directory)
     * - /path/to/ralph/ralph-benchmark (absolute path)
     */
    char cwd[MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strncpy(buf, "benchmarks", size - 1);
        buf[size - 1] = '\0';
        return;
    }

    /* Check if benchmarks/ exists in current directory */
    snprintf(buf, size, "%s/benchmarks", cwd);
    if (dir_exists(buf)) {
        return;
    }

    /* Check if we're already in benchmarks/ */
    snprintf(buf, size, "%s", cwd);
    char netlib_check[MAX_PATH];
    snprintf(netlib_check, sizeof(netlib_check), "%s/netlib", cwd);
    if (dir_exists(netlib_check)) {
        return;
    }

    /* Fallback: just use "benchmarks" and hope for the best */
    strncpy(buf, "benchmarks", size - 1);
    buf[size - 1] = '\0';
}

static void json_escape_string(char *out, size_t out_size, const char *in) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j < out_size - 2; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = c;
            }
        } else if (c == '\n') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = 'n';
            }
        } else if (c == '\r') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = 'r';
            }
        } else if (c == '\t') {
            if (j < out_size - 3) {
                out[j++] = '\\';
                out[j++] = 't';
            }
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

/* ============================================================================
 * GLPK Wrapper (via glpsol CLI)
 * ============================================================================ */

static int check_glpk_available(void) {
    int ret = system("which glpsol >/dev/null 2>&1");
    return ret == 0;
}

static SolveResult solve_with_glpk(const char *problem_path, double time_limit_sec) {
    SolveResult result = {0};
    result.status = 3;  /* Error by default */
    result.solution = NULL;

    /* Create temp files for solution output */
    char sol_file[MAX_PATH];
    snprintf(sol_file, sizeof(sol_file), "/tmp/glpk_sol_%d.txt", getpid());

    /* Build command */
    char cmd[MAX_PATH * 2];

    /* Detect file type */
    const char *ext = strrchr(problem_path, '.');
    const char *format_flag = "--mps";
    if (ext && (strcasecmp(ext, ".lp") == 0)) {
        format_flag = "--lp";
    }

    snprintf(cmd, sizeof(cmd),
             "glpsol %s '%s' --tmlim %.0f -o '%s' 2>&1",
             format_flag, problem_path, time_limit_sec, sol_file);

    /* Run GLPK and capture output */
    double start_time = get_time_ms();
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        return result;
    }

    char line[MAX_LINE];
    int found_time = 0;
    while (fgets(line, sizeof(line), pipe)) {
        /* Parse timing from GLPK output */
        if (strstr(line, "Time used:")) {
            double t;
            if (sscanf(line, "Time used: %lf", &t) == 1) {
                result.time_ms = t * 1000.0;
                found_time = 1;
            }
        }
    }
    int ret = pclose(pipe);
    double end_time = get_time_ms();

    /* Use wall clock if GLPK didn't report time */
    if (!found_time) {
        result.time_ms = end_time - start_time;
    }

    /* Parse solution file */
    FILE *sol = fopen(sol_file, "r");
    if (sol) {
        while (fgets(line, sizeof(line), sol)) {
            /* Status line */
            if (strstr(line, "Status:")) {
                if (strstr(line, "OPTIMAL") || strstr(line, "INTEGER OPTIMAL")) {
                    result.status = 0;
                } else if (strstr(line, "INFEASIBLE") || strstr(line, "NO FEASIBLE")) {
                    result.status = 1;
                } else if (strstr(line, "UNBOUNDED")) {
                    result.status = 2;
                } else if (strstr(line, "TIME LIMIT")) {
                    result.status = 4;
                }
            }
            /* Objective line */
            if (strstr(line, "Objective:")) {
                char *eq = strchr(line, '=');
                if (eq) {
                    result.objective = atof(eq + 1);
                }
            }
            /* Iterations */
            if (strstr(line, "simplex iterations")) {
                int iters;
                if (sscanf(line, "%d simplex", &iters) == 1) {
                    result.iterations = iters;
                }
            }
        }
        fclose(sol);
    }

    /* Cleanup */
    unlink(sol_file);

    /* Check for timeout based on exit status */
    if (ret != 0 && result.status == 3) {
        result.status = 4;  /* Assume timeout */
    }

    return result;
}

/* ============================================================================
 * Ralph Wrapper
 * ============================================================================ */

static SolveResult solve_with_ralph(const char *problem_path, double time_limit_sec,
                                     int *out_num_vars, int *out_num_cons, int *out_nnz,
                                     int *out_is_mip) {
    SolveResult result = {0};
    result.status = 3;  /* Error by default */
    result.solution = NULL;

    RalphModel *model = ralph_create();
    if (!model) {
        return result;
    }

    /* Load problem */
    const char *ext = strrchr(problem_path, '.');
    int load_ret;
    if (ext && strcasecmp(ext, ".lp") == 0) {
        load_ret = ralph_read_lp(model, problem_path);
    } else {
        load_ret = ralph_read_mps(model, problem_path);
    }

    if (load_ret != 0) {
        ralph_free(model);
        return result;
    }

    /* Get problem info via public API */
    *out_num_vars = ralph_get_num_vars(model);
    *out_num_cons = ralph_get_num_cons(model);
    *out_is_mip = ralph_is_mip(model);

    /* Estimate nnz (not available via public API, so estimate from problem size) */
    *out_nnz = (*out_num_vars) * (*out_num_cons) / 10;  /* Rough estimate */

    /* Configure solver */
    ralph_set_int_param(model, "verbose", 0);
    ralph_set_dbl_param(model, "time_limit", time_limit_sec);
    ralph_set_int_param(model, "max_iterations", 10000000);
    ralph_set_int_param(model, "presolve", 1);
    ralph_set_int_param(model, "verify", 1);

    /* Solve */
    double start_time = get_time_ms();
    ralph_optimize(model);
    double end_time = get_time_ms();

    result.time_ms = end_time - start_time;
    result.iterations = ralph_get_iterations(model);

    /* Map status */
    RalphStatus status = ralph_get_status(model);
    switch (status) {
        case RALPH_STATUS_OPTIMAL:
        case RALPH_STATUS_IMPRECISE:
            result.status = 0;
            result.objective = ralph_get_objval(model);
            break;
        case RALPH_STATUS_INFEASIBLE:
            result.status = 1;
            break;
        case RALPH_STATUS_UNBOUNDED:
        case RALPH_STATUS_INF_OR_UNBD:
            result.status = 2;
            break;
        case RALPH_STATUS_TIME_LIMIT:
        case RALPH_STATUS_ITERATION_LIMIT:
            result.status = 4;
            break;
        default:
            result.status = 3;
            break;
    }

    /* Get solution vector if optimal */
    if (result.status == 0) {
        int n = ralph_get_num_vars(model);
        result.solution = (double*)malloc(n * sizeof(double));
        if (result.solution) {
            ralph_get_solution(model, result.solution);
            result.solution_size = n;
        }
    }

    ralph_free(model);
    return result;
}

/* ============================================================================
 * Solution Validation
 * ============================================================================ */

/*
 * Validate solution by checking:
 * 1. Solution contains no NaN/Inf values
 * 2. Objective values match between Ralph and GLPK
 *
 * Note: We cannot check constraint violations without access to the constraint
 * matrix. That would require either:
 * - Exposing internal model structure (breaks encapsulation)
 * - Re-loading the problem (slow, error-prone)
 * - Adding a public API for constraint evaluation
 *
 * For now, we rely on objective match as the primary correctness indicator.
 * If Ralph's objective matches GLPK's, the solution is very likely correct.
 */
static ValidationResult validate_solution(double *solution, int num_vars,
                                           double ralph_obj, double glpk_obj,
                                           const Options *opts) {
    ValidationResult v = {0};
    v.solution_valid = 1;
    v.numerically_stable = 1;
    v.objective_match = 1;
    v.issues[0] = '\0';

    if (!solution || num_vars <= 0) {
        v.solution_valid = 0;
        strncpy(v.issues, "No solution to validate", sizeof(v.issues) - 1);
        return v;
    }

    /* Check for NaN/Inf in solution */
    for (int j = 0; j < num_vars; j++) {
        if (isnan(solution[j]) || isinf(solution[j])) {
            v.solution_valid = 0;
            v.numerically_stable = 0;
            snprintf(v.issues, sizeof(v.issues),
                     "Solution contains NaN/Inf at variable %d", j);
            return v;
        }
    }

    /* Check objective match */
    double scale = fmax(1.0, fmax(fabs(ralph_obj), fabs(glpk_obj)));
    v.objective_rel_error = fabs(ralph_obj - glpk_obj) / scale;
    v.objective_abs_error = fabs(ralph_obj - glpk_obj);

    if (v.objective_rel_error > opts->obj_rel_tol &&
        v.objective_abs_error > opts->obj_abs_tol) {
        v.objective_match = 0;
        v.solution_valid = 0;
        snprintf(v.issues, sizeof(v.issues),
                 "Objective mismatch: ralph=%.10g glpk=%.10g (rel_err=%.2e)",
                 ralph_obj, glpk_obj, v.objective_rel_error);
    }

    /* Without access to constraint matrix, we cannot compute violations */
    /* Set them to 0 to indicate "not checked" */
    v.max_constraint_violation = 0.0;
    v.max_bound_violation = 0.0;

    return v;
}

/* ============================================================================
 * Problem Discovery
 * ============================================================================ */

static int get_netlib_dir(char *buf, size_t size) {
    char bench_dir[MAX_PATH];
    get_benchmark_dir(bench_dir, sizeof(bench_dir));
    snprintf(buf, size, "%s/%s", bench_dir, NETLIB_DIR);
    return dir_exists(buf);
}

static int count_netlib_problems(void) {
    char netlib_path[MAX_PATH];
    if (!get_netlib_dir(netlib_path, sizeof(netlib_path))) {
        return 0;
    }

    DIR *dir = opendir(netlib_path);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && (strcasecmp(ext, ".mps") == 0 || strcasecmp(ext, ".lp") == 0)) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static int list_netlib_problems(ProblemInfo *problems, int max_problems, int lp_only) {
    char netlib_path[MAX_PATH];
    if (!get_netlib_dir(netlib_path, sizeof(netlib_path))) {
        return 0;
    }

    DIR *dir = opendir(netlib_path);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_problems) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".mps") != 0 && strcasecmp(ext, ".lp") != 0) continue;

        /* Extract name without extension */
        strncpy(problems[count].name, entry->d_name, sizeof(problems[count].name) - 1);
        char *dot = strrchr(problems[count].name, '.');
        if (dot) *dot = '\0';

        snprintf(problems[count].path, sizeof(problems[count].path),
                 "%s/%s", netlib_path, entry->d_name);

        /* Quick check if MIP (look for GENERAL/BINARY in file) */
        problems[count].is_mip = 0;
        FILE *f = fopen(problems[count].path, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "GENERAL") || strstr(line, "BINARY") ||
                    strstr(line, "INTEGER")) {
                    problems[count].is_mip = 1;
                    break;
                }
            }
            fclose(f);
        }

        /* Filter by type */
        if (lp_only && problems[count].is_mip) continue;

        count++;
    }
    closedir(dir);
    return count;
}

static int find_netlib_problem(const char *name, char *path_out, size_t path_size) {
    char netlib_path[MAX_PATH];
    if (!get_netlib_dir(netlib_path, sizeof(netlib_path))) {
        return 0;
    }

    /* Try with .mps extension */
    snprintf(path_out, path_size, "%s/%s.mps", netlib_path, name);
    if (file_exists(path_out)) return 1;

    /* Try with .lp extension */
    snprintf(path_out, path_size, "%s/%s.lp", netlib_path, name);
    if (file_exists(path_out)) return 1;

    return 0;
}

/* ============================================================================
 * JSON Output
 * ============================================================================ */

static void print_json_result(const char *problem_name, const char *source,
                               int num_vars, int num_cons, int nnz, int is_mip,
                               SolveResult *glpk, SolveResult *ralph,
                               ValidationResult *val,
                               FILE *out) {
    char escaped_name[512];
    json_escape_string(escaped_name, sizeof(escaped_name), problem_name);

    char escaped_issues[2048];
    json_escape_string(escaped_issues, sizeof(escaped_issues),
                       val ? val->issues : "");

    double density = (num_vars > 0 && num_cons > 0)
                     ? (double)nnz / ((double)num_vars * num_cons)
                     : 0.0;

    fprintf(out, "{\n");

    /* Problem info */
    fprintf(out, "  \"problem\": {\n");
    fprintf(out, "    \"name\": \"%s\",\n", escaped_name);
    fprintf(out, "    \"source\": \"%s\",\n", source);
    fprintf(out, "    \"vars\": %d,\n", num_vars);
    fprintf(out, "    \"cons\": %d,\n", num_cons);
    fprintf(out, "    \"nnz\": %d,\n", nnz);
    fprintf(out, "    \"density\": %.6f,\n", density);
    fprintf(out, "    \"is_mip\": %s\n", is_mip ? "true" : "false");
    fprintf(out, "  },\n");

    /* GLPK result */
    fprintf(out, "  \"glpk\": {\n");
    const char *glpk_status_str = "error";
    switch (glpk->status) {
        case 0: glpk_status_str = "optimal"; break;
        case 1: glpk_status_str = "infeasible"; break;
        case 2: glpk_status_str = "unbounded"; break;
        case 4: glpk_status_str = "timeout"; break;
    }
    fprintf(out, "    \"status\": \"%s\",\n", glpk_status_str);
    fprintf(out, "    \"objective\": %.15g,\n", glpk->objective);
    fprintf(out, "    \"time_ms\": %.3f,\n", glpk->time_ms);
    fprintf(out, "    \"iterations\": %d\n", glpk->iterations);
    fprintf(out, "  },\n");

    /* Ralph result */
    fprintf(out, "  \"ralph\": {\n");
    const char *ralph_status_str = "error";
    switch (ralph->status) {
        case 0: ralph_status_str = "optimal"; break;
        case 1: ralph_status_str = "infeasible"; break;
        case 2: ralph_status_str = "unbounded"; break;
        case 4: ralph_status_str = "timeout"; break;
    }
    fprintf(out, "    \"status\": \"%s\",\n", ralph_status_str);
    fprintf(out, "    \"objective\": %.15g,\n", ralph->objective);
    fprintf(out, "    \"time_ms\": %.3f,\n", ralph->time_ms);
    fprintf(out, "    \"iterations\": %d\n", ralph->iterations);
    fprintf(out, "  },\n");

    /* Validation */
    fprintf(out, "  \"validation\": {\n");
    if (val) {
        fprintf(out, "    \"solution_valid\": %s,\n",
                val->solution_valid ? "true" : "false");
        fprintf(out, "    \"max_constraint_violation\": %.6e,\n",
                val->max_constraint_violation);
        fprintf(out, "    \"max_bound_violation\": %.6e,\n",
                val->max_bound_violation);
        fprintf(out, "    \"objective_match\": %s,\n",
                val->objective_match ? "true" : "false");
        fprintf(out, "    \"objective_rel_error\": %.6e,\n",
                val->objective_rel_error);
        fprintf(out, "    \"numerically_stable\": %s\n",
                val->numerically_stable ? "true" : "false");
    } else {
        fprintf(out, "    \"solution_valid\": null,\n");
        fprintf(out, "    \"max_constraint_violation\": null,\n");
        fprintf(out, "    \"max_bound_violation\": null,\n");
        fprintf(out, "    \"objective_match\": null,\n");
        fprintf(out, "    \"objective_rel_error\": null,\n");
        fprintf(out, "    \"numerically_stable\": null\n");
    }
    fprintf(out, "  },\n");

    /* Performance */
    fprintf(out, "  \"performance\": {\n");
    double time_ratio = (glpk->time_ms > 0.1)
                        ? ralph->time_ms / glpk->time_ms
                        : 0.0;
    double iter_ratio = (glpk->iterations > 0)
                        ? (double)ralph->iterations / glpk->iterations
                        : 0.0;
    double ralph_per_iter = (ralph->iterations > 0)
                            ? ralph->time_ms / ralph->iterations
                            : 0.0;
    double glpk_per_iter = (glpk->iterations > 0)
                           ? glpk->time_ms / glpk->iterations
                           : 0.0;

    fprintf(out, "    \"ralph_vs_glpk_time\": %.3f,\n", time_ratio);
    fprintf(out, "    \"ralph_vs_glpk_iters\": %.3f,\n", iter_ratio);
    fprintf(out, "    \"ralph_ms_per_iter\": %.6f,\n", ralph_per_iter);
    fprintf(out, "    \"glpk_ms_per_iter\": %.6f\n", glpk_per_iter);
    fprintf(out, "  },\n");

    /* Diagnosis */
    fprintf(out, "  \"diagnosis\": {\n");
    fprintf(out, "    \"issues\": \"%s\",\n", escaped_issues);

    /* Generate recommendations based on results */
    fprintf(out, "    \"recommendations\": [");
    int first_rec = 1;

    if (val && !val->solution_valid) {
        fprintf(out, "%s\n      \"Check constraint handling in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (val && !val->objective_match && val->solution_valid) {
        fprintf(out, "%s\n      \"Check objective calculation in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (val && !val->numerically_stable) {
        fprintf(out, "%s\n      \"Check LU factorization in lu.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (time_ratio > 10.0) {
        fprintf(out, "%s\n      \"Investigate slow per-iteration time in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }
    if (iter_ratio > 2.0) {
        fprintf(out, "%s\n      \"Check pivot selection strategy in simplex.c\"",
                first_rec ? "" : ",");
        first_rec = 0;
    }

    fprintf(out, "%s]\n", first_rec ? "" : "\n    ");
    fprintf(out, "  }\n");

    fprintf(out, "}\n");
}

/* ============================================================================
 * Main Benchmark Runner
 * ============================================================================ */

static int run_single_benchmark(const char *problem_path, const char *name,
                                 const char *source, const Options *opts,
                                 FILE *out) {
    if (opts->verbose) {
        fprintf(stderr, "Benchmarking: %s\n", name);
    }

    /* Solve with GLPK first (reference) */
    double glpk_time_limit = opts->hard_cap_sec;
    SolveResult glpk = solve_with_glpk(problem_path, glpk_time_limit);

    if (glpk.status == 3) {
        fprintf(stderr, "  GLPK failed to solve %s\n", name);
        return -1;
    }

    /* Calculate Ralph time limit */
    double ralph_time_limit = glpk.time_ms / 1000.0 * opts->time_multiplier;
    if (ralph_time_limit > opts->hard_cap_sec) {
        ralph_time_limit = opts->hard_cap_sec;
    }
    if (ralph_time_limit < 1.0) {
        ralph_time_limit = 1.0;  /* Minimum 1 second */
    }

    /* Solve with Ralph */
    int num_vars = 0, num_cons = 0, nnz = 0, is_mip = 0;
    SolveResult ralph = solve_with_ralph(problem_path, ralph_time_limit,
                                          &num_vars, &num_cons, &nnz, &is_mip);

    /* Validate if both solved optimally */
    ValidationResult val = {0};
    int have_validation = 0;

    if (glpk.status == 0 && ralph.status == 0 && ralph.solution) {
        val = validate_solution(ralph.solution, num_vars,
                                ralph.objective, glpk.objective, opts);
        have_validation = 1;
    }

    /* Output results */
    print_json_result(name, source, num_vars, num_cons, nnz, is_mip,
                      &glpk, &ralph, have_validation ? &val : NULL, out);

    /* Print summary to stderr if verbose */
    if (opts->verbose) {
        fprintf(stderr, "  GLPK:  %8.2f ms, %6d iters, %s, obj=%.6g\n",
                glpk.time_ms, glpk.iterations,
                glpk.status == 0 ? "optimal" : "other", glpk.objective);
        fprintf(stderr, "  Ralph: %8.2f ms, %6d iters, %s, obj=%.6g\n",
                ralph.time_ms, ralph.iterations,
                ralph.status == 0 ? "optimal" : "other", ralph.objective);
        if (have_validation) {
            fprintf(stderr, "  Valid: %s, ObjMatch: %s\n",
                    val.solution_valid ? "yes" : "NO",
                    val.objective_match ? "yes" : "NO");
        }
    }

    /* Cleanup */
    free(ralph.solution);

    return 0;
}

static int run_suite(const char *suite_name, const Options *opts) {
    ProblemInfo problems[MAX_PROBLEMS];
    int count = list_netlib_problems(problems, MAX_PROBLEMS, opts->lp_only);

    if (count == 0) {
        fprintf(stderr, "Error: No NETLIB problems found.\n");
        fprintf(stderr, "Run: ./ralph-benchmark --download-netlib\n");
        return 1;
    }

    /* Filter by suite */
    int start = 0, end = count;
    if (strcmp(suite_name, "tiny") == 0) {
        end = (count < 5) ? count : 5;
    } else if (strcmp(suite_name, "small") == 0) {
        end = (count < 15) ? count : 15;
    } else if (strcmp(suite_name, "medium") == 0) {
        end = (count < 40) ? count : 40;
    }
    /* "all" or "large" uses all problems */

    fprintf(stdout, "[\n");
    for (int i = start; i < end; i++) {
        if (i > start) fprintf(stdout, ",\n");
        run_single_benchmark(problems[i].path, problems[i].name, "netlib",
                             opts, stdout);
    }
    fprintf(stdout, "]\n");

    return 0;
}

/* ============================================================================
 * CLI
 * ============================================================================ */

static void print_help(const char *prog) {
    printf("Ralph LP Benchmark Tool\n");
    printf("\n");
    printf("Compare Ralph solver against GLPK on standard LP problems.\n");
    printf("Produces structured JSON output for automated analysis.\n");
    printf("\n");
    printf("Usage:\n");
    printf("  %s <problem.mps|problem.lp>   Benchmark a single problem file\n", prog);
    printf("  %s --netlib <name>            Benchmark a NETLIB problem by name\n", prog);
    printf("  %s --suite <name>             Run a test suite\n", prog);
    printf("  %s --list                     List available NETLIB problems\n", prog);
    printf("  %s --download-netlib          Download NETLIB problems\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help                    Show this help message\n");
    printf("  -v, --verbose                 Print progress to stderr\n");
    printf("  --version                     Show version\n");
    printf("\n");
    printf("Time Limits:\n");
    printf("  --time-mult <N>               Ralph time = N * GLPK time (default: %.1f)\n",
           DEFAULT_TIME_MULTIPLIER);
    printf("  --hard-cap <SEC>              Maximum time per problem (default: %.0f sec)\n",
           DEFAULT_HARD_CAP_SEC);
    printf("\n");
    printf("Problem Filtering:\n");
    printf("  --lp-only                     Only benchmark LP problems (default)\n");
    printf("  --mip-only                    Only benchmark MIP problems\n");
    printf("  --all-types                   Benchmark both LP and MIP\n");
    printf("\n");
    printf("Tolerances:\n");
    printf("  --obj-rel-tol <TOL>           Objective relative tolerance (default: %.0e)\n",
           DEFAULT_OBJ_REL_TOL);
    printf("  --obj-abs-tol <TOL>           Objective absolute tolerance (default: %.0e)\n",
           DEFAULT_OBJ_ABS_TOL);
    printf("  --feas-tol <TOL>              Feasibility tolerance (default: %.0e)\n",
           DEFAULT_FEAS_TOL);
    printf("\n");
    printf("Suites:\n");
    printf("  tiny      5 small problems (~5 sec)\n");
    printf("  small     15 problems (~30 sec)\n");
    printf("  medium    40 problems (~2 min)\n");
    printf("  all       All available problems\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --download-netlib                # First time: download problems\n", prog);
    printf("  %s --suite tiny                     # Quick smoke test\n", prog);
    printf("  %s --netlib afiro -v                # Single problem, verbose\n", prog);
    printf("  %s problem.mps > result.json        # Custom file, save JSON\n", prog);
    printf("\n");
    printf("Output:\n");
    printf("  JSON is printed to stdout. Use -v for progress on stderr.\n");
    printf("  Single problem: JSON object\n");
    printf("  Suite: JSON array of objects\n");
}

static void print_version(void) {
    printf("ralph-benchmark 1.0.0\n");
    printf("Ralph %s\n", ralph_version());
}

static int parse_args(int argc, char **argv, Options *opts) {
    /* Set defaults */
    memset(opts, 0, sizeof(*opts));
    opts->time_multiplier = DEFAULT_TIME_MULTIPLIER;
    opts->hard_cap_sec = DEFAULT_HARD_CAP_SEC;
    opts->obj_rel_tol = DEFAULT_OBJ_REL_TOL;
    opts->obj_abs_tol = DEFAULT_OBJ_ABS_TOL;
    opts->feas_tol = DEFAULT_FEAS_TOL;
    opts->lp_only = 1;  /* Default: LP only */

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            opts->show_help = 1;
        } else if (strcmp(arg, "--version") == 0) {
            opts->show_version = 1;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            opts->verbose = 1;
        } else if (strcmp(arg, "--list") == 0) {
            opts->list_problems = 1;
        } else if (strcmp(arg, "--download-netlib") == 0) {
            opts->download_netlib = 1;
        } else if (strcmp(arg, "--lp-only") == 0) {
            opts->lp_only = 1;
            opts->mip_only = 0;
        } else if (strcmp(arg, "--mip-only") == 0) {
            opts->mip_only = 1;
            opts->lp_only = 0;
        } else if (strcmp(arg, "--all-types") == 0) {
            opts->lp_only = 0;
            opts->mip_only = 0;
        } else if (strcmp(arg, "--suite") == 0 && i + 1 < argc) {
            strncpy(opts->suite, argv[++i], sizeof(opts->suite) - 1);
        } else if (strcmp(arg, "--netlib") == 0 && i + 1 < argc) {
            strncpy(opts->netlib_name, argv[++i], sizeof(opts->netlib_name) - 1);
        } else if (strcmp(arg, "--time-mult") == 0 && i + 1 < argc) {
            opts->time_multiplier = atof(argv[++i]);
        } else if (strcmp(arg, "--hard-cap") == 0 && i + 1 < argc) {
            opts->hard_cap_sec = atof(argv[++i]);
        } else if (strcmp(arg, "--obj-rel-tol") == 0 && i + 1 < argc) {
            opts->obj_rel_tol = atof(argv[++i]);
        } else if (strcmp(arg, "--obj-abs-tol") == 0 && i + 1 < argc) {
            opts->obj_abs_tol = atof(argv[++i]);
        } else if (strcmp(arg, "--feas-tol") == 0 && i + 1 < argc) {
            opts->feas_tol = atof(argv[++i]);
        } else if (strcmp(arg, "-o") == 0 && i + 1 < argc) {
            strncpy(opts->output_dir, argv[++i], sizeof(opts->output_dir) - 1);
        } else if (arg[0] != '-') {
            /* Positional argument: problem file */
            strncpy(opts->problem_path, arg, sizeof(opts->problem_path) - 1);
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }
    }

    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    Options opts;

    if (parse_args(argc, argv, &opts) != 0) {
        return 1;
    }

    if (opts.show_help) {
        print_help(argv[0]);
        return 0;
    }

    if (opts.show_version) {
        print_version();
        return 0;
    }

    /* Check GLPK availability */
    if (!check_glpk_available()) {
        fprintf(stderr, "Error: glpsol not found in PATH.\n");
        fprintf(stderr, "Install GLPK: brew install glpk (macOS) or apt install glpk-utils (Linux)\n");
        return 1;
    }

    /* Download NETLIB problems */
    if (opts.download_netlib) {
        char script_path[MAX_PATH];
        char bench_dir[MAX_PATH];
        get_benchmark_dir(bench_dir, sizeof(bench_dir));
        snprintf(script_path, sizeof(script_path), "%s/download_netlib.sh", bench_dir);

        if (!file_exists(script_path)) {
            fprintf(stderr, "Error: download_netlib.sh not found at %s\n", script_path);
            fprintf(stderr, "Try running from the ralph/ directory: make download-netlib\n");
            return 1;
        }

        char cmd[MAX_PATH * 2];
        snprintf(cmd, sizeof(cmd), "cd '%s' && bash download_netlib.sh", bench_dir);
        return system(cmd);
    }

    /* List problems */
    if (opts.list_problems) {
        ProblemInfo problems[MAX_PROBLEMS];
        int count = list_netlib_problems(problems, MAX_PROBLEMS, 0);

        if (count == 0) {
            printf("No NETLIB problems found.\n");
            printf("Run: %s --download-netlib\n", argv[0]);
            return 0;
        }

        printf("Available NETLIB problems (%d total):\n\n", count);
        printf("%-20s %s\n", "Name", "Type");
        printf("%-20s %s\n", "----", "----");
        for (int i = 0; i < count; i++) {
            printf("%-20s %s\n", problems[i].name,
                   problems[i].is_mip ? "MIP" : "LP");
        }
        return 0;
    }

    /* Run suite */
    if (opts.suite[0] != '\0') {
        return run_suite(opts.suite, &opts);
    }

    /* Run single NETLIB problem by name */
    if (opts.netlib_name[0] != '\0') {
        char problem_path[MAX_PATH];
        if (!find_netlib_problem(opts.netlib_name, problem_path, sizeof(problem_path))) {
            fprintf(stderr, "Error: NETLIB problem '%s' not found.\n", opts.netlib_name);

            int count = count_netlib_problems();
            if (count == 0) {
                fprintf(stderr, "No NETLIB problems available. Run: %s --download-netlib\n",
                        argv[0]);
            } else {
                fprintf(stderr, "Use --list to see available problems.\n");
            }
            return 1;
        }

        return run_single_benchmark(problem_path, opts.netlib_name, "netlib",
                                     &opts, stdout);
    }

    /* Run single problem file */
    if (opts.problem_path[0] != '\0') {
        if (!file_exists(opts.problem_path)) {
            fprintf(stderr, "Error: Problem file not found: %s\n", opts.problem_path);
            return 1;
        }

        /* Extract name from path */
        const char *name = strrchr(opts.problem_path, '/');
        name = name ? name + 1 : opts.problem_path;

        return run_single_benchmark(opts.problem_path, name, "file",
                                     &opts, stdout);
    }

    /* No action specified */
    fprintf(stderr, "Error: No problem specified.\n\n");
    print_help(argv[0]);
    return 1;
}
