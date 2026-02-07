/*
 * Carta API Handler - Transport-agnostic request handling
 *
 * This module provides request handling logic that can be used by:
 * - Mongoose HTTP server (production)
 * - WASM exports (browser demo)
 * - Direct C API calls (testing)
 *
 * The API handler is initialized with PBF data and handles path-based
 * routing internally. All responses are returned as allocated buffers
 * that the caller must free.
 */

#ifndef CARTA_CT_API_H
#define CARTA_CT_API_H

#include <stddef.h>
#include <stdint.h>
#include "ct_types.h"  /* For CTPBFContext, CTLODConfig */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * API Context
 * ============================================================================ */

/*
 * Opaque API context - holds all state needed for request handling.
 * Thread-safe for concurrent requests once initialized.
 */
typedef struct CTAPIContext CTAPIContext;

/*
 * API configuration options.
 */
typedef struct {
    int min_zoom;           /* Minimum zoom level (default: 0) */
    int max_zoom;           /* Maximum zoom level (default: 18) */
    int tile_size;          /* PNG tile size (default: 512) */
    int enable_lod;         /* Enable LOD filtering (default: 1) */
    const char *name;       /* Server name for TileJSON (optional) */
} CTAPIConfig;

/*
 * Initialize default API configuration.
 */
void ct_api_config_init(CTAPIConfig *config);

/*
 * Create API context from PBF data in memory.
 *
 * This is the primary initialization path for both server and WASM.
 * The data is copied internally - caller can free pbf_data after this call.
 *
 * Returns NULL on failure.
 */
CTAPIContext *ct_api_create(const uint8_t *pbf_data, size_t pbf_len,
                            const CTAPIConfig *config);

/*
 * Create API context from an existing PBF context.
 *
 * This allows sharing a PBF context between the API handler and other
 * code (e.g., the mongoose server's caching layer).
 *
 * The PBF context is NOT owned by the API context - caller must keep
 * it alive and free it separately.
 *
 * Returns NULL on failure.
 */
CTAPIContext *ct_api_create_from_pbf(CTPBFContext *pbf_ctx,
                                     const CTAPIConfig *config);

/*
 * Free API context.
 *
 * If created with ct_api_create(), frees the internal PBF context.
 * If created with ct_api_create_from_pbf(), does NOT free the PBF context.
 */
void ct_api_free(CTAPIContext *ctx);

/*
 * Get the underlying PBF context (for advanced use).
 */
CTPBFContext *ct_api_get_pbf(CTAPIContext *ctx);

/* ============================================================================
 * Request/Response
 * ============================================================================ */

/*
 * API request structure.
 *
 * The path should be the URI path (e.g., "/tiles/14/8529/5974.png").
 * The query string is optional and should NOT include the leading '?'.
 */
typedef struct {
    const char *path;       /* URI path (required) */
    const char *query;      /* Query string without '?' (optional, NULL ok) */
    const char *host;       /* Host header for TileJSON URLs (optional) */
} CTAPIRequest;

/*
 * API response structure.
 *
 * The body is heap-allocated and must be freed by the caller using
 * ct_api_response_free() or free().
 */
typedef struct {
    int status_code;        /* HTTP status code (200, 400, 404, 500, etc.) */
    const char *content_type; /* MIME type (static string, do not free) */
    uint8_t *body;          /* Response body (caller must free) */
    size_t body_len;        /* Response body length in bytes */
} CTAPIResponse;

/*
 * Handle an API request.
 *
 * Routes the request based on path and generates the appropriate response.
 * The response body is heap-allocated - caller must free with
 * ct_api_response_free().
 *
 * Supported paths:
 *   /tiles/{z}/{x}/{y}.png  - PNG tile
 *   /tiles/{z}/{x}/{y}.mvt  - MVT tile
 *   /tiles.json             - TileJSON metadata
 *   /api/v1/health          - Health check
 *   /api/v1/stats           - PBF statistics
 *
 * Returns 0 on success (response filled in), -1 on internal error.
 */
int ct_api_handle(CTAPIContext *ctx,
                  const CTAPIRequest *req,
                  CTAPIResponse *resp);

/*
 * Free response body.
 *
 * Safe to call with NULL response or NULL body.
 */
void ct_api_response_free(CTAPIResponse *resp);

/* ============================================================================
 * Individual Handlers (for advanced use)
 * ============================================================================ */

/*
 * Generate a PNG tile.
 *
 * Returns allocated PNG data, or NULL on failure.
 * Caller must free the returned buffer.
 */
uint8_t *ct_api_generate_png(CTAPIContext *ctx,
                             int z, int x, int y,
                             size_t *out_len);

/*
 * Generate an MVT tile.
 *
 * Returns allocated MVT data, or NULL on failure (empty tiles return
 * a minimal valid MVT).
 * Caller must free the returned buffer.
 */
uint8_t *ct_api_generate_mvt(CTAPIContext *ctx,
                             int z, int x, int y,
                             size_t *out_len);

/*
 * Generate TileJSON metadata.
 *
 * The host parameter is used to build tile URLs (e.g., "localhost:8081").
 * If NULL, uses "localhost".
 *
 * Returns allocated JSON string, or NULL on failure.
 * Caller must free the returned buffer.
 */
char *ct_api_generate_tilejson(CTAPIContext *ctx,
                               const char *host,
                               size_t *out_len);

/*
 * Generate health check response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *ct_api_generate_health(CTAPIContext *ctx, size_t *out_len);

/*
 * Generate stats response.
 *
 * Returns allocated JSON string.
 * Caller must free the returned buffer.
 */
char *ct_api_generate_stats(CTAPIContext *ctx, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* CARTA_CT_API_H */
