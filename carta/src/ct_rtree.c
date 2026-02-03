/*
 * ct_rtree.c - R-Tree spatial index implementation
 *
 * Uses Hilbert curve packing (Sort-Tile-Recursive) for optimal query performance.
 * Build time: O(n log n), Query time: O(log n + k) where k = results.
 */

#include "ct_rtree.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Hilbert Curve Index
 * ============================================================================
 *
 * Maps 2D coordinates to 1D Hilbert index for spatial locality.
 * Using 16-bit resolution (65536 x 65536 grid).
 */

#define HILBERT_ORDER 16
#define HILBERT_N (1 << HILBERT_ORDER)

/*
 * Convert (x, y) in [0, n) to Hilbert curve index.
 * Based on the algorithm from "Hacker's Delight" by Henry Warren.
 */
static uint32_t xy_to_hilbert(uint32_t x, uint32_t y, int order)
{
    uint32_t rx, ry, s, d = 0;
    uint32_t n = 1U << order;

    for (s = n / 2; s > 0; s /= 2) {
        rx = (x & s) > 0;
        ry = (y & s) > 0;
        d += s * s * ((3 * rx) ^ ry);

        /* Rotate quadrant */
        if (ry == 0) {
            if (rx == 1) {
                x = n - 1 - x;
                y = n - 1 - y;
            }
            uint32_t t = x;
            x = y;
            y = t;
        }
    }
    return d;
}

/* ============================================================================
 * Bounding Box Utilities
 * ============================================================================ */

static CTBBox way_bbox(const CTOSMWay *way)
{
    CTBBox bb = {
        .min_lat = way->coords[0].lat,
        .max_lat = way->coords[0].lat,
        .min_lon = way->coords[0].lon,
        .max_lon = way->coords[0].lon
    };

    for (int i = 1; i < way->num_coords; i++) {
        if (way->coords[i].lat < bb.min_lat) bb.min_lat = way->coords[i].lat;
        if (way->coords[i].lat > bb.max_lat) bb.max_lat = way->coords[i].lat;
        if (way->coords[i].lon < bb.min_lon) bb.min_lon = way->coords[i].lon;
        if (way->coords[i].lon > bb.max_lon) bb.max_lon = way->coords[i].lon;
    }

    return bb;
}

static CTBBox bbox_union(CTBBox a, CTBBox b)
{
    CTBBox u = {
        .min_lat = a.min_lat < b.min_lat ? a.min_lat : b.min_lat,
        .max_lat = a.max_lat > b.max_lat ? a.max_lat : b.max_lat,
        .min_lon = a.min_lon < b.min_lon ? a.min_lon : b.min_lon,
        .max_lon = a.max_lon > b.max_lon ? a.max_lon : b.max_lon
    };
    return u;
}

static int bbox_intersects(CTBBox a, CTBBox b)
{
    return !(a.max_lon < b.min_lon || a.min_lon > b.max_lon ||
             a.max_lat < b.min_lat || a.min_lat > b.max_lat);
}

/* ============================================================================
 * Sorting for Hilbert Packing
 * ============================================================================ */

typedef struct {
    uint32_t index;
    uint32_t hilbert;
    CTBBox bbox;
} CTSortEntry;

static int compare_hilbert(const void *a, const void *b)
{
    const CTSortEntry *ea = (const CTSortEntry *)a;
    const CTSortEntry *eb = (const CTSortEntry *)b;
    if (ea->hilbert < eb->hilbert) return -1;
    if (ea->hilbert > eb->hilbert) return 1;
    return 0;
}

/* ============================================================================
 * Packed R-Tree Structure
 * ============================================================================
 *
 * Instead of individual node allocations, we use flat arrays:
 * - One array for leaf level (way indices + bboxes)
 * - One array for each internal level (child indices + bboxes)
 *
 * This is much faster to build and more cache-friendly for queries.
 * CTPackedNode is defined in ct_types.h
 */

/* ============================================================================
 * Sort-Tile-Recursive Bulk Loading
 * ============================================================================
 *
 * Algorithm:
 * 1. Compute Hilbert index and bbox for each way
 * 2. Sort by Hilbert index
 * 3. Build leaf level: group into nodes of NODE_CAPACITY
 * 4. Build each internal level from the previous level
 * 5. Repeat until we have a single root node
 */

