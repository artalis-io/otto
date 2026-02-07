/**
 * Locus WASM API Demo
 *
 * Provides REST-compatible API endpoints via WASM, demonstrating
 * architecture parity between server and browser deployments.
 *
 * The Monaco geocoding index is embedded at compile time for a self-contained demo.
 * All API endpoints work identically to the mongoose server.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "locus.h"
#include "lc_serialize.h"
#include "lc_mmap.h"
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
 * read-only thereafter. Response buffers are written per-request with
 * no concurrent access possible in the JS event loop model.
 */
static LCIndex *g_index = NULL;

/* Response buffer for JSON (single-threaded WASM - no concurrent access) */
static char g_response_buf[65536];
static size_t g_response_len = 0;
static int g_response_status = 200;
static const char *g_response_content_type = "application/json";

/* Feature class to string */
static const char *fclass_to_str(LCFeatureClass fc) {
    switch (fc) {
        case LC_CLASS_COUNTRY: return "country";
        case LC_CLASS_STATE: return "state";
        case LC_CLASS_COUNTY: return "county";
        case LC_CLASS_CITY: return "city";
        case LC_CLASS_TOWN: return "town";
        case LC_CLASS_VILLAGE: return "village";
        case LC_CLASS_SUBURB: return "suburb";
        case LC_CLASS_NEIGHBOURHOOD: return "neighbourhood";
        case LC_CLASS_HAMLET: return "hamlet";
        case LC_CLASS_LOCALITY: return "locality";
        case LC_CLASS_STREET: return "street";
        case LC_CLASS_ADDRESS: return "address";
        case LC_CLASS_POI: return "poi";
        case LC_CLASS_WATER: return "water";
        case LC_CLASS_OTHER: return "other";
        default: return "unknown";
    }
}

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
    if (g_index) return 0;  /* Already initialized */

    g_index = lc_index_load_memory(monaco_lcx_data, monaco_lcx_data_len);
    return g_index ? 0 : -1;
}

/**
 * Free API context. Call on page unload (optional).
 */
WASM_EXPORT
void locus_api_free(void) {
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
    return g_index != NULL;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

/* URL decode a string in place */
static void url_decode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int val;
            if (sscanf(src + 1, "%2x", &val) == 1) {
                *dst++ = (char)val;
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

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
            url_decode(buf);
            return buf;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return NULL;
}

/* Handle GET /api/v1/search */
static void handle_search(const char *query) {
    char buf[256];

    /* Parse query string */
    const char *q = get_query_param(query, "q", buf, sizeof(buf));
    if (!q || !*q) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing q parameter\"}");
        return;
    }

    /* Parse limit */
    int limit = 10;
    const char *limit_str = get_query_param(query, "limit", buf, sizeof(buf));
    if (limit_str) {
        int l = atoi(limit_str);
        if (l > 0 && l <= 100) limit = l;
    }

    /* Perform search */
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = (size_t)limit;
    opts.fuzzy = 1;

    LCSearchResult result;
    lc_search(g_index, q, &opts, &result);

    /* Build JSON response */
    int offset = snprintf(g_response_buf, sizeof(g_response_buf),
        "{\n"
        "  \"query\": \"%s\",\n"
        "  \"total\": %zu,\n"
        "  \"results\": [", q, result.num_results);

    const LCMmapIndex *idx = g_index->mmap_idx;
    for (size_t i = 0; i < result.num_results && offset < (int)sizeof(g_response_buf) - 200; i++) {
        if (i > 0) offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset, ",");

        uint32_t eid = result.matches[i].entity_id;
        const char *name = lc_mmap_entity_name(idx, eid);
        LCFeatureClass fc = lc_mmap_entity_fclass(idx, eid);
        SHCoord coord = lc_mmap_entity_centroid(idx, eid);

        offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset,
            "\n    {"
            "\"name\": \"%s\", "
            "\"class\": \"%s\", "
            "\"lat\": %.6f, "
            "\"lon\": %.6f, "
            "\"score\": %.2f"
            "}",
            name ? name : "",
            fclass_to_str(fc),
            coord.lat, coord.lon,
            result.matches[i].score);
    }

    offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset,
        "\n  ]\n}");

    g_response_len = offset;
    g_response_status = 200;

    lc_search_result_free(&result);
}

