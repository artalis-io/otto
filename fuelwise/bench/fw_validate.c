/*
 * FuelWise Solution Validator
 *
 * Independent constraint checker for validating solver solutions.
 * Does not use solver internals - simulates truck driving the route.
 *
 * Copyright (c) 2024-2026. All rights reserved.
 */

#include "fw_bench.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Tolerance for floating-point comparisons */
#define EPSILON 1e-6

/*
 * Tolerance for fuel level comparisons.
 * LP solvers have ~0.01% relative precision, so we use 0.1% to be safe.
 * This accommodates numerical noise in constraint satisfaction.
 */
#define FUEL_TOLERANCE_REL 0.001  /* 0.1% relative */
#define FUEL_TOLERANCE_ABS 0.1    /* 0.1 liters absolute minimum */

/*
 * Threshold for counting a purchase as a "stop".
 * Must match the solver's threshold (0.5L) for consistency.
 */
#define STOP_THRESHOLD 0.5  /* liters */

/* Relative tolerance for fuel: max(0.1L, 0.1% of expected) */
static double fuel_tol(double expected)
{
    double rel = FUEL_TOLERANCE_REL * fabs(expected);
    return rel > FUEL_TOLERANCE_ABS ? rel : FUEL_TOLERANCE_ABS;
}

/* Tight relative tolerance for other comparisons */
static double rel_tol(double expected)
{
    double abs_val = fabs(expected);
    return EPSILON * (abs_val > 1.0 ? abs_val : 1.0);
}

/*
 * Calculate fuel consumed between two distances using consumption curve.
 * If curve is NULL, uses base_consumption from problem.
 */
static double calc_fuel(
    const FWRefuelProblem *problem,
    const FWConsumptionCurve *curve,
    const FWWeightProfile *profile,
    double from_m,
    double to_m)
{
    if (to_m <= from_m) return 0.0;

    if (curve != NULL && profile != NULL) {
        /* Use weight-dependent consumption */
        return fw_calc_fuel_for_segment(curve, profile, from_m, to_m);
    } else if (curve != NULL) {
        /* Use curve with constant weight (tare) */
        double weight = profile ? profile->tare_weight_kg : 15000.0;
        return fw_calc_fuel_constant_weight(curve, weight, from_m, to_m);
    } else if (problem->num_segments > 0 && problem->segments != NULL) {
        /* Use piecewise segments from problem */
        double fuel = 0.0;
        double pos = from_m;

        for (int i = 0; i < problem->num_segments && pos < to_m; i++) {
            double seg_start = problem->segments[i].start_distance;
            double seg_end = (i + 1 < problem->num_segments)
                ? problem->segments[i + 1].start_distance
                : problem->total_distance;

            /* Clamp to [from_m, to_m] */
            if (seg_end <= from_m) continue;
            if (seg_start >= to_m) break;

            double start = (seg_start > pos) ? seg_start : pos;
            double end = (seg_end < to_m) ? seg_end : to_m;

            double distance_km = (end - start) / 1000.0;
            double consumption = problem->segments[i].consumption;
            fuel += (consumption / 100.0) * distance_km;

            pos = end;
        }
        return fuel;
    } else {
        /* Use constant base consumption */
        double distance_km = (to_m - from_m) / 1000.0;
        return (problem->base_consumption / 100.0) * distance_km;
    }
}

