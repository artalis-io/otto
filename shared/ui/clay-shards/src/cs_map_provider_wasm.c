/**
 * ClayShards WASM Provider Implementation
 *
 * Integrates Velo (routing), Locus (geocoding), and Carta (tiles) directly.
 * Data is loaded from memory buffers provided by JavaScript.
 * All operations are synchronous once data is loaded.
 */

#include "cs_map_provider.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Include library headers - using the high-level APIs */
#include "velo.h"

/* Note: For a unified WASM build, we include the library sources directly.
 * Data loading is done via JS which writes to Emscripten's virtual filesystem. */

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* ============================================================================
 * Provider State
 * ============================================================================ */

/* Velo (routing) state */
static VLGraph *g_velo_graph = NULL;
static VLLandmarks *g_velo_landmarks = NULL;

/* Locus (geocoding) state */
static LCIndex *g_locus_index = NULL;

/* Carta (tiles) state */
static CTContext *g_carta_ctx = NULL;

/* Route state */
static CsGeoPoint g_route_points[CS_PROVIDER_MAX_ROUTE_POINTS];
static CsRouteResult g_route_result = {
    .points = g_route_points,
    .count = 0,
    .ready = false,
    .error = false,
};
static CsProviderStatus g_route_status = CS_PROVIDER_IDLE;

/* Search state */
static CsSearchResult g_search_results_data[CS_PROVIDER_MAX_SEARCH_RESULTS];
static CsSearchResults g_search_results = {
    .results = g_search_results_data,
    .count = 0,
    .ready = false,
    .error = false,
};
static CsProviderStatus g_search_status = CS_PROVIDER_IDLE;

/* Reverse geocoding state */
static CsReverseResult g_reverse_result = {
    .ready = false,
    .error = false,
};
static CsProviderStatus g_reverse_status = CS_PROVIDER_IDLE;

/* Tile URL buffer */
static char g_tile_url_buffer[512];

/* ============================================================================
 * Data Loading (called from JS with fetched data)
 * ============================================================================ */

/**
 * Load Velo graph from binary buffer (.vlg format)
 * @param data Pointer to binary data
 * @param size Size in bytes
 * @return 1 on success, 0 on failure
 */
EXPORT int cs_wasm_load_velo_graph(const uint8_t *data, size_t size) {
    if (g_velo_graph) {
        vl_graph_free(g_velo_graph);
        g_velo_graph = NULL;
    }
    if (g_velo_landmarks) {
        vl_landmarks_free(g_velo_landmarks);
        g_velo_landmarks = NULL;
    }

    g_velo_graph = vl_load_binary_memory(data, size);
    if (!g_velo_graph) {
        return 0;
    }

    /* Create landmarks for faster routing */
    g_velo_landmarks = vl_landmarks_create(g_velo_graph, 16);

    return 1;
}

/**
 * Load Locus index from PBF buffer
 * @param data Pointer to PBF data
 * @param size Size in bytes
 * @return 1 on success, 0 on failure
 */
EXPORT int cs_wasm_load_locus_pbf(const uint8_t *data, size_t size) {
    if (g_locus_index) {
        lc_index_free(g_locus_index);
        g_locus_index = NULL;
    }

    g_locus_index = lc_index_create();
    if (!g_locus_index) {
        return 0;
    }

    if (lc_index_build_from_pbf_memory(g_locus_index, data, size) != 0) {
        lc_index_free(g_locus_index);
        g_locus_index = NULL;
        return 0;
    }

    return 1;
}

/**
 * Load Carta context from PBF buffer
 * @param data Pointer to PBF data
 * @param size Size in bytes
 * @return 1 on success, 0 on failure
 */
EXPORT int cs_wasm_load_carta_pbf(const uint8_t *data, size_t size) {
    if (g_carta_ctx) {
        ct_context_free(g_carta_ctx);
        g_carta_ctx = NULL;
    }

    g_carta_ctx = ct_load_pbf_memory(data, size);
    return g_carta_ctx ? 1 : 0;
}

