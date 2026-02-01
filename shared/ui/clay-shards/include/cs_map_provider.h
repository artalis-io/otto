/**
 * ClayShards Map Provider Interface
 *
 * Abstract interface for map data providers (tiles, routing, geocoding).
 * Allows compile-time or runtime selection between:
 *   - API provider: fetches from carta-api/velo-api/locus-api servers
 *   - WASM provider: embedded Carta/Velo/Locus (offline capable)
 *
 * Usage:
 *   // Request a route
 *   CsGeoPoint from = {47.5, 19.0};
 *   CsGeoPoint to = {46.2, 20.1};
 *   cs_provider_route(from, to);
 *
 *   // Check result next frame (async)
 *   if (cs_provider_route_ready()) {
 *       int count = cs_provider_route_count();
 *       for (int i = 0; i < count; i++) {
 *           CsGeoPoint pt = cs_provider_route_point(i);
 *       }
 *   }
 */

#ifndef CS_MAP_PROVIDER_H
#define CS_MAP_PROVIDER_H

#include "cs_common.h"
#include "cs_map.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Route points are stored dynamically - no hard limit */

#ifndef CS_PROVIDER_MAX_SEARCH_RESULTS
#define CS_PROVIDER_MAX_SEARCH_RESULTS 10
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/* Search result from geocoding */
typedef struct {
    double lat;
    double lon;
    char name[128];
    char type[32];      /* "city", "street", "poi", etc. */
    float score;        /* Relevance score 0-1 */
} CsSearchResult;

/* Route profile */
typedef enum {
    CS_PROFILE_CAR = 0,
    CS_PROFILE_TRUCK = 1
} CsRouteProfile;

/* Route optimization mode */
typedef enum {
    CS_MODE_FASTEST = 0,
    CS_MODE_SHORTEST = 1
} CsRouteMode;

/* Route result */
typedef struct {
    CsGeoPoint *points;
    int count;
    double distance_m;      /* Total distance in meters */
    double duration_s;      /* Estimated duration in seconds */
    double calc_time_ms;    /* Server calculation time in ms */
    bool ready;
    bool error;
    char error_msg[128];
} CsRouteResult;

/* Search results */
typedef struct {
    CsSearchResult *results;
    int count;
    bool ready;
    bool error;
    char error_msg[128];
} CsSearchResults;

/* Reverse geocoding result */
typedef struct {
    char name[256];
    char street[128];
    char city[64];
    char country[64];
    bool ready;
    bool error;
} CsReverseResult;

/* Provider status */
typedef enum {
    CS_PROVIDER_IDLE,
    CS_PROVIDER_LOADING,
    CS_PROVIDER_READY,
    CS_PROVIDER_ERROR
} CsProviderStatus;

/* ============================================================================
 * Tile Provider
 * ============================================================================ */

/**
 * Get tile URL for the current provider
 * For API provider: returns server URL
 * For WASM provider: returns empty (tiles rendered directly)
 *
 * @param z Zoom level
 * @param x Tile X
 * @param y Tile Y
 * @param layer Layer type (0=standard, 1=satellite, etc.)
 * @return URL string (static buffer, valid until next call)
 */
const char* cs_provider_tile_url(int z, int x, int y, int layer);

/**
 * Check if provider serves tiles directly (WASM) or via URL (API)
 */
bool cs_provider_has_direct_tiles(void);

/* ============================================================================
 * Routing Provider
 * ============================================================================ */

/**
 * Set route profile (car, truck) - affects next route request
 */
void cs_provider_set_route_profile(CsRouteProfile profile);

/**
 * Get current route profile
 */
CsRouteProfile cs_provider_get_route_profile(void);

/**
 * Set route mode (fastest, shortest) - affects next route request
 */
void cs_provider_set_route_mode(CsRouteMode mode);

/**
 * Get current route mode
 */
CsRouteMode cs_provider_get_route_mode(void);

