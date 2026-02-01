/**
 * ClayShards Map Provider Implementation
 *
 * API provider implementation - uses JS fetch to REST APIs.
 * WASM provider would be a separate file linked at build time.
 */

#include "cs_map_provider.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* ============================================================================
 * State
 * ============================================================================ */

/* Server URLs */
static char g_tile_server[256] = "http://localhost:8081";
static char g_route_server[256] = "http://localhost:8082";
static char g_geocode_server[256] = "http://localhost:8083";

/* Route state */
static CsGeoPoint g_route_points[CS_PROVIDER_MAX_ROUTE_POINTS];
static CsRouteResult g_route_result = {
    .points = g_route_points,
    .count = 0,
    .ready = false,
    .error = false,
};
static CsProviderStatus g_route_status = CS_PROVIDER_IDLE;

/* Pending route request (for JS to read) */
static CsGeoPoint g_route_from;
static CsGeoPoint g_route_to;
static bool g_route_pending = false;

/* Search state */
static CsSearchResult g_search_results_data[CS_PROVIDER_MAX_SEARCH_RESULTS];
static CsSearchResults g_search_results = {
    .results = g_search_results_data,
    .count = 0,
    .ready = false,
    .error = false,
};
static CsProviderStatus g_search_status = CS_PROVIDER_IDLE;

/* Pending search request */
static char g_search_query[256];
static double g_search_bias_lat = 0;
static double g_search_bias_lon = 0;
static bool g_search_has_bias = false;
static bool g_search_pending = false;

/* Reverse geocoding state */
static CsReverseResult g_reverse_result = {
    .ready = false,
    .error = false,
};
static CsProviderStatus g_reverse_status = CS_PROVIDER_IDLE;

/* Pending reverse request */
static double g_reverse_lat;
static double g_reverse_lon;
static bool g_reverse_pending = false;

/* Tile URL buffer */
static char g_tile_url_buffer[512];

/* ============================================================================
 * Tile Provider
 * ============================================================================ */

EXPORT const char* cs_provider_tile_url(int z, int x, int y, int layer) {
    (void)layer; /* TODO: support different layers */
    snprintf(g_tile_url_buffer, sizeof(g_tile_url_buffer),
             "%s/tiles/%d/%d/%d.png", g_tile_server, z, x, y);
    return g_tile_url_buffer;
}

EXPORT bool cs_provider_has_direct_tiles(void) {
    return false; /* API provider uses URLs, not direct rendering */
}

/* ============================================================================
 * Routing Provider
 * ============================================================================ */

EXPORT void cs_provider_route(CsGeoPoint from, CsGeoPoint to) {
    g_route_from = from;
    g_route_to = to;
    g_route_pending = true;
    g_route_status = CS_PROVIDER_LOADING;
    g_route_result.ready = false;
    g_route_result.error = false;
    g_route_result.count = 0;
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
    g_route_pending = false;
}

/* Route request accessors for JS */
EXPORT bool cs_provider_route_is_pending(void) {
    return g_route_pending;
}

EXPORT double cs_provider_route_from_lat(void) { return g_route_from.lat; }
EXPORT double cs_provider_route_from_lon(void) { return g_route_from.lon; }
EXPORT double cs_provider_route_to_lat(void) { return g_route_to.lat; }
EXPORT double cs_provider_route_to_lon(void) { return g_route_to.lon; }

EXPORT void cs_provider_route_mark_fetching(void) {
    g_route_pending = false;
}

EXPORT const char* cs_provider_route_server(void) {
    return g_route_server;
}

/* ============================================================================
 * Search Provider
 * ============================================================================ */

EXPORT void cs_provider_search(const char *query) {
    strncpy(g_search_query, query, sizeof(g_search_query) - 1);
    g_search_query[sizeof(g_search_query) - 1] = '\0';
    g_search_has_bias = false;
    g_search_pending = true;
    g_search_status = CS_PROVIDER_LOADING;
    g_search_results.ready = false;
    g_search_results.error = false;
    g_search_results.count = 0;
}

EXPORT void cs_provider_search_near(const char *query, double lat, double lon) {
    strncpy(g_search_query, query, sizeof(g_search_query) - 1);
    g_search_query[sizeof(g_search_query) - 1] = '\0';
    g_search_bias_lat = lat;
    g_search_bias_lon = lon;
    g_search_has_bias = true;
    g_search_pending = true;
    g_search_status = CS_PROVIDER_LOADING;
    g_search_results.ready = false;
    g_search_results.error = false;
    g_search_results.count = 0;
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
    g_search_pending = false;
}

/* Search request accessors for JS */
EXPORT bool cs_provider_search_is_pending(void) {
    return g_search_pending;
}

EXPORT const char* cs_provider_search_query(void) {
    return g_search_query;
}

EXPORT bool cs_provider_search_has_bias(void) {
    return g_search_has_bias;
}

EXPORT double cs_provider_search_bias_lat(void) { return g_search_bias_lat; }
EXPORT double cs_provider_search_bias_lon(void) { return g_search_bias_lon; }

