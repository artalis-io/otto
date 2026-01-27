/*
 * vl_landmarks.c - ALT algorithm (A* with Landmarks and Triangle inequality)
 *
 * Uses precomputed distances to landmark nodes for tighter lower bounds.
 * For landmarks L, the heuristic is:
 *   h(v, t) = max over all L of |dist(v, L) - dist(t, L)|
 *
 * This is admissible and often much tighter than haversine distance.
 */

#include "vl_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

double vl_haversine(VLCoord a, VLCoord b);

/* ============================================================================
 * Landmark Selection
 * ============================================================================ */

/*
 * Select landmarks using the "farthest" strategy:
 * 1. Pick a random starting node
 * 2. Repeatedly pick the node farthest from all current landmarks
 */
static void select_landmarks_farthest(const VLGraph *graph, int num_landmarks,
                                       uint32_t *landmarks)
{
    uint32_t num_nodes = graph->num_nodes;

    /* Start with a node near the center of the bounding box */
    double center_lat = (graph->bbox_min.lat + graph->bbox_max.lat) / 2;
    double center_lon = (graph->bbox_min.lon + graph->bbox_max.lon) / 2;
    VLCoord center = {center_lat, center_lon};

    /* Find node closest to center */
    double min_dist = 1e18;
    uint32_t first_landmark = 0;
    for (uint32_t i = 0; i < num_nodes; i++) {
        VLCoord c = VL_FIXED_TO_COORD(graph->nodes[i].coord);
        double d = vl_haversine(c, center);
        if (d < min_dist) {
            min_dist = d;
            first_landmark = i;
        }
    }
    landmarks[0] = first_landmark;

    /* Track minimum distance to any landmark for each node */
    double *min_to_landmark = malloc(num_nodes * sizeof(double));
    if (!min_to_landmark) return;

    for (uint32_t i = 0; i < num_nodes; i++) {
        VLCoord c = VL_FIXED_TO_COORD(graph->nodes[i].coord);
        VLCoord l = VL_FIXED_TO_COORD(graph->nodes[first_landmark].coord);
        min_to_landmark[i] = vl_haversine(c, l);
    }

    /* Iteratively select farthest node from current landmarks */
    for (int k = 1; k < num_landmarks; k++) {
        double max_dist = -1;
        uint32_t farthest = 0;

        for (uint32_t i = 0; i < num_nodes; i++) {
            if (min_to_landmark[i] > max_dist) {
                max_dist = min_to_landmark[i];
                farthest = i;
            }
        }

        landmarks[k] = farthest;

        /* Update min distances */
        VLCoord farthest_coord = VL_FIXED_TO_COORD(graph->nodes[farthest].coord);
        for (uint32_t i = 0; i < num_nodes; i++) {
            VLCoord c = VL_FIXED_TO_COORD(graph->nodes[i].coord);
            double d = vl_haversine(c, farthest_coord);
            if (d < min_to_landmark[i]) {
                min_to_landmark[i] = d;
            }
        }
    }

    free(min_to_landmark);
}

/*
 * Select landmarks at corners of the bounding box.
 */
static void select_landmarks_planar(const VLGraph *graph, int num_landmarks,
                                     uint32_t *landmarks)
{
    /* Define target locations at corners and edges of bounding box */
    double lat_min = graph->bbox_min.lat;
    double lat_max = graph->bbox_max.lat;
    double lon_min = graph->bbox_min.lon;
    double lon_max = graph->bbox_max.lon;
    double lat_mid = (lat_min + lat_max) / 2;
    double lon_mid = (lon_min + lon_max) / 2;

    VLCoord targets[16] = {
        {lat_min, lon_min},  /* SW corner */
        {lat_max, lon_min},  /* NW corner */
        {lat_max, lon_max},  /* NE corner */
        {lat_min, lon_max},  /* SE corner */
        {lat_mid, lon_min},  /* W edge */
        {lat_mid, lon_max},  /* E edge */
        {lat_min, lon_mid},  /* S edge */
        {lat_max, lon_mid},  /* N edge */
        {lat_min + (lat_max - lat_min) * 0.25, lon_min + (lon_max - lon_min) * 0.25},
        {lat_min + (lat_max - lat_min) * 0.75, lon_min + (lon_max - lon_min) * 0.25},
        {lat_min + (lat_max - lat_min) * 0.25, lon_min + (lon_max - lon_min) * 0.75},
        {lat_min + (lat_max - lat_min) * 0.75, lon_min + (lon_max - lon_min) * 0.75},
        {lat_mid, lon_mid},  /* Center */
        {lat_min + (lat_max - lat_min) * 0.33, lon_mid},
        {lat_min + (lat_max - lat_min) * 0.67, lon_mid},
        {lat_mid, lon_min + (lon_max - lon_min) * 0.33},
    };

    /* Find nearest node to each target location */
    for (int k = 0; k < num_landmarks && k < 16; k++) {
        double min_dist = 1e18;
        uint32_t nearest = 0;

        for (uint32_t i = 0; i < graph->num_nodes; i++) {
            VLCoord c = VL_FIXED_TO_COORD(graph->nodes[i].coord);
            double d = vl_haversine(c, targets[k]);
            if (d < min_dist) {
                min_dist = d;
                nearest = i;
            }
        }

        landmarks[k] = nearest;
    }
}

