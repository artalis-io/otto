/*
 * ct_rtree.h - R-Tree spatial index for map features
 *
 * Packed Hilbert R-Tree for O(log n) spatial queries.
 * Built once after PBF parsing, used for all tile generation.
 */

#ifndef CT_RTREE_H
#define CT_RTREE_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * R-Tree Configuration
 * ============================================================================ */

#define CT_RTREE_NODE_CAPACITY 16   /* Max children per node */
#define CT_RTREE_MIN_ENTRIES   4    /* Min children per node (except root) */

/* ============================================================================
 * R-Tree Node Structure
 * ============================================================================ */

struct CTRTreeNode {
    CTBBox bbox;                    /* Bounding box of this node and all children */
    int is_leaf;                    /* 1 if leaf node, 0 if internal */
    int count;                      /* Number of children/entries */
    union {
        struct CTRTreeNode *children[CT_RTREE_NODE_CAPACITY];  /* Internal: child nodes */
        uint32_t way_indices[CT_RTREE_NODE_CAPACITY];          /* Leaf: indices into ways array */
    };
};

/* ============================================================================
 * R-Tree API
 * ============================================================================ */

/*
 * Build R-Tree from ways array using Hilbert Sort-Tile-Recursive packing.
 * This is an O(n log n) operation, called once after PBF parsing.
 *
 * @param ways      Array of parsed OSM ways
 * @param num_ways  Number of ways
 * @param bbox      Bounding box of all data (for Hilbert normalization)
 * @return          Allocated R-Tree, or NULL on failure
 */
CTRTree *ct_rtree_build(const CTOSMWay *ways, size_t num_ways, CTBBox bbox);

/*
 * Free R-Tree and all nodes.
 */
void ct_rtree_free(CTRTree *tree);

/*
 * Query R-Tree for all ways intersecting a bounding box.
 *
 * @param tree         R-Tree to query
 * @param bbox         Query bounding box
 * @param results      Output array for way indices (caller-allocated)
 * @param max_results  Maximum results to return
 * @return             Number of results found
 */
size_t ct_rtree_query(const CTRTree *tree, CTBBox bbox,
                      uint32_t *results, size_t max_results);

/*
 * Get R-Tree statistics for debugging/profiling.
 */
void ct_rtree_stats(const CTRTree *tree, size_t *num_nodes, size_t *height,
                    size_t *total_entries);

#ifdef __cplusplus
}
#endif

#endif /* CT_RTREE_H */
