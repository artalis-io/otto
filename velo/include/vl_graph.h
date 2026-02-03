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
 * Degree-2 Node Contraction
 * ============================================================================ */

/*
 * Contract degree-2 nodes to reduce graph size.
 * Chains like A -> B -> C -> D (where B, C have degree 2) become A -> D.
 * Intermediate nodes are stored for path reconstruction.
 *
 * This is an optional optimization that reduces node count by 40-60%
 * on typical road networks, speeding up routing queries.
 *
 * graph: graph to contract (modified in place)
 *
 * Returns VL_OK on success.
 */
VLStatus vl_graph_contract_degree2(VLGraph *graph);

/*
 * Unpack a contracted path to include intermediate nodes.
 * Call this after routing to get the full path through original nodes.
 *
 * graph: the contracted graph
 * node_indices: array of node indices from route result
 * num_nodes: number of nodes in the path
 * out_indices: (out) unpacked node indices (caller must free)
 * out_num_nodes: (out) number of nodes in unpacked path
 *
 * Returns VL_OK on success.
 */
VLStatus vl_graph_unpack_path(const VLGraph *graph,
                               const uint32_t *node_indices, int num_nodes,
                               uint32_t **out_indices, int *out_num_nodes);

/* ============================================================================
 * Cache-Friendly Layout (Hilbert Curve Ordering)
 * ============================================================================ */

/*
 * Reorder graph nodes using Hilbert curve for cache locality.
 * Geographically close nodes will be stored contiguously in memory,
 * improving cache performance during routing queries.
 *
 * NOTE: This is experimental and may have bugs with some graph types.
 * Use with freshly loaded PBF graphs, not binary (.vlg) files.
 *
 * graph: graph to reorder (modified in place)
 *
 * Returns VL_OK on success.
 */
VLStatus vl_graph_reorder_hilbert(VLGraph *graph);

/* ============================================================================
 * Graph Validation
 * ============================================================================ */

/*
 * Validate graph integrity and edge weights.
 * Checks:
 *   - All edge targets are within valid node range
 *   - Edge weights are reasonable (not 0 for non-self-loops, not saturated)
 *   - Node edge_start/edge_count are consistent
 *
 * graph: graph to validate
 * out_errors: (out) number of errors found
 *
 * Returns VL_OK if valid, VL_ERROR_INVALID_ARGUMENT if issues found.
 */
VLStatus vl_graph_validate(const VLGraph *graph, int *out_errors);

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