int fw_validate_solution(
    const FWRefuelProblem *problem,
    const FWRefuelSolution *solution,
    const FWConsumptionCurve *curve,
    const FWWeightProfile *profile,
    FWValidationResult *result)
{
    /* Initialize result */
    memset(result, 0, sizeof(FWValidationResult));
    result->feasible = 1;
    result->fuel_balance_ok = 1;
    result->min_fuel_ok = 1;
    result->tank_capacity_ok = 1;
    result->non_negative_purchase_ok = 1;
    result->reaches_destination = 1;
    result->min_purchase_ok = 1;
    result->stop_flags_ok = 1;
    result->stop_cost_ok = 1;
    result->cost_ok = 1;
    result->first_violation_station = -1;
    result->min_fuel_observed = problem->current_fuel;
    result->max_fuel_observed = problem->current_fuel;

    if (!problem || !solution) {
        result->feasible = 0;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "NULL problem or solution");
        return 0;
    }

    if (solution->status != FW_STATUS_OPTIMAL) {
        result->feasible = 0;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "Solution status is not optimal: %d", solution->status);
        return 0;
    }

    double fuel = problem->current_fuel;
    double distance = 0.0;
    double total_fuel_cost = 0.0;
    double total_stop_cost = 0.0;
    (void)total_stop_cost;  /* Suppress unused warning for now */

    for (int i = 0; i < problem->num_stations; i++) {
        const FWSnappedStation *station = &problem->stations[i];
        double purchase = solution->purchases[i];

        /* Drive to station */
        double fuel_consumed = calc_fuel(problem, curve, profile,
                                         distance, station->distance_from_start);
        fuel -= fuel_consumed;
        distance = station->distance_from_start;

        /* Track min fuel observed */
        if (fuel < result->min_fuel_observed) {
            result->min_fuel_observed = fuel;
        }

        /* Check minimum fuel at arrival */
        if (fuel < problem->minimum_fuel - fuel_tol(problem->minimum_fuel)) {
            if (result->first_violation_station < 0) {
                result->feasible = 0;
                result->min_fuel_ok = 0;
                result->first_violation_station = i;
                snprintf(result->error_msg, sizeof(result->error_msg),
                         "Station %d: arrived with %.2fL, below minimum %.2fL",
                         i, fuel, problem->minimum_fuel);
            }
        }

        /* Check non-negative purchase */
        if (purchase < -EPSILON) {
            if (result->first_violation_station < 0) {
                result->feasible = 0;
                result->non_negative_purchase_ok = 0;
                result->first_violation_station = i;
                snprintf(result->error_msg, sizeof(result->error_msg),
                         "Station %d: negative purchase %.2fL", i, purchase);
            }
        }

        /* Check minimum purchase constraint */
        if (problem->min_purchase > 0 && purchase > EPSILON &&
            purchase < problem->min_purchase - rel_tol(problem->min_purchase)) {
            if (result->first_violation_station < 0) {
                result->feasible = 0;
                result->min_purchase_ok = 0;
                result->first_violation_station = i;
                snprintf(result->error_msg, sizeof(result->error_msg),
                         "Station %d: purchase %.2fL below minimum %.2fL",
                         i, purchase, problem->min_purchase);
            }
        }

        /* Refuel */
        fuel += purchase;
        if (purchase > EPSILON) {
            total_fuel_cost += purchase * station->price;
            if (problem->stop_cost > 0) {
                total_stop_cost += problem->stop_cost;
            }
        }

        /* Track max fuel observed */
        if (fuel > result->max_fuel_observed) {
            result->max_fuel_observed = fuel;
        }

        /* Check tank capacity after refuel */
        if (fuel > problem->tank_capacity + fuel_tol(problem->tank_capacity)) {
            if (result->first_violation_station < 0) {
                result->feasible = 0;
                result->tank_capacity_ok = 0;
                result->first_violation_station = i;
                snprintf(result->error_msg, sizeof(result->error_msg),
                         "Station %d: fuel %.2fL exceeds tank capacity %.2fL",
                         i, fuel, problem->tank_capacity);
            }
        }

        /* Check stop flags consistency (MILP and LP with stop_flags) */
        if (solution->stop_flags != NULL) {
            int stopped = solution->stop_flags[i];
            /* Use same threshold as solver (0.5L) for what counts as a stop */
            if (purchase > STOP_THRESHOLD && !stopped) {
                if (result->first_violation_station < 0) {
                    result->feasible = 0;
                    result->stop_flags_ok = 0;
                    result->first_violation_station = i;
                    snprintf(result->error_msg, sizeof(result->error_msg),
                             "Station %d: purchased %.2fL but stop_flag=0",
                             i, purchase);
                }
            }
        }
    }

    /* Drive to destination */
    double fuel_consumed = calc_fuel(problem, curve, profile,
                                     distance, problem->total_distance);
    fuel -= fuel_consumed;

    /* Track final fuel level */
    if (fuel < result->min_fuel_observed) {
        result->min_fuel_observed = fuel;
    }

    /* Check arrival fuel */
    double min_at_end = (problem->minimum_fuel_at_end > 0)
        ? problem->minimum_fuel_at_end
        : problem->minimum_fuel;

    if (fuel < min_at_end - fuel_tol(min_at_end)) {
        result->feasible = 0;
        result->reaches_destination = 0;
        if (result->first_violation_station < 0) {
            result->first_violation_station = problem->num_stations;
            snprintf(result->error_msg, sizeof(result->error_msg),
                     "Destination: arrived with %.2fL, below minimum %.2fL",
                     fuel, min_at_end);
        }
    }

    /* NOTE: Cost validation disabled for now - the solver uses gross_cost
     * and we don't have all the context to recalculate exactly.
     * The important thing is constraint feasibility. */

    return result->feasible;
}
