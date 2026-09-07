/*
 * Locus API Handler - Transport-agnostic request handling
 *
 * This module provides request handling logic that can be used by:
 * - Keel HTTP server (production)
 * - WASM exports (browser demo)
 * - Direct C API calls (testing)
 *
 * The API handler is initialized with a geocoding index and handles
 * path-based routing internally. All responses are returned as
 * allocated buffers that the caller must free.
 */

#ifndef LOCUS_LC_API_H
#define LOCUS_LC_API_H

#include <stddef.h>
#include <stdint.h>
#include "locus.h"
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
 * GET /api/v1/search
 * Forward geocoding - search for places by name
 *
 * @query q:string Search query (e.g., "Monte Carlo")
 * @query limit:int:10 Maximum results (1-100)
 *
 * @returns application/json Search results with locations and scores
 * @error 400 Missing or invalid query parameter
 * @error 503 Index not loaded
 *
 * @response_json
 * {
 *   "query": "Monte Carlo",
 *   "total": 5,
 *   "took_ms": 0.12,
 *   "results": [
 *     {
 *       "osm_id": 12345,
 *       "osm_type": "node",
 *       "name": "Monte Carlo",
 *       "class": "place",
 *       "lat": 43.7396,
 *       "lon": 7.4269,
 *       "score": 1.0
 *     }
 *   ]
 * }
 *
 * @example curl "http://localhost:8083/api/v1/search?q=Monte%20Carlo&limit=5"
 * @example_comment Search for "Monte Carlo" with max 5 results
 *
 * @demo json
 * @demo_title Search Monaco places using WASM with embedded index
 * @demo_input q:text:Monte Carlo
 */

/*@api
 * GET /api/v1/autocomplete
 * Autocomplete suggestions for partial input
 *
 * @query q:string Prefix to complete (e.g., "Mon")
 * @query limit:int:10 Maximum suggestions (1-20)
 *
 * @returns application/json Array of suggested place names
 * @error 400 Missing query parameter
 * @error 503 Index not loaded
 *
 * @response_json
 * [
 *   "Monaco",
 *   "Monte Carlo",
 *   "Moneghetti"
 * ]
 *
 * @example curl "http://localhost:8083/api/v1/autocomplete?q=Mon&limit=10"
 * @example_comment Get autocomplete suggestions for "Mon"
 *
 * @demo json
 * @demo_title Autocomplete suggestions using WASM with embedded Monaco index
 * @demo_input q:text:Mon
 */

/*@api
 * GET /api/v1/reverse
 * Reverse geocoding - find address for coordinates
 *
 * @query lat:float Latitude (-90 to 90)
 * @query lon:float Longitude (-180 to 180)
 *
 * @returns application/json Address information for the location
 * @error 400 Missing or invalid coordinates
 * @error 503 Index not loaded
 *
 * @response_json
 * {
 *   "lat": 43.7384,
 *   "lon": 7.4246,
 *   "display_name": "Avenue de Monte-Carlo, Monaco",
 *   "distance_m": 45.2,
 *   "place": "Monaco",
 *   "street": "Avenue de Monte-Carlo"
 * }
 *
 * @example curl "http://localhost:8083/api/v1/reverse?lat=43.7384&lon=7.4246"
 * @example_comment Reverse geocode coordinates in Monaco
 *
 * @demo json
 * @demo_title Reverse geocode a Monaco location using WASM
 * @demo_input lat:number:43.7384
 * @demo_input lon:number:7.4246
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
 *   "service": "locus",
 *   "version": "0.1.0"
 * }
 *
 * @demo json
 * @demo_title Check WASM health status
 * @demo_fetch /api/v1/health
 */