/* ============================================================================
 * Heap-based Dijkstra for Landmark Distance Computation
 * ============================================================================ */

/* Forward declarations for heap functions */
VLHeap *vl_heap_create(size_t num_nodes);
void vl_heap_free(VLHeap *heap);
void vl_heap_clear(VLHeap *heap);
int vl_heap_empty(const VLHeap *heap);
VLStatus vl_heap_push(VLHeap *heap, uint32_t node, double priority);
VLStatus vl_heap_pop(VLHeap *heap, VLHeapEntry *entry);

/*
 * Run Dijkstra from a single source to all nodes, storing distances.
 * Uses heap-based O((V+E) log V) algorithm.
 */
static void dijkstra_single_source(const VLGraph *graph, uint32_t source,
                                   double *distances, int use_duration)
{
    uint32_t num_nodes = graph->num_nodes;

    /* Initialize distances */
    for (uint32_t i = 0; i < num_nodes; i++) {
        distances[i] = VL_INF;
    }
    distances[source] = 0;

    /* Create heap for priority queue */
    VLHeap *heap = vl_heap_create(num_nodes);
    if (!heap) return;

    vl_heap_push(heap, source, 0);

    while (!vl_heap_empty(heap)) {
        VLHeapEntry entry;
        vl_heap_pop(heap, &entry);
        uint32_t u = entry.node;
        double dist_u = entry.priority;

        /* Skip if we already found a shorter path */
        if (dist_u > distances[u]) continue;

        /* Relax edges */
        const VLNode *node = &graph->nodes[u];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            const VLEdge *edge = &graph->edges[node->edge_start + e];
            uint32_t v = edge->target;

            double w = use_duration ? (edge->duration * 0.1) : (edge->distance * 0.001);
            double new_dist = dist_u + w;

            if (new_dist < distances[v]) {
                distances[v] = new_dist;
                vl_heap_push(heap, v, new_dist);
            }
        }
    }

    vl_heap_free(heap);
}

/*
 * Run Dijkstra from all nodes to a single target (reverse graph).
 * Uses heap-based O((V+E) log V) algorithm.
 */
