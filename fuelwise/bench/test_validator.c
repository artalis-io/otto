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
#include <math.h>

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
 * Test: MILP enforces minimum purchase constraint
 * ============================================================================ */
void test_min_purchase_milp(void)
{
    printf("\n=== Test: MILP Enforces Minimum Purchase ===\n");

    /* Small problem so MILP is fast */
    FWSnappedStation stations[3] = {
        {.station_id = 0, .distance_from_start = 100000, .price = 1.50},
        {.station_id = 1, .distance_from_start = 200000, .price = 1.40},
        {.station_id = 2, .distance_from_start = 300000, .price = 1.45},
    };

    FWRefuelProblem problem = {
        .total_distance = 400000,       /* 400 km */
        .base_consumption = 25.0,       /* 25 L/100km = 100L total */
        .tank_capacity = 150,
        .current_fuel = 50,             /* Need 50L more */
        .minimum_fuel = 10,
        .minimum_fuel_at_end = 10,
        .min_purchase = 20.0,           /* MILP: 20L minimum per stop */
        .num_stations = 3,
        .stations = stations,
    };

    FWRefuelSolution solution;
    memset(&solution, 0, sizeof(solution));
    int rc = fw_solve_refuel_milp(&problem, &solution);
    ASSERT(rc == 0 && solution.status == FW_STATUS_OPTIMAL, "MILP finds optimal");

    /* Check all purchases are either 0 or >= min_purchase */
    int min_purchase_ok = 1;
    for (int i = 0; i < problem.num_stations; i++) {
        if (solution.purchases[i] > 0.01 && solution.purchases[i] < problem.min_purchase - 0.01) {
            printf("  Station %d: purchase %.2fL < min %.2fL\n",
                   i, solution.purchases[i], problem.min_purchase);
            min_purchase_ok = 0;
        }
    }
    ASSERT(min_purchase_ok, "All purchases >= min_purchase or zero");

    fw_free_solution(&solution);
}

/* ============================================================================
 * Test: Don't overfill when cheaper station ahead
 * ============================================================================ */
void test_cheaper_ahead_no_overfill(void)
{
    printf("\n=== Test: Don't Overfill When Cheaper Ahead ===\n");

    /* Station 0 is expensive, Station 1 is cheap */
    FWSnappedStation stations[2] = {
        {.station_id = 0, .distance_from_start = 100000, .price = 2.00},  /* Expensive */
        {.station_id = 1, .distance_from_start = 200000, .price = 1.00},  /* Cheap */
    };

    FWRefuelProblem problem = {
        .total_distance = 300000,       /* 300 km */
        .base_consumption = 25.0,       /* 25 L/100km */
        .tank_capacity = 200,
        .current_fuel = 30,             /* Just enough to reach station 0 + minimum */
        .minimum_fuel = 5,
        .minimum_fuel_at_end = 5,
        .num_stations = 2,
        .stations = stations,
    };

    /* Fuel needed: 75L total, have 30L, need 45L more + 5L buffer = 50L
     * Optimal: Buy minimum at expensive station 0, fill up at cheap station 1 */

    FWRefuelSolution solution;
    memset(&solution, 0, sizeof(solution));
    int rc = fw_solve_refuel_lp(&problem, &solution);
    ASSERT(rc == 0 && solution.status == FW_STATUS_OPTIMAL, "Solver finds optimal");

    /* At station 0: should buy just enough to reach station 1 with minimum fuel
     * Fuel at station 0 arrival: 30 - 25 = 5L (at minimum)
     * Need to reach station 1 (100km = 25L) with minimum (5L)
     * So buy: 25 + 5 - 5 = 25L at station 0 */
    double fuel_to_next = 25.0;  /* 100km at 25L/100km */
    double min_needed = fuel_to_next + problem.minimum_fuel;

    printf("  Station 0 (expensive $2.00): bought %.2fL\n", solution.purchases[0]);
    printf("  Station 1 (cheap $1.00): bought %.2fL\n", solution.purchases[1]);

    /* Station 0 purchase should be close to minimum needed, not filling tank */
    ASSERT(solution.purchases[0] < problem.tank_capacity * 0.5,
           "Don't fill up at expensive station");
    ASSERT(solution.purchases[1] > solution.purchases[0],
           "Buy more at cheaper station");

    fw_free_solution(&solution);
}