/* Handle GET /api/v1/autocomplete */
static void handle_autocomplete(const char *query) {
    char buf[256];

    /* Parse query string */
    const char *q = get_query_param(query, "q", buf, sizeof(buf));
    if (!q || !*q) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing q parameter\"}");
        return;
    }

    /* Parse limit */
    int limit = 10;
    const char *limit_str = get_query_param(query, "limit", buf, sizeof(buf));
    if (limit_str) {
        int l = atoi(limit_str);
        if (l > 0 && l <= 20) limit = l;
    }

    /* Perform autocomplete */
    LCSearchResult result;
    lc_autocomplete(g_index, q, (size_t)limit, &result);

    /* Build JSON response (simple array of names) */
    int offset = snprintf(g_response_buf, sizeof(g_response_buf), "[");

    const LCMmapIndex *idx = g_index->mmap_idx;
    for (size_t i = 0; i < result.num_results && offset < (int)sizeof(g_response_buf) - 100; i++) {
        if (i > 0) offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset, ",");

        const char *name = lc_mmap_entity_name(idx, result.matches[i].entity_id);
        offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset,
            "\n  \"%s\"", name ? name : "");
    }

    offset += snprintf(g_response_buf + offset, sizeof(g_response_buf) - offset, "\n]");

    g_response_len = offset;
    g_response_status = 200;

    lc_search_result_free(&result);
}

/* Handle GET /api/v1/reverse */
static void handle_reverse(const char *query) {
    char buf[64];

    /* Parse coordinates */
    const char *lat_str = get_query_param(query, "lat", buf, sizeof(buf));
    if (!lat_str) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing lat parameter\"}");
        return;
    }
    double lat = atof(lat_str);

    const char *lon_str = get_query_param(query, "lon", buf, sizeof(buf));
    if (!lon_str) {
        g_response_status = 400;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"Missing lon parameter\"}");
        return;
    }
    double lon = atof(lon_str);

    /* Perform reverse geocoding */
    SHCoord coord = {lat, lon};
    LCReverseOptions opts;
    lc_reverse_options_default(&opts);
    opts.radius_m = 100.0;

    LCReverseResult result;
    lc_reverse(g_index, coord, &opts, &result);

    /* Extract names from entities */
    const char *street_name = result.street ? result.street->name : NULL;
    const char *place_name = result.place ? result.place->name : NULL;

    /* Build JSON response */
    g_response_status = 200;
    g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
        "{\n"
        "  \"lat\": %.6f,\n"
        "  \"lon\": %.6f,\n"
        "  \"display_name\": \"%s%s%s\",\n"
        "  \"distance_m\": %.1f\n"
        "}",
        lat, lon,
        street_name ? street_name : "",
        street_name && place_name ? ", " : "",
        place_name ? place_name : "",
        result.distance_m);

    lc_reverse_result_free(&result);
}

/* Handle GET /api/v1/health */
static void handle_health(void) {
    g_response_status = 200;
    g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"locus-wasm-demo\",\n"
        "  \"version\": \"%d.%d.%d\"\n"
        "}",
        LC_VERSION_MAJOR, LC_VERSION_MINOR, LC_VERSION_PATCH);
}

/* Handle GET /api/v1/stats */
static void handle_stats(void) {
    g_response_status = 200;

    const LCMmapIndex *idx = g_index->mmap_idx;
    const LCBinaryHeaderV4 *hdr = idx->header;

    g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
        "{\n"
        "  \"index_source\": \"monaco.lcx (embedded)\",\n"
        "  \"num_entities\": %u,\n"
        "  \"bbox\": {\n"
        "    \"min_lat\": %.4f,\n"
        "    \"min_lon\": %.4f,\n"
        "    \"max_lat\": %.4f,\n"
        "    \"max_lon\": %.4f\n"
        "  }\n"
        "}",
        hdr->entity_count,
        hdr->min_lat, hdr->min_lon,
        hdr->max_lat, hdr->max_lon);
}

/**
 * Handle an API request (REST-compatible interface).
 *
 * Routes to the appropriate handler based on path:
 *   /api/v1/search      - Forward geocoding
 *   /api/v1/autocomplete - Autocomplete suggestions
 *   /api/v1/reverse     - Reverse geocoding
 *   /api/v1/health      - Health check
 *   /api/v1/stats       - Statistics
 *
 * @param path  Request path (e.g., "/api/v1/search")
 * @param query Query string without '?' (optional, can be NULL)
 * @return 0 on success
 */
WASM_EXPORT
int locus_api_handle(const char *path, const char *query) {
    if (!g_index) {
        g_response_status = 500;
        g_response_len = snprintf(g_response_buf, sizeof(g_response_buf),
            "{\"error\": \"API not initialized\"}");
        return -1;
    }

    g_response_content_type = "application/json";

    if (strcmp(path, "/api/v1/search") == 0) {
        handle_search(query);
    } else if (strcmp(path, "/api/v1/autocomplete") == 0) {
        handle_autocomplete(query);
    } else if (strcmp(path, "/api/v1/reverse") == 0) {
        handle_reverse(query);
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
int locus_response_status(void) {
    return g_response_status;
}

WASM_EXPORT
const char *locus_response_content_type(void) {
    return g_response_content_type;
}

WASM_EXPORT
const char *locus_response_body(void) {
    return g_response_buf;
}

WASM_EXPORT
size_t locus_response_body_len(void) {
    return g_response_len;
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
