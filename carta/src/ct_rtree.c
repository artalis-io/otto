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

/*
 * Compute Hilbert index for a way's centroid.
 */
static uint32_t way_hilbert_index(const CTOSMWay *way, const CTBBox *data_bbox)
{
    /* Compute centroid */
    double cx = 0, cy = 0;
    for (int i = 0; i < way->num_coords; i++) {
        cx += way->coords[i].lon;
        cy += way->coords[i].lat;
    }
    cx /= way->num_coords;
    cy /= way->num_coords;

    /* Normalize to [0, HILBERT_N) */
    double lon_range = data_bbox->max_lon - data_bbox->min_lon;
    double lat_range = data_bbox->max_lat - data_bbox->min_lat;

    if (lon_range < 1e-9) lon_range = 1e-9;
    if (lat_range < 1e-9) lat_range = 1e-9;

    uint32_t hx = (uint32_t)(((cx - data_bbox->min_lon) / lon_range) * (HILBERT_N - 1));
    uint32_t hy = (uint32_t)(((cy - data_bbox->min_lat) / lat_range) * (HILBERT_N - 1));

    if (hx >= HILBERT_N) hx = HILBERT_N - 1;
    if (hy >= HILBERT_N) hy = HILBERT_N - 1;

    return xy_to_hilbert(hx, hy, HILBERT_ORDER);
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
 * Node Allocation
 * ============================================================================ */

static CTRTreeNode *alloc_node(void)
{
    CTRTreeNode *node = calloc(1, sizeof(CTRTreeNode));
    return node;
}

static void free_node(CTRTreeNode *node)
{
    if (!node) return;

    if (!node->is_leaf) {
        for (int i = 0; i < node->count; i++) {
            free_node(node->children[i]);
        }
    }
    free(node);
}

/* ============================================================================
 * Sort-Tile-Recursive Packing
 * ============================================================================
 *
 * Build tree bottom-up:
 * 1. Sort entries by Hilbert index
 * 2. Group into leaves of size NODE_CAPACITY
 * 3. Recursively build parent level from leaf bboxes
 */

static CTRTreeNode *build_level(CTSortEntry *entries, size_t count, int is_leaf)
{
    if (count == 0) return NULL;

    /* Single node needed */
    if (count <= CT_RTREE_NODE_CAPACITY) {
        CTRTreeNode *node = alloc_node();
        if (!node) return NULL;

        node->is_leaf = is_leaf;
        node->count = (int)count;
        node->bbox = entries[0].bbox;

        for (size_t i = 0; i < count; i++) {
            if (is_leaf) {
                node->way_indices[i] = entries[i].index;
            }
            if (i > 0) {
                node->bbox = bbox_union(node->bbox, entries[i].bbox);
            }
        }

        return node;
    }

    /* Multiple nodes needed - create leaves and recurse */
    size_t num_nodes = (count + CT_RTREE_NODE_CAPACITY - 1) / CT_RTREE_NODE_CAPACITY;
    CTSortEntry *parent_entries = malloc(num_nodes * sizeof(CTSortEntry));
    CTRTreeNode **children = malloc(num_nodes * sizeof(CTRTreeNode *));

    if (!parent_entries || !children) {
        free(parent_entries);
        free(children);
        return NULL;
    }

    /* Create leaf/internal nodes */
    for (size_t i = 0; i < num_nodes; i++) {
        size_t start = i * CT_RTREE_NODE_CAPACITY;
        size_t end = start + CT_RTREE_NODE_CAPACITY;
        if (end > count) end = count;
        size_t node_count = end - start;

        CTRTreeNode *node = alloc_node();
        if (!node) {
            for (size_t j = 0; j < i; j++) free_node(children[j]);
            free(parent_entries);
            free(children);
            return NULL;
        }

        node->is_leaf = is_leaf;
        node->count = (int)node_count;
        node->bbox = entries[start].bbox;

        for (size_t j = 0; j < node_count; j++) {
            if (is_leaf) {
                node->way_indices[j] = entries[start + j].index;
            }
            if (j > 0) {
                node->bbox = bbox_union(node->bbox, entries[start + j].bbox);
            }
        }

        children[i] = node;
        parent_entries[i].index = (uint32_t)i;
        parent_entries[i].bbox = node->bbox;

        /* Compute Hilbert for parent (use centroid of bbox) */
        double cx = (node->bbox.min_lon + node->bbox.max_lon) / 2;
        double cy = (node->bbox.min_lat + node->bbox.max_lat) / 2;

        /* Normalize - use first entry's bbox as reference for now */
        CTBBox ref = entries[0].bbox;
        for (size_t j = 1; j < count; j++) {
            ref = bbox_union(ref, entries[j].bbox);
        }

        double lon_range = ref.max_lon - ref.min_lon;
        double lat_range = ref.max_lat - ref.min_lat;
        if (lon_range < 1e-9) lon_range = 1e-9;
        if (lat_range < 1e-9) lat_range = 1e-9;

        uint32_t hx = (uint32_t)(((cx - ref.min_lon) / lon_range) * (HILBERT_N - 1));
        uint32_t hy = (uint32_t)(((cy - ref.min_lat) / lat_range) * (HILBERT_N - 1));
        if (hx >= HILBERT_N) hx = HILBERT_N - 1;
        if (hy >= HILBERT_N) hy = HILBERT_N - 1;

        parent_entries[i].hilbert = xy_to_hilbert(hx, hy, HILBERT_ORDER);
    }

    /* Sort parent entries by Hilbert index */
    qsort(parent_entries, num_nodes, sizeof(CTSortEntry), compare_hilbert);

    /* Reorder children array to match sorted order */
    CTRTreeNode **sorted_children = malloc(num_nodes * sizeof(CTRTreeNode *));
    if (!sorted_children) {
        for (size_t i = 0; i < num_nodes; i++) free_node(children[i]);
        free(parent_entries);
        free(children);
        return NULL;
    }

    for (size_t i = 0; i < num_nodes; i++) {
        sorted_children[i] = children[parent_entries[i].index];
        parent_entries[i].index = (uint32_t)i;  /* Update for recursive call */
    }
    free(children);

    /* Recursively build parent level */
    CTRTreeNode *parent = build_level(parent_entries, num_nodes, 0);

    /* Attach children to parent nodes */
    if (parent) {
        /* Walk tree and attach sorted_children */
        /* For simple case (single parent node), attach directly */
        if (num_nodes <= CT_RTREE_NODE_CAPACITY) {
            for (size_t i = 0; i < num_nodes; i++) {
                parent->children[i] = sorted_children[i];
            }
        } else {
            /* Multiple parent nodes - need to walk and attach */
            /* This is handled by the recursive structure */
            size_t child_idx = 0;
            CTRTreeNode *stack[64];
            int stack_depth = 0;
            stack[stack_depth++] = parent;

            while (stack_depth > 0) {
                CTRTreeNode *node = stack[--stack_depth];
                if (node->is_leaf) {
                    /* This is actually an internal node at the level above leaves */
                    /* Attach children */
                    for (int i = 0; i < node->count && child_idx < num_nodes; i++) {
                        node->children[i] = sorted_children[child_idx++];
                    }
                    node->is_leaf = 0;  /* It's now internal */
                } else {
                    /* Add children to stack in reverse order */
                    for (int i = node->count - 1; i >= 0; i--) {
                        if (node->children[i]) {
                            stack[stack_depth++] = node->children[i];
                        }
                    }
                }
            }
        }
    }

    free(parent_entries);
    free(sorted_children);

    return parent;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

CTRTree *ct_rtree_build(const CTOSMWay *ways, size_t num_ways, CTBBox data_bbox)
{
    if (!ways || num_ways == 0) return NULL;

    CTRTree *tree = calloc(1, sizeof(CTRTree));
    if (!tree) return NULL;

    /* Create sort entries with Hilbert indices */
    CTSortEntry *entries = malloc(num_ways * sizeof(CTSortEntry));
    if (!entries) {
        free(tree);
        return NULL;
    }

    for (size_t i = 0; i < num_ways; i++) {
        entries[i].index = (uint32_t)i;
        entries[i].hilbert = way_hilbert_index(&ways[i], &data_bbox);
        entries[i].bbox = way_bbox(&ways[i]);
    }

    /* Sort by Hilbert index */
    qsort(entries, num_ways, sizeof(CTSortEntry), compare_hilbert);

    /* Build tree bottom-up */
    tree->root = build_level(entries, num_ways, 1);
    tree->num_entries = num_ways;

    free(entries);

    if (!tree->root) {
        free(tree);
        return NULL;
    }

    return tree;
}

void ct_rtree_free(CTRTree *tree)
{
    if (!tree) return;
    free_node(tree->root);
    free(tree);
}

/* ============================================================================
 * Query
 * ============================================================================ */

static void query_node(const CTRTreeNode *node, CTBBox bbox,
                       uint32_t *results, size_t *count, size_t max_results)
{
    if (!node || *count >= max_results) return;

    /* Check if node bbox intersects query bbox */
    if (!bbox_intersects(node->bbox, bbox)) return;

    if (node->is_leaf) {
        /* Add all entries (they already passed parent bbox test) */
        for (int i = 0; i < node->count && *count < max_results; i++) {
            results[(*count)++] = node->way_indices[i];
        }
    } else {
        /* Recurse into children */
        for (int i = 0; i < node->count; i++) {
            query_node(node->children[i], bbox, results, count, max_results);
        }
    }
}

size_t ct_rtree_query(const CTRTree *tree, CTBBox bbox,
                      uint32_t *results, size_t max_results)
{
    if (!tree || !tree->root || !results) return 0;

    size_t count = 0;
    query_node(tree->root, bbox, results, &count, max_results);
    return count;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

static void count_nodes(const CTRTreeNode *node, size_t *num_nodes,
                        size_t *max_depth, size_t depth, size_t *total_entries)
{
    if (!node) return;

    (*num_nodes)++;
    if (depth > *max_depth) *max_depth = depth;

    if (node->is_leaf) {
        *total_entries += node->count;
    } else {
        for (int i = 0; i < node->count; i++) {
            count_nodes(node->children[i], num_nodes, max_depth, depth + 1, total_entries);
        }
    }
}

void ct_rtree_stats(const CTRTree *tree, size_t *num_nodes, size_t *height,
                    size_t *total_entries)
{
    size_t nodes = 0, max_depth = 0, entries = 0;

    if (tree && tree->root) {
        count_nodes(tree->root, &nodes, &max_depth, 0, &entries);
    }

    if (num_nodes) *num_nodes = nodes;
    if (height) *height = max_depth + 1;
    if (total_entries) *total_entries = entries;
}
