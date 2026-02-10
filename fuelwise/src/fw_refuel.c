/*
 * FuelWise - Truck Refueling Optimization Library
 * Refueling Optimization Implementation
 *
 * Copyright (c) 2024. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "fw_refuel.h"
#include "ralph.h"

/* Maximum stations to prevent integer overflow in allocations */
#define FW_MAX_STATIONS 100000

/* ============================================================================
 * Fuel Consumption Calculation
 *
 * Distance is in meters, consumption is L/100km, fuel is in liters.
 * Formula: fuel (L) = distance (m) / 100000.0 * consumption (L/100km)
 * ============================================================================ */

double fw_calc_fuel_consumed(
    const FWRefuelProblem *problem,
    double from_distance,
    double to_distance)
{
    if (from_distance >= to_distance) return 0.0;

    /* Use constant rate if no segments defined */
    if (problem->num_segments == 0 || problem->segments == NULL) {
        double distance_m = to_distance - from_distance;
        return (distance_m / 100000.0) * problem->base_consumption;
    }

    /* Piecewise linear: sum fuel consumed in each segment */
    double total_fuel = 0.0;
    double current_dist = from_distance;

    for (int s = 0; s < problem->num_segments && current_dist < to_distance; s++) {
        double seg_start = problem->segments[s].start_distance;
        double seg_end;

        /* Determine segment end */
        if (s + 1 < problem->num_segments) {
            seg_end = problem->segments[s + 1].start_distance;
        } else {
            seg_end = problem->total_distance;  /* Last segment extends to end */
        }

        /* Skip segments before our range */
        if (seg_end <= from_distance) continue;

        /* Clip to our range */
        double range_start = (current_dist > seg_start) ? current_dist : seg_start;
        double range_end = (to_distance < seg_end) ? to_distance : seg_end;

        if (range_start < range_end) {
            double segment_distance_m = range_end - range_start;
            double segment_fuel = (segment_distance_m / 100000.0) * problem->segments[s].consumption;
            total_fuel += segment_fuel;
            current_dist = range_end;
        }
    }

    return total_fuel;
}

double fw_calc_total_fuel_consumed(const FWRefuelProblem *problem)
{
    return fw_calc_fuel_consumed(problem, 0, problem->total_distance);
}

/* ============================================================================
 * Refueling Problem Solving - LP
 * ============================================================================ */

