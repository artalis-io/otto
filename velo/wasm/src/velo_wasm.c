/**
 * Velo WASM Entry Points
 *
 * Provides browser-friendly API for the Velo routing engine.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "velo.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

// =============================================================================
// Memory Management
// =============================================================================

WASM_EXPORT
void* wasm_malloc(int size) {
    return malloc(size);
}

WASM_EXPORT
void wasm_free(void* ptr) {
    free(ptr);
}

// =============================================================================
// Version
// =============================================================================

WASM_EXPORT
int wasm_version(void) {
    return VL_VERSION_MAJOR * 10000 + VL_VERSION_MINOR * 100 + VL_VERSION_PATCH;
}

// =============================================================================
// Graph Loading
// =============================================================================

/**
 * Load graph from memory buffer (binary .vlg format)
 * Returns graph pointer or NULL on error
 */
WASM_EXPORT
VLGraph* wasm_load_graph_memory(const uint8_t* data, size_t size) {
    if (!data || size == 0) return NULL;
    return vl_load_binary_memory(data, size);
}

WASM_EXPORT
void wasm_graph_free(VLGraph* graph) {
    if (graph) {
        vl_graph_free(graph);
    }
}

WASM_EXPORT
uint32_t wasm_graph_node_count(const VLGraph* graph) {
    return graph ? graph->node_count : 0;
}

WASM_EXPORT
uint32_t wasm_graph_edge_count(const VLGraph* graph) {
    return graph ? graph->edge_count : 0;
}

// =============================================================================
// Routing
// =============================================================================

// Route result structure for WASM
typedef struct {
    double distance_m;
    double duration_s;
    uint32_t node_count;
    double* coords;  // lat, lon pairs
    int status;
} WasmRoute;

/**
 * Calculate route between two coordinates
 *
 * @param graph     Graph pointer
 * @param from_lat  Origin latitude
 * @param from_lon  Origin longitude
 * @param to_lat    Destination latitude
 * @param to_lon    Destination longitude
 * @param profile   0=car, 1=truck, 2=bike, 3=foot
 * @param mode      0=fastest, 1=shortest
 * @return Route pointer or NULL on error
 */
WASM_EXPORT
WasmRoute* wasm_route(VLGraph* graph,
                      double from_lat, double from_lon,
                      double to_lat, double to_lon,
                      int profile, int mode) {
    if (!graph) return NULL;

    WasmRoute* result = malloc(sizeof(WasmRoute));
    if (!result) return NULL;

    memset(result, 0, sizeof(WasmRoute));

    VLCoord from = {from_lat, from_lon};
    VLCoord to = {to_lat, to_lon};

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.profile = (VLProfile)profile;
    opts.weight = (mode == 1) ? VL_WEIGHT_DISTANCE : VL_WEIGHT_DURATION;
    opts.geometry = 1;

    VLRoute route;
    memset(&route, 0, sizeof(route));
    VLStatus status = vl_route_coords(graph, from, to, &opts, &route);

    result->status = status;

    if (status == VL_OK) {
        result->distance_m = route.distance_m;
        result->duration_s = route.duration_s;
        result->node_count = route.num_coords;

        // Copy coordinates (with overflow check)
        if (route.num_coords > 0 && route.coords) {
            if (route.num_coords <= SIZE_MAX / (2 * sizeof(double))) {
                result->coords = malloc((size_t)route.num_coords * 2 * sizeof(double));
                if (result->coords) {
                    for (int i = 0; i < route.num_coords; i++) {
                        result->coords[i * 2] = route.coords[i].lat;
                        result->coords[i * 2 + 1] = route.coords[i].lon;
                    }
                }
            }
        }

        vl_free_route(&route);
    }

    return result;
}

WASM_EXPORT
void wasm_route_free(WasmRoute* route) {
    if (route) {
        if (route->coords) free(route->coords);
        free(route);
    }
}

WASM_EXPORT
double wasm_route_distance(const WasmRoute* route) {
    return route ? route->distance_m : 0;
}

WASM_EXPORT
double wasm_route_duration(const WasmRoute* route) {
    return route ? route->duration_s : 0;
}

WASM_EXPORT
uint32_t wasm_route_node_count(const WasmRoute* route) {
    return route ? route->node_count : 0;
}

/**
 * Get route coordinates into provided buffer
 * Buffer should be node_count * 2 doubles (lat, lon pairs)
 * Returns number of coordinates written
 */
WASM_EXPORT
uint32_t wasm_route_get_coords(const WasmRoute* route, double* coords) {
    if (!route || !route->coords || !coords) return 0;

    /* Check for overflow before memcpy */
    if (route->node_count > SIZE_MAX / (2 * sizeof(double))) return 0;
    size_t size = (size_t)route->node_count * 2 * sizeof(double);
    memcpy(coords, route->coords, size);
    return route->node_count;
}

// =============================================================================
// Utilities
// =============================================================================

/**
 * Find nearest node to coordinates
 * Returns node index or UINT32_MAX if not found
 */
WASM_EXPORT
uint32_t wasm_nearest_node(const VLGraph* graph, double lat, double lon) {
    if (!graph) return UINT32_MAX;

    VLCoord coord = {lat, lon};
    return vl_graph_nearest_node(graph, coord);
}