/**
 * Check if data is loaded
 */
EXPORT int cs_wasm_velo_ready(void) { return g_velo_graph != NULL; }
EXPORT int cs_wasm_locus_ready(void) { return g_locus_index != NULL; }
EXPORT int cs_wasm_carta_ready(void) { return g_carta_ctx != NULL; }

/* ============================================================================
 * Tile Provider (WASM renders directly)
 * ============================================================================ */

EXPORT const char* cs_provider_tile_url(int z, int x, int y, int layer) {
    (void)z; (void)x; (void)y; (void)layer;
    /* WASM provider doesn't use URLs - tiles are rendered directly */
    return "";
}

EXPORT bool cs_provider_has_direct_tiles(void) {
    return true; /* WASM provider renders tiles directly */
}

/**
 * Generate PNG tile directly (for WASM provider)
 * @param z Zoom level
 * @param x Tile X
 * @param y Tile Y
 * @param size Tile size (typically 256 or 512)
 * @param buffer Output buffer for PNG data
 * @param capacity Buffer capacity
 * @return PNG size in bytes, or 0 on error
 */
EXPORT int cs_wasm_generate_tile_png(int z, int x, int y, int size, uint8_t *buffer, int capacity) {
    if (!g_carta_ctx) return 0;
    return ct_generate_png(g_carta_ctx, z, x, y, size, buffer, (size_t)capacity);
}

/**
 * Generate MVT tile directly (for WASM provider)
 * @param z Zoom level
 * @param x Tile X
 * @param y Tile Y
 * @param buffer Output buffer for MVT data
 * @param capacity Buffer capacity
 * @return MVT size in bytes, or 0 on error
 */
EXPORT int cs_wasm_generate_tile_mvt(int z, int x, int y, uint8_t *buffer, int capacity) {
    if (!g_carta_ctx) return 0;
    return ct_generate_mvt(g_carta_ctx, z, x, y, buffer, (size_t)capacity);
}

/* ============================================================================
 * Routing Provider (synchronous for WASM)
 * ============================================================================ */

EXPORT void cs_provider_route(CsGeoPoint from, CsGeoPoint to) {
    g_route_status = CS_PROVIDER_LOADING;
    g_route_result.ready = false;
    g_route_result.error = false;
    g_route_result.count = 0;

    if (!g_velo_graph) {
        snprintf(g_route_result.error_msg, sizeof(g_route_result.error_msg), "Graph not loaded");
        g_route_result.error = true;
        g_route_result.ready = true;
        g_route_status = CS_PROVIDER_ERROR;
        return;
    }

    /* Compute route using Velo */
    VLRoute route;
    memset(&route, 0, sizeof(route));

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALG_ASTAR_BIDIR;
    opts.profile = VL_PROFILE_CAR;
    opts.weight = VL_WEIGHT_DURATION;
    opts.geometry = true;

    VLCoord origin = { from.lat, from.lon };
    VLCoord dest = { to.lat, to.lon };

    int result;
    if (g_velo_landmarks) {
        result = vl_route_astar_landmarks_coords(g_velo_graph, g_velo_landmarks, origin, dest, &opts, &route);
    } else {
        result = vl_route_coords(g_velo_graph, origin, dest, &opts, &route);
    }

    if (result != 0 || route.node_count == 0) {
        snprintf(g_route_result.error_msg, sizeof(g_route_result.error_msg), "No route found");
        g_route_result.error = true;
        g_route_result.ready = true;
        g_route_status = CS_PROVIDER_ERROR;
        vl_free_route(&route);
        return;
    }

    /* Copy route geometry */
    int count = route.node_count;
    if (count > CS_PROVIDER_MAX_ROUTE_POINTS) {
        count = CS_PROVIDER_MAX_ROUTE_POINTS;
    }

    for (int i = 0; i < count; i++) {
        g_route_points[i].lat = route.geometry[i].lat;
        g_route_points[i].lon = route.geometry[i].lon;
    }

    g_route_result.count = count;
    g_route_result.distance_m = route.distance_m;
    g_route_result.duration_s = route.duration_s;
    g_route_result.ready = true;
    g_route_result.error = false;
    g_route_status = CS_PROVIDER_READY;

    vl_free_route(&route);
}

