/*
 * FuelWise Benchmark CLI
 *
 * Command-line interface for running FuelWise validation benchmarks.
 *
 * Usage:
 *   fuelwise-bench [options]
 *
 * Options:
 *   --scenario NAME  Run specific scenario (urban, highway, long, tight)
 *   --runs N         Number of runs per scenario (default: 100)
 *   --seed N         Random seed (default: time-based)
 *   --milp           Use MILP solver (default: LP)
 *   --json           Output as JSON
 *   --verbose        Print per-run details
 *   --all            Run all scenarios
 *   --help           Show this help
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "fw_bench.h"
#include "fw_refuel.h"  /* For fw_set_presolve */
#include "sh_args.h"    /* For sh_parse_int */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(const char *prog)
{
    printf("FuelWise Validation Benchmark\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --scenario NAME  Run specific scenario:\n");
    printf("                     urban     - 1500km (3 days), dense stations\n");
    printf("                     highway   - 3000km (5 days), cross-country (default)\n");
    printf("                     long      - 5000km (7+ days), transcontinental\n");
    printf("                     tight     - 2500km, sparse infrastructure, tight margins\n");
    printf("                     us        - 2000mi US interstate, Class 8 (imperial)\n");
    printf("                     benders30 - ~30 stations, Benders scalability test\n");
    printf("                     benders50 - ~50 stations, Benders scalability test\n");
    printf("                     benders100- ~100 stations, Benders stress test\n");
    printf("                     milp15    - ~15 stations, MIP sanity (sub-ms)\n");
    printf("                     milp30    - ~30 stations, MIP with high stop cost\n");
    printf("                     milp50    - ~50 stations, MIP high price variance\n");
    printf("                     milp75    - ~75 stations, MIP tight tank (reach cuts)\n");
    printf("                     milp100   - ~100 stations, MIP scalability\n");
    printf("                     milp200   - ~200 stations, MIP stress test\n");
    printf("  --runs N         Number of runs per scenario (default: 100)\n");
    printf("  --seed N         Random seed (default: time-based)\n");
    printf("  --milp           Use MILP solver (default: LP)\n");
    printf("  --benders        Use Benders decomposition solver\n");
    printf("  --glpk           Compare against GLPK (requires glpsol)\n");
    printf("  --presolve       Enable Ralph presolve for MILP\n");
    printf("  --presolve-mask N  Presolve technique bitmask (hex, default 0xFFFF=all)\n");
    printf("  --json           Output as JSON\n");
    printf("  --verbose        Print per-run details\n");
    printf("  --all            Run all scenarios\n");
    printf("  --help           Show this help\n");
}

typedef struct {
    const char *scenario;
    int num_runs;
    uint64_t seed;
    FWSolverType solver_type;
    int glpk_compare;
    int presolve;       /* 1=explicitly enable, -1=explicitly disable, 0=default */
    unsigned int presolve_mask;
    int as_json;
    int verbose;
    int run_all;
} BenchOptions;

static int parse_args(int argc, char **argv, BenchOptions *opts)
{
    opts->scenario = "highway";
    opts->num_runs = 100;
    opts->seed = 0;
    opts->solver_type = FW_SOLVER_LP;
    opts->glpk_compare = 0;
    opts->presolve = 0;
    opts->presolve_mask = 0x110F;  /* Lightweight: matches fw_refuel.c default */
    opts->as_json = 0;
    opts->verbose = 0;
    opts->run_all = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            opts->scenario = argv[++i];
        } else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            opts->num_runs = sh_parse_int(argv[++i], 100, 1, 10000);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            opts->seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--milp") == 0) {
            opts->solver_type = FW_SOLVER_MILP;
        } else if (strcmp(argv[i], "--benders") == 0) {
            opts->solver_type = FW_SOLVER_BENDERS;
        } else if (strcmp(argv[i], "--glpk") == 0) {
            opts->glpk_compare = 1;
        } else if (strcmp(argv[i], "--presolve") == 0) {
            opts->presolve = 1;
        } else if (strcmp(argv[i], "--no-presolve") == 0) {
            opts->presolve = -1;
        } else if (strcmp(argv[i], "--presolve-mask") == 0 && i + 1 < argc) {
            opts->presolve_mask = (unsigned int)strtoul(argv[++i], NULL, 0);
            opts->presolve = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            opts->as_json = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            opts->verbose = 1;
        } else if (strcmp(argv[i], "--all") == 0) {
            opts->run_all = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    return 0;
}

