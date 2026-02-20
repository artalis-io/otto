/**
 * Surge WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * Unlike Velo/Carta which embed pre-built data, Surge VRP problems are
 * fully specified in each request JSON. No initialization data needed.
 *
 * All API endpoints work identically to the Mongoose server.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "surge.h"
#include "sg_api.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* Response state (single-threaded WASM, no concurrency) */
static char *g_response_buf = NULL;
static size_t g_response_len = 0;
static int g_response_status = 200;
static const char *g_response_content_type = "application/json";

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize the Surge API.
 * No pre-loaded data needed — VRP problems are fully specified per-request.
 *
 * @return 0 on success (always succeeds)
 */
WASM_EXPORT
int surge_api_init(void) {
    return 0;
}

/**
 * Free API resources.
 */
WASM_EXPORT
void surge_api_free(void) {
    free(g_response_buf);
    g_response_buf = NULL;
    g_response_len = 0;
}

/**
 * Check if API is initialized.
 * @return 1 (always ready — no pre-loaded data required)
 */
WASM_EXPORT
int surge_api_ready(void) {
    return 1;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static void set_response(int status, char *body, size_t len) {
    free(g_response_buf);
    g_response_buf = body;
    g_response_len = len;
    g_response_status = status;
    g_response_content_type = "application/json";
}

static void set_error(int status, const char *message) {
    free(g_response_buf);
    size_t msg_len = strlen(message);
    /* {"error": "..."} */
    g_response_buf = malloc(msg_len + 32);
    if (g_response_buf) {
        g_response_len = (size_t)snprintf(g_response_buf, msg_len + 32,
                                          "{\"error\":\"%s\"}", message);
    } else {
        g_response_buf = NULL;
        g_response_len = 0;
    }
    g_response_status = status;
    g_response_content_type = "application/json";
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/**
 * Handle a solve request with JSON body.
 *
 * @param body     JSON request body
 * @param body_len Body length in bytes
 * @return 0 on success
 */
WASM_EXPORT
int surge_api_solve(const char *body, size_t body_len) {
    if (!body || body_len == 0) {
        set_error(400, "Empty request body");
        return 0;
    }

    int status_code = 500;
    size_t out_len = 0;
    char *result = sg_api_solve(body, body_len, &status_code, &out_len);

    if (result) {
        set_response(status_code, result, out_len);
    } else {
        set_error(500, "Solver internal error");
    }

    return 0;
}

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routes to the appropriate handler based on path:
 *   POST /api/v1/solve   - Solve VRP problem
 *   GET  /api/v1/health  - Health check
 *   GET  /api/v1/version - Version info
 *
 * @param path     Request path (e.g., "/api/v1/solve")
 * @param query    Query string without '?' (optional, can be NULL)
 * @param body     Request body (optional, can be NULL for GET)
 * @param body_len Body length in bytes
 * @return 0 on success
 */
WASM_EXPORT
int surge_api_handle(const char *path, const char *query,
                     const char *body, size_t body_len) {
    (void)query;

    g_response_content_type = "application/json";

    if (strcmp(path, "/api/v1/solve") == 0) {
        return surge_api_solve(body, body_len);
    } else if (strcmp(path, "/api/v1/health") == 0) {
        size_t out_len;
        char *json = sg_api_health(&out_len);
        if (json) {
            set_response(200, json, out_len);
        } else {
            set_error(500, "Internal error");
        }
    } else if (strcmp(path, "/api/v1/version") == 0) {
        size_t out_len;
        char *json = sg_api_version(&out_len);
        if (json) {
            set_response(200, json, out_len);
        } else {
            set_error(500, "Internal error");
        }
    } else {
        set_error(404, "Not found");
    }

    return 0;
}

/* ============================================================================
 * Response Accessors (for JS access)
 * ============================================================================ */

WASM_EXPORT
int surge_response_status(void) {
    return g_response_status;
}

WASM_EXPORT
const char *surge_response_content_type(void) {
    return g_response_content_type;
}

WASM_EXPORT
const char *surge_response_body(void) {
    return g_response_buf ? g_response_buf : "";
}

WASM_EXPORT
size_t surge_response_body_len(void) {
    return g_response_len;
}

/* ============================================================================
 * Convenience Functions
 * ============================================================================ */

WASM_EXPORT
const char *surge_api_version_string(void) {
    return sg_version();
}