/* ============================================================================
 * Test: Fill up at cheapest station on route
 * ============================================================================ */
void test_fill_at_cheapest(void)
{
    printf("\n=== Test: Fill Up at Cheapest Station ===\n");

    /* Station 1 is cheapest */
    FWSnappedStation stations[3] = {
        {.station_id = 0, .distance_from_start = 100000, .price = 1.50},
        {.station_id = 1, .distance_from_start = 200000, .price = 1.00},  /* Cheapest */
        {.station_id = 2, .distance_from_start = 300000, .price = 1.60},
    };

    FWRefuelProblem problem = {
        .total_distance = 400000,       /* 400 km */
        .base_consumption = 25.0,       /* 25 L/100km = 100L total */
        .tank_capacity = 150,
        .current_fuel = 50,             /* Start with 50L */
        .minimum_fuel = 10,
        .minimum_fuel_at_end = 10,
        .num_stations = 3,
        .stations = stations,
    };

    FWRefuelSolution solution;
    memset(&solution, 0, sizeof(solution));
    int rc = fw_solve_refuel_lp(&problem, &solution);
    ASSERT(rc == 0 && solution.status == FW_STATUS_OPTIMAL, "Solver finds optimal");

    printf("  Station 0 ($1.50): %.2fL\n", solution.purchases[0]);
    printf("  Station 1 ($1.00 cheapest): %.2fL\n", solution.purchases[1]);
    printf("  Station 2 ($1.60): %.2fL\n", solution.purchases[2]);

    /* Station 1 should have the largest purchase */
    ASSERT(solution.purchases[1] >= solution.purchases[0],
           "Buy more at cheapest than at station 0");
    ASSERT(solution.purchases[1] >= solution.purchases[2],
           "Buy more at cheapest than at station 2");

    fw_free_solution(&solution);
}

/* ============================================================================
 * Test: Total cost matches sum of purchases * prices
 * ============================================================================ */
void test_cost_matches_purchases(void)
{
    printf("\n=== Test: Cost Matches Purchases ===\n");

    FWSnappedStation stations[2] = {
        {.station_id = 0, .distance_from_start = 200000, .price = 1.50},
        {.station_id = 1, .distance_from_start = 400000, .price = 1.40},
    };

    FWRefuelProblem problem = {
        .total_distance = 500000,
        .base_consumption = 30.0,
        .tank_capacity = 300,
        .current_fuel = 100,
        .minimum_fuel = 20,
        .minimum_fuel_at_end = 20,
        .num_stations = 2,
        .stations = stations,
    };

    FWRefuelSolution solution;
    memset(&solution, 0, sizeof(solution));
    int rc = fw_solve_refuel_lp(&problem, &solution);
    ASSERT(rc == 0 && solution.status == FW_STATUS_OPTIMAL, "Solver finds optimal");

    /* Calculate expected cost */
    double expected_cost = 0.0;
    for (int i = 0; i < problem.num_stations; i++) {
        expected_cost += solution.purchases[i] * stations[i].price;
    }

    printf("  Calculated cost: %.2f\n", expected_cost);
    printf("  Reported gross_cost: %.2f\n", solution.gross_cost);

    double diff = fabs(expected_cost - solution.gross_cost);
    ASSERT(diff < 0.01, "Cost matches sum of purchases * prices");

    fw_free_solution(&solution);
}

/* ============================================================================
 * Test: Arrive with minimum fuel (don't waste money)
 * ============================================================================ */