EXPORT void cs_provider_route_waypoints(const CsGeoPoint *points, int count) {
    if (count < 2) return;
    /* For now, just use first and last as from/to */
    cs_provider_route(points[0], points[count - 1]);
}

EXPORT CsProviderStatus cs_provider_route_status(void) {
    return g_route_status;
}

EXPORT bool cs_provider_route_ready(void) {
    return g_route_result.ready;
}

EXPORT const CsRouteResult* cs_provider_route_result(void) {
    return &g_route_result;
}

EXPORT int cs_provider_route_count(void) {
    return g_route_result.count;
}

EXPORT CsGeoPoint cs_provider_route_point(int index) {
    if (index < 0 || index >= g_route_result.count) {
        return (CsGeoPoint){0, 0};
    }
    return g_route_points[index];
}

EXPORT void cs_provider_route_clear(void) {
    g_route_result.count = 0;
    g_route_result.ready = false;
    g_route_result.error = false;
    g_route_status = CS_PROVIDER_IDLE;
}

/* Route result accessors for JS */
EXPORT double cs_provider_route_distance(void) {
    return g_route_result.distance_m;
}

EXPORT double cs_provider_route_duration(void) {
    return g_route_result.duration_s;
}

EXPORT double cs_provider_route_point_lat(int index) {
    if (index < 0 || index >= g_route_result.count) return 0;
    return g_route_points[index].lat;
}

EXPORT double cs_provider_route_point_lon(int index) {
    if (index < 0 || index >= g_route_result.count) return 0;
    return g_route_points[index].lon;
}

/* ============================================================================
 * Search Provider (synchronous for WASM)
 * ============================================================================ */

EXPORT void cs_provider_search(const char *query) {
    cs_provider_search_near(query, 0, 0);
}

EXPORT void cs_provider_search_near(const char *query, double lat, double lon) {
    g_search_status = CS_PROVIDER_LOADING;
    g_search_results.ready = false;
    g_search_results.error = false;
    g_search_results.count = 0;

    if (!g_locus_index) {
        snprintf(g_search_results.error_msg, sizeof(g_search_results.error_msg), "Index not loaded");
        g_search_results.error = true;
        g_search_results.ready = true;
        g_search_status = CS_PROVIDER_ERROR;
        return;
    }

    /* Perform search */
    LCSearchOptions opts = {0};
    opts.limit = CS_PROVIDER_MAX_SEARCH_RESULTS;
    if (lat != 0 || lon != 0) {
        opts.bias_lat = lat;
        opts.bias_lon = lon;
        opts.bias_radius_km = 50.0;
    }

    LCSearchResult results;
    int ret = lc_search(g_locus_index, query, &opts, &results);

    if (ret != 0) {
        snprintf(g_search_results.error_msg, sizeof(g_search_results.error_msg), "Search failed");
        g_search_results.error = true;
        g_search_results.ready = true;
        g_search_status = CS_PROVIDER_ERROR;
        return;
    }

    /* Copy results */
    int count = results.count;
    if (count > CS_PROVIDER_MAX_SEARCH_RESULTS) {
        count = CS_PROVIDER_MAX_SEARCH_RESULTS;
    }

    for (int i = 0; i < count; i++) {
        LCEntity *entity = lc_index_get_entity(g_locus_index, results.entity_ids[i]);
        if (!entity) continue;

        g_search_results_data[i].lat = entity->lat;
        g_search_results_data[i].lon = entity->lon;
        g_search_results_data[i].score = results.scores[i];

        strncpy(g_search_results_data[i].name, entity->name, sizeof(g_search_results_data[i].name) - 1);
        g_search_results_data[i].name[sizeof(g_search_results_data[i].name) - 1] = '\0';

        /* Type based on entity type */
        const char *type_str = "place";
        if (entity->type == LC_ENTITY_CITY) type_str = "city";
        else if (entity->type == LC_ENTITY_TOWN) type_str = "town";
        else if (entity->type == LC_ENTITY_VILLAGE) type_str = "village";
        else if (entity->type == LC_ENTITY_STREET) type_str = "street";
        else if (entity->type == LC_ENTITY_POI) type_str = "poi";

        strncpy(g_search_results_data[i].type, type_str, sizeof(g_search_results_data[i].type) - 1);
        g_search_results_data[i].type[sizeof(g_search_results_data[i].type) - 1] = '\0';
    }

    lc_search_result_free(&results);

    g_search_results.count = count;
    g_search_results.ready = true;
    g_search_results.error = false;
    g_search_status = CS_PROVIDER_READY;
}

