/*
 * FuelWise Validator Tests
 *
 * Tests that the solution validator correctly catches constraint violations.
 * These are independent of the solver - we create deliberately bad solutions
 * and verify the validator catches them.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "fw_bench.h"
#include "fuelwise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while(0)

/* ============================================================================
 * Test: Valid solution passes validation
 * ============================================================================ */
void test_valid_solution_passes(void)
{
    printf("\n=== Test: Valid Solution Passes Validation ===\n");

    /* Create a simple problem */
    FWSnappedStation stations[2] = {
        {.station_id = 0, .distance_from_start = 200000, .price = 1.50},  /* 200km */
        {.station_id = 1, .distance_from_start = 400000, .price = 1.40},  /* 400km */
    };

    FWRefuelProblem problem = {
        .total_distance = 500000,       /* 500 km */
        .base_consumption = 30.0,       /* 30 L/100km */
        .tank_capacity = 300,           /* 300 L */
        .current_fuel = 100,            /* Start with 100 L */
        .minimum_fuel = 20,             /* Keep 20 L minimum */
        .minimum_fuel_at_end = 20,
        .num_stations = 2,
        .stations = stations,
    };

    /* Solve it */
    FWRefuelSolution solution;
    memset(&solution, 0, sizeof(solution));
    int rc = fw_solve_refuel_lp(&problem, &solution);
    ASSERT(rc == 0 && solution.status == FW_STATUS_OPTIMAL, "Solver finds optimal");

    /* Validate */
    FWValidationResult result;
    int feasible = fw_validate_solution(&problem, &solution, NULL, NULL, &result);
    ASSERT(feasible == 1, "Valid solution passes validation");
    ASSERT(result.min_fuel_ok == 1, "Minimum fuel constraint satisfied");
    ASSERT(result.tank_capacity_ok == 1, "Tank capacity constraint satisfied");
    ASSERT(result.reaches_destination == 1, "Reaches destination");

    printf("  Min fuel observed: %.2f L\n", result.min_fuel_observed);
    printf("  Max fuel observed: %.2f L\n", result.max_fuel_observed);

    fw_free_solution(&solution);
}

/* ============================================================================
 * Test: Insufficient purchase fails (runs out of fuel)
 * ============================================================================ */
void test_insufficient_fuel_fails(void)
{
    printf("\n=== Test: Insufficient Fuel Fails Validation ===\n");

    FWSnappedStation stations[1] = {
        {.station_id = 0, .distance_from_start = 200000, .price = 1.50},
    };

    FWRefuelProblem problem = {
        .total_distance = 500000,       /* 500 km */
        .base_consumption = 30.0,       /* 30 L/100km = 150L total */
        .tank_capacity = 200,
        .current_fuel = 50,             /* Only 50 L start */
        .minimum_fuel = 20,
        .minimum_fuel_at_end = 20,
        .num_stations = 1,
        .stations = stations,
    };

    /* Create a deliberately bad solution - don't buy enough */
    double purchases[] = {10.0};        /* Only buy 10L - not enough! */
    FWRefuelSolution solution = {
        .status = FW_STATUS_OPTIMAL,
        .num_stops = 1,
        .total_cost = 15.0,
        .purchases = purchases,
        .stop_flags = NULL,
        .remaining_fuel = 0,
    };

    FWValidationResult result;
    int feasible = fw_validate_solution(&problem, &solution, NULL, NULL, &result);
    ASSERT(feasible == 0, "Insufficient fuel fails validation");
    ASSERT(result.reaches_destination == 0 || result.min_fuel_ok == 0,
           "Detected fuel constraint violation");
    printf("  Error: %s\n", result.error_msg);
}

/* ============================================================================
 * Test: Tank overflow fails
 * ============================================================================ */
