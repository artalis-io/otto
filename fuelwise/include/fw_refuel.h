/*
 * FuelWise - Truck Refueling Optimization Library
 * Refueling Optimization
 *
 * Provides LP and MILP formulations for optimal fuel purchasing.
 *
 * Copyright (c) 2024. All rights reserved.
 */

#ifndef FW_REFUEL_H
#define FW_REFUEL_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Refueling Problem Solving
 * ============================================================================ */

/*
 * Solve a refueling optimization problem using LP relaxation.
 *
 * This is the fast solution method that may produce fractional purchases.
 * Use this when you don't need discrete stop decisions.
 *
 * The LP formulation:
 *   Variables: x[i] = fuel purchased at station i
 *              y[i] = cumulative fuel before arriving at station i
 *
 *   Objective: minimize sum(x[i] * price[i]) - remaining_fuel * estimated_price
 *
 *   Constraints:
 *   - Fuel balance: y[i] = current_fuel + sum(x[j] for j < i)
 *   - Min fuel at arrival: y[i] >= min_fuel + consumed_to_i
 *   - Tank capacity: y[i] + x[i] <= tank_capacity + consumed_to_i
 *   - Reach destination: sum(x[i]) >= total_consumed + min_end_fuel - current_fuel
 *
 * Parameters:
 *   problem  - The refueling problem definition
 *   solution - Output: solution (caller must call fw_free_solution)
 *
 * Returns:
 *   0 on success, -1 on error
 *
 * Notes:
 *   - If remaining_fuel_value > 0, the solution credits remaining fuel
 *   - The solution's total_cost includes this credit adjustment
 */
int fw_solve_refuel_lp(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution
);

/*
 * Solve a refueling optimization problem using MILP.
 *
 * This formulation adds binary decision variables for stop/no-stop at each station.
 * Use this when you need to enforce minimum purchase amounts or stop costs.
 *
 * Additional MILP formulation:
 *   Variables: z[i] = 1 if stopping at station i, 0 otherwise
 *
 *   Additional constraints:
 *   - Link purchase to stop: x[i] <= tank_capacity * z[i]
 *   - Minimum purchase: x[i] >= min_purchase * z[i] (if min_purchase > 0)
 *
 *   Additional objective:
 *   - Stop cost: + sum(stop_cost * z[i]) (if stop_cost > 0)
 *
 * Parameters:
 *   problem  - The refueling problem definition
 *   solution - Output: solution (caller must call fw_free_solution)
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_solve_refuel_milp(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution
);

/*
 * Solve a refueling optimization problem using Benders decomposition.
 *
 * This decomposes the MILP into:
 *   - Master problem: binary z[i] variables (stop decisions)
 *   - Subproblem: continuous x[i], y[i] given fixed z
 *
 * When the subproblem is infeasible for a given z, a Farkas feasibility
 * cut is added to the master problem.
 *
 * This approach can be faster than full MILP for problems with many
 * integer variables but relatively simple continuous structure.
 *
 * Parameters:
 *   problem  - The refueling problem definition
 *   solution - Output: solution (caller must call fw_free_solution)
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_solve_refuel_benders(
    const FWRefuelProblem *problem,
    FWRefuelSolution *solution
);

/*
 * Set the threshold for using Benders decomposition.
 *
 * If num_stations > threshold, use Benders; otherwise use MILP.
 * Default is 0, meaning always use Benders decomposition.
 *
 * Parameters:
 *   threshold - Station count threshold (0 = always use Benders)
 */
void fw_set_benders_threshold(int threshold);

/*
 * Get the current Benders threshold.
 *
 * Returns:
 *   Current threshold value
 */
int fw_get_benders_threshold(void);

/* ============================================================================
 * MIP Hint Flags
 *
 * Control which domain-specific MIP enhancements are active.
 * Default (0) enables all hints. Set flags to disable specific hints.
 * Useful for benchmarking the impact of individual optimizations.
 * NOT thread-safe if modified concurrently with solving.
 * ============================================================================ */

#define FW_HINT_NO_PRIORITIES     (1 << 0)  /* Disable branching priorities */
#define FW_HINT_NO_DIRECTIONS     (1 << 1)  /* Disable branching directions */
#define FW_HINT_NO_REACH_CUTS     (1 << 2)  /* Disable reach-cut callback */
#define FW_HINT_NO_MANDATORY_FIX  (1 << 3)  /* Disable mandatory station fixing */
#define FW_HINT_NO_DOMINATED_ELIM (1 << 4)  /* Disable dominated station elimination */
#define FW_HINT_NO_SYMMETRY_BREAK (1 << 5)  /* Disable symmetry-breaking constraints */

/* Disable all hints (raw MILP, no domain intelligence) */
#define FW_HINT_NONE              (0x3F)

void fw_set_mip_hint_flags(int flags);
int fw_get_mip_hint_flags(void);
void fw_set_presolve(int enable, unsigned int mask);

/*
 * Export the MILP model as an LP file (without domain hints).
 *
 * Builds the raw MILP formulation and writes it in CPLEX LP format.
 * The exported model does not include reach cuts, branching priorities,
 * or directions — suitable for solving with external solvers like GLPK.
 *
 * Parameters:
 *   problem - The refueling problem definition
 *   path    - Output file path (.lp)
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_export_milp_lp(
    const FWRefuelProblem *problem,
    const char *path
);

/*
 * Free resources allocated in a solution.
 *
 * Parameters:
 *   solution - Solution to free (can be NULL)
 */
void fw_free_solution(FWRefuelSolution *solution);

/* ============================================================================
 * Fuel Consumption Calculation
 * ============================================================================ */

/*
 * Calculate fuel consumed between two distances along the route.
 *
 * Uses piecewise linear segments if defined, otherwise uses constant rate.
 *
 * Parameters:
 *   problem       - The refueling problem (for consumption rates)
 *   from_distance - Starting distance in meters
 *   to_distance   - Ending distance in meters
 *
 * Returns:
 *   Fuel consumed in liters
 */
double fw_calc_fuel_consumed(
    const FWRefuelProblem *problem,
    double from_distance,
    double to_distance
);

/*
 * Calculate total fuel consumed for the entire route.
 *
 * Parameters:
 *   problem - The refueling problem
 *
 * Returns:
 *   Total fuel consumed in liters
 */
double fw_calc_total_fuel_consumed(const FWRefuelProblem *problem);

/* ============================================================================
 * Problem Validation
 * ============================================================================ */

/*
 * Validate a refueling problem for feasibility.
 *
 * Checks:
 *   - Stations are sorted by distance
 *   - Tank capacity is sufficient to reach each station
 *   - Current fuel + available purchases can meet destination requirement
 *
 * Parameters:
 *   problem - The refueling problem to validate
 *   error_msg - Output: error message if invalid (can be NULL)
 *   error_msg_size - Size of error_msg buffer
 *
 * Returns:
 *   1 if valid, 0 if invalid
 */
int fw_validate_problem(
    const FWRefuelProblem *problem,
    char *error_msg,
    int error_msg_size
);

#ifdef __cplusplus
}
#endif

#endif /* FW_REFUEL_H */
