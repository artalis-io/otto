/*
 * FuelWise Benchmark Driver
 *
 * Runs benchmark iterations and collects statistics.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "sh_pal.h"
#include "fw_bench.h"
#include "fuelwise.h"
#include "sh_units.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* GLPK comparison helper (fw_glpk.c) */
typedef struct {
    int solved;
    double objective;
    double solve_time_ms;
} FWGlpkResult;
int fw_glpk_solve(const FWRefuelProblem *problem, FWGlpkResult *result);

/* ============================================================================
 * Timing Utilities
 * ============================================================================ */

static double get_time_ms(void)
{
    return (double)sh_monotonic_ns() / 1.0e6;
}

int fw_bench_get_config(const char *scenario, uint64_t seed, FWBenchConfig *out)
{
    FWBenchConfig cfg;

    if (!scenario || !out) return -1;

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
        return -1;
    }

    if (seed != 0) {
        cfg.seed = seed;
    }

    *out = cfg;
    return 0;
}

/* ============================================================================
 * Benchmark Driver
 * ============================================================================ */

int fw_bench_run(
    const FWBenchConfig *config,
    int num_runs,
    FWSolverType solver_type,
    FWBenchResults *results)
{
    if (!config || !results || num_runs <= 0) return -1;

    memset(results, 0, sizeof(FWBenchResults));
    results->num_runs = num_runs;
    results->solve_time_min = 1e9;
    results->cost_min = 1e9;

    /* GLPK comparison tracking */
    int glpk_compare = config->glpk_compare;
    if (glpk_compare) {
        results->glpk_enabled = 1;
        results->glpk_solve_time_min = 1e9;
    }
    double total_glpk_time = 0;
    double total_speedup = 0;
    double total_gap_pct = 0;

    double total_solve_time = 0;
    double total_validate_time = 0;
    double total_cost = 0;
    double total_stops = 0;

    /* Create mutable config for seed iteration */
    FWBenchConfig cfg = *config;

    for (int i = 0; i < num_runs; i++) {
        /* Update seed for each run */
        if (config->seed == 0) {
            cfg.seed = (uint64_t)(i + 1) * 0x9E3779B97F4A7C15ULL;
        } else {
            cfg.seed = config->seed + (uint64_t)i;
        }

        /* Generate problem */
        FWBenchInstance instance;
        if (fw_bench_generate(&cfg, &instance) != 0) {
            results->num_errors++;
            continue;
        }

        /* Solve */
        FWRefuelSolution solution;
        memset(&solution, 0, sizeof(solution));
        int restore_hint_flags = 0;
        int previous_hint_flags = 0;

        double t0 = get_time_ms();

        /* Select solver based on type and problem characteristics */
        FWSolverType effective_solver = solver_type;

        /* Force MILP/Benders if min_purchase requires binary decisions */
        if (effective_solver == FW_SOLVER_LP && instance.problem.min_purchase > 0.01) {
            effective_solver = FW_SOLVER_MILP;
        }

        if (effective_solver == FW_SOLVER_MILP && config->compare_raw) {
            previous_hint_flags = fw_get_mip_hint_flags();
            fw_set_mip_hint_flags(FW_HINT_NONE);
            restore_hint_flags = 1;
        }

        int rc;
        switch (effective_solver) {
            case FW_SOLVER_BENDERS:
                rc = fw_solve_refuel_benders(&instance.problem, &solution);
                break;
            case FW_SOLVER_MILP:
                rc = fw_solve_refuel_milp(&instance.problem, &solution);
                break;
            case FW_SOLVER_LP:
            default:
                rc = fw_solve_refuel_lp(&instance.problem, &solution);
                break;
        }

        if (restore_hint_flags) {
            fw_set_mip_hint_flags(previous_hint_flags);
        }

        double solve_time = get_time_ms() - t0;

        if (rc != 0 || solution.status != FW_STATUS_OPTIMAL) {
            if (solution.status == FW_STATUS_INFEASIBLE) {
                results->num_infeasible++;
            } else {
                results->num_errors++;
            }
            fw_free_solution(&solution);
            fw_bench_free_instance(&instance);
            continue;
        }

        results->num_solved++;
        total_solve_time += solve_time;

        if (solve_time < results->solve_time_min) {
            results->solve_time_min = solve_time;
        }
        if (solve_time > results->solve_time_max) {
            results->solve_time_max = solve_time;
        }

        /* Validate */
        FWValidationResult vresult;
        double t1 = get_time_ms();

        int feasible = fw_validate_solution(
            &instance.problem,
            &solution,
            instance.curve,
            instance.weight_profile,
            &vresult
        );

        double validate_time = get_time_ms() - t1;
        total_validate_time += validate_time;

        if (feasible) {
            results->num_feasible++;
            results->num_optimal++;

            total_cost += solution.total_cost;
            total_stops += solution.num_stops;

            if (solution.total_cost < results->cost_min) {
                results->cost_min = solution.total_cost;
            }
            if (solution.total_cost > results->cost_max) {
                results->cost_max = solution.total_cost;
            }
        }

        /* GLPK comparison (if enabled and Ralph solved successfully) */
        if (glpk_compare && feasible) {
            FWGlpkResult glpk_result;
            if (fw_glpk_solve(&instance.problem, &glpk_result) == 0 &&
                glpk_result.solved) {
                results->glpk_num_solved++;
                total_glpk_time += glpk_result.solve_time_ms;

                if (glpk_result.solve_time_ms < results->glpk_solve_time_min) {
                    results->glpk_solve_time_min = glpk_result.solve_time_ms;
                }
                if (glpk_result.solve_time_ms > results->glpk_solve_time_max) {
                    results->glpk_solve_time_max = glpk_result.solve_time_ms;
                }

                /* Speedup: GLPK time / Ralph time (> 1 means Ralph faster) */
                if (solve_time > 1e-6) {
                    total_speedup += glpk_result.solve_time_ms / solve_time;
                }

                /* Compare objectives (tolerance: 0.01% relative difference) */
                double ralph_obj = solution.total_cost;
                double glpk_obj = glpk_result.objective;
                double rel_diff = 0;
                if (fabs(glpk_obj) > 1e-10) {
                    rel_diff = fabs(ralph_obj - glpk_obj) / fabs(glpk_obj);
                } else if (fabs(ralph_obj) > 1e-10) {
                    rel_diff = 1.0;  /* One zero, one not */
                }
                if (rel_diff < 0.0001) {
                    results->glpk_num_match++;
                }

                /* Track objective gap: (ralph - glpk) / glpk * 100 */
                if (fabs(glpk_obj) > 1e-10) {
                    double gap_pct = (ralph_obj - glpk_obj) / fabs(glpk_obj) * 100.0;
                    if (results->glpk_gap_count == 0) {
                        results->glpk_gap_min_pct = gap_pct;
                        results->glpk_gap_max_pct = gap_pct;
                    } else {
                        if (gap_pct < results->glpk_gap_min_pct) results->glpk_gap_min_pct = gap_pct;
                        if (gap_pct > results->glpk_gap_max_pct) results->glpk_gap_max_pct = gap_pct;
                    }
                    total_gap_pct += gap_pct;
                    results->glpk_gap_count++;
                }
            }
        }

        fw_free_solution(&solution);
        fw_bench_free_instance(&instance);
    }

    /* Calculate averages */
    if (results->num_solved > 0) {
        results->solve_time_avg = total_solve_time / results->num_solved;
        results->validate_time_avg = total_validate_time / results->num_solved;
    }

    if (results->num_feasible > 0) {
        results->cost_avg = total_cost / results->num_feasible;
        results->stops_avg = total_stops / results->num_feasible;
    }

    /* GLPK comparison averages */
    if (results->glpk_num_solved > 0) {
        results->glpk_solve_time_avg = total_glpk_time / results->glpk_num_solved;
        results->glpk_speedup_avg = total_speedup / results->glpk_num_solved;
    }
    if (results->glpk_gap_count > 0) {
        results->glpk_gap_avg_pct = total_gap_pct / results->glpk_gap_count;
    }

    return 0;
}