/**
 * Request a route (async)
 * Call cs_provider_route_status() to check completion
 */
void cs_provider_route(CsGeoPoint from, CsGeoPoint to);

/**
 * Request a route with waypoints (async)
 */
void cs_provider_route_waypoints(const CsGeoPoint *points, int count);

/**
 * Get routing status
 */
CsProviderStatus cs_provider_route_status(void);

/**
 * Check if route is ready
 */
bool cs_provider_route_ready(void);

/**
 * Get route result (valid when ready)
 */
const CsRouteResult* cs_provider_route_result(void);

/**
 * Get route point count
 */
int cs_provider_route_count(void);

/**
 * Get route point by index
 */
CsGeoPoint cs_provider_route_point(int index);

/**
 * Clear current route
 */
void cs_provider_route_clear(void);

/* ============================================================================
 * Geocoding Provider (Forward Search)
 * ============================================================================ */

/**
 * Search for locations by text (async)
 */
void cs_provider_search(const char *query);

/**
 * Search with geographic bias (async)
 */
void cs_provider_search_near(const char *query, double lat, double lon);

/**
 * Get search status
 */
CsProviderStatus cs_provider_search_status(void);

/**
 * Check if search is ready
 */
bool cs_provider_search_ready(void);

/**
 * Get search results (valid when ready)
 */
const CsSearchResults* cs_provider_search_results(void);

/**
 * Get search result count
 */
int cs_provider_search_count(void);

/**
 * Get search result by index
 */
const CsSearchResult* cs_provider_search_result(int index);

/**
 * Clear search results
 */
void cs_provider_search_clear(void);

/* ============================================================================
 * Reverse Geocoding Provider
 * ============================================================================ */

/**
 * Reverse geocode a location (async)
 */
void cs_provider_reverse(double lat, double lon);

/**
 * Get reverse geocoding status
 */
CsProviderStatus cs_provider_reverse_status(void);

/**
 * Check if reverse geocoding is ready
 */
bool cs_provider_reverse_ready(void);

/**
 * Get reverse geocoding result (valid when ready)
 */
const CsReverseResult* cs_provider_reverse_result(void);

/**
 * Clear reverse result
 */
void cs_provider_reverse_clear(void);

/* ============================================================================
 * Provider Configuration
 * ============================================================================ */

/**
 * Set API server base URLs (for API provider)
 * Default: localhost with standard ports
 */
void cs_provider_set_tile_server(const char *url);     /* Default: http://localhost:8081 */
void cs_provider_set_route_server(const char *url);    /* Default: http://localhost:8082 */
void cs_provider_set_geocode_server(const char *url);  /* Default: http://localhost:8083 */

/**
 * Get current provider type
 */
const char* cs_provider_type(void);  /* "api" or "wasm" */

/**
 * Cleanup provider resources (free dynamic buffers)
 */
void cs_provider_cleanup(void);

/* ============================================================================
 * Provider Callbacks (set by JS layer for async operations)
 * ============================================================================ */

/**
 * Get route calculation time (round-trip time in ms)
 */
double cs_provider_route_calc_time(void);

/**
 * Called by JS when route fetch completes
 */
void cs_provider_on_route_complete(
    const double *lats, const double *lons, int count,
    double distance_m, double duration_s, double calc_time_ms
);

void cs_provider_on_route_error(const char *message);

/**
 * Called by JS to set individual search results before completing
 */
void cs_provider_set_search_result(
    int index, double lat, double lon,
    const char *name, const char *type, float score
);

/**
 * Called by JS when search fetch completes (after setting individual results)
 */
void cs_provider_on_search_complete(int count);

void cs_provider_on_search_error(const char *message);

/**
 * Called by JS when reverse geocoding completes
 */
void cs_provider_on_reverse_complete(
    const char *name, const char *street,
    const char *city, const char *country
);

void cs_provider_on_reverse_error(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* CS_MAP_PROVIDER_H */
