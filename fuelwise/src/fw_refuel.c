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
 * ============================================================================ */

double fw_calc_fuel_consumed(
    const FWRefuelProblem *problem,
    double from_distance,
    double to_distance)
{
    if (from_distance >= to_distance) return 0.0;

    /* Use constant rate if no segments defined */
    if (problem->num_segments == 0 || problem->segments == NULL) {
        return (to_distance - from_distance) / problem->base_consumption_mpg;
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
            double segment_distance = range_end - range_start;
            double segment_fuel = segment_distance / problem->segments[s].consumption_mpg;
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
        double obj_coeff = problem->stations[i].price_per_gallon - est_price;
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
            gross_cost += solution->purchases[i] * problem->stations[i].price_per_gallon;
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
        double obj_coeff = problem->stations[i].price_per_gallon - est_price;
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

            gross_cost += solution->purchases[i] * problem->stations[i].price_per_gallon;
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
 * ============================================================================ */

/*
 * Bookkeeping for subproblem constraint mapping.
 * Tracks which constraints depend on z and their RHS values for Farkas cuts.
 */
typedef struct {
    int num_stations;
    int num_constraints;

    /* Constraint indices (into subproblem constraint array) */
    int *upper_bound_con_idx;   /* x[i] <= tank_capacity * z[i] */
    int *lower_bound_con_idx;   /* x[i] >= min_purchase * z[i] (or -1 if not present) */

    /* RHS values for all constraints (for Farkas cut computation) */
    double *rhs;

    /* Problem parameters needed for cut coefficients */
    double tank_capacity;
    double min_purchase;
} BendersSubproblemMap;

static BendersSubproblemMap* benders_map_create(int k, double tank_cap, double min_purch) {
    BendersSubproblemMap *map = malloc(sizeof(BendersSubproblemMap));
    if (!map) return NULL;

    map->num_stations = k;
    map->num_constraints = 0;
    map->tank_capacity = tank_cap;
    map->min_purchase = min_purch;

    map->upper_bound_con_idx = malloc(k * sizeof(int));
    map->lower_bound_con_idx = malloc(k * sizeof(int));
    /* Allocate enough for max constraints: k fuel balance + k min fuel + k tank cap + k upper + k lower + 1 reach */
    map->rhs = malloc((5 * k + 1) * sizeof(double));

    if (!map->upper_bound_con_idx || !map->lower_bound_con_idx || !map->rhs) {
        free(map->upper_bound_con_idx);
        free(map->lower_bound_con_idx);
        free(map->rhs);
        free(map);
        return NULL;
    }

    for (int i = 0; i < k; i++) {
        map->upper_bound_con_idx[i] = -1;
        map->lower_bound_con_idx[i] = -1;
    }

    return map;
}

static void benders_map_free(BendersSubproblemMap *map) {
    if (map) {
        free(map->upper_bound_con_idx);
        free(map->lower_bound_con_idx);
        free(map->rhs);
        free(map);
    }
}

/*
 * Build the subproblem LP with fixed z values.
 * Returns the model and populates the constraint map for Farkas cut generation.
 */
static RalphModel* build_benders_subproblem(
    const FWRefuelProblem *problem,
    const int *z_fixed,
    BendersSubproblemMap *map)
{
    int k = problem->num_stations;

    RalphModel *model = ralph_create();
    if (!model) return NULL;

    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    int x_start = 0;
    int y_start = k;
    int con_idx = 0;

    /* Add x[i] variables */
    double est_price = problem->remaining_fuel_value;
    for (int i = 0; i < k; i++) {
        double obj_coeff = problem->stations[i].price_per_gallon - est_price;
        ralph_add_var(model, 0.0, problem->tank_capacity, obj_coeff, RALPH_CONTINUOUS);
    }

    /* Add y[i] variables */
    double y_upper = problem->current_fuel + k * problem->tank_capacity;
    for (int i = 0; i < k; i++) {
        ralph_add_var(model, 0.0, y_upper, 0.0, RALPH_CONTINUOUS);
    }

    /* Constraint 1: Fuel balance - y[i] = current_fuel + sum(x[j] for j < i) */
    for (int i = 0; i < k; i++) {
        int nnz = 1 + i;
        int *indices = malloc(nnz * sizeof(int));
        double *values = malloc(nnz * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            return NULL;
        }

        indices[0] = y_start + i;
        values[0] = 1.0;

        for (int j = 0; j < i; j++) {
            indices[1 + j] = x_start + j;
            values[1 + j] = -1.0;
        }

        double rhs = problem->current_fuel;
        ralph_add_constraint(model, nnz, indices, values, RALPH_EQUAL, rhs);
        map->rhs[con_idx++] = rhs;

        free(indices);
        free(values);
    }

    /* Constraint 2: Minimum fuel at arrival */
    for (int i = 0; i < k; i++) {
        int indices[1] = {y_start + i};
        double values[1] = {1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->minimum_fuel + fuel_consumed;

        ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
        map->rhs[con_idx++] = rhs;
    }

    /* Constraint 3: Tank capacity after refuel */
    for (int i = 0; i < k; i++) {
        int indices[2] = {y_start + i, x_start + i};
        double values[2] = {1.0, 1.0};
        double fuel_consumed = fw_calc_fuel_consumed(problem, 0,
            problem->stations[i].distance_from_start);
        double rhs = problem->tank_capacity + fuel_consumed;

        ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, rhs);
        map->rhs[con_idx++] = rhs;
    }

    /* Constraint 4: Link x[i] to z[i] (upper bound) - x[i] <= tank_capacity * z[i]
     * With fixed z, this becomes: x[i] <= tank_capacity * z_fixed[i]
     * For Farkas cut, we store the CONSTANT part of RHS (0), not the z-dependent part.
     */
    for (int i = 0; i < k; i++) {
        int indices[1] = {x_start + i};
        double values[1] = {1.0};
        double rhs = problem->tank_capacity * z_fixed[i];

        ralph_add_constraint(model, 1, indices, values, RALPH_LESS_EQUAL, rhs);
        map->upper_bound_con_idx[i] = con_idx;
        map->rhs[con_idx++] = 0.0;  /* Constant part of RHS is 0 */
    }

    /* Constraint 5: Minimum purchase (lower bound) - x[i] >= min_purchase * z[i]
     * With fixed z: x[i] >= min_purchase * z_fixed[i]
     * For Farkas cut, we store the CONSTANT part of RHS (0).
     */
    if (problem->min_purchase > 0.01) {
        for (int i = 0; i < k; i++) {
            int indices[1] = {x_start + i};
            double values[1] = {1.0};
            double rhs = problem->min_purchase * z_fixed[i];

            ralph_add_constraint(model, 1, indices, values, RALPH_GREATER_EQUAL, rhs);
            map->lower_bound_con_idx[i] = con_idx;
            map->rhs[con_idx++] = 0.0;  /* Constant part of RHS is 0 */
        }
    }

    /* Constraint 6: Reach destination */
    {
        int *indices = malloc(k * sizeof(int));
        double *values = malloc(k * sizeof(double));
        if (!indices || !values) {
            free(indices);
            free(values);
            ralph_free(model);
            return NULL;
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
            map->rhs[con_idx++] = rhs;
        }

        free(indices);
        free(values);
    }

    map->num_constraints = con_idx;
    return model;
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

    /* For small k, enumerate all 2^k z combinations directly.
     * This is simpler and more robust than Benders with MIP master.
     * For k <= 20, 2^k = ~1M which is tractable.
     */
    if (k > 20) {
        /* Fall back to MILP for large problems */
        return fw_solve_refuel_milp(problem, solution);
    }

    /* Working arrays */
    int *z_fixed = malloc(k * sizeof(int));
    BendersSubproblemMap *map = benders_map_create(k, problem->tank_capacity, problem->min_purchase);

    if (!z_fixed || !map) {
        free(z_fixed);
        benders_map_free(map);
        solution->status = FW_STATUS_ERROR;
        return -1;
    }

    int found_optimal = 0;
    double best_obj = 1e30;
    double *best_purchases = NULL;
    int *best_z = NULL;

    /* Enumerate all 2^k combinations */
    int num_combinations = 1 << k;  /* 2^k */

    for (int combo = 1; combo < num_combinations; combo++) {
        /* Decode combo into z values (skip combo=0 which is all zeros) */
        for (int i = 0; i < k; i++) {
            z_fixed[i] = (combo >> i) & 1;
        }

        /* Quick lower bound check: if stop costs alone exceed best, skip */
        double stop_cost_sum = 0.0;
        for (int i = 0; i < k; i++) {
            stop_cost_sum += z_fixed[i] * problem->stop_cost;
        }
        if (stop_cost_sum >= best_obj) {
            continue;  /* Can't improve */
        }

        /* Build and solve subproblem with fixed z */
        RalphModel *subproblem = build_benders_subproblem(problem, z_fixed, map);
        if (!subproblem) {
            continue;  /* Skip this z */
        }

        ralph_set_int_param(subproblem, "verbose", 0);
        ralph_optimize(subproblem);
        RalphStatus sub_status = ralph_get_status(subproblem);

        if (sub_status == RALPH_STATUS_OPTIMAL) {
            double sub_obj = ralph_get_objval(subproblem);
            double total_obj = sub_obj + stop_cost_sum;

            if (total_obj < best_obj) {
                best_obj = total_obj;

                /* Store best solution */
                if (!best_purchases) {
                    best_purchases = malloc(k * sizeof(double));
                    best_z = malloc(k * sizeof(int));
                }

                int num_vars = 2 * k;
                double *sub_sol = malloc(num_vars * sizeof(double));
                ralph_get_solution(subproblem, sub_sol);

                for (int i = 0; i < k; i++) {
                    best_purchases[i] = sub_sol[i];
                    best_z[i] = z_fixed[i];
                }

                free(sub_sol);
                found_optimal = 1;
            }
        }

        ralph_free(subproblem);
    }

    /* Build final solution */
    if (found_optimal && best_purchases && best_z) {
        solution->purchases = malloc(k * sizeof(double));
        solution->stop_flags = malloc(k * sizeof(int));
        if (!solution->purchases || !solution->stop_flags) {
            free(solution->purchases);
            free(solution->stop_flags);
            free(z_fixed);
            free(best_purchases);
            free(best_z);
            benders_map_free(map);
            solution->status = FW_STATUS_ERROR;
            return -1;
        }

        double gross_cost = 0.0;
        double total_purchased = 0.0;
        double stop_costs = 0.0;
        int num_stops = 0;

        for (int i = 0; i < k; i++) {
            solution->purchases[i] = best_purchases[i];
            solution->stop_flags[i] = best_z[i];

            gross_cost += solution->purchases[i] * problem->stations[i].price_per_gallon;
            total_purchased += solution->purchases[i];
            stop_costs += best_z[i] * problem->stop_cost;

            if (best_z[i]) num_stops++;
        }

        double total_consumed = fw_calc_total_fuel_consumed(problem);
        solution->remaining_fuel = problem->current_fuel + total_purchased - total_consumed;
        solution->gross_cost = gross_cost;

        double est_price = problem->remaining_fuel_value;
        if (est_price > 0.0) {
            double empty_capacity = problem->tank_capacity - solution->remaining_fuel;
            solution->total_cost = gross_cost + stop_costs + empty_capacity * est_price;
        } else {
            solution->total_cost = gross_cost + stop_costs;
        }

        solution->num_stops = num_stops;
        solution->status = FW_STATUS_OPTIMAL;
    } else {
        solution->status = FW_STATUS_INFEASIBLE;
    }

    /* Cleanup */
    free(z_fixed);
    free(best_purchases);
    free(best_z);
    benders_map_free(map);

    return (solution->status == FW_STATUS_OPTIMAL) ? 0 : -1;
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

    if (problem->base_consumption_mpg <= 0 && problem->num_segments == 0) {
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