void test_tank_overflow_fails(void)
{
    printf("\n=== Test: Tank Overflow Fails Validation ===\n");

    FWSnappedStation stations[1] = {
        {.station_id = 0, .distance_from_start = 100000, .price = 1.50},
    };

    FWRefuelProblem problem = {
        .total_distance = 200000,
        .base_consumption = 30.0,
        .tank_capacity = 100,           /* Small tank */
        .current_fuel = 80,             /* Nearly full */
        .minimum_fuel = 10,
        .minimum_fuel_at_end = 10,
        .num_stations = 1,
        .stations = stations,
    };

    /* Create solution that overflows tank */
    double purchases[] = {100.0};       /* Buy 100L when tank nearly full! */
    FWRefuelSolution solution = {
        .status = FW_STATUS_OPTIMAL,
        .num_stops = 1,
        .total_cost = 150.0,
        .purchases = purchases,
        .stop_flags = NULL,
        .remaining_fuel = 0,
    };

    FWValidationResult result;
    int feasible = fw_validate_solution(&problem, &solution, NULL, NULL, &result);
    ASSERT(feasible == 0, "Tank overflow fails validation");
    ASSERT(result.tank_capacity_ok == 0, "Detected tank capacity violation");
    printf("  Error: %s\n", result.error_msg);
    printf("  Max fuel observed: %.2f L (tank: %.2f L)\n",
           result.max_fuel_observed, problem.tank_capacity);
}

/* ============================================================================
 * Test: Negative purchase fails
 * ============================================================================ */
void test_negative_purchase_fails(void)
{
    printf("\n=== Test: Negative Purchase Fails Validation ===\n");

    FWSnappedStation stations[1] = {
        {.station_id = 0, .distance_from_start = 100000, .price = 1.50},
    };

    FWRefuelProblem problem = {
        .total_distance = 200000,
        .base_consumption = 30.0,
        .tank_capacity = 200,
        .current_fuel = 100,
        .minimum_fuel = 10,
        .minimum_fuel_at_end = 10,
        .num_stations = 1,
        .stations = stations,
    };

    /* Negative purchase (selling fuel?) */
    double purchases[] = {-50.0};
    FWRefuelSolution solution = {
        .status = FW_STATUS_OPTIMAL,
        .num_stops = 0,
        .total_cost = -75.0,
        .purchases = purchases,
        .stop_flags = NULL,
        .remaining_fuel = 0,
    };

    FWValidationResult result;
    int feasible = fw_validate_solution(&problem, &solution, NULL, NULL, &result);
    ASSERT(feasible == 0, "Negative purchase fails validation");
    ASSERT(result.non_negative_purchase_ok == 0, "Detected negative purchase");
    printf("  Error: %s\n", result.error_msg);
}

/* ============================================================================
 * Test: Benchmark generated problems all pass
 * ============================================================================ */
void test_benchmark_solutions_valid(void)
{
    printf("\n=== Test: Benchmark Solutions All Valid ===\n");

    FWBenchConfig cfg = fw_bench_config_tight_margins();
    cfg.seed = 42;  /* Fixed seed for reproducibility */

    int num_tests = 20;
    int all_valid = 1;
    int num_solved = 0;

    for (int i = 0; i < num_tests; i++) {
        cfg.seed = 42 + i;

        FWBenchInstance instance;
        if (fw_bench_generate(&cfg, &instance) != 0) {
            continue;
        }

        FWRefuelSolution solution;
        memset(&solution, 0, sizeof(solution));
        int rc = fw_solve_refuel_lp(&instance.problem, &solution);

        if (rc == 0 && solution.status == FW_STATUS_OPTIMAL) {
            num_solved++;

            FWValidationResult result;
            int feasible = fw_validate_solution(
                &instance.problem, &solution,
                instance.curve, instance.weight_profile,
                &result);

            if (!feasible) {
                printf("  Run %d FAILED: %s\n", i, result.error_msg);
                all_valid = 0;
            }
        }

        fw_free_solution(&solution);
        fw_bench_free_instance(&instance);
    }

    printf("  Solved %d/%d problems\n", num_solved, num_tests);
    ASSERT(all_valid, "All solved solutions pass validation");
    ASSERT(num_solved >= num_tests * 0.8, "At least 80% solvable");
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(void)
{
    printf("FuelWise Validator Tests\n");
    printf("========================\n");

    test_valid_solution_passes();
    test_insufficient_fuel_fails();
    test_tank_overflow_fails();
    test_negative_purchase_fails();
    test_benchmark_solutions_valid();

    printf("\n========================\n");
    printf("Tests passed: %d/%d\n", tests_passed, tests_run);
    printf("========================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
