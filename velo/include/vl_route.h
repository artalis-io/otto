/*
 * vl_route.h - Routing API
 *
 * Functions for computing routes between locations.
 */

#ifndef VL_ROUTE_H
#define VL_ROUTE_H

#include "vl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Route Options
 * ============================================================================ */

/*
 * Initialize route options with default values.
 *
 * opts: options structure to initialize
 *
 * Defaults:
 *   algorithm: VL_ALGORITHM_ASTAR_BIDIR
 *   weight: VL_WEIGHT_DURATION
 *   include_geometry: 1
 *   max_distance: 0 (unlimited)
 *   max_duration: 0 (unlimited)
 */
void vl_default_options(VLRouteOptions *opts);

/* ============================================================================
 * Routing Functions
 * ============================================================================ */

/*
 * Compute a route between two nodes.
 *
 * graph: the road network graph
 * source: source node index
 * target: target node index
 * opts: routing options (can be NULL for defaults)
 * route: (out) computed route
 *
 * Returns VL_OK on success, VL_ERROR_NO_ROUTE if no path exists.
 */
VLStatus vl_route(const VLGraph *graph, uint32_t source, uint32_t target,
                  const VLRouteOptions *opts, VLRoute *route);

/*
 * Compute a route between two coordinates.
 * Automatically finds nearest nodes to the given coordinates.
 *
 * graph: the road network graph
 * origin: origin coordinate
 * destination: destination coordinate
 * opts: routing options (can be NULL for defaults)
 * route: (out) computed route
 *
 * Returns VL_OK on success.
 */
VLStatus vl_route_coords(const VLGraph *graph, VLCoord origin, VLCoord destination,
                         const VLRouteOptions *opts, VLRoute *route);

/* ============================================================================
 * Query Context (for high-throughput routing)
 * ============================================================================ */

/*
 * Create a reusable query context.
 * Allocates memory once, reuses across queries with lazy initialization.
 *
 * graph: the road network graph
 *
 * Returns context, or NULL on allocation failure.
 */
VLQueryContext *vl_query_context_create(const VLGraph *graph);

/*
 * Free a query context.
 */
void vl_query_context_free(VLQueryContext *ctx);

/*
 * Compute a route using a pre-allocated query context.
 * Faster than vl_route() when making many queries.
 *
 * graph: the road network graph
 * ctx: reusable query context
 * source: source node index
 * target: target node index
 * opts: routing options (can be NULL for defaults)
 * route: (out) computed route
 *
 * Returns VL_OK on success.
 */
VLStatus vl_route_with_context(const VLGraph *graph, VLQueryContext *ctx,
                               uint32_t source, uint32_t target,
                               const VLRouteOptions *opts, VLRoute *route);

/* ============================================================================
 * Route Memory Management
 * ============================================================================ */

/*
 * Free route memory (coords and node_indices arrays).
 */
void vl_free_route(VLRoute *route);

#ifdef __cplusplus
}
#endif

#endif /* VL_ROUTE_H */
