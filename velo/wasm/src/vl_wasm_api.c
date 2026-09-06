/**
 * Velo WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * The Monaco routing graph is embedded at compile time for a self-contained demo.
 * All API endpoints work identically to the Keel server.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "velo.h"
#include "monaco_vlg.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* Global graph (initialized once) */
static VLGraph *g_graph = NULL;

/* Response buffer for JSON */
static char g_response_buf[65536];
static size_t g_response_len = 0;
static int g_response_status = 200;
static const char *g_response_content_type = "application/json";

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
    if (g_graph) return 0;  /* Already initialized */

    g_graph = vl_load_binary_memory(monaco_vlg_data, monaco_vlg_data_len);
    return g_graph ? 0 : -1;
}

/**
 * Free API context. Call on page unload (optional).
 */
WASM_EXPORT
void velo_api_free(void) {
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
    return g_graph != NULL;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/* Parse query parameter value */
static const char *get_query_param(const char *query, const char *name, char *buf, size_t buf_size) {
    if (!query || !name) return NULL;

    size_t name_len = strlen(name);
    const char *p = query;

    while (*p) {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *value = p + name_len + 1;
            const char *end = strchr(value, '&');
            size_t len = end ? (size_t)(end - value) : strlen(value);
            if (len >= buf_size) len = buf_size - 1;
            memcpy(buf, value, len);
            buf[len] = '\0';
            return buf;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return NULL;
}

/* Safe coordinate parsing with validation */
static int safe_parse_coord(const char *str, double *out, double min_val, double max_val) {
    if (!str || !*str) return -1;

    char *end;
    double val = strtod(str, &end);

    /* No digits consumed */
    if (end == str) return -1;

    /* Reject inf/nan */
    if (!isfinite(val)) return -1;

    /* Range check */
    if (val < min_val || val > max_val) return -1;

    *out = val;
    return 0;
}

/* Handle GET /api/v1/route */
static void handle_route(const char *query) {
    char buf[64];
    double from_lat, from_lon, to_lat, to_lon;

    /* Parse coordinates with validation */
    const char *from_lat_str = get_query_param(query, "from_lat", buf, sizeof(buf));
    if (!from_lat_str || safe_parse_coord(from_lat_str, &from_lat, -90.0, 90.0) != 0) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing or invalid from_lat parameter\"}");
        return;
    }

    const char *from_lon_str = get_query_param(query, "from_lon", buf, sizeof(buf));
    if (!from_lon_str || safe_parse_coord(from_lon_str, &from_lon, -180.0, 180.0) != 0) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing or invalid from_lon parameter\"}");
        return;
    }

    const char *to_lat_str = get_query_param(query, "to_lat", buf, sizeof(buf));
    if (!to_lat_str || safe_parse_coord(to_lat_str, &to_lat, -90.0, 90.0) != 0) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing or invalid to_lat parameter\"}");
        return;
    }

    const char *to_lon_str = get_query_param(query, "to_lon", buf, sizeof(buf));
    if (!to_lon_str || safe_parse_coord(to_lon_str, &to_lon, -180.0, 180.0) != 0) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing or invalid to_lon parameter\"}");
        return;
    }

    /* Parse optional parameters */
    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.include_geometry = 0;  /* Default: no geometry */

    const char *profile_str = get_query_param(query, "profile", buf, sizeof(buf));
    if (profile_str) {
        if (strcmp(profile_str, "truck") == 0) opts.profile = VL_PROFILE_TRUCK;
        else if (strcmp(profile_str, "bike") == 0) opts.profile = VL_PROFILE_BIKE;
        else if (strcmp(profile_str, "foot") == 0) opts.profile = VL_PROFILE_FOOT;
    }

    const char *mode_str = get_query_param(query, "mode", buf, sizeof(buf));
    if (mode_str && strcmp(mode_str, "shortest") == 0) {
        opts.weight = VL_WEIGHT_DISTANCE;
    }

    const char *geom_str = get_query_param(query, "geometry", buf, sizeof(buf));
    if (geom_str && strcmp(geom_str, "true") == 0) {
        opts.include_geometry = 1;
    }

    /* Calculate route */
    VLCoord from = {from_lat, from_lon};
    VLCoord to = {to_lat, to_lon};
    VLRoute route;
    memset(&route, 0, sizeof(route));

    VLStatus status = vl_route_coords(g_graph, from, to, &opts, &route);

    if (status != VL_OK) {
        g_response_status = 422;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"No route found\", \"status\": %d}", status);
        return;
    }

    /* Build JSON response */
    int offset;
    if (opts.include_geometry) {
        offset = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\n"
            "  \"status\": \"ok\",\n"
            "  \"route\": {\n"
            "    \"distance\": %.1f,\n"
            "    \"duration\": %.1f,\n"
            "    \"profile\": \"%s\",\n"
            "    \"mode\": \"%s\",\n"
            "    \"from\": [%.6f, %.6f],\n"
            "    \"to\": [%.6f, %.6f],\n"
            "    \"geometry\": [",
            route.distance_m,
            route.duration_s,
            opts.profile == VL_PROFILE_TRUCK ? "truck" :
            opts.profile == VL_PROFILE_BIKE ? "bike" :
            opts.profile == VL_PROFILE_FOOT ? "foot" : "car",
            opts.weight == VL_WEIGHT_DISTANCE ? "shortest" : "fastest",
            from_lat, from_lon,
            to_lat, to_lon);

        /* Add geometry coordinates */
        for (uint32_t i = 0; i < route.num_coords && offset < (int)sizeof(g_response_buf) - 100; i++) {
            if (i > 0) {
                offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset, ",");
            }
            offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset,
                "[%.6f,%.6f]", route.coords[i].lat, route.coords[i].lon);
        }

        offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset,
            "]\n  }\n}");
    } else {
        offset = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\n"
            "  \"status\": \"ok\",\n"
            "  \"route\": {\n"
            "    \"distance\": %.1f,\n"
            "    \"duration\": %.1f,\n"
            "    \"profile\": \"%s\",\n"
            "    \"mode\": \"%s\",\n"
            "    \"from\": [%.6f, %.6f],\n"
            "    \"to\": [%.6f, %.6f]\n"
            "  }\n"
            "}",
            route.distance_m,
            route.duration_s,
            opts.profile == VL_PROFILE_TRUCK ? "truck" :
            opts.profile == VL_PROFILE_BIKE ? "bike" :
            opts.profile == VL_PROFILE_FOOT ? "foot" : "car",
            opts.weight == VL_WEIGHT_DISTANCE ? "shortest" : "fastest",
            from_lat, from_lon,
            to_lat, to_lon);
    }

    g_response_len = offset;
    g_response_status = 200;

    vl_free_route(&route);
}