/* ============================================================================
 * Output Formatting
 * ============================================================================ */

void fw_bench_write_json(
    FILE *out,
    const FWBenchConfig *config,
    const FWBenchResults *results,
    const char *scenario)
{
    if (!out || !config || !results) return;

    fprintf(out, "{\n");
    fprintf(out, "  \"scenario\": \"%s\",\n", scenario ? scenario : "custom");
    fprintf(out, "  \"seed\": %llu,\n", (unsigned long long)config->seed);
    fprintf(out, "  \"route_length_km\": %.1f,\n", config->route_length_m / 1000.0);
    fprintf(out, "  \"num_runs\": %d,\n", results->num_runs);
    fprintf(out, "  \"solver\": {\n");
    fprintf(out, "    \"solved\": %d,\n", results->num_solved);
    fprintf(out, "    \"feasible\": %d,\n", results->num_feasible);
    fprintf(out, "    \"infeasible\": %d,\n", results->num_infeasible);
    fprintf(out, "    \"errors\": %d,\n", results->num_errors);
    fprintf(out, "    \"solve_time_ms\": {\n");
    fprintf(out, "      \"avg\": %.2f,\n", results->solve_time_avg);
    fprintf(out, "      \"min\": %.2f,\n", results->solve_time_min);
    fprintf(out, "      \"max\": %.2f\n", results->solve_time_max);
    fprintf(out, "    }\n");
    fprintf(out, "  },\n");
    fprintf(out, "  \"validation\": {\n");
    fprintf(out, "    \"feasible\": %d,\n", results->num_feasible);
    fprintf(out, "    \"validate_time_ms\": %.2f\n", results->validate_time_avg);
    fprintf(out, "  },\n");
    fprintf(out, "  \"solution\": {\n");
    fprintf(out, "    \"cost\": {\n");
    fprintf(out, "      \"avg\": %.2f,\n", results->cost_avg);
    fprintf(out, "      \"min\": %.2f,\n", results->cost_min);
    fprintf(out, "      \"max\": %.2f\n", results->cost_max);
    fprintf(out, "    },\n");
    fprintf(out, "    \"stops_avg\": %.1f\n", results->stops_avg);
    fprintf(out, "  }");
    if (results->glpk_enabled) {
        fprintf(out, ",\n");
        fprintf(out, "  \"glpk\": {\n");
        fprintf(out, "    \"solved\": %d,\n", results->glpk_num_solved);
        fprintf(out, "    \"objective_match\": %d,\n", results->glpk_num_match);
        fprintf(out, "    \"solve_time_ms\": {\n");
        fprintf(out, "      \"avg\": %.2f,\n", results->glpk_solve_time_avg);
        fprintf(out, "      \"min\": %.2f,\n", results->glpk_solve_time_min);
        fprintf(out, "      \"max\": %.2f\n", results->glpk_solve_time_max);
        fprintf(out, "    },\n");
        fprintf(out, "    \"speedup\": %.1f\n", results->glpk_speedup_avg);
        fprintf(out, "  }\n");
    } else {
        fprintf(out, "\n");
    }
    fprintf(out, "}\n");
}