EXPORT void cs_provider_search_mark_fetching(void) {
    g_search_pending = false;
}

EXPORT const char* cs_provider_geocode_server(void) {
    return g_geocode_server;
}

/* ============================================================================
 * Reverse Geocoding Provider
 * ============================================================================ */

EXPORT void cs_provider_reverse(double lat, double lon) {
    g_reverse_lat = lat;
    g_reverse_lon = lon;
    g_reverse_pending = true;
    g_reverse_status = CS_PROVIDER_LOADING;
    g_reverse_result.ready = false;
    g_reverse_result.error = false;
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
    g_reverse_pending = false;
}

/* Reverse request accessors for JS */
EXPORT bool cs_provider_reverse_is_pending(void) {
    return g_reverse_pending;
}

EXPORT double cs_provider_reverse_lat(void) { return g_reverse_lat; }
EXPORT double cs_provider_reverse_lon(void) { return g_reverse_lon; }

EXPORT void cs_provider_reverse_mark_fetching(void) {
    g_reverse_pending = false;
}

/* ============================================================================
 * Server Configuration
 * ============================================================================ */

EXPORT void cs_provider_set_tile_server(const char *url) {
    strncpy(g_tile_server, url, sizeof(g_tile_server) - 1);
    g_tile_server[sizeof(g_tile_server) - 1] = '\0';
}

EXPORT void cs_provider_set_route_server(const char *url) {
    strncpy(g_route_server, url, sizeof(g_route_server) - 1);
    g_route_server[sizeof(g_route_server) - 1] = '\0';
}

EXPORT void cs_provider_set_geocode_server(const char *url) {
    strncpy(g_geocode_server, url, sizeof(g_geocode_server) - 1);
    g_geocode_server[sizeof(g_geocode_server) - 1] = '\0';
}

EXPORT const char* cs_provider_type(void) {
    return "api";
}

/* ============================================================================
 * Douglas-Peucker Polyline Simplification (for route compression)
 * ============================================================================ */

/* Initial epsilon for simplification (roughly 100m at equator) */
#define SIMPLIFY_EPSILON_INITIAL 0.001

/* Perpendicular distance from point to line segment */
static double perp_distance(
    double px, double py,
    double x1, double y1,
    double x2, double y2
) {
    double dx = x2 - x1;
    double dy = y2 - y1;
    double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-12) {
        dx = px - x1;
        dy = py - y1;
        return sqrt(dx * dx + dy * dy);
    }

    double t = ((px - x1) * dx + (py - y1) * dy) / len_sq;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    double proj_x = x1 + t * dx;
    double proj_y = y1 + t * dy;
    dx = px - proj_x;
    dy = py - proj_y;
    return sqrt(dx * dx + dy * dy);
}

/* Recursive Douglas-Peucker */
static void dp_recursive(
    const double *lats, const double *lons,
    int start, int end,
    double epsilon,
    bool *keep
) {
    if (end <= start + 1) return;

    double max_dist = 0.0;
    int max_idx = start;

    for (int i = start + 1; i < end; i++) {
        double dist = perp_distance(
            lons[i], lats[i],
            lons[start], lats[start],
            lons[end], lats[end]
        );
        if (dist > max_dist) {
            max_dist = dist;
            max_idx = i;
        }
    }

    if (max_dist > epsilon) {
        keep[max_idx] = true;
        dp_recursive(lats, lons, start, max_idx, epsilon, keep);
        dp_recursive(lats, lons, max_idx, end, epsilon, keep);
    }
}

/* Count points that would be kept with given epsilon */
static int count_simplified(const double *lats, const double *lons, int count, double epsilon) {
    if (count <= 2) return count;

    bool *keep = (bool *)calloc((size_t)count, sizeof(bool));
    if (!keep) return 2;

    keep[0] = true;
    keep[count - 1] = true;
    dp_recursive(lats, lons, 0, count - 1, epsilon, keep);

    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (keep[i]) kept++;
    }
    free(keep);
    return kept;
}

/* Simplify route with adaptive epsilon to fit in max_out points */
static int simplify_route(
    const double *lats, const double *lons, int count,
    CsGeoPoint *out, int max_out
) {
    if (count <= max_out) {
        /* No simplification needed */
        for (int i = 0; i < count; i++) {
            out[i].lat = lats[i];
            out[i].lon = lons[i];
        }
        return count;
    }

    if (max_out < 2) {
        out[0].lat = lats[0];
        out[0].lon = lons[0];
        return 1;
    }

    /* Adaptive epsilon: increase until we fit */
    double epsilon = SIMPLIFY_EPSILON_INITIAL;
    int kept = count_simplified(lats, lons, count, epsilon);

    for (int iter = 0; iter < 20 && kept > max_out; iter++) {
        epsilon *= 2.0;
        kept = count_simplified(lats, lons, count, epsilon);
    }

    /* If still too many, use uniform sampling */
    if (kept > max_out) {
        out[0].lat = lats[0];
        out[0].lon = lons[0];
        out[max_out - 1].lat = lats[count - 1];
        out[max_out - 1].lon = lons[count - 1];

        if (max_out > 2) {
            double step = (double)(count - 1) / (double)(max_out - 1);
            for (int i = 1; i < max_out - 1; i++) {
                int idx = (int)(i * step);
                if (idx >= count) idx = count - 1;
                out[i].lat = lats[idx];
                out[i].lon = lons[idx];
            }
        }
        return max_out;
    }

    /* Final pass: collect kept points */
    bool *keep = (bool *)calloc((size_t)count, sizeof(bool));
    if (!keep) {
        out[0].lat = lats[0];
        out[0].lon = lons[0];
        out[1].lat = lats[count - 1];
        out[1].lon = lons[count - 1];
        return 2;
    }

    keep[0] = true;
    keep[count - 1] = true;
    dp_recursive(lats, lons, 0, count - 1, epsilon, keep);

    int out_count = 0;
    for (int i = 0; i < count && out_count < max_out; i++) {
        if (keep[i]) {
            out[out_count].lat = lats[i];
            out[out_count].lon = lons[i];
            out_count++;
        }
    }

    free(keep);
    return out_count;
}