/* Handle GET /api/v1/health */
static void handle_health(void) {
    g_response_status = 200;
    g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"velo-wasm-demo\",\n"
        "  \"version\": \"%d.%d.%d\"\n"
        "}",
        VL_VERSION_MAJOR, VL_VERSION_MINOR, VL_VERSION_PATCH);
}

/* Handle GET /api/v1/stats */
static void handle_stats(void) {
    g_response_status = 200;
    g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
        "{\n"
        "  \"graph_source\": \"monaco.vlg (embedded)\",\n"
        "  \"num_nodes\": %u,\n"
        "  \"num_edges\": %u,\n"
        "  \"bbox\": {\n"
        "    \"min_lat\": %.4f,\n"
        "    \"min_lon\": %.4f,\n"
        "    \"max_lat\": %.4f,\n"
        "    \"max_lon\": %.4f\n"
        "  }\n"
        "}",
        g_graph->num_nodes,
        g_graph->num_edges,
        g_graph->bbox_min.lat, g_graph->bbox_min.lon,
        g_graph->bbox_max.lat, g_graph->bbox_max.lon);
}

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routes to the appropriate handler based on path:
 *   /api/v1/route   - Calculate route
 *   /api/v1/health  - Health check
 *   /api/v1/stats   - Statistics
 *
 * @param path  Request path (e.g., "/api/v1/route")
 * @param query Query string without '?' (optional, can be NULL)
 * @return 0 on success
 */
WASM_EXPORT
int velo_api_handle(const char *path, const char *query) {
    if (!g_graph) {
        g_response_status = 500;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"API not initialized\"}");
        return -1;
    }

    g_response_content_type = "application/json";

    if (strcmp(path, "/api/v1/route") == 0) {
        handle_route(query);
    } else if (strcmp(path, "/api/v1/health") == 0) {
        handle_health();
    } else if (strcmp(path, "/api/v1/stats") == 0) {
        handle_stats();
    } else {
        g_response_status = 404;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Not found\"}");
    }

    return 0;
}

/* ============================================================================
 * Response Accessors (for JS access)
 * ============================================================================ */

WASM_EXPORT
int velo_response_status(void) {
    return g_response_status;
}

WASM_EXPORT
const char *velo_response_content_type(void) {
    return g_response_content_type;
}

WASM_EXPORT
const char *velo_response_body(void) {
    return g_response_buf;
}

WASM_EXPORT
size_t velo_response_body_len(void) {
    return g_response_len;
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
