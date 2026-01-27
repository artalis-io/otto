/*
 * vl_graph.h - Graph operations API
 *
 * Functions for building, saving, loading, and querying road network graphs.
 */

#ifndef VL_GRAPH_H
#define VL_GRAPH_H

#include "vl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Graph Builder
 * ============================================================================ */

/*
 * Create a new graph builder.
 *
 * expected_nodes: estimated number of nodes (for pre-allocation)
 *
 * Returns NULL on allocation failure.
 */
VLGraphBuilder *vl_graph_builder_create(size_t expected_nodes);

/*
 * Free a graph builder.
 */
void vl_graph_builder_free(VLGraphBuilder *builder);

/*
 * Build graph edges from parsed PBF data.
 *
 * builder: graph builder
 * ctx: PBF context with parsed nodes and ways
 *
 * Returns VL_OK on success.
 */
VLStatus vl_graph_build_from_pbf(VLGraphBuilder *builder, const VLPBFContext *ctx);

/*
 * Finalize the graph and convert to CSR format.
 * After this call, the builder should be freed.
 *
 * Returns the final graph, or NULL on failure.
 */
VLGraph *vl_graph_finalize(VLGraphBuilder *builder);

/* ============================================================================
 * Reverse Graph Index
 * ============================================================================ */

/*
 * Build reverse graph index for bidirectional search.
 * Called automatically during finalize, but can be called manually
 * if loading a graph without reverse index.
 *
 * Returns VL_OK on success.
 */
VLStatus vl_graph_build_reverse_index(VLGraph *graph);

/* ============================================================================
 * Grid Spatial Index
 * ============================================================================ */

/*
 * Build grid spatial index for fast nearest-node queries.
 * Called automatically during finalize.
 *
 * Returns VL_OK on success.
 */
VLStatus vl_graph_build_grid_index(VLGraph *graph);

/*
 * Find nearest node using grid index (O(1) average case).
 */
uint32_t vl_graph_nearest_node_grid(const VLGraph *graph, VLCoord coord);

/* ============================================================================
 * Graph Memory Management
 * ============================================================================ */

/*
 * Free a graph.
 */
void vl_graph_free(VLGraph *graph);

/* ============================================================================
 * Binary Save/Load
 * ============================================================================ */

/*
 * Save graph to binary file format (.vlg).
 *
 * graph: graph to save
 * filename: output file path
 *
 * Returns VL_OK on success.
 */
VLStatus vl_graph_save(const VLGraph *graph, const char *filename);

/*
 * Load graph from binary file.
 *
 * filename: path to .vlg file
 *
 * Returns loaded graph, or NULL on failure.
 */
VLGraph *vl_graph_load(const char *filename);

#ifndef _WIN32
/*
 * Memory-map a binary graph file for faster loading.
 * The returned graph does not own its memory.
 *
 * filename: path to .vlg file
 *
 * Returns loaded graph, or NULL on failure.
 */
VLGraph *vl_graph_mmap(const char *filename);
#endif

/* ============================================================================
 * Nearest Node Queries
 * ============================================================================ */

/*
 * Find the nearest node to a coordinate.
 *
 * graph: the road network graph
 * coord: query coordinate
 *
 * Returns node index, or VL_INVALID_NODE if graph is empty.
 */
uint32_t vl_graph_nearest_node(const VLGraph *graph, VLCoord coord);

/*
 * Find the nearest node within a maximum distance.
 *
 * graph: the road network graph
 * coord: query coordinate
 * max_dist: maximum distance in meters
 *
 * Returns node index, or VL_INVALID_NODE if no node within range.
 */
uint32_t vl_graph_nearest_node_within(const VLGraph *graph, VLCoord coord, double max_dist);

/* ============================================================================
 * Graph Statistics
 * ============================================================================ */

/*
 * Get graph statistics.
 *
 * graph: the road network graph
 * num_nodes: (out) number of nodes
 * num_edges: (out) number of edges
 * max_out_degree: (out) maximum out-degree
 * avg_out_degree: (out) average out-degree
 *
 * Any output parameter can be NULL if not needed.
 */
void vl_graph_stats(const VLGraph *graph, uint32_t *num_nodes, uint32_t *num_edges,
                    uint32_t *max_out_degree, double *avg_out_degree);

#ifdef __cplusplus
}
#endif

#endif /* VL_GRAPH_H */