CTRTree *ct_rtree_build(const CTOSMWay *ways, size_t num_ways, CTBBox data_bbox)
{
    if (!ways || num_ways == 0) return NULL;

    /* Count classified ways (skip CT_OSM_UNKNOWN - those are geometry-only for relations) */
    size_t classified_count = 0;
    for (size_t i = 0; i < num_ways; i++) {
        if (ways[i].feature_class != CT_OSM_UNKNOWN) {
            classified_count++;
        }
    }

    if (classified_count == 0) return NULL;

    CTRTree *tree = calloc(1, sizeof(CTRTree));
    if (!tree) return NULL;

    /* Step 1: Compute Hilbert indices and bboxes for classified ways only */
    CTSortEntry *entries = malloc(classified_count * sizeof(CTSortEntry));
    if (!entries) {
        free(tree);
        return NULL;
    }

    double lon_range = data_bbox.max_lon - data_bbox.min_lon;
    double lat_range = data_bbox.max_lat - data_bbox.min_lat;
    if (lon_range < 1e-9) lon_range = 1e-9;
    if (lat_range < 1e-9) lat_range = 1e-9;

    size_t e_idx = 0;
    for (size_t i = 0; i < num_ways; i++) {
        /* Skip unclassified ways (geometry-only, kept for multipolygon assembly) */
        if (ways[i].feature_class == CT_OSM_UNKNOWN) continue;

        entries[e_idx].index = (uint32_t)i;
        entries[e_idx].bbox = way_bbox(&ways[i]);

        /* Compute centroid and Hilbert index */
        double cx = (entries[e_idx].bbox.min_lon + entries[e_idx].bbox.max_lon) / 2;
        double cy = (entries[e_idx].bbox.min_lat + entries[e_idx].bbox.max_lat) / 2;

        uint32_t hx = (uint32_t)(((cx - data_bbox.min_lon) / lon_range) * (HILBERT_N - 1));
        uint32_t hy = (uint32_t)(((cy - data_bbox.min_lat) / lat_range) * (HILBERT_N - 1));
        if (hx >= HILBERT_N) hx = HILBERT_N - 1;
        if (hy >= HILBERT_N) hy = HILBERT_N - 1;

        entries[e_idx].hilbert = xy_to_hilbert(hx, hy, HILBERT_ORDER);
        e_idx++;
    }

    num_ways = classified_count;  /* Use filtered count for tree building */

    /* Step 2: Sort by Hilbert index */
    qsort(entries, num_ways, sizeof(CTSortEntry), compare_hilbert);

    /* Step 3: Calculate tree structure sizes */
    size_t num_leaves = (num_ways + CT_RTREE_NODE_CAPACITY - 1) / CT_RTREE_NODE_CAPACITY;

    /* Count nodes at each level */
    size_t level_sizes[32];
    int num_levels = 0;
    size_t n = num_leaves;
    while (n > 0) {
        level_sizes[num_levels++] = n;
        if (n == 1) break;
        n = (n + CT_RTREE_NODE_CAPACITY - 1) / CT_RTREE_NODE_CAPACITY;
    }

    /* Total nodes needed */
    size_t total_nodes = 0;
    for (int i = 0; i < num_levels; i++) {
        total_nodes += level_sizes[i];
    }

    /* Allocate all nodes at once */
    CTPackedNode *nodes = calloc(total_nodes, sizeof(CTPackedNode));
    uint32_t *leaf_indices = malloc(num_ways * sizeof(uint32_t));
    if (!nodes || !leaf_indices) {
        free(entries);
        free(nodes);
        free(leaf_indices);
        free(tree);
        return NULL;
    }

    /* Step 4: Build leaf level */
    size_t node_idx = 0;
    size_t entry_idx = 0;

    for (size_t i = 0; i < num_leaves; i++) {
        CTPackedNode *node = &nodes[node_idx++];
        node->is_leaf = 1;
        node->first_child = (uint32_t)entry_idx;

        /* Compute how many entries in this leaf */
        size_t remaining = num_ways - entry_idx;
        size_t count = remaining < CT_RTREE_NODE_CAPACITY ? remaining : CT_RTREE_NODE_CAPACITY;
        node->num_children = (uint16_t)count;

        /* Copy indices and compute bbox */
        node->bbox = entries[entry_idx].bbox;
        leaf_indices[entry_idx] = entries[entry_idx].index;

        for (size_t j = 1; j < count; j++) {
            leaf_indices[entry_idx + j] = entries[entry_idx + j].index;
            node->bbox = bbox_union(node->bbox, entries[entry_idx + j].bbox);
        }

        entry_idx += count;
    }

    /* Step 5: Build internal levels */
    size_t prev_level_start = 0;
    size_t prev_level_count = num_leaves;

    for (int level = 1; level < num_levels; level++) {
        size_t this_level_count = level_sizes[level];
        size_t child_idx = 0;

        for (size_t i = 0; i < this_level_count; i++) {
            CTPackedNode *node = &nodes[node_idx++];
            node->is_leaf = 0;
            node->first_child = (uint32_t)(prev_level_start + child_idx);

            /* Compute how many children in this node */
            size_t remaining = prev_level_count - child_idx;
            size_t count = remaining < CT_RTREE_NODE_CAPACITY ? remaining : CT_RTREE_NODE_CAPACITY;
            node->num_children = (uint16_t)count;

            /* Compute bbox from children */
            node->bbox = nodes[prev_level_start + child_idx].bbox;
            for (size_t j = 1; j < count; j++) {
                node->bbox = bbox_union(node->bbox, nodes[prev_level_start + child_idx + j].bbox);
            }

            child_idx += count;
        }

        prev_level_start += prev_level_count;
        prev_level_count = this_level_count;
    }

    /* Store in tree structure */
    tree->nodes = nodes;
    tree->leaf_indices = leaf_indices;
    tree->num_nodes = total_nodes;
    tree->num_entries = num_ways;
    tree->root_idx = total_nodes - 1;  /* Root is the last node */

    free(entries);

    return tree;
}

