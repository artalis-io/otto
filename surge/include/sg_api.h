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
 * @response_json
 * {
 *   "status": "ok",
 *   "stats": {
 *     "iterations": 1000,
 *     "total_cost": 12345.67,
 *     "total_distance": 828.94,
 *     "unassigned": 0,
 *     "vehicles_used": 10
 *   }
 * }
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
 */

/* ============================================================================
 * Request/Response
 * ============================================================================ */

typedef struct {
    const char *path;       /* URI path (required) */
    const char *query;      /* Query string without '?' (optional, NULL ok) */
    const char *body;       /* Request body (optional, NULL ok for GET) */
    size_t body_len;        /* Body length in bytes */
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
int sg_api_handle(const SGAPIRequest *req, SGAPIResponse *resp);

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

#ifdef __cplusplus
}
#endif

#endif /* SURGE_SG_API_H */
