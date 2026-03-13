/*
 * Surge API Handler - Transport-agnostic request handling
 *
 * This module provides request handling logic that can be used by:
 * - Mongoose HTTP server (production)
 * - WASM exports (browser demo)
 * - Direct C API calls (testing)
 *
 * The API handler parses JSON VRP problems, solves them via the
 * Surge solver, and returns JSON solutions. All responses are returned
 * as allocated buffers that the caller must free.
 */

#ifndef SURGE_SG_API_H
#define SURGE_SG_API_H

#include <stddef.h>
#include <stdint.h>

#include "sg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*@api
 * POST /api/v1/solve
 * Solve a Vehicle Routing Problem
 *
 * @body application/json VRP problem definition
 * @returns application/json Solution with routes, stats, and unassigned
 * @error 400 Invalid or malformed request body
 * @error 500 Internal solver error
 *
 * @request_body json
 * {
 *   "vehicles": [
 *     {"id": 0, "depot_start": 0, "depot_end": 0,
 *      "capacity": [20], "shift": [0, 1000]}
 *   ],
 *   "depots": [
 *     {"id": 0, "x": 40.0, "y": 50.0, "tw": [0, 1000]}
 *   ],
 *   "tasks": [
 *     {"id": 0, "x": 45.0, "y": 55.0, "tw": [0, 500], "service": 10, "demand": [5]},
 *     {"id": 1, "x": 42.0, "y": 58.0, "tw": [0, 500], "service": 10, "demand": [3]},
 *     {"id": 2, "x": 38.0, "y": 52.0, "tw": [100, 800], "service": 10, "demand": [4]}
 *   ],
 *   "requests": [
 *     {"id": 0, "delivery_task": 0},
 *     {"id": 1, "delivery_task": 1},
 *     {"id": 2, "delivery_task": 2}
 *   ],
 *   "config": {"max_iterations": 1000}
 * }
 *
 * @response_json
 * {
 *   "status": "ok",
 *   "stats": {
 *     "iterations": 1000,
 *     "total_cost": 12345.67,
 *     "total_distance": 828.94,
 *     "unassigned": 0,
 *     "vehicles_used": 1
 *   }
 * }
 *
 * @example
 * curl -X POST http://localhost:8085/api/v1/solve \
 *   -H "Content-Type: application/json" \
 *   -d '{"vehicles":[{"id":0,"depot_start":0,"depot_end":0,"capacity":[20],"shift":[0,1000]}],"depots":[{"id":0,"x":40,"y":50,"tw":[0,1000]}],"tasks":[{"id":0,"x":45,"y":55,"tw":[0,500],"service":10,"demand":[5]}],"requests":[{"id":0,"delivery_task":0}],"config":{"max_iterations":1000}}'
 *
 * @demo json
 * @demo_title Solve a VRP problem using WASM
 */

/*@api
 * GET /api/v1/health
 * Health check endpoint
 *
 * @returns application/json Health status with service name and version
 *
 * @response_json
 * {
 *   "status": "healthy",
 *   "service": "surge",
 *   "version": "0.1.0-dev"
 * }
 *
 * @example
 * curl http://localhost:8085/api/v1/health
 *
 * @demo json
 * @demo_title Check Surge health status
 */

/*@api
 * GET /api/v1/stats
 * Statistics and status
 *
 * @returns application/json Service status with version
 *
 * @response_json
 * {
 *   "service": "surge",
 *   "version": "0.1.0-dev",
 *   "status": "ok"
 * }
 *
 * @example
 * curl http://localhost:8085/api/v1/stats
 */

/*@api
 * GET /api/v1/version
 * Version information
 *
 * @returns application/json Version string
 *
 * @response_json
 * {
 *   "version": "0.1.0-dev"
 * }
 *
 * @example
 * curl http://localhost:8085/api/v1/version
 */

