/*
 * FuelWise benchmark gate for Ralph-vs-GLPK MILP matrix runs.
 */

#include "fw_bench.h"
#include "fw_refuel.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
    const char *scenario;
    int runs;
    double slowdown_warn;
} FWGateScenario;

typedef struct {
    const char *artifact_dir;
    int compare_raw;
    int strict_runtime;
    int verbose;
} FWGateOptions;

static const FWGateScenario g_matrix[] = {
    {"milp30", 5, 8.0},
    {"milp50", 5, 10.0},
    {"milp75", 5, 20.0},
    {"milp100", 5, 35.0},
    {"milp200", 3, 80.0}
};

static const uint64_t g_seeds[] = {42u, 123u};

static void print_usage(const char *prog)
{
    printf("FuelWise Benchmark Gate\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --artifact-dir DIR  Output directory for JSON artifacts\n");
    printf("                      (default: /tmp/fuelwise-benchmark-gate)\n");
    printf("  --compare-raw       Disable FuelWise MILP hints so Ralph solves the raw model\n");
    printf("  --strict-runtime    Fail on runtime warnings as well as parity failures\n");
    printf("  --verbose           Print per-entry status lines\n");
    printf("  --help              Show this help\n");
}

static int parse_args(int argc, char **argv, FWGateOptions *opts)
{
    if (!opts) return -1;

    opts->artifact_dir = "/tmp/fuelwise-benchmark-gate";
    opts->compare_raw = 0;
    opts->strict_runtime = 0;
    opts->verbose = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--artifact-dir") == 0 && i + 1 < argc) {
            opts->artifact_dir = argv[++i];
        } else if (strcmp(argv[i], "--compare-raw") == 0) {
            opts->compare_raw = 1;
        } else if (strcmp(argv[i], "--strict-runtime") == 0) {
            opts->strict_runtime = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            opts->verbose = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    return 0;
}

