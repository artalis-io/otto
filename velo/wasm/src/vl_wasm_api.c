/**
 * Velo WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * The Monaco routing graph is embedded at compile time for a self-contained
 * demo. All API endpoints work identically to the Keel server -- and now they
 * really do, because both call the same function.
 *
 * This file used to carry its own copy of the three endpoints, and it did not
 * merely differ from the server in style. It spoke a different API:
 *
 *   - it read from_lat, from_lon, to_lat and to_lon as four query parameters,
 *     where the documented API (and the server) takes from=lat,lon and
 *     to=lat,lon -- so the demo was not exercising the product it documents
 *   - it returned "geometry" as a raw [[lat,lon],...] array, where the server
 *     returns a Google Polyline encoded string, which is what
 *     site/api-config.json tells the reader to expect
 *   - it answered 422 for "no route", where the server answers 404
 *   - it emitted no "meta" object at all
 *   - it defaulted geometry off, where the server defaults it on
 *
 * All of that is gone. What remains is a bridge: marshal what JS gives us into
 * an ShApiRequest, call vl_api_handle(), hold the ShApiResponse for the
 * accessors below. The exported names and their shapes are unchanged;
 * site/js/velo-api-demo.js now sends the documented from=/to= parameters.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "velo.h"
#include "vl_api.h"
#include "sh_api.h"
#include "monaco_vlg.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/*
 * Global State
 *
 * THREAD SAFETY: these statics are acceptable because WASM runs
 * single-threaded in the browser. The graph is initialized once and read-only
 * thereafter; the response is written per-request with no concurrent access
 * possible in the JS event loop model.
 */
static VLGraph *g_graph = NULL;
static VLAPIContext *g_api_ctx = NULL;

/* The response from the most recent call, held so the accessors below can
 * read it. Freed at the top of the next call and on teardown. */
static ShApiResponse g_response = {0};

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize the Velo API with embedded Monaco graph.
 * Call once on page load. Thread-safe for single-threaded WASM.
 *
 * @return 0 on success, -1 on failure
 */
WASM_EXPORT
int velo_api_init(void) {
    if (g_api_ctx) return 0;  /* Already initialized */

    g_graph = vl_load_binary_memory(monaco_vlg_data, monaco_vlg_data_len);
    if (!g_graph) return -1;

    VLAPIConfig cfg;
    vl_api_config_init(&cfg);

    /* No landmarks in the demo build: the embedded graph ships without them,
     * and vl_api_route() falls back to plain bidirectional A* when they are
     * absent. */
    g_api_ctx = vl_api_create(g_graph, NULL, &cfg);
    if (!g_api_ctx) {
        vl_graph_free(g_graph);
        g_graph = NULL;
        return -1;
    }
    return 0;
}

/**
 * Free API context. Call on page unload (optional).
 */
WASM_EXPORT
void velo_api_free(void) {
    sh_api_response_free(&g_response);
    if (g_api_ctx) {
        vl_api_free(g_api_ctx);
        g_api_ctx = NULL;
    }
    if (g_graph) {
        vl_graph_free(g_graph);
        g_graph = NULL;
    }
}

/**
 * Check if API is initialized.
 * @return 1 if ready, 0 if not
 */
WASM_EXPORT
int velo_api_ready(void) {
    return g_api_ctx != NULL;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routing, parameter parsing and JSON building all live in vl_api_handle();
 * this function only marshals and holds the result.
 *
 * Supported paths:
 *   /api/v1/route   - Calculate route (?from=lat,lon&to=lat,lon)
 *   /api/v1/health  - Health check
 *   /api/v1/stats   - Statistics
 *
 * @param path  Request path (e.g., "/api/v1/route")
 * @param query Query string without '?' (optional, can be NULL)
 * @return 0 on success, -1 if the API is not initialized or handling failed
 */
WASM_EXPORT
int velo_api_handle(const char *path, const char *query) {
    sh_api_response_free(&g_response);

    if (!g_api_ctx) {
        sh_api_response_error(&g_response, 500, "API not initialized");
        return -1;
    }

    ShApiRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = path;
    req.query = query;
    req.host = "wasm.demo";

    if (vl_api_handle(g_api_ctx, &req, &g_response) != 0) {
        sh_api_response_error(&g_response, 500, "Internal error");
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Response Accessors (for JS access)
 * ============================================================================ */

WASM_EXPORT
int velo_response_status(void) {
    return g_response.status_code;
}

WASM_EXPORT
const char *velo_response_content_type(void) {
    return g_response.content_type ? g_response.content_type
                                   : "application/json";
}

WASM_EXPORT
const char *velo_response_body(void) {
    /* Never NULL: the JS wrapper takes this pointer before it looks at the
     * length, and a null pointer there indexes the heap view at 0. */
    return g_response.body ? (const char *)g_response.body : "";
}

WASM_EXPORT
size_t velo_response_body_len(void) {
    return g_response.body_len;
}

/* ============================================================================
 * Convenience Functions
 * ============================================================================ */

WASM_EXPORT
unsigned int velo_api_graph_size(void) {
    return monaco_vlg_data_len;
}

WASM_EXPORT
const char *velo_api_version(void) {
    static char version[32];
    snprintf(version, sizeof(version), "%d.%d.%d",
        VL_VERSION_MAJOR, VL_VERSION_MINOR, VL_VERSION_PATCH);
    return version;
}

WASM_EXPORT
unsigned int velo_api_node_count(void) {
    return g_graph ? g_graph->num_nodes : 0;
}

WASM_EXPORT
unsigned int velo_api_edge_count(void) {
    return g_graph ? g_graph->num_edges : 0;
}
