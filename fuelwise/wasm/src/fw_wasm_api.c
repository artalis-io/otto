/**
 * FuelWise WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * FuelWise is stateless - no embedded data needed. All computation
 * is done on-demand from JSON request bodies.
 */

#include <stdlib.h>
#include <string.h>
#include "fw_api.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/*
 * Static response for JS access (reused across calls).
 * Thread-safety: WASM is single-threaded, so this is safe.
 * Do NOT use this pattern in multi-threaded contexts.
 */
static ShApiResponse g_response = {0};

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize the FuelWise API.
 * FuelWise is stateless, so this is a no-op (always succeeds).
 *
 * @return 0 on success (always)
 */
WASM_EXPORT
int fuelwise_api_init(void) {
    /* FuelWise is stateless - nothing to initialize */
    return 0;
}

/**
 * Free API resources. Call on page unload (optional).
 */
WASM_EXPORT
void fuelwise_api_free(void) {
    sh_api_response_free(&g_response);
    memset(&g_response, 0, sizeof(g_response));
}

/**
 * Check if API is initialized.
 * @return 1 always (FuelWise is stateless)
 */
WASM_EXPORT
int fuelwise_api_ready(void) {
    return 1;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routes to the appropriate handler based on path:
 *   POST /api/v1/solve    - Solve refueling problem
 *   POST /api/v1/filter   - Filter stations to route
 *   POST /api/v1/optimize - Full optimization pipeline
 *   GET  /api/v1/health   - Health check
 *   GET  /api/v1/stats    - Statistics
 *
 * @param path     Request path (e.g., "/api/v1/solve")
 * @param query    Query string without '?' (optional, can be NULL)
 * @param body     Request body for POST requests (can be NULL)
 * @param body_len Request body length
 * @return Pointer to static response, or NULL on error
 */
WASM_EXPORT
ShApiResponse *fuelwise_api_handle(const char *path, const char *query,
                                   const char *body, size_t body_len) {
    /* Free previous response body */
    sh_api_response_free(&g_response);
    memset(&g_response, 0, sizeof(g_response));

    /* Build request */
    ShApiRequest req = {
        .path = path,
        .query = query,
        .body = body,
        .body_len = body_len,
        .host = "wasm.demo"  /* Fake host for info */
    };

    /* Handle request */
    if (fw_api_handle(NULL, &req, &g_response) != 0) {
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
int fuelwise_response_status(const ShApiResponse *resp) {
    return resp ? resp->status_code : 500;
}

/**
 * Get content type string from response.
 */
WASM_EXPORT
const char *fuelwise_response_content_type(const ShApiResponse *resp) {
    return resp ? resp->content_type : "text/plain";
}

/**
 * Get response body pointer.
 */
WASM_EXPORT
const char *fuelwise_response_body(const ShApiResponse *resp) {
    return resp ? resp->body : NULL;
}

/**
 * Get response body length.
 */
WASM_EXPORT
size_t fuelwise_response_body_len(const ShApiResponse *resp) {
    return resp ? resp->body_len : 0;
}

/* ============================================================================
 * Convenience Functions
 * ============================================================================ */

/**
 * Get FuelWise version string pointer.
 */
WASM_EXPORT
const char *fuelwise_api_version(void) {
    return "1.0.0";
}

/* ============================================================================
 * Memory Management (for JS interop)
 * ============================================================================ */

/**
 * Allocate memory (exposed for JS).
 */
WASM_EXPORT
void *wasm_malloc(size_t size) {
    return malloc(size);
}

/**
 * Free memory (exposed for JS).
 */
WASM_EXPORT
void wasm_free(void *ptr) {
    free(ptr);
}
