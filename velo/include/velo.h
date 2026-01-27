/*
 * velo.h - Velo Routing Engine Unified API
 *
 * Very Efficient Location Optimizer
 *
 * A zero-dependency routing engine that parses OSM PBF files and computes
 * shortest/fastest routes using bidirectional Dijkstra and A* algorithms.
 *
 * Basic usage:
 *
 *   // Load graph from PBF
 *   VLGraph *graph = vl_load_pbf("map.osm.pbf");
 *
 *   // Or load preprocessed binary (faster)
 *   // VLGraph *graph = vl_load_binary("map.vlg");
 *
 *   // Route between coordinates
 *   VLCoord origin = {47.4979, 19.0402};
 *   VLCoord destination = {46.2530, 20.1414};
 *
 *   VLRouteOptions opts;
 *   vl_default_options(&opts);
 *
 *   VLRoute route;
 *   if (vl_route_coords(graph, origin, destination, &opts, &route) == VL_OK) {
 *       printf("Distance: %.1f km\n", route.distance_m / 1000.0);
 *       printf("Duration: %.1f min\n", route.duration_s / 60.0);
 *   }
 *
 *   vl_free_route(&route);
 *   vl_free_graph(graph);
 */

#ifndef VELO_H
#define VELO_H

/* Include all component headers */
#include "vl_types.h"
#include "vl_pbf.h"
#include "vl_graph.h"
#include "vl_route.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WASM export macro */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define VL_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define VL_EXPORT
#endif

/* ============================================================================
 * Version Information
 * ============================================================================ */

#define VL_VERSION_MAJOR 1
#define VL_VERSION_MINOR 0
#define VL_VERSION_PATCH 0
#define VL_VERSION_STRING "1.0.0"

/*
 * Get version string.
 */
VL_EXPORT const char *vl_version(void);

/* ============================================================================
 * High-Level API
 * ============================================================================ */

/*
 * Load a graph from an OSM PBF file.
 * This is a convenience function that handles the full pipeline:
 *   1. Parse PBF file
 *   2. Build graph
 *   3. Finalize to CSR format
 *
 * filename: path to .osm.pbf file
 *
 * Returns loaded graph, or NULL on failure.
 * Caller must free with vl_free_graph().
 */
VL_EXPORT VLGraph *vl_load_pbf(const char *filename);

/*
 * Load a preprocessed binary graph file.
 *
 * filename: path to .vlg file
 *
 * Returns loaded graph, or NULL on failure.
 */
VL_EXPORT VLGraph *vl_load_binary(const char *filename);

/*
 * Save a graph to binary format.
 *
 * graph: graph to save
 * filename: output path (.vlg)
 *
 * Returns VL_OK on success.
 */
VL_EXPORT VLStatus vl_save_binary(const VLGraph *graph, const char *filename);

/* ============================================================================
 * Geo Utilities
 * ============================================================================ */

/*
 * Calculate great-circle distance between two coordinates.
 *
 * a, b: coordinates
 *
 * Returns distance in meters.
 */
VL_EXPORT double vl_haversine(VLCoord a, VLCoord b);

/*
 * Fast equirectangular distance approximation.
 * Much faster than haversine, accurate within ~0.5% for distances < 500km.
 *
 * Returns distance in meters.
 */
VL_EXPORT double vl_distance_fast(VLCoord a, VLCoord b);

/*
 * Check if a coordinate is valid.
 */
VL_EXPORT int vl_coord_valid(VLCoord c);

/* ============================================================================
 * Status Utilities
 * ============================================================================ */

/*
 * Get human-readable error message for a status code.
 */
VL_EXPORT const char *vl_status_string(VLStatus status);

#ifdef __cplusplus
}
#endif

#endif /* VELO_H */