static FWBenchConfig get_config(const char *scenario, uint64_t seed)
{
    FWBenchConfig cfg;

    if (strcmp(scenario, "urban") == 0) {
        cfg = fw_bench_config_short_urban();
    } else if (strcmp(scenario, "highway") == 0) {
        cfg = fw_bench_config_highway();
    } else if (strcmp(scenario, "long") == 0) {
        cfg = fw_bench_config_long_haul();
    } else if (strcmp(scenario, "tight") == 0) {
        cfg = fw_bench_config_tight_margins();
    } else if (strcmp(scenario, "us") == 0) {
        cfg = fw_bench_config_us_interstate();
    } else if (strcmp(scenario, "benders30") == 0) {
        cfg = fw_bench_config_benders_30();
    } else if (strcmp(scenario, "benders50") == 0) {
        cfg = fw_bench_config_benders_50();
    } else if (strcmp(scenario, "benders100") == 0) {
        cfg = fw_bench_config_benders_100();
    } else if (strcmp(scenario, "milp15") == 0) {
        cfg = fw_bench_config_milp_15();
    } else if (strcmp(scenario, "milp30") == 0) {
        cfg = fw_bench_config_milp_30();
    } else if (strcmp(scenario, "milp50") == 0) {
        cfg = fw_bench_config_milp_50();
    } else if (strcmp(scenario, "milp75") == 0) {
        cfg = fw_bench_config_milp_75();
    } else if (strcmp(scenario, "milp100") == 0) {
        cfg = fw_bench_config_milp_100();
    } else if (strcmp(scenario, "milp200") == 0) {
        cfg = fw_bench_config_milp_200();
    } else {
        fprintf(stderr, "Unknown scenario: %s\n", scenario);
        cfg = fw_bench_config_highway();
    }

    if (seed != 0) {
        cfg.seed = seed;
    }

    return cfg;
}

static int run_scenario(
    const char *scenario,
    const BenchOptions *opts)
{
    FWBenchConfig cfg = get_config(scenario, opts->seed);
    cfg.glpk_compare = opts->glpk_compare;
    FWBenchResults results;

    if (opts->verbose && !opts->as_json) {
        const char *solver_name = "LP";
        if (opts->solver_type == FW_SOLVER_MILP) solver_name = "MILP";
        else if (opts->solver_type == FW_SOLVER_BENDERS) solver_name = "Benders";
        printf("Running %s scenario (%d runs, %s solver)...\n",
               scenario, opts->num_runs, solver_name);
    }

    int rc = fw_bench_run(&cfg, opts->num_runs, opts->solver_type, &results);
    if (rc != 0) {
        fprintf(stderr, "Benchmark failed for scenario: %s\n", scenario);
        return -1;
    }

    fw_bench_print_results(&cfg, &results, scenario, opts->as_json);

    /* Return non-zero if any validation failures */
    if (results.num_feasible < results.num_solved) {
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    BenchOptions opts;
    if (parse_args(argc, argv, &opts) != 0) {
        return 1;
    }

    /* Default seed from time if not specified */
    if (opts.seed == 0) {
        opts.seed = (uint64_t)time(NULL);
    }

    /* Configure presolve */
    if (opts.presolve == 1) {
        fw_set_presolve(1, opts.presolve_mask);
    } else if (opts.presolve == -1) {
        fw_set_presolve(0, 0);
    }

    int failures = 0;

    if (opts.run_all) {
        const char *scenarios[] = {
            "urban", "highway", "long", "tight", "us",
            "milp15", "milp30", "milp50", "milp75", "milp100"
        };
        int num_scenarios = sizeof(scenarios) / sizeof(scenarios[0]);

        if (opts.as_json) {
            printf("[\n");
        }

        for (int i = 0; i < num_scenarios; i++) {
            if (opts.as_json && i > 0) {
                printf(",\n");
            }
            if (run_scenario(scenarios[i], &opts) != 0) {
                failures++;
            }
        }

        if (opts.as_json) {
            printf("]\n");
        }
    } else {
        if (run_scenario(opts.scenario, &opts) != 0) {
            failures++;
        }
    }

    return failures > 0 ? 1 : 0;
}