EXPORT CsProviderStatus cs_provider_search_status(void) {
    return g_search_status;
}

EXPORT bool cs_provider_search_ready(void) {
    return g_search_results.ready;
}

EXPORT const CsSearchResults* cs_provider_search_results(void) {
    return &g_search_results;
}

EXPORT int cs_provider_search_count(void) {
    return g_search_results.count;
}

EXPORT const CsSearchResult* cs_provider_search_result(int index) {
    if (index < 0 || index >= g_search_results.count) {
        return NULL;
    }
    return &g_search_results_data[index];
}

EXPORT void cs_provider_search_clear(void) {
    g_search_results.count = 0;
    g_search_results.ready = false;
    g_search_results.error = false;
    g_search_status = CS_PROVIDER_IDLE;
}

/* Search result accessors for JS */
EXPORT double cs_provider_search_result_lat(int index) {
    if (index < 0 || index >= g_search_results.count) return 0;
    return g_search_results_data[index].lat;
}

EXPORT double cs_provider_search_result_lon(int index) {
    if (index < 0 || index >= g_search_results.count) return 0;
    return g_search_results_data[index].lon;
}

EXPORT const char* cs_provider_search_result_name(int index) {
    if (index < 0 || index >= g_search_results.count) return "";
    return g_search_results_data[index].name;
}

EXPORT const char* cs_provider_search_result_type(int index) {
    if (index < 0 || index >= g_search_results.count) return "";
    return g_search_results_data[index].type;
}

EXPORT float cs_provider_search_result_score(int index) {
    if (index < 0 || index >= g_search_results.count) return 0;
    return g_search_results_data[index].score;
}

/* ============================================================================
 * Reverse Geocoding Provider (synchronous for WASM)
 * ============================================================================ */

EXPORT void cs_provider_reverse(double lat, double lon) {
    g_reverse_status = CS_PROVIDER_LOADING;
    g_reverse_result.ready = false;
    g_reverse_result.error = false;

    if (!g_locus_index) {
        g_reverse_result.error = true;
        g_reverse_result.ready = true;
        g_reverse_status = CS_PROVIDER_ERROR;
        return;
    }

    /* Perform reverse geocoding */
    LCReverseOptions opts = {0};
    opts.radius_m = 100;
    opts.limit = 1;

    LCReverseResult result;
    int ret = lc_reverse(g_locus_index, lat, lon, &opts, &result);

    if (ret != 0 || result.count == 0) {
        g_reverse_result.error = true;
        g_reverse_result.ready = true;
        g_reverse_status = CS_PROVIDER_ERROR;
        return;
    }

    /* Get entity details */
    LCEntity *entity = lc_index_get_entity(g_locus_index, result.entity_ids[0]);
    if (!entity) {
        g_reverse_result.error = true;
        g_reverse_result.ready = true;
        g_reverse_status = CS_PROVIDER_ERROR;
        lc_reverse_result_free(&result);
        return;
    }

    strncpy(g_reverse_result.name, entity->name, sizeof(g_reverse_result.name) - 1);
    g_reverse_result.name[sizeof(g_reverse_result.name) - 1] = '\0';

    /* These would come from additional entity metadata if available */
    g_reverse_result.street[0] = '\0';
    g_reverse_result.city[0] = '\0';
    g_reverse_result.country[0] = '\0';

    lc_reverse_result_free(&result);

    g_reverse_result.ready = true;
    g_reverse_result.error = false;
    g_reverse_status = CS_PROVIDER_READY;
}

