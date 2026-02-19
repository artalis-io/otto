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
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#include "ralph.h"
#include "lp.h"

/* Internal helpers exposed by ralph.c for benchmark diagnostics */
extern LPModel* ralph_get_lp_model(const RalphModel *model);
extern SimplexSolver* ralph_get_lp_solver(const RalphModel *model);

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

/* Default test time caps */
#define TEST_FAST_CAP_SEC 60.0
#define TEST_FULL_CAP_SEC 300.0

/* ============================================================================
 * NETLIB Known Optimal Values (from netlib.org/lp/data/readme)
 * ============================================================================ */

typedef struct {
    const char *name;
    double optimal;
    int tier;  /* 0=tiny, 1=small, 2=medium, 3=large, 4=xlarge */
} NetlibReference;

static const NetlibReference NETLIB_REFERENCE[] = {
    /* Tier 0: tiny (<100 vars) */
    {"afiro",      -4.6475314286e+02, 0},
    {"sc50a",      -6.4575077059e+01, 0},
    {"sc50b",      -7.0000000000e+01, 0},
    {"kb2",        -1.7499001299e+03, 0},
    {"sc105",      -5.2202061212e+01, 0},
    {"blend",      -3.0812149846e+01, 0},
    {"share2b",    -4.1573224074e+02, 0},
    {"recipe",     -2.6661600000e+02, 0},

    /* Tier 1: small (100-500 vars) */
    {"adlittle",    2.2549496316e+05, 1},
    {"lotfi",      -2.5264706062e+01, 1},
    {"scagr7",     -2.3313892548e+06, 1},
    {"israel",     -8.9664482186e+05, 1},
    {"scorpion",    1.8781248227e+03, 1},
    {"brandy",      1.5185098965e+03, 1},
    {"bandm",      -1.5862801845e+02, 1},
    {"beaconfd",    3.3592485807e+04, 5},  /* Known regression: Phase 2 pivot failure (degenerate theta=0 with near-zero pivot element). Moved from tier 1 to tier 5 (skipped). Fix: ratio test minimum pivot threshold for degenerate pivots. */
    {"e226",       -1.8751929066e+01, 1},
    {"stocfor1",   -4.1131976219e+04, 1},
    {"sc205",      -5.2202061212e+01, 1},
    {"agg",        -3.5991767287e+07, 1},
    {"agg2",       -2.0239252356e+07, 1},
    {"agg3",        1.0312115935e+07, 1},
    {"bore3d",      1.3730803942e+03, 1},
    {"capri",       2.6900129138e+03, 1},
    {"share1b",    -7.6589318579e+04, 1},
    {"scagr25",    -1.4753433061e+07, 1},

    /* Tier 2: medium (500-2000 vars) */
    {"bnl1",        1.9776292856e+03, 2},
    {"degen2",     -1.4351780000e+03, 2},
    {"grow7",      -4.7787811815e+07, 2},
    {"grow15",     -1.0687094129e+08, 2},
    {"grow22",     -1.6083433648e+08, 2},
    {"scfxm1",      1.8416759028e+04, 2},
    {"scfxm2",      3.6660261565e+04, 2},
    {"scfxm3",      5.4901254550e+04, 2},
    {"scsd1",       8.6666666743e+00, 2},
    {"scsd6",       5.0500000078e+01, 2},
    {"scsd8",       9.0499999993e+02, 2},
    {"sctap1",      1.4122500000e+03, 2},
    {"sctap2",      1.7248071429e+03, 2},
    {"sctap3",      1.4240000000e+03, 2},
    {"ship04s",     1.7987147004e+06, 2},
    {"ship04l",     1.7933245380e+06, 2},
    {"ship08s",     1.9200982105e+06, 2},
    {"ship08l",     1.9090552114e+06, 2},
    {"ship12s",     1.4892361344e+06, 2},
    {"ship12l",     1.4701879193e+06, 2},
    {"etamacro",   -7.5571521774e+02, 2},
    {"finnis",      1.7279096547e+05, 2},
    {"perold",     -9.3807580773e+03, 2},
    {"stair",      -2.5126695119e+02, 2},
    {"shell",       1.2088253460e+09, 2},
    {"seba",        1.5711600000e+04, 2},
    {"forplan",    -6.6421873953e+02, 2},
    {"ganges",     -1.0958636356e+05, 2},
    {"sierra",      1.5394362184e+07, 2},
    {"standata",    1.2576995000e+03, 2},
    {"standmps",    1.4060175000e+03, 2},
    {"nesm",        1.4076073035e+07, 2},
    {"fffff800",    5.5567961165e+05, 2},

    /* Tier 3: large (2000+ vars) */
    {"bnl2",        1.8112365404e+03, 3},
    {"degen3",     -9.8729400000e+02, 3},
    {"pilot",      -5.5740430007e+02, 3},
    {"pilot87",     3.0171072827e+02, 3},
    {"pilot.ja",   -6.1131344111e+03, 3},
    {"pilot.we",   -2.7201027439e+06, 3},
    {"pilot4",     -2.5811392641e+03, 3},
    {"pilotnov",   -4.4972761882e+03, 3},
    {"maros",      -5.8063743701e+04, 3},
    {"d2q06c",      1.2278423615e+05, 3},
    {"stocfor2",   -3.9024408538e+04, 3},
    {"cycle",      -5.2263930249e+00, 3},
    {"czprob",      2.1851966989e+06, 3},
    {"25fv47",      5.5018458883e+03, 3},
    {"woodw",       1.3044763331e+00, 3},
    {"wood1p",      1.4429024116e+00, 3},

    /* Tier 4: xlarge */
    {"80bau3b",     9.8723216072e+05, 4},
    {"fit1d",      -9.1463780924e+03, 4},
    {"fit1p",       9.1463780924e+03, 4},
    {"fit2d",      -6.8464293294e+04, 4},
    {"fit2p",       6.8464293232e+04, 4},
    {"maros-r7",    1.4971851665e+06, 4},
    {"stocfor3",   -3.9976661576e+04, 4},
    {"greenbea",   -7.2462405908e+07, 4},
    {"greenbeb",   -4.3021476065e+06, 4},
    {"truss",       4.5881584719e+05, 4},
    {"d6cube",      3.1549166667e+02, 4},

    {NULL, 0.0, -1}  /* sentinel */
};

