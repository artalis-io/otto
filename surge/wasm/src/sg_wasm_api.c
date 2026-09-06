/**
 * Surge WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * Unlike Velo/Carta which embed pre-built data, Surge VRP problems are
 * fully specified in each request JSON. No initialization data needed.
 *
 * All API endpoints work identically to the Keel server.
 */

#include <stdlib.h>
#include <string.h>
#include "surge.h"
#include "sg_api.h"
#include "sh_json.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* API context */
static SGAPIContext *g_ctx = NULL;

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
    g_ctx = sg_api_create();
    return g_ctx ? 0 : -1;
}

/**
 * Free API resources.
 */
WASM_EXPORT
void surge_api_free(void) {
    sg_api_free(g_ctx);
    g_ctx = NULL;
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

    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "error");
    sh_json_write_string(&jw, message);
    sh_json_write_object_end(&jw);

    if (jw.error) {
        sh_json_buf_free(&jb);
        g_response_buf = NULL;
        g_response_len = 0;
    } else {
        g_response_len = jb.len;
        g_response_buf = sh_json_buf_take(&jb);
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
    SGAPIRequest req = {
        .path = path,
        .query = query,
        .body = body,
        .body_len = body_len,
        .host = NULL
    };
    SGAPIResponse resp = {0};

    int rc = sg_api_handle(g_ctx, &req, &resp);

    if (rc == 0 && resp.body) {
        /* Transfer ownership from sg_api_handle response */
        free(g_response_buf);
        g_response_buf = resp.body;
        g_response_len = resp.body_len;
        g_response_status = resp.status_code;
        g_response_content_type = resp.content_type;
    } else {
        set_error(resp.status_code ? resp.status_code : 500, "Internal error");
        sg_api_response_free(&resp);
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
