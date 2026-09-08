/**
 * Locus WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * The Monaco geocoding index is embedded at compile time for a self-contained
 * demo. All API endpoints work identically to the Keel server -- and now they
 * really do, because both call the same function.
 *
 * This file used to carry its own copy of the five endpoints: its own routing,
 * its own query parsing and URL decoding, and its own JSON building into a
 * fixed 64 KB static buffer. That copy disagreed with the library and with the
 * server on nearly every endpoint. It reported "total" as the size of the page
 * rather than the number of matches; it omitted osm_id and osm_type entirely;
 * it built display_name by hand instead of calling lc_format_address(); it
 * dereferenced index->mmap_idx->header without checking, so a non-mmap index
 * would have faulted; and it interpolated place names into JSON with a bare
 * %s, so a name containing a quote produced a broken response.
 *
 * What remains is a bridge: marshal what JS gives us into an ShApiRequest,
 * call lc_api_handle(), and hold the ShApiResponse for the accessors below.
 * The exported names and their shapes are unchanged, so
 * site/js/locus-api-demo.js needs no edit.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "locus.h"
#include "lc_serialize.h"
#include "lc_mmap.h"
#include "lc_api.h"
#include "sh_api.h"
#include "monaco_lcx.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/*
 * Global State
 *
 * THREAD SAFETY: These static globals are acceptable because WASM runs
 * single-threaded in the browser. The index is initialized once and
 * read-only thereafter. The response is written per-request with no
 * concurrent access possible in the JS event loop model.
 */
static LCIndex *g_index = NULL;
static LCAPIContext *g_api_ctx = NULL;

/* The response from the most recent call, held so the accessors below can
 * read it. Freed at the top of the next call and on teardown. */
static ShApiResponse g_response = {0};

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize the Locus API with embedded Monaco index.
 * Call once on page load. Thread-safe for single-threaded WASM.
 *
 * @return 0 on success, -1 on failure
 */
WASM_EXPORT
int locus_api_init(void) {
    if (g_api_ctx) return 0;  /* Already initialized */

    g_index = lc_index_load_memory(monaco_lcx_data, monaco_lcx_data_len);
    if (!g_index) return -1;

    LCAPIConfig cfg;
    lc_api_config_init(&cfg);
    cfg.name = "locus-wasm-demo";

    g_api_ctx = lc_api_create(g_index, &cfg);
    if (!g_api_ctx) {
        lc_index_free(g_index);
        g_index = NULL;
        return -1;
    }
    return 0;
}

/**
 * Free API context. Call on page unload (optional).
 */
WASM_EXPORT
void locus_api_free(void) {
    sh_api_response_free(&g_response);
    if (g_api_ctx) {
        lc_api_free(g_api_ctx);
        g_api_ctx = NULL;
    }
    if (g_index) {
        lc_index_free(g_index);
        g_index = NULL;
    }
}

/**
 * Check if API is initialized.
 * @return 1 if ready, 0 if not
 */
WASM_EXPORT
int locus_api_ready(void) {
    return g_api_ctx != NULL;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routing, query parsing (including percent-decoding of `q`) and JSON building
 * all live in lc_api_handle(); this function only marshals and holds the
 * result.
 *
 * Supported paths:
 *   /api/v1/search       - Forward geocoding (?q=, ?limit=)
 *   /api/v1/autocomplete - Autocomplete suggestions (?q=, ?limit=)
 *   /api/v1/reverse      - Reverse geocoding (?lat=, ?lon=)
 *   /api/v1/health       - Health check
 *   /api/v1/stats        - Statistics
 *
 * @param path  Request path (e.g., "/api/v1/search")
 * @param query Query string without '?' (optional, can be NULL)
 * @return 0 on success, -1 if the API is not initialized or handling failed
 */
WASM_EXPORT
int locus_api_handle(const char *path, const char *query) {
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

    if (lc_api_handle(g_api_ctx, &req, &g_response) != 0) {
        sh_api_response_error(&g_response, 500, "Internal error");
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Response Accessors (for JS access)
 * ============================================================================ */

WASM_EXPORT
int locus_response_status(void) {
    return g_response.status_code;
}

WASM_EXPORT
const char *locus_response_content_type(void) {
    return g_response.content_type ? g_response.content_type
                                   : "application/json";
}

WASM_EXPORT
const char *locus_response_body(void) {
    /* Never NULL: the JS wrapper takes this pointer before it looks at the
     * length, and a null pointer there indexes the heap view at 0. */
    return g_response.body ? (const char *)g_response.body : "";
}

WASM_EXPORT
size_t locus_response_body_len(void) {
    return g_response.body_len;
}

/* ============================================================================
 * Convenience Functions
 * ============================================================================ */

WASM_EXPORT
unsigned int locus_api_index_size(void) {
    return monaco_lcx_data_len;
}

WASM_EXPORT
const char *locus_api_version(void) {
    static char version[32];
    snprintf(version, sizeof(version), "%d.%d.%d",
        LC_VERSION_MAJOR, LC_VERSION_MINOR, LC_VERSION_PATCH);
    return version;
}

WASM_EXPORT
unsigned int locus_api_entity_count(void) {
    return g_index ? g_index->num_entities : 0;
}
