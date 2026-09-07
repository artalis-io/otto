/**
 * Ralph WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * Unlike Carta/Velo/Locus, Ralph is stateless - no data needs to be
 * embedded. The WASM module accepts LP/MPS problems and returns solutions.
 */

#include <stdlib.h>
#include <string.h>
#include "ralph_core.h"
#include "ralph_api.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* Global API context (initialized once) */
static RalphAPIContext *g_api_ctx = NULL;

/* Static response for JS access (reused across calls) */
static ShApiResponse g_response = {0};

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize the Ralph API.
 * Call once on page load. Thread-safe for single-threaded WASM.
 *
 * @return 0 on success, -1 on failure
 */
WASM_EXPORT
int ralph_api_init(void) {
    if (g_api_ctx) return 0;  /* Already initialized */

    g_api_ctx = ralph_api_create();
    return g_api_ctx ? 0 : -1;
}

/**
 * Free API context. Call on page unload (optional).
 */
WASM_EXPORT
void ralph_wasm_api_free(void) {
    if (g_api_ctx) {
        sh_api_response_free(&g_response);
        ralph_api_free(g_api_ctx);
        g_api_ctx = NULL;
    }
}

/**
 * Check if API is initialized.
 * @return 1 if ready, 0 if not
 */
WASM_EXPORT
int ralph_wasm_api_ready(void) {
    return g_api_ctx != NULL;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routes to the appropriate handler based on path:
 *   POST /api/v1/solve    - Solve LP/MIP problem
 *   GET  /api/v1/formats  - List supported formats
 *   GET  /api/v1/health   - Health check
 *
 * @param method HTTP method ("GET" or "POST")
 * @param path   Request path (e.g., "/api/v1/solve")
 * @param query  Query string without '?' (optional, can be NULL)
 * @param body   Request body for POST (optional, can be NULL)
 * @param body_len Body length in bytes
 * @return Pointer to static response, or NULL on error
 */
WASM_EXPORT
ShApiResponse *ralph_wasm_api_handle(const char *method,
                                         const char *path,
                                         const char *query,
                                         const char *body,
                                         size_t body_len) {
    if (!g_api_ctx) return NULL;

    /* Free previous response body */
    sh_api_response_free(&g_response);

    /* Build request */
    ShApiRequest req = {
        .method = method,
        .path = path,
        .query = query,
        .body = body,
        .body_len = body_len
    };

    /* Handle request */
    if (ralph_api_handle(g_api_ctx, &req, &g_response) != 0) {
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
int ralph_wasm_response_status(const ShApiResponse *resp) {
    return resp ? resp->status_code : 500;
}

/**
 * Get content type string from response.
 */
WASM_EXPORT
const char *ralph_wasm_response_content_type(const ShApiResponse *resp) {
    return resp ? resp->content_type : "text/plain";
}

/**
 * Get response body pointer.
 */
WASM_EXPORT
const uint8_t *ralph_wasm_response_body(const ShApiResponse *resp) {
    return resp ? resp->body : NULL;
}

/**
 * Get response body length.
 */
WASM_EXPORT
size_t ralph_wasm_response_body_len(const ShApiResponse *resp) {
    return resp ? resp->body_len : 0;
}

/* ============================================================================
 * Convenience Functions
 * ============================================================================ */

/**
 * Get Ralph version string pointer.
 */
WASM_EXPORT
const char *ralph_wasm_api_version(void) {
    return ralph_api_version();
}
