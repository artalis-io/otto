/**
 * Carta WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * The Monaco PBF is embedded at compile time for a self-contained demo.
 * All API endpoints work identically to the Keel server.
 */

#include <stdlib.h>
#include <string.h>
#include "carta.h"
#include "ct_api.h"
#include "monaco_pbf.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* Global API context (initialized once) */
static CTAPIContext *g_api_ctx = NULL;

/* Static response for JS access (reused across calls) */
static CTAPIResponse g_response = {0};

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize the Carta API with embedded Monaco PBF.
 * Call once on page load. Thread-safe for single-threaded WASM.
 *
 * @return 0 on success, -1 on failure
 */
WASM_EXPORT
int carta_api_init(void) {
    if (g_api_ctx) return 0;  /* Already initialized */

    CTAPIConfig config;
    ct_api_config_init(&config);
    config.name = "Carta WASM Demo";
    config.min_zoom = 0;
    config.max_zoom = 18;
    config.tile_size = 512;
    config.enable_lod = 1;

    g_api_ctx = ct_api_create(monaco_pbf_data, monaco_pbf_data_len, &config);
    return g_api_ctx ? 0 : -1;
}

/**
 * Free API context. Call on page unload (optional).
 */
WASM_EXPORT
void carta_api_free(void) {
    if (g_api_ctx) {
        ct_api_response_free(&g_response);
        ct_api_free(g_api_ctx);
        g_api_ctx = NULL;
    }
}

/**
 * Check if API is initialized.
 * @return 1 if ready, 0 if not
 */
WASM_EXPORT
int carta_api_ready(void) {
    return g_api_ctx != NULL;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routes to the appropriate handler based on path:
 *   /tiles/{z}/{x}/{y}.png  - PNG tile
 *   /tiles/{z}/{x}/{y}.mvt  - MVT tile
 *   /tiles.json             - TileJSON metadata
 *   /api/v1/health          - Health check
 *   /api/v1/stats           - Statistics
 *
 * @param path  Request path (e.g., "/tiles/14/8529/5974.png")
 * @param query Query string without '?' (optional, can be NULL)
 * @return Pointer to static response, or NULL on error
 */
WASM_EXPORT
CTAPIResponse *carta_api_handle(const char *path, const char *query) {
    if (!g_api_ctx) return NULL;

    /* Free previous response body */
    ct_api_response_free(&g_response);

    /* Build request */
    CTAPIRequest req = {
        .path = path,
        .query = query,
        .host = "wasm.demo"  /* Fake host for TileJSON */
    };

    /* Handle request */
    if (ct_api_handle(g_api_ctx, &req, &g_response) != 0) {
        return NULL;
    }

    return &g_response;
}

/* ============================================================================
 * Response Accessors (for JS access)
 * ============================================================================ */

/**
 * Get HTTP status code from response.
 */
WASM_EXPORT
int carta_response_status(const CTAPIResponse *resp) {
    return resp ? resp->status_code : 500;
}

/**
 * Get content type string from response.
 */
WASM_EXPORT
const char *carta_response_content_type(const CTAPIResponse *resp) {
    return resp ? resp->content_type : "text/plain";
}

/**
 * Get response body pointer.
 */
WASM_EXPORT
const uint8_t *carta_response_body(const CTAPIResponse *resp) {
    return resp ? resp->body : NULL;
}

/**
 * Get response body length.
 */
WASM_EXPORT
size_t carta_response_body_len(const CTAPIResponse *resp) {
    return resp ? resp->body_len : 0;
}

/* ============================================================================
 * Convenience Functions
 * ============================================================================ */

/**
 * Get embedded PBF data size (for info display).
 */
WASM_EXPORT
unsigned int carta_api_pbf_size(void) {
    return monaco_pbf_data_len;
}

/**
 * Get Carta version string pointer.
 */
WASM_EXPORT
const char *carta_api_version(void) {
    return ct_version();
}