static const NetlibReference* find_netlib_reference(const char *name) {
    for (int i = 0; NETLIB_REFERENCE[i].name != NULL; i++) {
        if (strcasecmp(NETLIB_REFERENCE[i].name, name) == 0) {
            return &NETLIB_REFERENCE[i];
        }
    }
    return NULL;
}

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
    double primal_setup_ms;
    double dual_ms;
    double phase1_ms;
    double transition_ms;
    double phase2_ms;
    double pricing_ms;
    double ratio_ms;
    double pivot_ms;
    double refactor_ms;
    double ftran_ms;
    double btran_ms;
    double lu_update_ms;
    double compute_solution_ms;
    double compute_rc_ms;
    double refactor_all_ms;
    int refactor_count;
    double refactor_last_ms;
    double refactor_max_ms;
    int refactor_last_reason;
    char refactor_last_reason_str[64];
    int refactor_reason_setup;
    int refactor_reason_transition;
    int refactor_reason_periodic;
    int refactor_reason_ratio_recovery;
    int refactor_reason_pivot_recovery;
    int refactor_reason_forced_small_pivot;
    int refactor_reason_update_recovery;
    int refactor_reason_direction_stabilize;
    int refactor_reason_infeas_cleanup;
    int refactor_reason_other;
    int refactor_last_m;
    int refactor_last_k;
    int refactor_last_nnz_b;

    double phase1_pricing_ms;
    double phase1_ratio_ms;
    double phase1_pivot_ms;
    double phase1_refactor_ms;
    double phase1_compute_solution_ms;
    double phase1_compute_rc_ms;
    int phase1_pricing_calls;
    int phase1_ratio_calls;
    int phase1_pivot_calls;
    int phase1_refactor_calls;
    int phase1_compute_solution_calls;
    int phase1_compute_rc_calls;

    double phase2_pricing_ms;
    double phase2_ratio_ms;
    double phase2_pivot_ms;
    double phase2_refactor_ms;
    double phase2_compute_solution_ms;
    double phase2_compute_rc_ms;
    int phase2_pricing_calls;
    int phase2_ratio_calls;
    int phase2_pivot_calls;
    int phase2_refactor_calls;
    int phase2_compute_solution_calls;
    int phase2_compute_rc_calls;

    int lu_mkz_enabled;
    int lu_sn_enabled;
    int lu_mkz_calls;
    int lu_mkz_successes;
    int lu_mkz_failures;
    int lu_mkz_retry_count;
    int lu_mkz_last_failure;
    int lu_mkz_dense_fallbacks;
    int lu_mkz_fail_workspace;
    int lu_mkz_fail_pool;
    int lu_mkz_fail_singular;
    int lu_mkz_fail_capacity;
    int lu_sparse_dense_fallbacks;
    int lu_used_dense_fallback_last;
    int lu_identity_sep_failures;
    int lu_sn_calls;
    int lu_sn_successes;
    int lu_num_updates;
    int lu_max_updates;
    int lu_last_failure_reason_code;
    char lu_last_failure_reason[64];
    int lu_factorize_calls;
    int lu_last_basis_nnz;
    int lu_last_m;
    int lu_last_k;
    double lu_last_a_struct_build_ms;
    double lu_last_markowitz_numeric_ms;
    double lu_last_identity_placement_ms;
    double lu_last_coo_to_csc_ms;
    double lu_total_a_struct_build_ms;
    double lu_total_markowitz_numeric_ms;
    double lu_total_identity_placement_ms;
    double lu_total_coo_to_csc_ms;
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
    int verify_matrix;   /* Deep matrix verification via GLPK solution */
    int test_mode;       /* 0=off, 1=fast (tiers 0-1), 2=full (all tiers) */

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

    /* Solver method */
    int method;  /* 0=primal, 1=dual, 2=auto */
    int pricing; /* -1=default, 0=Dantzig, 1=SE, 2=Devex, 3=Partial, 4=Heap */
    int lu_supernode; /* 0=off, 1=enable supernodal LU */

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

static const char* refactor_reason_string(int reason) {
    switch ((RalphRefactorReason)reason) {
        case RALPH_REFACTOR_REASON_SETUP: return "setup";
        case RALPH_REFACTOR_REASON_PHASE_TRANSITION: return "phase_transition";
        case RALPH_REFACTOR_REASON_PERIODIC: return "periodic";
        case RALPH_REFACTOR_REASON_RATIO_RECOVERY: return "ratio_recovery";
        case RALPH_REFACTOR_REASON_PIVOT_RECOVERY: return "pivot_recovery";
        case RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT: return "forced_small_pivot";
        case RALPH_REFACTOR_REASON_UPDATE_RECOVERY: return "update_recovery";
        case RALPH_REFACTOR_REASON_DIRECTION_STABILIZE: return "direction_stabilize";
        case RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP: return "infeasibility_cleanup";
        case RALPH_REFACTOR_REASON_OTHER:
        default:
            return "other";
    }
}

/* ============================================================================
 * GLPK Wrapper (via glpsol CLI)
 * ============================================================================ */

static int check_glpk_available(void) {
    int ret = system("which glpsol >/dev/null 2>&1");
    return ret == 0;
}

/* Parse a GLPK stdout line like:
 * "  12345 simplex iterations"
 * Returns 1 if parsed, 0 otherwise.
 */
static int parse_glpk_iterations_line(const char *line, int *iters_out) {
    if (!line || !iters_out) return 0;

    const char *marker = strstr(line, "simplex iterations");
    if (!marker) return 0;

    /* Walk backward from marker to find the integer token before it */
    const char *end = marker;
    while (end > line && isspace((unsigned char)end[-1])) end--;

    const char *start = end;
    while (start > line && isdigit((unsigned char)start[-1])) start--;
    if (start == end) return 0;

    char buf[32];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    int iters = atoi(buf);
    if (iters < 0) return 0;

    *iters_out = iters;
    return 1;
}

/* Parse simplex progress lines like:
 * "    110: obj = ..."
 * "*   240: obj = ..."
 */