void test_arrive_with_minimum(void)
{
    printf("\n=== Test: Arrive with Minimum Fuel ===\n");

    FWSnappedStation stations[2] = {
        {.station_id = 0, .distance_from_start = 150000, .price = 1.50},
        {.station_id = 1, .distance_from_start = 300000, .price = 1.50},
    };

    FWRefuelProblem problem = {
        .total_distance = 400000,       /* 400 km */
        .base_consumption = 25.0,       /* 25 L/100km = 100L total */
        .tank_capacity = 200,
        .current_fuel = 60,             /* Start with 60L */
        .minimum_fuel = 10,
        .minimum_fuel_at_end = 15,      /* Need 15L at end */
        .num_stations = 2,
        .stations = stations,
    };

    FWRefuelSolution solution;
    memset(&solution, 0, sizeof(solution));
    int rc = fw_solve_refuel_lp(&problem, &solution);
    ASSERT(rc == 0 && solution.status == FW_STATUS_OPTIMAL, "Solver finds optimal");

    /* Calculate final fuel */
    double fuel = problem.current_fuel;
    for (int i = 0; i < problem.num_stations; i++) {
        double dist_to = (i == 0) ? stations[i].distance_from_start
                                  : stations[i].distance_from_start - stations[i-1].distance_from_start;
        fuel -= (problem.base_consumption / 100.0) * (dist_to / 1000.0);
        fuel += solution.purchases[i];
    }
    double final_leg = problem.total_distance - stations[problem.num_stations - 1].distance_from_start;
    fuel -= (problem.base_consumption / 100.0) * (final_leg / 1000.0);

    printf("  Final fuel: %.2fL (minimum: %.2fL)\n", fuel, problem.minimum_fuel_at_end);

    /* Should arrive close to minimum (within tolerance), not with excess */
    double excess = fuel - problem.minimum_fuel_at_end;
    ASSERT(excess >= -0.5, "Arrives with at least minimum fuel");
    ASSERT(excess < 5.0, "Doesn't waste money on excess fuel");

    fw_free_solution(&solution);
}

/* ============================================================================
 * Test: Skip expensive stations when possible
 * ============================================================================ */
void test_skip_expensive_stations(void)
{
    printf("\n=== Test: Skip Expensive Stations When Possible ===\n");

    /* Station 1 is very expensive and can be skipped */
    FWSnappedStation stations[3] = {
        {.station_id = 0, .distance_from_start = 50000, .price = 1.20},   /* Cheap */
        {.station_id = 1, .distance_from_start = 100000, .price = 3.00},  /* Very expensive */
        {.station_id = 2, .distance_from_start = 150000, .price = 1.30},  /* Cheap */
    };

    FWRefuelProblem problem = {
        .total_distance = 200000,       /* 200 km */
        .base_consumption = 25.0,       /* 25 L/100km = 50L total */
        .tank_capacity = 100,           /* Can easily skip station 1 */
        .current_fuel = 20,
        .minimum_fuel = 5,
        .minimum_fuel_at_end = 5,
        .num_stations = 3,
        .stations = stations,
    };

    FWRefuelSolution solution;
    memset(&solution, 0, sizeof(solution));
    int rc = fw_solve_refuel_lp(&problem, &solution);
    ASSERT(rc == 0 && solution.status == FW_STATUS_OPTIMAL, "Solver finds optimal");

    printf("  Station 0 ($1.20): %.2fL\n", solution.purchases[0]);
    printf("  Station 1 ($3.00 expensive): %.2fL\n", solution.purchases[1]);
    printf("  Station 2 ($1.30): %.2fL\n", solution.purchases[2]);

    /* Should skip or minimize purchase at expensive station 1 */
    ASSERT(solution.purchases[1] < 1.0,
           "Skip or minimize purchase at expensive station");

    fw_free_solution(&solution);
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(void)
{
    printf("FuelWise Validator Tests\n");
    printf("========================\n");

    /* Constraint violation tests */
    test_valid_solution_passes();
    test_insufficient_fuel_fails();
    test_tank_overflow_fails();
    test_negative_purchase_fails();
    test_benchmark_solutions_valid();

    /* MILP constraint test */
    test_min_purchase_milp();

    /* Economic optimality tests */
    printf("\n--- Economic Optimality Tests ---\n");
    test_cheaper_ahead_no_overfill();
    test_fill_at_cheapest();
    test_cost_matches_purchases();
    test_arrive_with_minimum();
    test_skip_expensive_stations();

    printf("\n========================\n");
    printf("Tests passed: %d/%d\n", tests_passed, tests_run);
    printf("========================\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