static int ensure_dir(const char *path)
{
    char tmp[1024];
    size_t len;

    if (!path) return -1;
    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;

    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void write_summary_header(FILE *out, const FWGateOptions *opts)
{
    fprintf(out, "{\n");
    fprintf(out, "  \"mode\": \"%s\",\n", opts->compare_raw ? "raw" : "default");
    fprintf(out, "  \"strict_runtime\": %s,\n", opts->strict_runtime ? "true" : "false");
    fprintf(out, "  \"entries\": [\n");
}

static void write_summary_entry(
    FILE *out,
    const FWGateScenario *gate,
    uint64_t seed,
    const char *artifact_name,
    const FWBenchResults *results,
    double slowdown,
    int parity_ok,
    int runtime_warn,
    int first_entry)
{
    if (!first_entry) fprintf(out, ",\n");
    fprintf(out, "    {\n");
    fprintf(out, "      \"scenario\": \"%s\",\n", gate->scenario);
    fprintf(out, "      \"seed\": %llu,\n", (unsigned long long)seed);
    fprintf(out, "      \"runs\": %d,\n", gate->runs);
    fprintf(out, "      \"artifact\": \"%s\",\n", artifact_name);
    fprintf(out, "      \"parity_ok\": %s,\n", parity_ok ? "true" : "false");
    fprintf(out, "      \"runtime_warning\": %s,\n", runtime_warn ? "true" : "false");
    fprintf(out, "      \"ralph_solved\": %d,\n", results->num_solved);
    fprintf(out, "      \"ralph_feasible\": %d,\n", results->num_feasible);
    fprintf(out, "      \"glpk_solved\": %d,\n", results->glpk_num_solved);
    fprintf(out, "      \"objective_match\": %d,\n", results->glpk_num_match);
    fprintf(out, "      \"ralph_solve_time_ms_avg\": %.2f,\n", results->solve_time_avg);
    fprintf(out, "      \"glpk_solve_time_ms_avg\": %.2f,\n", results->glpk_solve_time_avg);
    fprintf(out, "      \"ralph_over_glpk\": %.3f\n", slowdown);
    fprintf(out, "    }");
}

static void write_summary_footer(
    FILE *out,
    int parity_failures,
    int runtime_warnings,
    int runtime_failures)
{
    fprintf(out, "\n  ],\n");
    fprintf(out, "  \"summary\": {\n");
    fprintf(out, "    \"parity_failures\": %d,\n", parity_failures);
    fprintf(out, "    \"runtime_warnings\": %d,\n", runtime_warnings);
    fprintf(out, "    \"runtime_failures\": %d\n", runtime_failures);
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
}

int main(int argc, char **argv)
{
    FWGateOptions opts;
    FILE *summary = NULL;
    char summary_path[1024];
    int parity_failures = 0;
    int runtime_warnings = 0;
    int runtime_failures = 0;
    int first_entry = 1;

    if (parse_args(argc, argv, &opts) != 0) return 1;

    if (ensure_dir(opts.artifact_dir) != 0) {
        fprintf(stderr, "Could not create artifact dir: %s\n", opts.artifact_dir);
        return 1;
    }

    snprintf(summary_path, sizeof(summary_path), "%s/%s_summary.json",
             opts.artifact_dir, opts.compare_raw ? "raw" : "default");
    summary = fopen(summary_path, "w");
    if (!summary) {
        fprintf(stderr, "Could not open summary artifact: %s\n", summary_path);
        return 1;
    }
    write_summary_header(summary, &opts);

    fw_set_presolve(1, 0x100F);

    for (size_t i = 0; i < sizeof(g_matrix) / sizeof(g_matrix[0]); ++i) {
        for (size_t j = 0; j < sizeof(g_seeds) / sizeof(g_seeds[0]); ++j) {
            FWBenchConfig cfg;
            FWBenchResults results;
            char artifact_name[256];
            char artifact_path[1024];
            FILE *artifact;
            int parity_ok;
            int runtime_warn = 0;
            double slowdown = 0.0;

            if (fw_bench_get_config(g_matrix[i].scenario, g_seeds[j], &cfg) != 0) {
                fprintf(stderr, "Unknown scenario in matrix: %s\n", g_matrix[i].scenario);
                parity_failures++;
                continue;
            }

            cfg.glpk_compare = 1;
            cfg.compare_raw = opts.compare_raw;

            if (fw_bench_run(&cfg, g_matrix[i].runs, FW_SOLVER_MILP, &results) != 0) {
                fprintf(stderr, "Benchmark run failed for %s seed %llu\n",
                        g_matrix[i].scenario, (unsigned long long)g_seeds[j]);
                parity_failures++;
                continue;
            }

            snprintf(artifact_name, sizeof(artifact_name), "%s_seed%llu_%s.json",
                     g_matrix[i].scenario, (unsigned long long)g_seeds[j],
                     opts.compare_raw ? "raw" : "default");
            snprintf(artifact_path, sizeof(artifact_path), "%s/%s",
                     opts.artifact_dir, artifact_name);

            artifact = fopen(artifact_path, "w");
            if (!artifact) {
                fprintf(stderr, "Could not open artifact file: %s\n", artifact_path);
                parity_failures++;
                continue;
            }
            fw_bench_write_json(artifact, &cfg, &results, g_matrix[i].scenario);
            fclose(artifact);

            parity_ok =
                (results.num_solved == g_matrix[i].runs) &&
                (results.num_feasible == g_matrix[i].runs) &&
                (results.glpk_num_solved == g_matrix[i].runs) &&
                (results.glpk_num_match == g_matrix[i].runs);
            if (!parity_ok) parity_failures++;

            if (results.glpk_solve_time_avg > 1e-9) {
                slowdown = results.solve_time_avg / results.glpk_solve_time_avg;
                if (slowdown > g_matrix[i].slowdown_warn) {
                    runtime_warn = 1;
                    runtime_warnings++;
                    if (opts.strict_runtime) runtime_failures++;
                }
            }

            write_summary_entry(summary, &g_matrix[i], g_seeds[j], artifact_name,
                                &results, slowdown, parity_ok, runtime_warn, first_entry);
            first_entry = 0;

            if (opts.verbose) {
                printf("%s seed=%llu runs=%d match=%d/%d ralph=%.2fms glpk=%.2fms slowdown=%.2fx%s\n",
                       g_matrix[i].scenario,
                       (unsigned long long)g_seeds[j],
                       g_matrix[i].runs,
                       results.glpk_num_match,
                       g_matrix[i].runs,
                       results.solve_time_avg,
                       results.glpk_solve_time_avg,
                       slowdown,
                       runtime_warn ? " WARN" : "");
            }
        }
    }

    write_summary_footer(summary, parity_failures, runtime_warnings, runtime_failures);
    fclose(summary);

    if (parity_failures > 0) {
        fprintf(stderr, "FuelWise benchmark gate failed: %d parity failures\n", parity_failures);
        fprintf(stderr, "Artifacts: %s\n", opts.artifact_dir);
        return 1;
    }
    if (runtime_failures > 0) {
        fprintf(stderr, "FuelWise benchmark gate failed: %d strict runtime failures\n", runtime_failures);
        fprintf(stderr, "Artifacts: %s\n", opts.artifact_dir);
        return 1;
    }

    if (runtime_warnings > 0) {
        fprintf(stderr, "FuelWise benchmark gate passed with %d runtime warnings\n", runtime_warnings);
    }
    printf("FuelWise benchmark gate passed. Artifacts: %s\n", opts.artifact_dir);
    return 0;
}