void fw_bench_print_results(
    const FWBenchConfig *config,
    const FWBenchResults *results,
    const char *scenario,
    int as_json)
{
    if (!results) return;

    if (as_json) {
        fw_bench_write_json(stdout, config, results, scenario);
    } else {
        /* Determine units for display */
        int imperial = (config->units == SH_UNITS_IMPERIAL);
        const char *dist_unit = imperial ? "mi" : "km";
        double route_dist = imperial
            ? sh_m_to_miles(config->route_length_m)
            : config->route_length_m / 1000.0;

        printf("\nFuelWise Benchmark Results\n");
        printf("==========================\n");
        printf("Scenario: %s (%.0f%s, seed=%llu)\n",
               scenario ? scenario : "custom",
               route_dist, dist_unit,
               (unsigned long long)config->seed);
        printf("\n");

        printf("Solver Performance:\n");
        printf("  Problems solved: %d/%d (%.1f%%)\n",
               results->num_solved, results->num_runs,
               100.0 * results->num_solved / results->num_runs);
        if (results->num_infeasible > 0) {
            printf("  Infeasible: %d\n", results->num_infeasible);
        }
        if (results->num_errors > 0) {
            printf("  Errors: %d\n", results->num_errors);
        }
        printf("  Solve time: %.2f ms avg (%.2f - %.2f ms)\n",
               results->solve_time_avg,
               results->solve_time_min,
               results->solve_time_max);
        printf("\n");

        printf("Validation:\n");
        printf("  Solutions feasible: %d/%d (%.1f%%)\n",
               results->num_feasible, results->num_solved,
               results->num_solved > 0
                   ? 100.0 * results->num_feasible / results->num_solved
                   : 0.0);
        printf("  Validation time: %.2f ms avg\n", results->validate_time_avg);
        printf("\n");

        printf("Solution Quality:\n");
        printf("  Total cost: $%.2f avg ($%.2f - $%.2f)\n",
               results->cost_avg, results->cost_min, results->cost_max);
        printf("  Stops: %.1f avg\n", results->stops_avg);
        printf("\n");

        /* GLPK comparison section */
        if (results->glpk_enabled) {
            printf("GLPK Comparison:\n");
            printf("  GLPK solved: %d/%d\n",
                   results->glpk_num_solved, results->num_feasible);
            printf("  GLPK time: %.2f ms avg (%.2f - %.2f ms)\n",
                   results->glpk_solve_time_avg,
                   results->glpk_solve_time_min,
                   results->glpk_solve_time_max);
            printf("  Ralph time: %.2f ms avg\n", results->solve_time_avg);
            printf("  Speedup: %.1fx (Ralph faster)\n",
                   results->glpk_speedup_avg);
            printf("  Objective match: %d/%d (tolerance: 0.01%%)\n",
                   results->glpk_num_match, results->glpk_num_solved);
            if (results->glpk_gap_count > 0) {
                printf("  Objective gap vs GLPK: %.2f%% avg (%.2f%% - %.2f%%)\n",
                       results->glpk_gap_avg_pct,
                       results->glpk_gap_min_pct,
                       results->glpk_gap_max_pct);
            }
            printf("\n");
        }
    }
}
