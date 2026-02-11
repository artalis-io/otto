/*
 * FuelWise Benchmark Driver
 *
 * Runs benchmark iterations and collects statistics.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
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

        double t0 = get_time_ms();

        /* Select solver based on type and problem characteristics */
        FWSolverType effective_solver = solver_type;

        /* Force MILP/Benders if min_purchase requires binary decisions */
        if (effective_solver == FW_SOLVER_LP && instance.problem.min_purchase > 0.01) {
            effective_solver = FW_SOLVER_MILP;
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

    return 0;
}

/* ============================================================================
 * Output Formatting
 * ============================================================================ */

void fw_bench_print_results(
    const FWBenchConfig *config,
    const FWBenchResults *results,
    const char *scenario,
    int as_json)
{
    if (!results) return;

    if (as_json) {
        printf("{\n");
        printf("  \"scenario\": \"%s\",\n", scenario ? scenario : "custom");
        printf("  \"seed\": %llu,\n", (unsigned long long)config->seed);
        printf("  \"route_length_km\": %.1f,\n", config->route_length_m / 1000.0);
        printf("  \"num_runs\": %d,\n", results->num_runs);
        printf("  \"solver\": {\n");
        printf("    \"solved\": %d,\n", results->num_solved);
        printf("    \"feasible\": %d,\n", results->num_feasible);
        printf("    \"infeasible\": %d,\n", results->num_infeasible);
        printf("    \"errors\": %d,\n", results->num_errors);
        printf("    \"solve_time_ms\": {\n");
        printf("      \"avg\": %.2f,\n", results->solve_time_avg);
        printf("      \"min\": %.2f,\n", results->solve_time_min);
        printf("      \"max\": %.2f\n", results->solve_time_max);
        printf("    }\n");
        printf("  },\n");
        printf("  \"validation\": {\n");
        printf("    \"feasible\": %d,\n", results->num_feasible);
        printf("    \"validate_time_ms\": %.2f\n", results->validate_time_avg);
        printf("  },\n");
        printf("  \"solution\": {\n");
        printf("    \"cost\": {\n");
        printf("      \"avg\": %.2f,\n", results->cost_avg);
        printf("      \"min\": %.2f,\n", results->cost_min);
        printf("      \"max\": %.2f\n", results->cost_max);
        printf("    },\n");
        printf("    \"stops_avg\": %.1f\n", results->stops_avg);
        printf("  }");
        if (results->glpk_enabled) {
            printf(",\n");
            printf("  \"glpk\": {\n");
            printf("    \"solved\": %d,\n", results->glpk_num_solved);
            printf("    \"objective_match\": %d,\n", results->glpk_num_match);
            printf("    \"solve_time_ms\": {\n");
            printf("      \"avg\": %.2f,\n", results->glpk_solve_time_avg);
            printf("      \"min\": %.2f,\n", results->glpk_solve_time_min);
            printf("      \"max\": %.2f\n", results->glpk_solve_time_max);
            printf("    },\n");
            printf("    \"speedup\": %.1f\n", results->glpk_speedup_avg);
            printf("  }\n");
        } else {
            printf("\n");
        }
        printf("}\n");
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
            printf("\n");
        }
    }
}