EXPORT CsProviderStatus cs_provider_reverse_status(void) {
    return g_reverse_status;
}

EXPORT bool cs_provider_reverse_ready(void) {
    return g_reverse_result.ready;
}

EXPORT const CsReverseResult* cs_provider_reverse_result(void) {
    return &g_reverse_result;
}

EXPORT void cs_provider_reverse_clear(void) {
    g_reverse_result.ready = false;
    g_reverse_result.error = false;
    g_reverse_status = CS_PROVIDER_IDLE;
}

/* Reverse result accessors for JS */
EXPORT const char* cs_provider_reverse_name(void) {
    return g_reverse_result.name;
}

EXPORT const char* cs_provider_reverse_street(void) {
    return g_reverse_result.street;
}

EXPORT const char* cs_provider_reverse_city(void) {
    return g_reverse_result.city;
}

EXPORT const char* cs_provider_reverse_country(void) {
    return g_reverse_result.country;
}

/* ============================================================================
 * Provider Configuration (not used for WASM, but implemented for API compat)
 * ============================================================================ */

EXPORT void cs_provider_set_tile_server(const char *url) { (void)url; }
EXPORT void cs_provider_set_route_server(const char *url) { (void)url; }
EXPORT void cs_provider_set_geocode_server(const char *url) { (void)url; }

EXPORT const char* cs_provider_type(void) {
    return "wasm";
}

/* ============================================================================
 * Stub functions for API compatibility (not needed for WASM but required by interface)
 * ============================================================================ */

/* These are for the API provider JS bridge - not used by WASM provider */
EXPORT bool cs_provider_route_is_pending(void) { return false; }
EXPORT double cs_provider_route_from_lat(void) { return 0; }
EXPORT double cs_provider_route_from_lon(void) { return 0; }
EXPORT double cs_provider_route_to_lat(void) { return 0; }
EXPORT double cs_provider_route_to_lon(void) { return 0; }
EXPORT void cs_provider_route_mark_fetching(void) { }
EXPORT const char* cs_provider_route_server(void) { return ""; }
EXPORT void cs_provider_on_route_complete(const double *lats, const double *lons, int count, double dist, double dur) {
    (void)lats; (void)lons; (void)count; (void)dist; (void)dur;
}
EXPORT void cs_provider_on_route_error(const char *msg) { (void)msg; }

EXPORT bool cs_provider_search_is_pending(void) { return false; }
EXPORT const char* cs_provider_search_query(void) { return ""; }
EXPORT bool cs_provider_search_has_bias(void) { return false; }
EXPORT double cs_provider_search_bias_lat(void) { return 0; }
EXPORT double cs_provider_search_bias_lon(void) { return 0; }
EXPORT void cs_provider_search_mark_fetching(void) { }
EXPORT const char* cs_provider_geocode_server(void) { return ""; }
EXPORT void cs_provider_set_search_result(int i, double lat, double lon, const char *n, const char *t, float s) {
    (void)i; (void)lat; (void)lon; (void)n; (void)t; (void)s;
}
EXPORT void cs_provider_on_search_complete(int count) { (void)count; }
EXPORT void cs_provider_on_search_error(const char *msg) { (void)msg; }

EXPORT bool cs_provider_reverse_is_pending(void) { return false; }
EXPORT double cs_provider_reverse_lat(void) { return 0; }
EXPORT double cs_provider_reverse_lon(void) { return 0; }
EXPORT void cs_provider_reverse_mark_fetching(void) { }
EXPORT void cs_provider_on_reverse_complete(const char *n, const char *s, const char *c, const char *co) {
    (void)n; (void)s; (void)c; (void)co;
}
EXPORT void cs_provider_on_reverse_error(const char *msg) { (void)msg; }
