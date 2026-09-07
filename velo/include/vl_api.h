/*
 * Velo API Handler - Transport-agnostic request handling
 *
 * This module provides request handling logic that can be used by:
 * - Keel HTTP server (production)
 * - WASM exports (browser demo)
 * - Direct C API calls (testing)
 *
 * The API handler is initialized with a graph and optional landmarks,
 * then handles path-based routing internally. All responses are returned
 * as allocated buffers that the caller must free.
 */

#ifndef VELO_VL_API_H
#define VELO_VL_API_H

#include <stddef.h>
#include <stdint.h>
#include "velo.h"
#include "sh_api.h"  /* ShApiRequest/ShApiResponse/ShApiHandler */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * API Endpoint Documentation
 *
 * These annotations are parsed by scripts/build-api-docs.py to generate site/api.html
 * ============================================================================ */

/*@api
 * GET /api/v1/route
 * Calculate route between coordinates
 *
 * @query from:string Origin coordinates as lat,lon (e.g., 47.5,19.0)
 * @query to:string Destination coordinates as lat,lon (e.g., 46.2,20.1)
 * @query profile:string:car Vehicle profile: car, truck, bike, foot
 * @query mode:string:fastest Optimization: fastest (time) or shortest (distance)
 * @query geometry:bool:false Include Google Polyline encoded geometry
 *
 * @returns application/json Route with distance, duration, and optional geometry
 * @error 400 Invalid coordinates or parameters
 * @error 404 No route found between coordinates
 * @error 503 Graph not loaded
 *
 * @response_json
 * {
 *   "status": "ok",
 *   "route": {
 *     "distance": 187432.5,
 *     "duration": 7234.2,
 *     "profile": "car",
 *     "mode": "fastest",
 *     "from": [47.497, 19.040],
 *     "to": [46.253, 20.148],
 *     "geometry": "encoded_polyline..."
 *   },
 *   "meta": {
 *     "nodes_explored": 12543,
 *     "search_time_ms": 34.5
 *   }
 * }
 *
 * @example curl "http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=car&mode=fastest"
 * @example_comment Calculate fastest car route between Budapest and Szeged
 *
 * @demo json
 * @demo_title Calculate a route in Monaco using WASM
 * @demo_input from_lat:number:43.7384
 * @demo_input from_lon:number:7.4246
 * @demo_input to_lat:number:43.7311
 * @demo_input to_lon:number:7.4197
 * @demo_input profile:select:car:car,truck,bike,foot
 * @demo_input mode:select:fastest:fastest,shortest
 * @demo_input geometry:select:false:false,true
 */

/*@api
 * POST /api/v1/route
 * Calculate route between coordinates (JSON body)
 *
 * @returns application/json Route with distance, duration, and optional geometry
 * @error 400 Invalid JSON or coordinates
 * @error 404 No route found between coordinates
 * @error 503 Graph not loaded
 *
 * @response_json
 * {
 *   "status": "ok",
 *   "route": {
 *     "distance": 187432.5,
 *     "duration": 7234.2,
 *     "profile": "car",
 *     "mode": "fastest",
 *     "from": [47.497, 19.040],
 *     "to": [46.253, 20.148],
 *     "geometry": "encoded_polyline..."
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
 *   "service": "velo-route-server",
 *   "version": "1.0.0"
 * }
 *
 * @demo json
 * @demo_title Check WASM health status
 * @demo_fetch /api/v1/health
 */

/*@api
 * GET /api/v1/stats
 * Graph and server statistics
 *
 * @returns application/json Graph info, work queue stats, rate limiter stats
 *
 * @response_json
 * {
 *   "graph_path": "/data/hungary.osm.pbf",
 *   "num_nodes": 2745632,
 *   "num_edges": 5891234,
 *   "landmarks_enabled": true,
 *   "landmark_count": 32,
 *   "bbox": {
 *     "min_lat": 45.74,
 *     "min_lon": 16.11,
 *     "max_lat": 48.58,
 *     "max_lon": 22.90
 *   }
 * }
 *
 * @demo json
 * @demo_title Get Monaco graph statistics
 * @demo_fetch /api/v1/stats
 */