void ct_rtree_free(CTRTree *tree)
{
    if (!tree) return;
    free(tree->nodes);
    free(tree->leaf_indices);
    free(tree);
}

/* ============================================================================
 * Query
 * ============================================================================ */

static void query_node_packed(const CTRTree *tree, uint32_t node_idx, CTBBox bbox,
                              uint32_t *results, size_t *count, size_t max_results)
{
    if (*count >= max_results) return;

    const CTPackedNode *node = &tree->nodes[node_idx];

    /* Check if node bbox intersects query bbox */
    if (!bbox_intersects(node->bbox, bbox)) return;

    if (node->is_leaf) {
        /* Add all entries from this leaf */
        for (uint16_t i = 0; i < node->num_children && *count < max_results; i++) {
            results[(*count)++] = tree->leaf_indices[node->first_child + i];
        }
    } else {
        /* Recurse into children */
        for (uint16_t i = 0; i < node->num_children; i++) {
            query_node_packed(tree, node->first_child + i, bbox, results, count, max_results);
        }
    }
}

size_t ct_rtree_query(const CTRTree *tree, CTBBox bbox,
                      uint32_t *results, size_t max_results)
{
    if (!tree || !tree->nodes || !results) return 0;

    size_t count = 0;
    query_node_packed(tree, tree->root_idx, bbox, results, &count, max_results);
    return count;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void ct_rtree_stats(const CTRTree *tree, size_t *num_nodes, size_t *height,
                    size_t *total_entries)
{
    if (!tree) {
        if (num_nodes) *num_nodes = 0;
        if (height) *height = 0;
        if (total_entries) *total_entries = 0;
        return;
    }

    if (num_nodes) *num_nodes = tree->num_nodes;
    if (total_entries) *total_entries = tree->num_entries;

    /* Calculate height by walking from root */
    if (height) {
        size_t h = 0;
        uint32_t idx = tree->root_idx;
        while (idx < tree->num_nodes) {
            h++;
            if (tree->nodes[idx].is_leaf) break;
            idx = tree->nodes[idx].first_child;
        }
        *height = h;
    }
}

/* ============================================================================
 * Legacy API Compatibility
 * ============================================================================
 *
 * The old API used CTRTreeNode pointers. We provide a shim for ct_pbf.c
 */

/* For legacy code that checks tree->root */
CTRTreeNode *ct_rtree_get_root(const CTRTree *tree)
{
    (void)tree;
    /* Return non-NULL to indicate tree is valid */
    return (CTRTreeNode *)(tree ? (void *)1 : NULL);
}
