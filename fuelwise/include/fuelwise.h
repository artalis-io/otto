/*
 * FuelWise - Truck Refueling Optimization Library
 * Unified Public API
 *
 * This header provides the main entry points for the FuelWise library.
 * Include this header to access all functionality.
 *
 * Copyright (c) 2024. All rights reserved.
 */

#ifndef FUELWISE_H
#define FUELWISE_H

/* Include all sub-modules */
#include "fw_types.h"
#include "fw_geo.h"
#include "fw_route.h"
#include "fw_refuel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Version Information
 * ============================================================================ */

#define FUELWISE_VERSION_MAJOR 1
#define FUELWISE_VERSION_MINOR 0
#define FUELWISE_VERSION_PATCH 0

/*
 * Get the library version string.
 *
 * Returns:
 *   Version string in format "major.minor.patch"
 */
const char* fw_version(void);

/* ============================================================================
 * High-Level Optimization API
 *
 * These functions provide a complete pipeline from raw input to solution.
 * ============================================================================ */

/*
 * Perform complete route optimization.
 *
 * This is the main entry point for optimization. It:
 * 1. Filters stations to those near the route
 * 2. Snaps them to the route polyline
 * 3. Solves the refueling optimization problem
 *
 * Parameters:
 *   request  - Complete optimization request
 *   response - Output: optimization result (caller must call fw_free_response)
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_optimize(
    const FWOptimizeRequest *request,
    FWOptimizeResponse *response
);

/*
 * Free an optimization response.
 *
 * Parameters:
 *   response - Response to free (can be NULL)
 */
void fw_free_response(FWOptimizeResponse *response);

/* ============================================================================
 * Convenience Functions
 *
 * Simpler interfaces for common use cases.
 * ============================================================================ */

/*
 * Simple route optimization with default settings.
 *
 * Parameters:
 *   stations        - Array of fuel stations
 *   num_stations    - Number of stations
 *   route           - Route polyline
 *   tank_capacity   - Tank capacity in gallons
 *   current_fuel    - Current fuel level in gallons
 *   consumption_mpg - Miles per gallon
 *   min_fuel        - Minimum fuel level to maintain
 *   solution        - Output: refueling solution
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_optimize_simple(
    const FWStation *stations,
    int num_stations,
    const FWPolyline *route,
    double tank_capacity,
    double current_fuel,
    double consumption_mpg,
    double min_fuel,
    FWRefuelSolution *solution
);

/* ============================================================================
 * JSON Serialization (for API/WASM integration)
 * ============================================================================ */

/*
 * Serialize a refueling solution to JSON.
 *
 * Parameters:
 *   solution - The solution to serialize
 *
 * Returns:
 *   Allocated JSON string (caller must call fw_free_json), or NULL on error
 */
char* fw_solution_to_json(const FWRefuelSolution *solution);

/*
 * Serialize an optimization response to JSON.
 *
 * Parameters:
 *   response - The response to serialize
 *
 * Returns:
 *   Allocated JSON string (caller must call fw_free_json), or NULL on error
 */
char* fw_response_to_json(const FWOptimizeResponse *response);

/*
 * FW_FUTURE_API: Parse a refueling problem from JSON.
 *
 * NOTE: Not yet implemented - returns -1. Use API server for JSON parsing.
 *
 * Parameters:
 *   json    - JSON string
 *   problem - Output: parsed problem (caller must free stations array)
 *
 * Returns:
 *   0 on success, -1 on error (currently always returns -1)
 */
FW_FUTURE_API int fw_problem_from_json(const char *json, FWRefuelProblem *problem);

/*
 * FW_FUTURE_API: Parse an optimization request from JSON.
 *
 * NOTE: Not yet implemented - returns -1. Use API server for JSON parsing.
 *
 * Parameters:
 *   json    - JSON string
 *   request - Output: parsed request (caller must free allocated arrays)
 *
 * Returns:
 *   0 on success, -1 on error (currently always returns -1)
 */
FW_FUTURE_API int fw_request_from_json(const char *json, FWOptimizeRequest *request);

/*
 * Free a JSON string returned by serialization functions.
 *
 * Parameters:
 *   json - JSON string to free (can be NULL)
 */
void fw_free_json(char *json);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * Get a human-readable status message.
 *
 * Parameters:
 *   status - Status code
 *
 * Returns:
 *   Static string describing the status
 */
const char* fw_status_string(FWStatus status);

/*
 * Create a default filter configuration.
 *
 * Parameters:
 *   config - Output: filter configuration with sensible defaults
 */
void fw_default_filter_config(FWFilterConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* FUELWISE_H */