/*@api
 * GET /metrics
 * Prometheus metrics endpoint
 *
 * @returns text/plain Prometheus exposition format metrics
 */

/* ============================================================================
 * API Context
 * ============================================================================ */

/*
 * Opaque API context - holds graph, landmarks, and configuration.
 * Thread-safe for concurrent requests once initialized.
 */
typedef struct VLAPIContext VLAPIContext;

/*
 * API configuration options.
 */
typedef struct {
    const char *graph_path;     /* Path for stats reporting */
    const char *name;           /* Server name for health check */
    int landmark_count;         /* Number of landmarks (0 if disabled) */
} VLAPIConfig;

/*
 * Initialize default API configuration.
 */
void vl_api_config_init(VLAPIConfig *config);

/*
 * Create API context from existing graph and landmarks.
 *
 * The graph and landmarks are NOT owned by the API context - caller must
 * keep them alive and free them separately.
 *
 * Returns NULL on failure.
 */
VLAPIContext *vl_api_create(VLGraph *graph, VLLandmarks *landmarks,
                            const VLAPIConfig *config);

/*
 * Free API context.
 *
 * Does NOT free the graph or landmarks.
 */
void vl_api_free(VLAPIContext *ctx);

/*
 * Get the underlying graph (for advanced use).
 */
VLGraph *vl_api_get_graph(VLAPIContext *ctx);

/*
 * Get the landmarks (for advanced use).
 */
VLLandmarks *vl_api_get_landmarks(VLAPIContext *ctx);

/* ============================================================================
 * Request/Response
 * ============================================================================ */

/*
 * Handle an API request.
 *
 * This is a plain ShApiHandler: the request and response are the shared types
 * from sh_api.h, so any transport can drive it -- Keel, the WASM bridge, a
 * direct call in a test. Velo used to declare its own VLAPIRequest and
 * VLAPIResponse here, and there were three implementations of these three
 * endpoints: this one, another in api/src/main.c, and a third in
 * wasm/src/vl_wasm_api.c that returned geometry as a raw coordinate array
 * where the other two return an encoded polyline. Now there is one.
 *
 * `ctx` is a VLAPIContext*, passed as void* so the signature matches
 * ShApiHandler exactly and the compiler checks that for us.
 *
 * Supported paths:
 *   /api/v1/route   - Calculate route (GET with ?from=&to=, or POST JSON)
 *   /api/v1/health  - Health check
 *   /api/v1/stats   - Graph statistics
 *
 * Returns 0 when the response has been filled in -- including error
 * responses, since a 404 is a successful handling -- and non-zero only on an
 * internal failure that leaves the response unusable.
 *
 * Free the response with sh_api_response_free().
 */
int vl_api_handle(void *ctx,
                  const ShApiRequest *req,
                  ShApiResponse *resp);

/* ============================================================================
 * Route Request Parameters
 * ============================================================================ */

/*
 * Parsed route request parameters.
 */
typedef struct {
    double from_lat, from_lon;
    double to_lat, to_lon;
    VLProfile profile;
    VLWeightType weight;
    int include_geometry;
} VLAPIRouteParams;

/*
 * Parse route parameters from query string.
 *
 * Returns 0 on success, -1 on error (error_msg filled in).
 */
int vl_api_parse_route_params(const char *query,
                              const char *body, size_t body_len,
                              const char *method,
                              VLAPIRouteParams *params,
                              char *error_msg, size_t error_msg_len);

/* ============================================================================
 * Individual Handlers (for advanced use)
 * ============================================================================ */

/*
 * Calculate a route and return JSON response.
 *
 * Returns allocated JSON string, or NULL on failure.
 * Caller must free the returned buffer.
 */
char *vl_api_route(VLAPIContext *ctx,
                   const VLAPIRouteParams *params,
                   int *status_code,
                   size_t *out_len);

/*
 * Generate health check response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *vl_api_health(VLAPIContext *ctx, size_t *out_len);

/*
 * Generate stats response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *vl_api_stats(VLAPIContext *ctx, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* VELO_VL_API_H */