int fw_solve_refuel_lp(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution)
{
    if (!problem || !solution) return -1;

    /* Initialize solution */
    memset(solution, 0, sizeof(FWRefuelSolution));

    int k = problem->num_stations;
    if (k == 0) {
        solution->status = FW_STATUS_OPTIMAL;
        solution->total_cost = 0.0;
        solution->remaining_fuel = problem->current_fuel -
            fw_calc_total_fuel_consumed(problem);
        return 0;
    }

    /* Validate k to prevent integer overflow in allocations */
    if (k < 0 || k > FW_MAX_STATIONS) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    int num_vars = 2 * k;

    RalphModel *model = ralph_create();
    if (!model) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    int x_start = 0;
    int y_start = k;

    /* Add x[i] variables - fuel purchased at each station
     *
     * If remaining_fuel_value > 0, we credit remaining fuel.
     * Effective objective coefficient: price[i] - remaining_fuel_value
     */
    double est_price = problem->remaining_fuel_value;
    for (int i = 0; i < k; i++) {
        double obj_coeff = problem->stations[i].price - est_price;
        ralph_add_var(model, 0.0, problem->tank_capacity, obj_coeff, RALPH_CONTINUOUS);
    }

    /* Add y[i] variables - cumulative fuel before arriving */
    double y_upper = problem->current_fuel + k * problem->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);
    }

    /* Constraint: Fuel balance - y[i] = fuel_current + sum(x[j] for j < i) */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_add_constraint(model, nnz, indices, values, RALPH_EQUAL, problem->current_fuel);
        free(indices);
        free(values);
    }

    /* Constraint: Minimum fuel at arrival at each station */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->minimum_fuel + fuel_consumed;

        ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
    }

    /* Constraint: Tank capacity after refueling */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->tank_capacity + fuel_consumed;

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, rhs);
    }

    /* Constraint: Must reach destination with minimum fuel at end */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double total_fuel_needed = fw_calc_total_fuel_consumed(problem);
        double min_end = (problem->minimum_fuel_at_end > 0) ?
                         problem->minimum_fuel_at_end : problem->minimum_fuel;
        double rhs = min_end + total_fuel_needed - problem->current_fuel;

        if (rhs > 0) {
            ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    /* Solve */
    ralph_set_int_param(model, "verbose", 0);
    int ret = ralph_optimize(model);
    RalphStatus status = ralph_get_status(model);

    if (ret == 0 && status == RALPH_STATUS_OPTIMAL) {
        double *x = malloc(num_vars * sizeof(double));
        if (!x) {
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }
        ralph_get_solution(model, x);

        /* Allocate solution arrays */
        solution->purchases = malloc(k * sizeof(double));
        solution->stop_flags = calloc(k, sizeof(int));
        if (!solution->purchases || !solution->stop_flags) {
            free(x);
            free(solution->purchases);
            free(solution->stop_flags);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        double gross_cost = 0.0;
        double total_purchased = 0.0;
        int num_stops = 0;

        for (int i = 0; i < k; i++) {
            solution->purchases[i] = x[x_start + i];
            gross_cost += solution->purchases[i] * problem->stations[i].price;
            total_purchased += solution->purchases[i];
            if (solution->purchases[i] > 0.5) {
                solution->stop_flags[i] = 1;
                num_stops++;
            }
        }

        /* Calculate remaining fuel and total cost */
        double total_consumed = fw_calc_total_fuel_consumed(problem);
        solution->remaining_fuel = problem->current_fuel + total_purchased - total_consumed;
        solution->gross_cost = gross_cost;

        if (est_price > 0.0) {
            double empty_capacity = problem->tank_capacity - solution->remaining_fuel;
            solution->total_cost = gross_cost + empty_capacity * est_price;
        } else {
            solution->total_cost = gross_cost;
        }

        solution->num_stops = num_stops;
        solution->status = FW_STATUS_OPTIMAL;

        free(x);
        ralph_free(model);
        return 0;
    }

    /* Handle failure */
    if (status == RALPH_STATUS_INFEASIBLE) {
        solution->status = FW_STATUS_INFEASIBLE;
    } else {
        solution->status = FW_STATUS_ERROR;
    }

    ralph_free(model);
    return -1;
}

/* ============================================================================
 * Refueling Problem Solving - MILP
 * ============================================================================ */

int fw_solve_refuel_milp(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution)
{
    if (!problem || !solution) return -1;

    /* Initialize solution */
    memset(solution, 0, sizeof(FWRefuelSolution));

    int k = problem->num_stations;
    if (k == 0) {
        solution->status = FW_STATUS_OPTIMAL;
        solution->total_cost = 0.0;
        solution->remaining_fuel = problem->current_fuel -
            fw_calc_total_fuel_consumed(problem);
        return 0;
    }

    /* Validate k to prevent integer overflow in allocations */
    if (k < 0 || k > FW_MAX_STATIONS) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    int num_vars = 3 * k;

    RalphModel *model = ralph_create();
    if (!model) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    int x_start = 0;
    int y_start = k;
    int z_start = 2 * k;

    /* Add x[i] variables - fuel purchased */
    double est_price = problem->remaining_fuel_value;
    for (int i = 0; i < k; i++) {
        double obj_coeff = problem->stations[i].price - est_price;
        ralph_add_var(model, 0.0, problem->tank_capacity, obj_coeff, RALPH_CONTINUOUS);
    }

    /* Add y[i] variables - cumulative fuel before arriving */
    double y_upper = problem->current_fuel + k * problem->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);
    }

    /* Add z[i] variables - binary stop indicators with stop_cost in objective */
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, 1.0, problem->stop_cost, RALPH_BINARY);
    }

    /* Constraint: Fuel balance */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_add_constraint(model, nnz, indices, values, RALPH_EQUAL, problem->current_fuel);
        free(indices);
        free(values);
    }

    /* Constraint: Minimum fuel at arrival */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->minimum_fuel + fuel_consumed;

        ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
    }

    /* Constraint: Tank capacity after refueling */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->tank_capacity + fuel_consumed;

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, rhs);
    }

    /* Constraint: Link x[i] to z[i] - x[i] <= max * z[i] */
    for (int i = 0; i < k; i++) {
        int indices[2] = {x_start + i, z_start + i};
        double values[2] = {1.0, -problem->tank_capacity};

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, 0.0);
    }

    /* Constraint: Minimum purchase if stopping - x[i] >= min_purchase * z[i] */
    if (problem->min_purchase > 0.01) {
        for (int i = 0; i < k; i++) {
            int indices[2] = {x_start + i, z_start + i};
            double values[2] = {1.0, -problem->min_purchase};

            ralph_add_constraint(model, 2, indices, values, RALPH_GREATER_EQUAL, 0.0);
        }
    }

    /* Constraint: Must reach destination */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double total_fuel_needed = fw_calc_total_fuel_consumed(problem);
        double min_end = (problem->minimum_fuel_at_end > 0) ?
                         problem->minimum_fuel_at_end : problem->minimum_fuel;
        double rhs = min_end + total_fuel_needed - problem->current_fuel;

        if (rhs > 0) {
            ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    /* Solve */
    ralph_set_int_param(model, "verbose", 0);
    int ret = ralph_optimize(model);
    RalphStatus status = ralph_get_status(model);

    if (ret == 0 && status == RALPH_STATUS_OPTIMAL) {
        double *x = malloc(num_vars * sizeof(double));
        if (!x) {
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }
        ralph_get_solution(model, x);

        /* Allocate solution arrays */
        solution->purchases = malloc(k * sizeof(double));
        solution->stop_flags = malloc(k * sizeof(int));
        if (!solution->purchases || !solution->stop_flags) {
            free(x);
            free(solution->purchases);
            free(solution->stop_flags);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        double gross_cost = 0.0;
        double total_purchased = 0.0;
        double stop_costs = 0.0;
        int num_stops = 0;

        for (int i = 0; i < k; i++) {
            solution->purchases[i] = x[x_start + i];
            solution->stop_flags[i] = (x[z_start + i] > 0.5) ? 1 : 0;

            gross_cost += solution->purchases[i] * problem->stations[i].price;
            total_purchased += solution->purchases[i];
            stop_costs += x[z_start + i] * problem->stop_cost;

            if (solution->stop_flags[i]) {
                num_stops++;
            }
        }

        /* Calculate remaining fuel and total cost */
        double total_consumed = fw_calc_total_fuel_consumed(problem);
        solution->remaining_fuel = problem->current_fuel + total_purchased - total_consumed;
        solution->gross_cost = gross_cost;

        if (est_price > 0.0) {
            double empty_capacity = problem->tank_capacity - solution->remaining_fuel;
            solution->total_cost = gross_cost + stop_costs + empty_capacity * est_price;
        } else {
            solution->total_cost = gross_cost + stop_costs;
        }

        solution->num_stops = num_stops;
        solution->status = FW_STATUS_OPTIMAL;

        free(x);
        ralph_free(model);
        return 0;
    }

    /* Handle failure */
    if (status == RALPH_STATUS_INFEASIBLE) {
        solution->status = FW_STATUS_INFEASIBLE;
    } else {
        solution->status = FW_STATUS_ERROR;
    }

    ralph_free(model);
    return -1;
}

/* ============================================================================
 * Refueling Problem Solving - Benders Decomposition
 *
 * True Benders decomposition using ralph_solve_benders():
 * - Master problem: binary z[i] variables (stop decisions) + θ (recourse cost)
 * - Subproblem: continuous x[i] (purchases), y[i] (cumulative fuel)
 * - Linking constraints: x[i] <= tank_capacity * z[i], x[i] >= min_purchase * z[i]
 *
 * This scales to k > 30 stations where enumeration (2^k) would timeout.
 * ============================================================================ */

/* Threshold for using Benders vs MILP. 0 = always use Benders. */
static int fw_benders_threshold = 0;

/*
 * Set the threshold for using Benders decomposition.
 * If num_stations > threshold, use Benders; otherwise use MILP.
 * Set to 0 to always use Benders (default).
 */
void fw_set_benders_threshold(int threshold) {
    fw_benders_threshold = threshold;
}

int fw_get_benders_threshold(void) {
    return fw_benders_threshold;
}

int fw_solve_refuel_benders(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution)
{
    if (!problem || !solution) return -1;

    memset(solution, 0, sizeof(FWRefuelSolution));

    int k = problem->num_stations;
    if (k == 0) {
        solution->status = FW_STATUS_OPTIMAL;
        solution->total_cost = 0.0;
        solution->remaining_fuel = problem->current_fuel -
            fw_calc_total_fuel_consumed(problem);
        return 0;
    }

    /* Validate k to prevent integer overflow in allocations */
    if (k < 0 || k > FW_MAX_STATIONS) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    /* If no integer constraints needed, use LP directly */
    if (problem->min_purchase < 0.01 && problem->stop_cost < 0.01) {
        return fw_solve_refuel_lp(problem, solution);
    }

    /* Use MILP for small k if threshold is set */
    if (fw_benders_threshold > 0 && k <= fw_benders_threshold) {
        return fw_solve_refuel_milp(problem, solution);
    }

    /*
     * Build full MILP model for Benders decomposition.
     *
     * Variable layout:
     *   x[0..k-1]     - fuel purchased at each station (continuous, subproblem)
     *   y[k..2k-1]    - cumulative fuel before arriving (continuous, subproblem)
     *   z[2k..3k-1]   - stop decisions (binary, master)
     *   θ = 3k        - recourse cost (continuous, master)
     *
     * Constraints:
     *   - Fuel balance: y[i] = current_fuel + sum(x[j] for j < i)
     *   - Min fuel at arrival: y[i] >= min_fuel + consumed_to_i
     *   - Tank capacity: y[i] + x[i] <= tank_capacity + consumed_to_i
     *   - Linking upper: x[i] <= tank_capacity * z[i]
     *   - Linking lower: x[i] >= min_purchase * z[i] (if min_purchase > 0)
     *   - Reach destination: sum(x[i]) >= total_needed
     */

    int x_start = 0;
    int y_start = k;
    int z_start = 2 * k;
    int theta_idx = 3 * k;
    int num_vars = 3 * k + 1;

    RalphModel *model = ralph_create();
    if (!model) {
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    /* Add x[i] variables - fuel purchased (subproblem) */
    double est_price = problem->remaining_fuel_value;
    for (int i = 0; i < k; i++) {
        double obj_coeff = problem->stations[i].price - est_price;
        ralph_add_var(model, 0.0, problem->tank_capacity, obj_coeff, RALPH_CONTINUOUS);
    }

    /* Add y[i] variables - cumulative fuel (subproblem) */
    double y_upper = problem->current_fuel + k * problem->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);
    }

    /* Add z[i] variables - stop decisions (master) with stop_cost in objective */
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, 1.0, problem->stop_cost, RALPH_BINARY);
    }

    /* Add θ variable - recourse cost (master) */
    /* Bounds: θ represents total fuel cost/credit. Use reasonable bounds to avoid
     * numerical issues in the LP solver. The bounds are based on:
     * - max_cost = k * tank_capacity * max_price
     * - max_credit = k * tank_capacity * remaining_fuel_value
     * Using 100x safety margin for robustness. */
    double max_price = 0.0;
    for (int i = 0; i < k; i++) {
        if (problem->stations[i].price > max_price) {
            max_price = problem->stations[i].price;
        }
    }
    double max_fuel_value = k * problem->tank_capacity *
        (max_price > est_price ? max_price : est_price);
    double theta_bound = 100.0 * (max_fuel_value + 1.0);  /* Safety margin */
    double theta_lb = -theta_bound;
    double theta_ub = theta_bound;
    ralph_add_var(model, theta_lb, theta_ub, 1.0, RALPH_CONTINUOUS);

    /* Constraint: Fuel balance - y[i] = current_fuel + sum(x[j] for j < i) */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        ralph_add_constraint(model, nnz, indices, values, RALPH_EQUAL, problem->current_fuel);
        free(indices);
        free(values);
    }

    /* Constraint: Minimum fuel at arrival */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->minimum_fuel + fuel_consumed;

        ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
    }

    /* Constraint: Tank capacity after refuel */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->tank_capacity + fuel_consumed;

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, rhs);
    }

    /* Linking constraint: x[i] <= tank_capacity * z[i]
     * Rewritten as: x[i] - tank_capacity * z[i] <= 0 */
    for (int i = 0; i < k; i++) {
        int indices[2] = {x_start + i, z_start + i};
        double values[2] = {1.0, -problem->tank_capacity};

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, 0.0);
    }

    /* Linking constraint: x[i] >= min_purchase * z[i] (if min_purchase > 0)
     * Rewritten as: x[i] - min_purchase * z[i] >= 0 */
    if (problem->min_purchase > 0.01) {
        for (int i = 0; i < k; i++) {
            int indices[2] = {x_start + i, z_start + i};
            double values[2] = {1.0, -problem->min_purchase};

            ralph_add_constraint(model, 2, indices, values, RALPH_GREATER_EQUAL, 0.0);
        }
    }

    /* Constraint: Reach destination */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = x_start + i;
            values[i] = 1.0;
        }

        double total_fuel_needed = fw_calc_total_fuel_consumed(problem);
        double min_end = (problem->minimum_fuel_at_end > 0) ?
                         problem->minimum_fuel_at_end : problem->minimum_fuel;
        double rhs = min_end + total_fuel_needed - problem->current_fuel;

        if (rhs > 0) {
            ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, rhs);
        }

        free(indices);
        free(values);
    }

    /* Add a trivial master constraint: sum(z[i]) >= 0
     * This is always satisfied (z[i] >= 0 by definition) but needed because
     * Benders requires at least one pure-master constraint to work correctly.
     * Without this, all constraints are linking (involve both z and x).
     */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        for (int i = 0; i < k; i++) {
            indices[i] = z_start + i;
            values[i] = 1.0;
        }

        ralph_add_constraint(model, k, indices, values, RALPH_GREATER_EQUAL, 0.0);
        free(indices);
        free(values);
    }

    /* Configure Benders decomposition */
    int *master_vars = malloc(k * sizeof(int));
    if (!master_vars) {
        ralph_free(model);
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    /* Master variables are z[0..k-1] (at indices z_start to z_start+k-1) */
    for (int i = 0; i < k; i++) {
        master_vars[i] = z_start + i;
    }

    RalphBendersConfig config = RALPH_BENDERS_CONFIG_DEFAULT;
    config.master_var_indices = master_vars;
    config.num_master_vars = k;
    config.theta_var = theta_idx;
    config.verbose = 2;  /* Debug: enable verbose output (level 2 for cut details) */

    /* Solve with Benders */
    double *x = malloc(num_vars * sizeof(double));
    RalphBendersResult result;

    if (!x) {
        free(master_vars);
        ralph_free(model);
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    int ret = ralph_solve_benders(model, &config, x, &result);

    free(master_vars);

    if (ret == 0 && result.status == RALPH_STATUS_OPTIMAL) {
        /* Allocate solution arrays */
        solution->purchases = malloc(k * sizeof(double));
        solution->stop_flags = malloc(k * sizeof(int));
        if (!solution->purchases || !solution->stop_flags) {
            free(x);
            free(solution->purchases);
            free(solution->stop_flags);
            ralph_free(model);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        double gross_cost = 0.0;
        double total_purchased = 0.0;
        double stop_costs = 0.0;
        int num_stops = 0;

        for (int i = 0; i < k; i++) {
            solution->purchases[i] = x[x_start + i];
            solution->stop_flags[i] = (x[z_start + i] > 0.5) ? 1 : 0;

            gross_cost += solution->purchases[i] * problem->stations[i].price;
            total_purchased += solution->purchases[i];
            stop_costs += solution->stop_flags[i] * problem->stop_cost;

            if (solution->stop_flags[i]) {
                num_stops++;
            }
        }

        double total_consumed = fw_calc_total_fuel_consumed(problem);
        solution->remaining_fuel = problem->current_fuel + total_purchased - total_consumed;
        solution->gross_cost = gross_cost;

        if (est_price > 0.0) {
            double empty_capacity = problem->tank_capacity - solution->remaining_fuel;
            solution->total_cost = gross_cost + stop_costs + empty_capacity * est_price;
        } else {
            solution->total_cost = gross_cost + stop_costs;
        }

        solution->num_stops = num_stops;
        solution->status = FW_STATUS_OPTIMAL;

        free(x);
        ralph_free(model);
        return 0;
    }

    /* Handle failure */
    free(x);
    ralph_free(model);

    if (result.status == RALPH_STATUS_INFEASIBLE) {
        solution->status = FW_STATUS_INFEASIBLE;
    } else {
        solution->status = FW_STATUS_ERROR;
    }

    return -1;
}

/* ============================================================================
 * Solution Management
 * ============================================================================ */

void fw_free_solution(FWRefuelSolution *solution)
{
    if (solution) {
        free(solution->purchases);
        free(solution->stop_flags);
        solution->purchases = NULL;
        solution->stop_flags = NULL;
    }
}

/* ============================================================================
 * Problem Validation
 * ============================================================================ */

int fw_validate_problem(
    const FWRefuelProblem *problem,
    char *error_msg,
    int error_msg_size)
{
    if (!problem) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Problem is NULL");
        return 0;
    }

    /* Check basic parameters */
    if (problem->tank_capacity <= 0) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid tank capacity");
        return 0;
    }

    if (problem->current_fuel < 0 || problem->current_fuel > problem->tank_capacity) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid current fuel level");
        return 0;
    }

    if (problem->minimum_fuel < 0 || problem->minimum_fuel > problem->tank_capacity) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid minimum fuel level");
        return 0;
    }

    if (problem->base_consumption <= 0 && problem->num_segments == 0) {
        if (error_msg) snprintf(error_msg, error_msg_size, "Invalid consumption rate");
        return 0;
    }

    /* Check stations are sorted */
    for (int i = 1; i < problem->num_stations; i++) {
        if (problem->stations[i].distance_from_start <
            problem->stations[i-1].distance_from_start) {
            if (error_msg) snprintf(error_msg, error_msg_size,
                "Stations not sorted by distance");
            return 0;
        }
    }

    /* Check feasibility: can we reach each station? */
    double fuel = problem->current_fuel;
    double last_dist = 0;

    for (int i = 0; i < problem->num_stations; i++) {
        double consumed = fw_calc_fuel_consumed(problem, last_dist,
            problem->stations[i].distance_from_start);

        fuel -= consumed;
        if (fuel < problem->minimum_fuel) {
            /* Could we reach with a full tank? */
            double max_fuel = problem->tank_capacity;
            double max_consumed = fw_calc_fuel_consumed(problem, last_dist,
                problem->stations[i].distance_from_start);
            if (max_fuel - max_consumed < problem->minimum_fuel) {
                if (error_msg) snprintf(error_msg, error_msg_size,
                    "Cannot reach station %d: too far from previous station",
                    problem->stations[i].station_id);
                return 0;
            }
        }

        /* Refuel to max */
        fuel = problem->tank_capacity;
        last_dist = problem->stations[i].distance_from_start;
    }

    /* Check we can reach destination */
    double final_consumed = fw_calc_fuel_consumed(problem, last_dist, problem->total_distance);
    fuel -= final_consumed;
    if (fuel < problem->minimum_fuel_at_end) {
        double max_fuel = problem->tank_capacity;
        if (max_fuel - final_consumed < problem->minimum_fuel_at_end) {
            if (error_msg) snprintf(error_msg, error_msg_size,
                "Cannot reach destination: too far from last station");
            return 0;
        }
    }

    return 1;
}