/*@wasm
 * @export surge_api_init
 * @export surge_api_free
 * @export surge_api_ready
 * @export surge_api_handle
 * @export surge_api_solve
 * @export surge_response_status
 * @export surge_response_content_type
 * @export surge_response_body
 * @export surge_response_body_len
 * @export surge_api_version_string
 * @export malloc
 * @export free
 */

/* ============================================================================
 * API Context
 * ============================================================================ */

/**
 * Opaque API context.
 * Currently stateless, but provided for transport-agnostic conformance.
 */
typedef struct SGAPIContext SGAPIContext;

/**
 * Create an API context.
 *
 * @return New context, or NULL on error. Caller must free with sg_api_free().
 */
SGAPIContext *sg_api_create(void);

/**
 * Free an API context.
 *
 * @param ctx Context to free (can be NULL)
 */
void sg_api_free(SGAPIContext *ctx);

/* ============================================================================
 * Request/Response
 * ============================================================================ */

typedef struct {
    const char *path;       /* URI path (required) */
    const char *query;      /* Query string without '?' (optional, NULL ok) */
    const char *body;       /* Request body (optional, NULL ok for GET) */
    size_t body_len;        /* Body length in bytes */
    const char *host;       /* Optional: host for URL generation */
} SGAPIRequest;

typedef struct {
    int status_code;        /* HTTP status code (200, 400, 404, 500, etc.) */
    const char *content_type; /* MIME type (static string, do not free) */
    char *body;             /* Response body (caller must free) */
    size_t body_len;        /* Response body length in bytes */
} SGAPIResponse;

/* ============================================================================
 * Handler
 * ============================================================================ */

/*
 * Handle an API request.
 *
 * Routes the request based on path and generates the appropriate response.
 * The response body is heap-allocated - caller must free with
 * sg_api_response_free().
 *
 * Supported paths:
 *   POST /api/v1/solve    - Solve VRP problem
 *   GET  /api/v1/health   - Health check
 *   GET  /api/v1/version  - Version info
 *
 * Returns 0 on success (response filled in), -1 on internal error.
 */
int sg_api_handle(SGAPIContext *ctx, const SGAPIRequest *req, SGAPIResponse *resp);

/*
 * Free response body.
 *
 * Safe to call with NULL response or NULL body.
 */
void sg_api_response_free(SGAPIResponse *resp);

/* ============================================================================
 * Individual Handlers (for advanced use / WASM)
 * ============================================================================ */

/*
 * Solve a VRP problem from JSON input.
 *
 * Returns allocated JSON string, or NULL on failure.
 * Caller must free the returned buffer.
 */
char *sg_api_solve(const char *json_body, size_t body_len,
                   int *status_code, size_t *out_len);

/*
 * Generate health check response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *sg_api_health(size_t *out_len);

/*
 * Generate version response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *sg_api_version(size_t *out_len);

/* ============================================================================
 * Model Building (programmatic JSON DOM access)
 * ============================================================================ */

typedef struct SGContext SGContext;
typedef struct ShJsonValue ShJsonValue;

/*
 * Build a model from a parsed JSON DOM.
 *
 * Processes top-level sections (config, locations, depots, vehicles, tasks,
 * requests, travel, zones, commodities, exclusion_groups, setup_times,
 * initial_routes) in dependency order.
 */
SGStatus sg_api_build_model(SGContext *ctx, const ShJsonValue *root);

/*
 * Build a model from a JSON file on disk.
 *
 * Reads file, parses JSON, then calls sg_api_build_model.
 */
SGStatus sg_api_build_model_file(SGContext *ctx, const char *path);

/*
 * Write solution output to a streaming JSON writer.
 *
 * Writes: status, stats, routes (with stops), unassigned, error.
 * Requires sh_json.h to be included for ShJsonWriter definition.
 */
#ifdef SH_JSON_H
SGStatus sg_api_write_solution(const SGContext *ctx, ShJsonWriter *w,
                                SGStatus solve_status);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SURGE_SG_API_H */
