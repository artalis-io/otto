/*
 * FuelWise Transport-Agnostic API Handler
 *
 * This header defines the API interface for FuelWise that can be used by:
 * - HTTP servers (Mongoose)
 * - WASM modules
 * - Unix sockets
 * - Direct C calls
 *
 * Copyright (c) 2024. All rights reserved.
 */

#ifndef FUELWISE_FW_API_H
#define FUELWISE_FW_API_H

#include <stddef.h>
#include <stdint.h>
#include "fuelwise.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * API Context and Types
 * ============================================================================ */

/**
 * Opaque API context.
 * Currently stateless, but provided for future expansion.
 */
typedef struct FWAPIContext FWAPIContext;

/**
 * API request structure.
 */
typedef struct {
    const char *path;       /* Request path (e.g., "/api/v1/solve") */
    const char *query;      /* Query string (for GET requests) */
    const char *body;       /* Request body (for POST requests) */
    size_t body_len;        /* Body length */
    const char *host;       /* Optional: host for URL generation */
} FWAPIRequest;

/**
 * API response structure.
 */
typedef struct {
    int status_code;        /* HTTP status code */
    const char *content_type; /* Content type (e.g., "application/json") */
    char *body;             /* Response body (caller frees via fw_api_response_free) */
    size_t body_len;        /* Body length */
} FWAPIResponse;

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================ */

/**
 * Create an API context.
 *
 * @return New context, or NULL on error. Caller must free with fw_api_free().
 */
FWAPIContext *fw_api_create(void);

/**
 * Free an API context.
 *
 * @param ctx Context to free (can be NULL)
 */
void fw_api_free(FWAPIContext *ctx);

/**
 * Free an API response.
 *
 * @param resp Response to free (can be NULL)
 */
void fw_api_response_free(FWAPIResponse *resp);

/* ============================================================================
 * Core Handler
 * ============================================================================ */

/**
 * Handle an API request.
 *
 * Routes the request to the appropriate handler based on path:
 *   POST /api/v1/solve    - Solve refueling problem
 *   POST /api/v1/filter   - Filter stations to route
 *   POST /api/v1/optimize - Full optimization pipeline
 *   GET  /api/v1/health   - Health check
 *   GET  /api/v1/stats    - Statistics
 *
 * @param ctx  API context (can be NULL for stateless operations)
 * @param req  Request to handle
 * @param resp Response (caller must call fw_api_response_free)
 * @return 0 on success, -1 on error
 */
int fw_api_handle(FWAPIContext *ctx,
                  const FWAPIRequest *req,
                  FWAPIResponse *resp);

/* ============================================================================
 * API Endpoint Annotations
 *
 * These annotations are parsed by scripts/build-api-docs.py to generate documentation.
 * ============================================================================ */

/*@api
 * POST /api/v1/solve
 * Solve a refueling optimization problem
 *
 * Given a set of fuel stations along a route, finds the minimum-cost
 * refueling plan that satisfies tank capacity and minimum fuel constraints.
 *
 * @body total_distance:number Total route distance in miles
 * @body tank_capacity:number Tank capacity in gallons
 * @body current_fuel:number Current fuel level in gallons
 * @body consumption_mpg:number:6.5 Base fuel consumption in miles per gallon
 * @body minimum_fuel:number:25 Minimum fuel level to maintain
 * @body min_purchase:number:0 Minimum gallons per stop (0 = no minimum, triggers MILP)
 * @body stop_cost:number:0 Fixed cost per fuel stop (0 = no cost, triggers MILP)
 * @body stations:array Array of stations with id, distance, and price
 * @body segments:array:[] Optional variable consumption segments
 *
 * @returns application/json Optimization solution with stops and costs
 * @error 400 Invalid request format or validation failure
 * @error 422 Problem is infeasible (cannot reach destination)
 * @error 500 Internal server error
 *
 * @request_body json
 * {
 *   "total_distance": 1000,
 *   "tank_capacity": 100,
 *   "current_fuel": 50,
 *   "consumption_mpg": 10,
 *   "minimum_fuel": 10,
 *   "stations": [
 *     {"id": 1, "distance": 200, "price": 1.20},
 *     {"id": 2, "distance": 500, "price": 1.00},
 *     {"id": 3, "distance": 700, "price": 1.30}
 *   ]
 * }
 *
 * @response_json
 * {
 *   "status": "OPTIMAL",
 *   "num_stops": 2,
 *   "total_cost": 68.00,
 *   "gross_cost": 68.00,
 *   "remaining_fuel": 10.00,
 *   "stops": [
 *     {"station_id": 1, "gallons": 30.00, "cost": 36.00},
 *     {"station_id": 2, "gallons": 30.00, "cost": 30.00}
 *   ]
 * }
 *
 * @example
 * curl -X POST http://localhost:8080/api/v1/solve \
 *   -H "Content-Type: application/json" \
 *   -d '{"total_distance":500,"tank_capacity":100,"current_fuel":30,"consumption_mpg":6.5,"minimum_fuel":25,"stations":[{"id":1,"distance":100,"price":3.50},{"id":2,"distance":250,"price":3.25}]}'
 *
 * @demo json
 * @demo_title Solve a refueling problem using LP optimization in WASM. No server required.
 */