/* ============================================================================
 * Callbacks from JS
 * ============================================================================ */

EXPORT void cs_provider_on_route_complete(
    const double *lats, const double *lons, int count,
    double distance_m, double duration_s
) {
    /* Simplify if needed to fit in buffer */
    int result_count = simplify_route(
        lats, lons, count,
        g_route_points, CS_PROVIDER_MAX_ROUTE_POINTS
    );

    g_route_result.count = result_count;
    g_route_result.distance_m = distance_m;
    g_route_result.duration_s = duration_s;
    g_route_result.ready = true;
    g_route_result.error = false;
    g_route_status = CS_PROVIDER_READY;
}

EXPORT void cs_provider_on_route_error(const char *message) {
    strncpy(g_route_result.error_msg, message, sizeof(g_route_result.error_msg) - 1);
    g_route_result.error_msg[sizeof(g_route_result.error_msg) - 1] = '\0';
    g_route_result.error = true;
    g_route_result.ready = true;
    g_route_status = CS_PROVIDER_ERROR;
}

EXPORT void cs_provider_on_search_complete(int count) {
    /* Individual results are set via cs_provider_set_search_result */
    g_search_results.count = count < CS_PROVIDER_MAX_SEARCH_RESULTS ? count : CS_PROVIDER_MAX_SEARCH_RESULTS;
    g_search_results.ready = true;
    g_search_results.error = false;
    g_search_status = CS_PROVIDER_READY;
}

EXPORT void cs_provider_set_search_result(
    int index, double lat, double lon,
    const char *name, const char *type, float score
) {
    if (index < 0 || index >= CS_PROVIDER_MAX_SEARCH_RESULTS) return;

    g_search_results_data[index].lat = lat;
    g_search_results_data[index].lon = lon;
    g_search_results_data[index].score = score;

    strncpy(g_search_results_data[index].name, name, sizeof(g_search_results_data[index].name) - 1);
    g_search_results_data[index].name[sizeof(g_search_results_data[index].name) - 1] = '\0';

    strncpy(g_search_results_data[index].type, type, sizeof(g_search_results_data[index].type) - 1);
    g_search_results_data[index].type[sizeof(g_search_results_data[index].type) - 1] = '\0';
}

EXPORT void cs_provider_on_search_error(const char *message) {
    strncpy(g_search_results.error_msg, message, sizeof(g_search_results.error_msg) - 1);
    g_search_results.error_msg[sizeof(g_search_results.error_msg) - 1] = '\0';
    g_search_results.error = true;
    g_search_results.ready = true;
    g_search_status = CS_PROVIDER_ERROR;
}

EXPORT void cs_provider_on_reverse_complete(
    const char *name, const char *street,
    const char *city, const char *country
) {
    strncpy(g_reverse_result.name, name, sizeof(g_reverse_result.name) - 1);
    g_reverse_result.name[sizeof(g_reverse_result.name) - 1] = '\0';

    strncpy(g_reverse_result.street, street, sizeof(g_reverse_result.street) - 1);
    g_reverse_result.street[sizeof(g_reverse_result.street) - 1] = '\0';

    strncpy(g_reverse_result.city, city, sizeof(g_reverse_result.city) - 1);
    g_reverse_result.city[sizeof(g_reverse_result.city) - 1] = '\0';

    strncpy(g_reverse_result.country, country, sizeof(g_reverse_result.country) - 1);
    g_reverse_result.country[sizeof(g_reverse_result.country) - 1] = '\0';

    g_reverse_result.ready = true;
    g_reverse_result.error = false;
    g_reverse_status = CS_PROVIDER_READY;
}

EXPORT void cs_provider_on_reverse_error(const char *message) {
    (void)message;
    g_reverse_result.error = true;
    g_reverse_result.ready = true;
    g_reverse_status = CS_PROVIDER_ERROR;
}

/* ============================================================================
 * Route Result Accessors for JS
 * ============================================================================ */

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
 * Search Result Accessors for JS
 * ============================================================================ */

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
 * Reverse Result Accessors for JS
 * ============================================================================ */

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