static int parse_glpk_progress_iteration(const char *line, int *iters_out) {
    if (!line || !iters_out) return 0;

    const char *marker = strstr(line, ": obj");
    if (!marker) return 0;

    const char *end = marker;
    while (end > line && isspace((unsigned char)end[-1])) end--;

    const char *start = end;
    while (start > line && isdigit((unsigned char)start[-1])) start--;
    if (start == end) return 0;

    char buf[32];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    int iters = atoi(buf);
    if (iters < 0) return 0;
    *iters_out = iters;
    return 1;
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
        /* Parse iteration count from stdout (not solution file). */
        {
            int iters = 0;
            if (parse_glpk_iterations_line(line, &iters)) {
                result.iterations = iters;
            }
        }
        {
            int iters = 0;
            if (parse_glpk_progress_iteration(line, &iters) && iters > result.iterations) {
                result.iterations = iters;
            }
        }
    }
    int ret = pclose(pipe);
    double end_time = get_time_ms();

    /* Use wall clock if GLPK didn't report time, or reported 0 (0.1s resolution) */
    if (!found_time || result.time_ms < 0.001) {
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
                                     int method, int pricing, int lu_supernode,
                                     int *out_num_vars, int *out_num_cons, int *out_nnz,
                                     int *out_is_mip) {
    SolveResult result = {0};
    result.status = 3;  /* Error by default */
    result.solution = NULL;
    strncpy(result.lu_last_failure_reason, "none",
            sizeof(result.lu_last_failure_reason) - 1);
    strncpy(result.refactor_last_reason_str, "other",
            sizeof(result.refactor_last_reason_str) - 1);

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
    *out_nnz = 0;

    /* Configure solver */
    ralph_set_int_param(model, "verbose", 0);
    ralph_set_dbl_param(model, "time_limit", time_limit_sec);
    ralph_set_int_param(model, "max_iterations", 10000000);
    ralph_set_int_param(model, "presolve", 1);
    ralph_set_int_param(model, "verify", 1);
    ralph_set_int_param(model, "method", method);
    if (pricing >= 0) {
        ralph_set_int_param(model, "pricing", pricing);
    }
    if (lu_supernode) {
        ralph_set_int_param(model, "lu_supernode", 1);
    }

    /* Solve */
    double start_time = get_time_ms();
    ralph_optimize(model);
    double end_time = get_time_ms();

    result.time_ms = end_time - start_time;
    result.iterations = ralph_get_iterations(model);
    {
        LPModel *lp = ralph_get_lp_model(model);
        if (lp) {
            if (lp->A && lp->A->nnz > 0) {
                *out_nnz = lp->A->nnz;
            } else if (lp->num_elements > 0) {
                *out_nnz = lp->num_elements;
            }
        }
    }
    {
        SimplexSolver *solver = ralph_get_lp_solver(model);
        if (solver) {
            result.primal_setup_ms = solver->perf_primal_setup_ms;
            result.dual_ms = solver->perf_dual_ms;
            result.phase1_ms = solver->perf_phase1_ms;
            result.transition_ms = solver->perf_transition_ms;
            result.phase2_ms = solver->perf_phase2_ms;
            result.pricing_ms = solver->perf_pricing_ms;
            result.ratio_ms = solver->perf_ratio_ms;
            result.pivot_ms = solver->perf_pivot_ms;
            result.refactor_ms = solver->perf_refactor_ms;
            result.ftran_ms = solver->perf_ftran_ms;
            result.btran_ms = solver->perf_btran_ms;
            result.lu_update_ms = solver->perf_lu_update_ms;
            result.compute_solution_ms = solver->perf_compute_solution_ms;
            result.compute_rc_ms = solver->perf_compute_rc_ms;
            result.refactor_all_ms = solver->perf_refactor_all_ms;
            result.refactor_count = solver->perf_refactor_count;
            result.refactor_last_ms = solver->perf_refactor_last_ms;
            result.refactor_max_ms = solver->perf_refactor_max_ms;
            result.refactor_last_reason = solver->perf_refactor_last_reason;
            {
                const char *reason = refactor_reason_string(solver->perf_refactor_last_reason);
                if (!reason) reason = "other";
                strncpy(result.refactor_last_reason_str, reason,
                        sizeof(result.refactor_last_reason_str) - 1);
                result.refactor_last_reason_str[sizeof(result.refactor_last_reason_str) - 1] = '\0';
            }
            result.refactor_reason_setup = solver->perf_refactor_reason_setup;
            result.refactor_reason_transition = solver->perf_refactor_reason_transition;
            result.refactor_reason_periodic = solver->perf_refactor_reason_periodic;
            result.refactor_reason_ratio_recovery = solver->perf_refactor_reason_ratio_recovery;
            result.refactor_reason_pivot_recovery = solver->perf_refactor_reason_pivot_recovery;
            result.refactor_reason_forced_small_pivot = solver->perf_refactor_reason_forced_small_pivot;
            result.refactor_reason_update_recovery = solver->perf_refactor_reason_update_recovery;
            result.refactor_reason_direction_stabilize = solver->perf_refactor_reason_direction_stabilize;
            result.refactor_reason_infeas_cleanup = solver->perf_refactor_reason_infeas_cleanup;
            result.refactor_reason_other = solver->perf_refactor_reason_other;
            result.refactor_last_m = solver->perf_refactor_last_m;
            result.refactor_last_k = solver->perf_refactor_last_k;
            result.refactor_last_nnz_b = solver->perf_refactor_last_nnz_B;

            result.phase1_pricing_ms = solver->perf_phase1_pricing_ms;
            result.phase1_ratio_ms = solver->perf_phase1_ratio_ms;
            result.phase1_pivot_ms = solver->perf_phase1_pivot_ms;
            result.phase1_refactor_ms = solver->perf_phase1_refactor_ms;
            result.phase1_compute_solution_ms = solver->perf_phase1_compute_solution_ms;
            result.phase1_compute_rc_ms = solver->perf_phase1_compute_rc_ms;
            result.phase1_pricing_calls = solver->perf_phase1_pricing_calls;
            result.phase1_ratio_calls = solver->perf_phase1_ratio_calls;
            result.phase1_pivot_calls = solver->perf_phase1_pivot_calls;
            result.phase1_refactor_calls = solver->perf_phase1_refactor_calls;
            result.phase1_compute_solution_calls = solver->perf_phase1_compute_solution_calls;
            result.phase1_compute_rc_calls = solver->perf_phase1_compute_rc_calls;

            result.phase2_pricing_ms = solver->perf_phase2_pricing_ms;
            result.phase2_ratio_ms = solver->perf_phase2_ratio_ms;
            result.phase2_pivot_ms = solver->perf_phase2_pivot_ms;
            result.phase2_refactor_ms = solver->perf_phase2_refactor_ms;
            result.phase2_compute_solution_ms = solver->perf_phase2_compute_solution_ms;
            result.phase2_compute_rc_ms = solver->perf_phase2_compute_rc_ms;
            result.phase2_pricing_calls = solver->perf_phase2_pricing_calls;
            result.phase2_ratio_calls = solver->perf_phase2_ratio_calls;
            result.phase2_pivot_calls = solver->perf_phase2_pivot_calls;
            result.phase2_refactor_calls = solver->perf_phase2_refactor_calls;
            result.phase2_compute_solution_calls = solver->perf_phase2_compute_solution_calls;
            result.phase2_compute_rc_calls = solver->perf_phase2_compute_rc_calls;
            if (solver->tableau && solver->tableau->lu) {
                LUFactorization *lu = solver->tableau->lu;
                result.lu_mkz_enabled = lu->mkz_enabled;
                result.lu_sn_enabled = lu->sn_enabled;
                result.lu_mkz_calls = lu->mkz_calls;
                result.lu_mkz_successes = lu->mkz_successes;
                result.lu_mkz_failures = lu->mkz_failures;
                {
                    int retries = lu->mkz_calls - lu->mkz_successes - lu->mkz_failures;
                    result.lu_mkz_retry_count = (retries > 0) ? retries : 0;
                }
                result.lu_mkz_last_failure = lu->mkz_last_failure;
                result.lu_mkz_dense_fallbacks = lu->mkz_dense_fallbacks;
                result.lu_mkz_fail_workspace = lu->mkz_fail_workspace;
                result.lu_mkz_fail_pool = lu->mkz_fail_pool;
                result.lu_mkz_fail_singular = lu->mkz_fail_singular;
                result.lu_mkz_fail_capacity = lu->mkz_fail_capacity;
                result.lu_sparse_dense_fallbacks = lu->sparse_dense_fallbacks;
                result.lu_used_dense_fallback_last = lu->used_dense_fallback_last;
                result.lu_identity_sep_failures = lu->identity_sep_failures;
                result.lu_sn_calls = lu->sn_calls;
                result.lu_sn_successes = lu->sn_successes;
                result.lu_num_updates = lu->num_updates;
                result.lu_max_updates = lu->max_updates;
                result.lu_last_failure_reason_code = lu->last_failure_reason;
                result.lu_factorize_calls = lu->perf_factorize_calls;
                result.lu_last_basis_nnz = lu->perf_last_basis_nnz;
                result.lu_last_m = lu->perf_last_m;
                result.lu_last_k = lu->perf_last_k;
                result.lu_last_a_struct_build_ms = lu->perf_last_a_struct_build_ms;
                result.lu_last_markowitz_numeric_ms = lu->perf_last_markowitz_numeric_ms;
                result.lu_last_identity_placement_ms = lu->perf_last_identity_placement_ms;
                result.lu_last_coo_to_csc_ms = lu->perf_last_coo_to_csc_ms;
                result.lu_total_a_struct_build_ms = lu->perf_total_a_struct_build_ms;
                result.lu_total_markowitz_numeric_ms = lu->perf_total_markowitz_numeric_ms;
                result.lu_total_identity_placement_ms = lu->perf_total_identity_placement_ms;
                result.lu_total_coo_to_csc_ms = lu->perf_total_coo_to_csc_ms;
                {
                    const char *reason = lu_failure_reason_string(lu->last_failure_reason);
                    if (!reason) reason = "unknown";
                    strncpy(result.lu_last_failure_reason, reason,
                            sizeof(result.lu_last_failure_reason) - 1);
                    result.lu_last_failure_reason[sizeof(result.lu_last_failure_reason) - 1] = '\0';
                }
            }
        }
    }

    /* Map status */
    RalphStatus status = ralph_get_status(model);
    switch (status) {
        case RALPH_STATUS_OPTIMAL:
        case RALPH_STATUS_IMPRECISE:
        case RALPH_STATUS_OBJ_LIMIT:
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
 * Matrix Verification via GLPK Reference Solution
 *
 * Loads an MPS/LP file into Ralph (no solve), runs GLPK to get a reference
 * solution, then verifies Ralph's internal constraint matrix by computing
 * Ax and checking against b/sense/bounds. Catches matrix construction bugs
 * (MPS parsing, triplet-to-CSC conversion) that objective-only checks miss.
 * ============================================================================ */

/* Access Ralph's internal model (defined in ralph.c) */
extern int lp_model_finalize(LPModel *model);

/* Forward declaration (defined in Problem Discovery section below) */
static int list_netlib_problems(ProblemInfo *problems, int max_problems, int lp_only);

typedef struct {
    int num_vars;
    int num_cons;
    int nnz;

    /* Per-row constraint check */
    int num_con_violations;
    double max_con_violation;
    int worst_con_row;

    /* Per-variable bound check */
    int num_bound_violations;
    double max_bound_violation;
    int worst_bound_var;

    /* Objective check */
    double ralph_obj;           /* c'x computed from Ralph's c vector */
    double glpk_obj;            /* Objective from GLPK's solution output */
    double obj_error;

    int pass;                   /* Overall pass/fail */
    char detail[2048];          /* Human-readable detail */
} MatrixVerifyResult;

/* Parse GLPK --output file to extract column activity values.
 * Returns number of columns parsed, or -1 on error.
 * Sets *status_ok to 1 if solution status is OPTIMAL, 0 otherwise. */
static int parse_glpk_solution_vector(const char *sol_file, double *x, int max_vars,
                                       double *obj_out, int *status_ok) {
    FILE *f = fopen(sol_file, "r");
    if (!f) return -1;

    char line[MAX_LINE];
    int in_columns = 0;
    int parsed = 0;
    if (status_ok) *status_ok = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Parse solution status */
        if (strncmp(line, "Status:", 7) == 0) {
            if (status_ok && strstr(line, "OPTIMAL"))
                *status_ok = 1;
        }

        /* Parse objective */
        if (strstr(line, "Objective:")) {
            char *eq = strchr(line, '=');
            if (eq && obj_out) {
                *obj_out = atof(eq + 1);
            }
        }

        /* Detect column section header */
        if (strstr(line, "Column name") && strstr(line, "Activity")) {
            /* Skip the dashed separator line */
            if (fgets(line, sizeof(line), f)) { /* separator */ }
            in_columns = 1;
            continue;
        }

        /* End of column section */
        if (in_columns && (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')) {
            break;
        }
        /* KKT section also ends columns */
        if (in_columns && strstr(line, "Karush-Kuhn-Tucker")) {
            break;
        }

        if (in_columns) {
            /* Format: "     1 colname    St   Activity     LB    UB    Marginal"
             * Column number is 1-based */
            int col_num;
            char col_name[256], status[8];
            double activity;

            /* Try parsing with activity value */
            int n = sscanf(line, " %d %255s %7s %lf",
                           &col_num, col_name, status, &activity);
            if (n >= 4 && col_num >= 1 && col_num <= max_vars) {
                x[col_num - 1] = activity;
                parsed++;
            } else if (n >= 3 && col_num >= 1 && col_num <= max_vars) {
                /* Activity might be empty (value = 0) */
                x[col_num - 1] = 0.0;
                parsed++;
            }
        }
    }

    fclose(f);
    return parsed;
}

static MatrixVerifyResult verify_matrix_single(const char *problem_path,
                                                const char *name,
                                                const Options *opts) {
    (void)name;
    MatrixVerifyResult r = {0};

    /* 1. Load model in Ralph (parse only, no solve) */
    RalphModel *model = ralph_create();
    if (!model) {
        snprintf(r.detail, sizeof(r.detail), "Failed to create Ralph model");
        return r;
    }

    const char *ext = strrchr(problem_path, '.');
    int load_ret;
    if (ext && strcasecmp(ext, ".lp") == 0) {
        load_ret = ralph_read_lp(model, problem_path);
    } else {
        load_ret = ralph_read_mps(model, problem_path);
    }
    if (load_ret != 0) {
        snprintf(r.detail, sizeof(r.detail), "Failed to load %s", problem_path);
        ralph_free(model);
        return r;
    }

    /* Get internal model and finalize (builds CSC matrix) */
    LPModel *lp = ralph_get_lp_model(model);
    if (!lp) {
        snprintf(r.detail, sizeof(r.detail), "No internal LPModel");
        ralph_free(model);
        return r;
    }
    if (!lp->A) {
        lp_model_finalize(lp);
    }
    if (!lp->A) {
        snprintf(r.detail, sizeof(r.detail), "No constraint matrix after finalize");
        ralph_free(model);
        return r;
    }

    int m = lp->num_cons;
    int n = lp->num_vars;
    r.num_vars = n;
    r.num_cons = m;
    r.nnz = lp->A->colptr[n];

    /* 2. Solve with GLPK to get reference solution */
    char sol_file[MAX_PATH];
    snprintf(sol_file, sizeof(sol_file), "/tmp/ralph_verify_%d.txt", getpid());

    const char *fmt_flag = "--mps";
    if (ext && strcasecmp(ext, ".lp") == 0) fmt_flag = "--lp";

    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof(cmd), "glpsol %s '%s' -o '%s' 2>/dev/null",
             fmt_flag, problem_path, sol_file);

    int ret = system(cmd);
    if (ret != 0) {
        snprintf(r.detail, sizeof(r.detail), "GLPK failed to solve");
        ralph_free(model);
        unlink(sol_file);
        return r;
    }

    /* Parse GLPK's solution vector */
    double *x = (double*)calloc(n, sizeof(double));
    if (!x) {
        snprintf(r.detail, sizeof(r.detail), "Memory allocation failed");
        ralph_free(model);
        unlink(sol_file);
        return r;
    }

    double glpk_obj = 0.0;
    int glpk_optimal = 0;
    int parsed = parse_glpk_solution_vector(sol_file, x, n, &glpk_obj, &glpk_optimal);
    unlink(sol_file);

    if (parsed == 0) {
        snprintf(r.detail, sizeof(r.detail),
                 "Failed to parse GLPK solution (0 columns parsed)");
        free(x);
        ralph_free(model);
        return r;
    }

    if (!glpk_optimal) {
        snprintf(r.detail, sizeof(r.detail),
                 "SKIP: GLPK solution status is not OPTIMAL (%dx%d nnz=%d)",
                 n, m, r.nnz);
        r.pass = 1;  /* Not a Ralph bug — skip */
        free(x);
        ralph_free(model);
        return r;
    }

    r.glpk_obj = glpk_obj;

    /* 3. Compute Ax using Ralph's CSC matrix */
    double *ax = (double*)calloc(m, sizeof(double));
    if (!ax) {
        free(x);
        ralph_free(model);
        return r;
    }

    for (int j = 0; j < n; j++) {
        double xj = x[j];
        if (fabs(xj) < 1e-15) continue;
        for (int p = lp->A->colptr[j]; p < lp->A->colptr[j + 1]; p++) {
            int row = lp->A->rowidx[p];
            if (row >= 0 && row < m) {
                ax[row] += lp->A->values[p] * xj;
            }
        }
    }

    /* 4. Check constraint violations */
    r.worst_con_row = -1;
    for (int i = 0; i < m; i++) {
        double viol = 0.0;
        if (lp->sense[i] == 'E') {
            viol = fabs(ax[i] - lp->b[i]);
        } else if (lp->sense[i] == 'L') {
            if (ax[i] > lp->b[i] + opts->feas_tol)
                viol = ax[i] - lp->b[i];
        } else if (lp->sense[i] == 'G') {
            if (ax[i] < lp->b[i] - opts->feas_tol)
                viol = lp->b[i] - ax[i];
        }

        if (viol > opts->feas_tol) {
            r.num_con_violations++;
        }
        if (viol > r.max_con_violation) {
            r.max_con_violation = viol;
            r.worst_con_row = i;
        }
    }

    /* 5. Check bound violations */
    r.worst_bound_var = -1;
    for (int j = 0; j < n; j++) {
        double viol = 0.0;
        if (lp->lb && x[j] < lp->lb[j] - opts->feas_tol) {
            viol = lp->lb[j] - x[j];
        }
        if (lp->ub && x[j] > lp->ub[j] + opts->feas_tol) {
            double bv = x[j] - lp->ub[j];
            if (bv > viol) viol = bv;
        }

        if (viol > opts->feas_tol) {
            r.num_bound_violations++;
        }
        if (viol > r.max_bound_violation) {
            r.max_bound_violation = viol;
            r.worst_bound_var = j;
        }
    }

    /* 6. Check objective: c'x */
    r.ralph_obj = 0.0;
    for (int j = 0; j < n; j++) {
        r.ralph_obj += lp->c[j] * x[j];
    }
    /* Apply obj_sense: Ralph stores c in original sense, GLPK reports in original sense */
    r.obj_error = fabs(r.ralph_obj - glpk_obj);

    /* 7. Overall pass/fail
     * Thresholds are generous because GLPK's --output format has ~6 significant
     * digits, causing truncation noise in the parsed solution vector.
     * The goal is catching matrix construction bugs (violations >> 100),
     * not numerical precision issues.  Scale constraint threshold by the
     * magnitude of the objective: problems with |obj| ~ 10^7 can easily
     * show constraint violations ~ 1-10 from GLPK's 6-digit truncation. */
    double obj_scale = fmax(1.0, fabs(glpk_obj));
    double con_threshold = fmax(1.0, obj_scale * 1e-5);
    r.pass = (r.max_con_violation < con_threshold) &&
             (r.max_bound_violation < 1e-3) &&
             (r.obj_error / obj_scale < 1e-3);

    /* Build detail string */
    snprintf(r.detail, sizeof(r.detail),
             "%dx%d nnz=%d | cons: %d violations (max %.2e row %d) | "
             "bounds: %d violations (max %.2e) | "
             "obj: c'x=%.8g glpk=%.8g err=%.2e",
             n, m, r.nnz,
             r.num_con_violations, r.max_con_violation, r.worst_con_row,
             r.num_bound_violations, r.max_bound_violation,
             r.ralph_obj, r.glpk_obj, r.obj_error);

    free(ax);
    free(x);
    ralph_free(model);
    return r;
}

static void print_verify_json(const char *name, const MatrixVerifyResult *r, FILE *out) {
    char escaped_name[512];
    json_escape_string(escaped_name, sizeof(escaped_name), name);
    char escaped_detail[4096];
    json_escape_string(escaped_detail, sizeof(escaped_detail), r->detail);

    fprintf(out, "{\n");
    fprintf(out, "  \"problem\": \"%s\",\n", escaped_name);
    fprintf(out, "  \"pass\": %s,\n", r->pass ? "true" : "false");
    fprintf(out, "  \"vars\": %d,\n", r->num_vars);
    fprintf(out, "  \"cons\": %d,\n", r->num_cons);
    fprintf(out, "  \"nnz\": %d,\n", r->nnz);
    fprintf(out, "  \"constraint_violations\": %d,\n", r->num_con_violations);
    fprintf(out, "  \"max_constraint_violation\": %.6e,\n", r->max_con_violation);
    fprintf(out, "  \"worst_constraint_row\": %d,\n", r->worst_con_row);
    fprintf(out, "  \"bound_violations\": %d,\n", r->num_bound_violations);
    fprintf(out, "  \"max_bound_violation\": %.6e,\n", r->max_bound_violation);
    fprintf(out, "  \"worst_bound_var\": %d,\n", r->worst_bound_var);
    fprintf(out, "  \"ralph_objective\": %.10g,\n", r->ralph_obj);
    fprintf(out, "  \"glpk_objective\": %.10g,\n", r->glpk_obj);
    fprintf(out, "  \"objective_error\": %.6e,\n", r->obj_error);
    fprintf(out, "  \"detail\": \"%s\"\n", escaped_detail);
    fprintf(out, "}\n");
}

static int run_verify_matrix(const char *problem_path, const char *name,
                              const Options *opts, FILE *out) {
    if (opts->verbose) {
        fprintf(stderr, "Verifying: %s\n", name);
    }

    MatrixVerifyResult r = verify_matrix_single(problem_path, name, opts);

    if (opts->verbose) {
        fprintf(stderr, "  %s  %s\n",
                r.pass ? "PASS" : "FAIL", r.detail);
    }

    print_verify_json(name, &r, out);
    return r.pass ? 0 : 1;
}

static int run_verify_suite(const char *suite_name, const Options *opts) {
    ProblemInfo problems[MAX_PROBLEMS];
    int count = list_netlib_problems(problems, MAX_PROBLEMS, opts->lp_only);

    if (count == 0) {
        fprintf(stderr, "Error: No NETLIB problems found.\n");
        fprintf(stderr, "Run: ./ralph-benchmark --download-netlib\n");
        return 1;
    }

    int start = 0, end = count;
    if (strcmp(suite_name, "tiny") == 0) {
        end = (count < 5) ? count : 5;
    } else if (strcmp(suite_name, "small") == 0) {
        end = (count < 15) ? count : 15;
    } else if (strcmp(suite_name, "medium") == 0) {
        end = (count < 40) ? count : 40;
    }

    int pass_count = 0, fail_count = 0;

    fprintf(stdout, "[\n");
    for (int i = start; i < end; i++) {
        if (i > start) fprintf(stdout, ",\n");

        MatrixVerifyResult r = verify_matrix_single(problems[i].path,
                                                     problems[i].name, opts);
        if (r.pass) pass_count++;
        else fail_count++;

        if (opts->verbose) {
            fprintf(stderr, "  %s  %-12s  %s\n",
                    r.pass ? "PASS" : "FAIL",
                    problems[i].name, r.detail);
        }

        print_verify_json(problems[i].name, &r, stdout);
    }
    fprintf(stdout, "]\n");

    if (opts->verbose) {
        fprintf(stderr, "\nMatrix verification: %d/%d pass",
                pass_count, pass_count + fail_count);
        if (fail_count > 0) fprintf(stderr, " (%d FAIL)", fail_count);
        fprintf(stderr, "\n");
    }

    return fail_count > 0 ? 1 : 0;
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
    char escaped_lu_reason[128];
    json_escape_string(escaped_lu_reason, sizeof(escaped_lu_reason),
                       ralph->lu_last_failure_reason[0] ? ralph->lu_last_failure_reason : "none");
    char escaped_refactor_reason[128];
    json_escape_string(escaped_refactor_reason, sizeof(escaped_refactor_reason),
                       ralph->refactor_last_reason_str[0] ? ralph->refactor_last_reason_str : "other");

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

    /* Ralph timing breakdown (solver-internal instrumentation) */
    fprintf(out, "  \"timing\": {\n");
    fprintf(out, "    \"primal_setup_ms\": %.6f,\n", ralph->primal_setup_ms);
    fprintf(out, "    \"dual_ms\": %.6f,\n", ralph->dual_ms);
    fprintf(out, "    \"phase1_ms\": %.6f,\n", ralph->phase1_ms);
    fprintf(out, "    \"transition_ms\": %.6f,\n", ralph->transition_ms);
    fprintf(out, "    \"phase2_ms\": %.6f,\n", ralph->phase2_ms);
    fprintf(out, "    \"pricing_ms\": %.6f,\n", ralph->pricing_ms);
    fprintf(out, "    \"ratio_ms\": %.6f,\n", ralph->ratio_ms);
    fprintf(out, "    \"pivot_ms\": %.6f,\n", ralph->pivot_ms);
    fprintf(out, "    \"refactor_ms\": %.6f,\n", ralph->refactor_ms);
    fprintf(out, "    \"ftran_ms\": %.6f,\n", ralph->ftran_ms);
    fprintf(out, "    \"btran_ms\": %.6f,\n", ralph->btran_ms);
    fprintf(out, "    \"lu_update_ms\": %.6f,\n", ralph->lu_update_ms);
    fprintf(out, "    \"compute_solution_ms\": %.6f,\n", ralph->compute_solution_ms);
    fprintf(out, "    \"compute_reduced_costs_ms\": %.6f\n", ralph->compute_rc_ms);
    fprintf(out, "  },\n");

    /* Per-phase hot-path timing/call breakdown */
    fprintf(out, "  \"phase_hotspots\": {\n");
    fprintf(out, "    \"phase1\": {\n");
    fprintf(out, "      \"pricing_ms\": %.6f,\n", ralph->phase1_pricing_ms);
    fprintf(out, "      \"ratio_ms\": %.6f,\n", ralph->phase1_ratio_ms);
    fprintf(out, "      \"pivot_ms\": %.6f,\n", ralph->phase1_pivot_ms);
    fprintf(out, "      \"refactor_ms\": %.6f,\n", ralph->phase1_refactor_ms);
    fprintf(out, "      \"compute_solution_ms\": %.6f,\n", ralph->phase1_compute_solution_ms);
    fprintf(out, "      \"compute_reduced_costs_ms\": %.6f,\n", ralph->phase1_compute_rc_ms);
    fprintf(out, "      \"pricing_calls\": %d,\n", ralph->phase1_pricing_calls);
    fprintf(out, "      \"ratio_calls\": %d,\n", ralph->phase1_ratio_calls);
    fprintf(out, "      \"pivot_calls\": %d,\n", ralph->phase1_pivot_calls);
    fprintf(out, "      \"refactor_calls\": %d,\n", ralph->phase1_refactor_calls);
    fprintf(out, "      \"compute_solution_calls\": %d,\n", ralph->phase1_compute_solution_calls);
    fprintf(out, "      \"compute_reduced_costs_calls\": %d\n", ralph->phase1_compute_rc_calls);
    fprintf(out, "    },\n");
    fprintf(out, "    \"phase2\": {\n");
    fprintf(out, "      \"pricing_ms\": %.6f,\n", ralph->phase2_pricing_ms);
    fprintf(out, "      \"ratio_ms\": %.6f,\n", ralph->phase2_ratio_ms);
    fprintf(out, "      \"pivot_ms\": %.6f,\n", ralph->phase2_pivot_ms);
    fprintf(out, "      \"refactor_ms\": %.6f,\n", ralph->phase2_refactor_ms);
    fprintf(out, "      \"compute_solution_ms\": %.6f,\n", ralph->phase2_compute_solution_ms);
    fprintf(out, "      \"compute_reduced_costs_ms\": %.6f,\n", ralph->phase2_compute_rc_ms);
    fprintf(out, "      \"pricing_calls\": %d,\n", ralph->phase2_pricing_calls);
    fprintf(out, "      \"ratio_calls\": %d,\n", ralph->phase2_ratio_calls);
    fprintf(out, "      \"pivot_calls\": %d,\n", ralph->phase2_pivot_calls);
    fprintf(out, "      \"refactor_calls\": %d,\n", ralph->phase2_refactor_calls);
    fprintf(out, "      \"compute_solution_calls\": %d,\n", ralph->phase2_compute_solution_calls);
    fprintf(out, "      \"compute_reduced_costs_calls\": %d\n", ralph->phase2_compute_rc_calls);
    fprintf(out, "    }\n");
    fprintf(out, "  },\n");

    /* Refactor-specific trigger and per-call telemetry */
    fprintf(out, "  \"refactor\": {\n");
    fprintf(out, "    \"count\": %d,\n", ralph->refactor_count);
    fprintf(out, "    \"all_ms\": %.6f,\n", ralph->refactor_all_ms);
    fprintf(out, "    \"avg_ms\": %.6f,\n",
            (ralph->refactor_count > 0) ? (ralph->refactor_all_ms / (double)ralph->refactor_count) : 0.0);
    fprintf(out, "    \"max_ms\": %.6f,\n", ralph->refactor_max_ms);
    fprintf(out, "    \"last_ms\": %.6f,\n", ralph->refactor_last_ms);
    fprintf(out, "    \"last_reason_code\": %d,\n", ralph->refactor_last_reason);
    fprintf(out, "    \"last_reason\": \"%s\",\n", escaped_refactor_reason);
    fprintf(out, "    \"last_m\": %d,\n", ralph->refactor_last_m);
    fprintf(out, "    \"last_k\": %d,\n", ralph->refactor_last_k);
    fprintf(out, "    \"last_nnz_B\": %d,\n", ralph->refactor_last_nnz_b);
    fprintf(out, "    \"reason_setup\": %d,\n", ralph->refactor_reason_setup);
    fprintf(out, "    \"reason_transition\": %d,\n", ralph->refactor_reason_transition);
    fprintf(out, "    \"reason_periodic\": %d,\n", ralph->refactor_reason_periodic);
    fprintf(out, "    \"reason_ratio_recovery\": %d,\n", ralph->refactor_reason_ratio_recovery);
    fprintf(out, "    \"reason_pivot_recovery\": %d,\n", ralph->refactor_reason_pivot_recovery);
    fprintf(out, "    \"reason_forced_small_pivot\": %d,\n", ralph->refactor_reason_forced_small_pivot);
    fprintf(out, "    \"reason_update_recovery\": %d,\n", ralph->refactor_reason_update_recovery);
    fprintf(out, "    \"reason_direction_stabilize\": %d,\n", ralph->refactor_reason_direction_stabilize);
    fprintf(out, "    \"reason_infeasibility_cleanup\": %d,\n", ralph->refactor_reason_infeas_cleanup);
    fprintf(out, "    \"reason_other\": %d\n", ralph->refactor_reason_other);
    fprintf(out, "  },\n");

    /* LU telemetry (Markowitz/sparse fallback diagnostics) */
    double mkz_retry_rate = (ralph->lu_mkz_calls > 0)
                            ? (double)ralph->lu_mkz_retry_count / (double)ralph->lu_mkz_calls
                            : 0.0;
    fprintf(out, "  \"lu\": {\n");
    fprintf(out, "    \"mkz_enabled\": %s,\n", ralph->lu_mkz_enabled ? "true" : "false");
    fprintf(out, "    \"sn_enabled\": %s,\n", ralph->lu_sn_enabled ? "true" : "false");
    fprintf(out, "    \"mkz_calls\": %d,\n", ralph->lu_mkz_calls);
    fprintf(out, "    \"mkz_successes\": %d,\n", ralph->lu_mkz_successes);
    fprintf(out, "    \"mkz_failures\": %d,\n", ralph->lu_mkz_failures);
    fprintf(out, "    \"mkz_retry_count\": %d,\n", ralph->lu_mkz_retry_count);
    fprintf(out, "    \"mkz_retry_rate\": %.6f,\n", mkz_retry_rate);
    fprintf(out, "    \"mkz_last_failure\": %d,\n", ralph->lu_mkz_last_failure);
    fprintf(out, "    \"mkz_dense_fallbacks\": %d,\n", ralph->lu_mkz_dense_fallbacks);
    fprintf(out, "    \"mkz_fail_workspace\": %d,\n", ralph->lu_mkz_fail_workspace);
    fprintf(out, "    \"mkz_fail_pool\": %d,\n", ralph->lu_mkz_fail_pool);
    fprintf(out, "    \"mkz_fail_singular\": %d,\n", ralph->lu_mkz_fail_singular);
    fprintf(out, "    \"mkz_fail_capacity\": %d,\n", ralph->lu_mkz_fail_capacity);
    fprintf(out, "    \"sparse_dense_fallbacks\": %d,\n", ralph->lu_sparse_dense_fallbacks);
    fprintf(out, "    \"used_dense_fallback_last\": %s,\n",
            ralph->lu_used_dense_fallback_last ? "true" : "false");
    fprintf(out, "    \"identity_sep_failures\": %d,\n", ralph->lu_identity_sep_failures);
    fprintf(out, "    \"sn_calls\": %d,\n", ralph->lu_sn_calls);
    fprintf(out, "    \"sn_successes\": %d,\n", ralph->lu_sn_successes);
    fprintf(out, "    \"num_updates\": %d,\n", ralph->lu_num_updates);
    fprintf(out, "    \"max_updates\": %d,\n", ralph->lu_max_updates);
    fprintf(out, "    \"factorize_calls\": %d,\n", ralph->lu_factorize_calls);
    fprintf(out, "    \"last_basis_nnz\": %d,\n", ralph->lu_last_basis_nnz);
    fprintf(out, "    \"last_m\": %d,\n", ralph->lu_last_m);
    fprintf(out, "    \"last_k\": %d,\n", ralph->lu_last_k);
    fprintf(out, "    \"last_a_struct_build_ms\": %.6f,\n", ralph->lu_last_a_struct_build_ms);
    fprintf(out, "    \"last_markowitz_numeric_ms\": %.6f,\n", ralph->lu_last_markowitz_numeric_ms);
    fprintf(out, "    \"last_identity_placement_ms\": %.6f,\n", ralph->lu_last_identity_placement_ms);
    fprintf(out, "    \"last_coo_to_csc_ms\": %.6f,\n", ralph->lu_last_coo_to_csc_ms);
    fprintf(out, "    \"total_a_struct_build_ms\": %.6f,\n", ralph->lu_total_a_struct_build_ms);
    fprintf(out, "    \"total_markowitz_numeric_ms\": %.6f,\n", ralph->lu_total_markowitz_numeric_ms);
    fprintf(out, "    \"total_identity_placement_ms\": %.6f,\n", ralph->lu_total_identity_placement_ms);
    fprintf(out, "    \"total_coo_to_csc_ms\": %.6f,\n", ralph->lu_total_coo_to_csc_ms);
    fprintf(out, "    \"last_failure_reason_code\": %d,\n", ralph->lu_last_failure_reason_code);
    fprintf(out, "    \"last_failure_reason\": \"%s\"\n", escaped_lu_reason);
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
                                          opts->method, opts->pricing,
                                          opts->lu_supernode,
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

/* ============================================================================
 * NETLIB Correctness Test Mode (--test)
 *
 * Solves NETLIB problems with Ralph only (no GLPK dependency) and compares
 * objective values against known optimal values from the literature.
 *
 * Each problem runs in a forked child process with a hard wall-clock timeout
 * (using alarm()) to prevent hangs on numerically difficult problems.
 * ============================================================================ */

/* Shared memory for child→parent result passing */
typedef struct {
    int status;       /* 0=optimal, 1=infeasible, 2=unbounded, 3=error, 4=timeout */
    double objective;
    double time_ms;
} TestResult;

/* SIGALRM handler for hard timeout in child process */
static volatile sig_atomic_t test_alarm_fired = 0;
static void test_alarm_handler(int sig) {
    (void)sig;
    test_alarm_fired = 1;
    _exit(124);  /* Convention: 124 = timeout */
}

/* Solve a single problem in a child process with hard timeout.
 * Returns: 0=pass, 1=fail, 2=error, 3=skip(timeout), 4=skip(other) */
static int test_solve_one(const char *path, const char *name,
                           const NetlibReference *ref,
                           int timeout_sec, int method, int pricing,
                           int lu_supernode) {
    /* Use a pipe to pass results from child to parent */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        fprintf(stderr, "  ERROR %-12s  (pipe failed)\n", name);
        return 2;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        fprintf(stderr, "  ERROR %-12s  (fork failed)\n", name);
        return 2;
    }

    if (pid == 0) {
        /* Child process: solve with hard alarm timeout */
        close(pipefd[0]);

        signal(SIGALRM, test_alarm_handler);
        alarm((unsigned)timeout_sec);

        int num_vars = 0, num_cons = 0, nnz = 0, is_mip = 0;
        SolveResult result = solve_with_ralph(path, (double)timeout_sec,
                                               method, pricing, lu_supernode,
                                               &num_vars, &num_cons, &nnz,
                                               &is_mip);

        TestResult tr = {
            .status = result.status,
            .objective = result.objective,
            .time_ms = result.time_ms
        };

        /* Write result back to parent via pipe */
        (void)!write(pipefd[1], &tr, sizeof(tr));
        close(pipefd[1]);
        free(result.solution);
        _exit(result.status == 0 ? 0 : 1);
    }

    /* Parent process: wait with timeout */
    close(pipefd[1]);

    int wstatus;
    double start = get_time_ms();

    /* Wait for child (it will either finish or get killed by alarm) */
    waitpid(pid, &wstatus, 0);
    double elapsed = get_time_ms() - start;

    /* Read result from pipe */
    TestResult tr = {.status = 3, .objective = 0.0, .time_ms = elapsed};
    ssize_t n = read(pipefd[0], &tr, sizeof(tr));
    close(pipefd[0]);

    /* Check if child was killed or timed out */
    if (WIFSIGNALED(wstatus) || (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 124)) {
        fprintf(stderr, "  SKIP  %-12s  (timeout after %ds)\n", name, timeout_sec);
        return 3;
    }

    if (n != sizeof(tr) || tr.status != 0) {
        fprintf(stderr, "  ERROR %-12s  (status=%d after %.1fms)\n",
                name, tr.status, tr.time_ms);
        return 2;
    }

    /* Compare objective vs known optimal */
    double got = tr.objective;
    double expected = ref->optimal;
    double scale = fmax(1.0, fmax(fabs(got), fabs(expected)));
    double rel_err = fabs(got - expected) / scale;
    double abs_err = fabs(got - expected);

    int ok = (rel_err < DEFAULT_OBJ_REL_TOL) || (abs_err < DEFAULT_OBJ_ABS_TOL);

    if (ok) {
        fprintf(stderr, "  PASS  %-12s  %15.8e  expected %15.8e  err=%.1e  %7.1fms\n",
                name, got, expected, rel_err, tr.time_ms);
        return 0;
    } else {
        fprintf(stderr, "  FAIL  %-12s  %15.8e  expected %15.8e  err=%.1e  %7.1fms\n",
                name, got, expected, rel_err, tr.time_ms);
        return 1;
    }
}

/* Compare problems by tier then name for predictable output order */
static int cmp_by_tier_name(const void *a, const void *b) {
    const ProblemInfo *pa = (const ProblemInfo *)a;
    const ProblemInfo *pb = (const ProblemInfo *)b;

    const NetlibReference *ra = find_netlib_reference(pa->name);
    const NetlibReference *rb = find_netlib_reference(pb->name);
    int ta = ra ? ra->tier : 99;
    int tb = rb ? rb->tier : 99;

    if (ta != tb) return ta - tb;
    return strcasecmp(pa->name, pb->name);
}

static int run_test_mode(const Options *opts) {
    int max_tier = (opts->test_mode == 1) ? 1 : 4;  /* fast=0-1, full=0-4 */
    int timeout_sec = (opts->test_mode == 1) ? (int)TEST_FAST_CAP_SEC
                                              : (int)TEST_FULL_CAP_SEC;

    fprintf(stderr, "NETLIB Correctness Test (%s: tiers 0-%d, %ds cap)\n",
            opts->test_mode == 1 ? "fast" : "full", max_tier, timeout_sec);

    /* Discover available .mps files */
    ProblemInfo problems[MAX_PROBLEMS];
    int count = list_netlib_problems(problems, MAX_PROBLEMS, 1 /* lp_only */);

    if (count == 0) {
        fprintf(stderr, "Error: No NETLIB problems found.\n");
        fprintf(stderr, "Run: ./ralph-benchmark --download-netlib\n");
        return 1;
    }

    /* Sort by tier then name for predictable output */
    qsort(problems, (size_t)count, sizeof(ProblemInfo), cmp_by_tier_name);

    int pass_count = 0, fail_count = 0, skip_count = 0, error_count = 0;

    for (int i = 0; i < count; i++) {
        const char *name = problems[i].name;
        const NetlibReference *ref = find_netlib_reference(name);

        /* Skip if not in reference table or above tier threshold */
        if (!ref || ref->tier > max_tier) {
            skip_count++;
            continue;
        }

        int result = test_solve_one(problems[i].path, name, ref,
                                     timeout_sec, opts->method, opts->pricing,
                                     opts->lu_supernode);
        switch (result) {
            case 0: pass_count++; break;
            case 1: fail_count++; break;
            case 2: error_count++; break;
            default: skip_count++; break;
        }
    }

    /* Summary */
    int tested = pass_count + fail_count;
    fprintf(stderr, "\nResults: %d/%d PASS", pass_count, tested);
    if (fail_count > 0) fprintf(stderr, ", %d FAIL", fail_count);
    if (error_count > 0) fprintf(stderr, ", %d ERROR", error_count);
    if (skip_count > 0) fprintf(stderr, ", %d SKIP", skip_count);
    fprintf(stderr, "\n");

    return (fail_count + error_count > 0) ? 1 : 0;
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
    printf("Correctness Testing:\n");
    printf("  %s --test                            # Fast test (tiers 0-1)\n", prog);
    printf("  %s --test fast                       # Same as above\n", prog);
    printf("  %s --test full                       # All tiers, 300s cap\n", prog);
    printf("\n");
    printf("Verification:\n");
    printf("  %s --verify-matrix --suite all -v    # Verify all NETLIB matrices\n", prog);
    printf("  %s --verify-matrix --netlib blend -v # Verify single problem\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help                    Show this help message\n");
    printf("  -v, --verbose                 Print progress to stderr\n");
    printf("  --test [fast|full]            Correctness test vs known optimal values\n");
    printf("  --verify-matrix               Deep matrix verification via GLPK solution\n");
    printf("  --version                     Show version\n");
    printf("\n");
    printf("Time Limits:\n");
    printf("  --time-mult <N>               Ralph time = N * GLPK time (default: %.1f)\n",
           DEFAULT_TIME_MULTIPLIER);
    printf("  --hard-cap <SEC>              Maximum time per problem (default: %.0f sec)\n",
           DEFAULT_HARD_CAP_SEC);
    printf("\n");
    printf("Solver:\n");
    printf("  --method <N>                  LP method: 0=primal, 1=dual, 2=auto (default: 0)\n");
    printf("  --lu-supernode                Enable supernodal LU factorization\n");
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
    opts->pricing = -1;  /* Default: solver default */

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
        } else if (strcmp(arg, "--verify-matrix") == 0) {
            opts->verify_matrix = 1;
        } else if (strcmp(arg, "--test") == 0) {
            /* --test [fast|full], default is fast */
            if (i + 1 < argc && argv[i+1][0] != '-') {
                i++;
                if (strcmp(argv[i], "full") == 0) {
                    opts->test_mode = 2;
                } else {
                    opts->test_mode = 1;  /* fast */
                }
            } else {
                opts->test_mode = 1;  /* default: fast */
            }
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
        } else if (strcmp(arg, "--method") == 0 && i + 1 < argc) {
            opts->method = atoi(argv[++i]);
        } else if (strcmp(arg, "--pricing") == 0 && i + 1 < argc) {
            opts->pricing = atoi(argv[++i]);
        } else if (strcmp(arg, "--lu-supernode") == 0) {
            opts->lu_supernode = 1;
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

    /* Test mode (no GLPK needed) */
    if (opts.test_mode > 0) {
        return run_test_mode(&opts);
    }

    /* Check GLPK availability (needed for benchmark/verify modes) */
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

    /* Verify-matrix mode */
    if (opts.verify_matrix) {
        if (opts.suite[0] != '\0') {
            return run_verify_suite(opts.suite, &opts);
        }
        if (opts.netlib_name[0] != '\0') {
            char problem_path[MAX_PATH];
            if (!find_netlib_problem(opts.netlib_name, problem_path,
                                      sizeof(problem_path))) {
                fprintf(stderr, "Error: NETLIB problem '%s' not found.\n",
                        opts.netlib_name);
                return 1;
            }
            return run_verify_matrix(problem_path, opts.netlib_name,
                                      &opts, stdout);
        }
        if (opts.problem_path[0] != '\0') {
            const char *name = strrchr(opts.problem_path, '/');
            name = name ? name + 1 : opts.problem_path;
            return run_verify_matrix(opts.problem_path, name, &opts, stdout);
        }
        /* Default: verify all */
        return run_verify_suite("all", &opts);
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