/*@api
 * POST /api/v1/filter
 * Filter fuel stations to those near a route
 *
 * Given a set of stations with geographic coordinates and a route polyline,
 * returns only the stations within max_distance of the route, sorted by
 * distance along the route.
 *
 * @body stations:array Array of stations with lat, lon, price, and optional id
 * @body route:array Route polyline as [[lat, lon], [lat, lon], ...]
 * @body max_distance:number:5 Maximum perpendicular distance in miles
 *
 * @returns application/json Filtered and snapped stations
 * @error 400 Invalid request format
 * @error 500 Filter operation failed
 *
 * @response_json
 * {
 *   "count": 3,
 *   "stations": [
 *     {
 *       "station_id": 1,
 *       "distance_from_start": 45.2,
 *       "perpendicular_distance": 0.8,
 *       "price_per_gallon": 3.45,
 *       "snap_point": [34.0522, -118.2437]
 *     }
 *   ]
 * }
 *
 * @example
 * curl -X POST http://localhost:8080/api/v1/filter \
 *   -H "Content-Type: application/json" \
 *   -d '{"stations":[{"lat":34.05,"lon":-118.25,"price":3.45}],"route":[[34.0,-118.0],[34.1,-118.5]],"max_distance":5}'
 */

/*@api
 * POST /api/v1/optimize
 * Full optimization pipeline: filter stations and solve refueling
 *
 * Combines station filtering and refueling optimization in a single call.
 * Accepts stations with geographic coordinates and a route polyline,
 * filters to nearby stations, and solves the optimal refueling problem.
 *
 * @body stations:array Array of stations with lat, lon, price, and optional id
 * @body route:array Route polyline as [[lat, lon], [lat, lon], ...]
 * @body tank_capacity:number:100 Tank capacity in gallons
 * @body current_fuel:number:50 Current fuel level in gallons
 * @body consumption_mpg:number:6.5 Base fuel consumption in miles per gallon
 * @body minimum_fuel:number:25 Minimum fuel level to maintain
 * @body max_distance:number:5 Maximum perpendicular distance for filtering
 * @body min_purchase:number:0 Minimum gallons per stop (triggers MILP)
 * @body stop_cost:number:0 Fixed cost per fuel stop (triggers MILP)
 * @body segments:array:[] Optional variable consumption segments
 *
 * @returns application/json Complete optimization result
 * @error 400 Invalid request format or validation failure
 * @error 422 No stations found or problem infeasible
 * @error 500 Internal server error
 *
 * @response_json
 * {
 *   "status": "OPTIMAL",
 *   "route_distance": 487.5,
 *   "stations_filtered": 5,
 *   "num_stops": 2,
 *   "total_cost": 245.50,
 *   "gross_cost": 245.50,
 *   "remaining_fuel": 28.3,
 *   "stops": [
 *     {"station_id": 3, "distance_from_start": 125.4, "gallons": 42.0, "cost": 142.80},
 *     {"station_id": 7, "distance_from_start": 356.2, "gallons": 35.5, "cost": 102.70}
 *   ]
 * }
 *
 * @example
 * curl -X POST http://localhost:8080/api/v1/optimize \
 *   -H "Content-Type: application/json" \
 *   -d '{"stations":[{"lat":34.05,"lon":-118.25,"price":3.40,"id":1}],"route":[[34.0,-118.0],[35.0,-119.0]],"tank_capacity":100,"current_fuel":50}'
 */

/*@api
 * GET /api/v1/health
 * Health check endpoint
 *
 * Returns server health status. Bypasses rate limiting and work queue
 * for reliable monitoring.
 *
 * @returns application/json Health status
 *
 * @response_json
 * {
 *   "status": "healthy",
 *   "service": "fuelwise-api",
 *   "version": "1.0.0"
 * }
 *
 * @demo json
 * @demo_title Check FuelWise API health status
 * @demo_fetch /api/v1/health
 */

/*@api
 * GET /api/v1/stats
 * Server statistics endpoint
 *
 * Returns server statistics including rate limiting and work queue status.
 * Bypasses work queue for reliable monitoring.
 *
 * @returns application/json Server statistics
 *
 * @response_json
 * {
 *   "service": "fuelwise-api",
 *   "version": "1.0.0",
 *   "work_queue": {
 *     "enabled": true,
 *     "depth": 0,
 *     "capacity": 100
 *   },
 *   "rate_limit": {
 *     "enabled": true,
 *     "rps": 10.0,
 *     "burst": 100.0
 *   }
 * }
 *
 * @demo json
 * @demo_title View FuelWise server statistics
 * @demo_fetch /api/v1/stats
 */

/*@wasm
 * @export fuelwise_api_init
 * @export fuelwise_api_free
 * @export fuelwise_api_ready
 * @export fuelwise_api_handle
 * @export fuelwise_response_status
 * @export fuelwise_response_content_type
 * @export fuelwise_response_body
 * @export fuelwise_response_body_len
 * @export fuelwise_api_version
 * @export malloc
 * @export free
 */

#ifdef __cplusplus
}
#endif

#endif /* FUELWISE_FW_API_H */