/*@api
 * GET /api/v1/stats
 * Index and server statistics
 *
 * @returns application/json Index info, work queue stats, rate limiter stats
 *
 * @response_json
 * {
 *   "entities": 2763,
 *   "memory_mb": 7.8,
 *   "bounds": {
 *     "min_lat": 43.7234,
 *     "min_lon": 7.4089,
 *     "max_lat": 43.7519,
 *     "max_lon": 7.4398
 *   },
 *   "rate_limit": {
 *     "enabled": true,
 *     "rps": 50.0,
 *     "burst": 100
 *   },
 *   "work_queue": {
 *     "enabled": true,
 *     "depth": 5,
 *     "capacity": 128
 *   }
 * }
 *
 * @demo json
 * @demo_title Get Monaco index statistics
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
 * Opaque API context - holds geocoding index and configuration.
 * Thread-safe for concurrent requests once initialized.
 */
typedef struct LCAPIContext LCAPIContext;

/*
 * API configuration options.
 */
typedef struct {
    const char *data_path;      /* Path for stats reporting */
    const char *name;           /* Server name for health check */
} LCAPIConfig;

/*
 * Initialize default API configuration.
 */
void lc_api_config_init(LCAPIConfig *config);

/*
 * Create API context from existing index.
 *
 * The index is NOT owned by the API context - caller must keep it alive
 * and free it separately.
 *
 * Returns NULL on failure.
 */
LCAPIContext *lc_api_create(LCIndex *index, const LCAPIConfig *config);

/*
 * Free API context.
 *
 * Does NOT free the index.
 */
void lc_api_free(LCAPIContext *ctx);

/*
 * Get the underlying index (for advanced use).
 */
LCIndex *lc_api_get_index(LCAPIContext *ctx);

/* ============================================================================
 * Request/Response
 * ============================================================================ */

/*
 * Handle an API request.
 *
 * This is a plain ShApiHandler: the request and response are the shared types
 * from sh_api.h, so any transport can drive it -- Keel, the WASM bridge, a
 * direct call in a test. Locus used to declare its own LCAPIRequest and
 * LCAPIResponse here, and there were three implementations of these five
 * endpoints: this one, another in api/src/main.c, and a third in
 * wasm/src/lc_wasm_api.c. They disagreed about what "total" counts, about
 * whether osm_id is reported at all, and about whether a name gets escaped
 * on the way into JSON. Now there is one.
 *
 * `ctx` is an LCAPIContext*, passed as void* so the signature matches
 * ShApiHandler exactly and the compiler checks that for us. A NULL ctx is
 * accepted: /api/v1/health answers without an index, and every other route
 * reports 503.
 *
 * Supported paths:
 *   /api/v1/search       - Forward geocoding (?q=, ?limit=)
 *   /api/v1/autocomplete - Autocomplete suggestions (?q=, ?limit=)
 *   /api/v1/reverse      - Reverse geocoding (?lat=, ?lon=)
 *   /api/v1/health       - Health check
 *   /api/v1/stats        - Index statistics
 *
 * `q` is percent-decoded, so ?q=Monte%20Carlo searches for "Monte Carlo".
 *
 * Returns 0 when the response has been filled in -- including error
 * responses, since a 404 is a successful handling -- and non-zero only on an
 * internal failure that leaves the response unusable.
 *
 * Free the response with sh_api_response_free().
 */
int lc_api_handle(void *ctx,
                  const ShApiRequest *req,
                  ShApiResponse *resp);

/* ============================================================================
 * Individual Handlers (for advanced use)
 * ============================================================================ */

/*
 * Forward geocoding search.
 *
 * Returns allocated JSON string, or NULL on failure.
 * Caller must free the returned buffer.
 */
char *lc_api_search(LCAPIContext *ctx,
                    const char *query,
                    int limit,
                    int *status_code,
                    size_t *out_len);

/*
 * Autocomplete suggestions.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *lc_api_autocomplete(LCAPIContext *ctx,
                          const char *prefix,
                          int limit,
                          int *status_code,
                          size_t *out_len);

/*
 * Reverse geocoding.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *lc_api_reverse(LCAPIContext *ctx,
                     double lat,
                     double lon,
                     int *status_code,
                     size_t *out_len);

/*
 * Generate health check response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *lc_api_health(LCAPIContext *ctx, size_t *out_len);

/*
 * Generate stats response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *lc_api_stats(LCAPIContext *ctx, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* LOCUS_LC_API_H */