static void dijkstra_single_target(const VLGraph *graph, uint32_t target,
                                   double *distances, int use_duration)
{
    if (!graph->rev_edge_start) {
        /* No reverse index - fall back to forward Dijkstra */
        dijkstra_single_source(graph, target, distances, use_duration);
        return;
    }

    uint32_t num_nodes = graph->num_nodes;

    for (uint32_t i = 0; i < num_nodes; i++) {
        distances[i] = VL_INF;
    }
    distances[target] = 0;

    VLHeap *heap = vl_heap_create(num_nodes);
    if (!heap) return;

    vl_heap_push(heap, target, 0);

    while (!vl_heap_empty(heap)) {
        VLHeapEntry entry;
        vl_heap_pop(heap, &entry);
        uint32_t u = entry.node;
        double dist_u = entry.priority;

        if (dist_u > distances[u]) continue;

        /* Relax reverse edges (edges coming INTO u) */
        uint32_t rev_start = graph->rev_edge_start[u];
        uint32_t rev_count = graph->rev_edge_count[u];

        for (uint32_t r = 0; r < rev_count; r++) {
            uint32_t v = graph->rev_edges[rev_start + r];
            uint32_t edge_idx = graph->rev_edge_idx[rev_start + r];
            const VLEdge *edge = &graph->edges[edge_idx];

            double w = use_duration ? (edge->duration * 0.1) : (edge->distance * 0.001);
            double new_dist = dist_u + w;

            if (new_dist < distances[v]) {
                distances[v] = new_dist;
                vl_heap_push(heap, v, new_dist);
            }
        }
    }

    vl_heap_free(heap);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

VLLandmarks *vl_landmarks_create(const VLGraph *graph, int num_landmarks)
{
    if (!graph || num_landmarks <= 0 || num_landmarks > VL_MAX_LANDMARKS) {
        return NULL;
    }

    VLLandmarks *lm = calloc(1, sizeof(VLLandmarks));
    if (!lm) return NULL;

    lm->num_landmarks = num_landmarks;
    lm->num_nodes = graph->num_nodes;

    /* Allocate landmark node array */
    lm->landmark_nodes = malloc(num_landmarks * sizeof(uint32_t));
    if (!lm->landmark_nodes) {
        free(lm);
        return NULL;
    }

    /* Allocate distance arrays */
    size_t dist_size = (size_t)graph->num_nodes * num_landmarks * sizeof(double);
    lm->dist_to_landmark = malloc(dist_size);
    lm->dist_from_landmark = malloc(dist_size);

    if (!lm->dist_to_landmark || !lm->dist_from_landmark) {
        free(lm->landmark_nodes);
        free(lm->dist_to_landmark);
        free(lm->dist_from_landmark);
        free(lm);
        return NULL;
    }

    printf("velo: Selecting %d landmarks...\n", num_landmarks);

    /* Select landmarks using farthest strategy */
    select_landmarks_farthest(graph, num_landmarks, lm->landmark_nodes);

    printf("velo: Computing landmark distances...\n");

    /* Compute distances from each landmark to all nodes (and vice versa) */
    #pragma omp parallel for schedule(dynamic)
    for (int k = 0; k < num_landmarks; k++) {
        uint32_t landmark = lm->landmark_nodes[k];
        double *dist_to = lm->dist_to_landmark + (size_t)k * graph->num_nodes;
        double *dist_from = lm->dist_from_landmark + (size_t)k * graph->num_nodes;

        /* Distance from landmark to all nodes */
        dijkstra_single_source(graph, landmark, dist_from, 0);

        /* Distance from all nodes to landmark */
        dijkstra_single_target(graph, landmark, dist_to, 0);

        #pragma omp critical
        {
            printf("velo: Landmark %d/%d computed (node %u)\n",
                   k + 1, num_landmarks, landmark);
        }
    }

    printf("velo: Landmarks ready (%d landmarks, %.1f MB)\n",
           num_landmarks, dist_size * 2.0 / (1024 * 1024));

    return lm;
}

void vl_landmarks_free(VLLandmarks *lm)
{
    if (!lm) return;
    free(lm->landmark_nodes);
    free(lm->dist_to_landmark);
    free(lm->dist_from_landmark);
    free(lm);
}

/*
 * Compute ALT heuristic: lower bound on distance from 'from' to 'to'.
 * Uses triangle inequality with all landmarks.
 *
 * For each landmark L:
 *   - dist(from, L) - dist(to, L) is a lower bound on dist(from, to)
 *   - dist(L, to) - dist(L, from) is also a lower bound
 *
 * Take the maximum across all landmarks. Negative values are valid
 * (they just mean that landmark doesn't help for this query).
 * Do NOT take absolute value - that would make the heuristic inadmissible.
 */
double vl_landmarks_heuristic(const VLLandmarks *lm, uint32_t from, uint32_t to)
{
    if (!lm) return 0;

    double max_bound = 0;

    for (int k = 0; k < lm->num_landmarks; k++) {
        size_t offset = (size_t)k * lm->num_nodes;

        /* Lower bound: dist(from, L) - dist(to, L) */
        double d1 = lm->dist_to_landmark[offset + from];
        double d2 = lm->dist_to_landmark[offset + to];
        double bound1 = d1 - d2;

        /* Lower bound: dist(L, to) - dist(L, from) */
        double d3 = lm->dist_from_landmark[offset + to];
        double d4 = lm->dist_from_landmark[offset + from];
        double bound2 = d3 - d4;

        /* Take max of both bounds from this landmark */
        double bound = (bound1 > bound2) ? bound1 : bound2;
        if (bound > max_bound) {
            max_bound = bound;
        }
    }

    return max_bound;
}

/*
 * Get distance from a node to a landmark (for debugging/analysis).
 */
double vl_landmarks_dist_to(const VLLandmarks *lm, uint32_t node, int landmark_idx)
{
    if (!lm || landmark_idx < 0 || landmark_idx >= lm->num_landmarks) {
        return VL_INF;
    }
    return lm->dist_to_landmark[(size_t)landmark_idx * lm->num_nodes + node];
}

double vl_landmarks_dist_from(const VLLandmarks *lm, uint32_t node, int landmark_idx)
{
    if (!lm || landmark_idx < 0 || landmark_idx >= lm->num_landmarks) {
        return VL_INF;
    }
    return lm->dist_from_landmark[(size_t)landmark_idx * lm->num_nodes + node];
}
