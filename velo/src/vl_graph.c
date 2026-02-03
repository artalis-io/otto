/*
 * vl_graph.c - Graph construction and storage
 *
 * Builds a CSR (Compressed Sparse Row) graph from parsed OSM data.
 * Includes reverse graph index for bidirectional search and
 * grid spatial index for fast nearest-node queries.
 *
 * OpenMP is used for parallel construction of indexes.
 */

#include "vl_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* Forward declarations */
double vl_haversine(VLCoord a, VLCoord b);
int vl_default_speed(int road_type);
double vl_travel_time(double distance_m, double speed_kmh);
uint32_t vl_graph_nearest_node(const VLGraph *graph, VLCoord coord);

/* ============================================================================
 * Node Map (Hash Table: OSM ID -> Node Index)
 * ============================================================================ */

#define NODE_MAP_LOAD_FACTOR 0.7

static uint64_t hash_osm_id(int64_t id)
{
    uint64_t h = (uint64_t)id;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static VLStatus node_map_init(VLNodeMap *map, size_t expected_size)
{
    map->num_buckets = (size_t)(expected_size / NODE_MAP_LOAD_FACTOR) + 1;
    if (map->num_buckets < 1024) map->num_buckets = 1024;

    map->buckets = calloc(map->num_buckets, sizeof(VLNodeMapEntry *));
    if (!map->buckets) return VL_ERROR_OUT_OF_MEMORY;

    map->num_entries = 0;
    return VL_OK;
}

static void node_map_free(VLNodeMap *map)
{
    if (!map->buckets) return;

    for (size_t i = 0; i < map->num_buckets; i++) {
        VLNodeMapEntry *entry = map->buckets[i];
        while (entry) {
            VLNodeMapEntry *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(map->buckets);
    map->buckets = NULL;
    map->num_buckets = 0;
    map->num_entries = 0;
}

static VLStatus node_map_insert(VLNodeMap *map, int64_t osm_id, uint32_t node_index)
{
    size_t bucket = hash_osm_id(osm_id) % map->num_buckets;

    VLNodeMapEntry *entry = map->buckets[bucket];
    while (entry) {
        if (entry->osm_id == osm_id) {
            return VL_OK;
        }
        entry = entry->next;
    }

    entry = malloc(sizeof(VLNodeMapEntry));
    if (!entry) return VL_ERROR_OUT_OF_MEMORY;

    entry->osm_id = osm_id;
    entry->node_index = node_index;
    entry->next = map->buckets[bucket];
    map->buckets[bucket] = entry;
    map->num_entries++;

    return VL_OK;
}

static uint32_t node_map_lookup(const VLNodeMap *map, int64_t osm_id)
{
    if (!map->buckets) return VL_INVALID_NODE;

    size_t bucket = hash_osm_id(osm_id) % map->num_buckets;
    VLNodeMapEntry *entry = map->buckets[bucket];

    while (entry) {
        if (entry->osm_id == osm_id) {
            return entry->node_index;
        }
        entry = entry->next;
    }

    return VL_INVALID_NODE;
}

/* ============================================================================
 * Graph Builder
 * ============================================================================ */

VLGraphBuilder *vl_graph_builder_create(size_t expected_nodes)
{
    VLGraphBuilder *builder = calloc(1, sizeof(VLGraphBuilder));
    if (!builder) return NULL;

    if (node_map_init(&builder->node_map, expected_nodes) != VL_OK) {
        free(builder);
        return NULL;
    }

    builder->nodes_capacity = expected_nodes > 0 ? expected_nodes : 65536;
    builder->nodes = malloc(builder->nodes_capacity * sizeof(VLNode));
    if (!builder->nodes) {
        node_map_free(&builder->node_map);
        free(builder);
        return NULL;
    }

    builder->temp_edges_capacity = expected_nodes * 2;
    if (builder->temp_edges_capacity < 65536) builder->temp_edges_capacity = 65536;
    builder->temp_edges = malloc(builder->temp_edges_capacity * sizeof(VLTempEdge));
    if (!builder->temp_edges) {
        free(builder->nodes);
        node_map_free(&builder->node_map);
        free(builder);
        return NULL;
    }

    return builder;
}

void vl_graph_builder_free(VLGraphBuilder *builder)
{
    if (!builder) return;
    node_map_free(&builder->node_map);
    free(builder->nodes);
    free(builder->temp_edges);
    free(builder);
}

static uint32_t get_or_create_node(VLGraphBuilder *builder, int64_t osm_id,
                                   double lat, double lon)
{
    /* Validate coordinates to prevent overflow in fixed-point conversion */
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return VL_INVALID_NODE;
    }

    uint32_t idx = node_map_lookup(&builder->node_map, osm_id);
    if (idx != VL_INVALID_NODE) {
        return idx;
    }

    if (builder->num_nodes >= builder->nodes_capacity) {
        size_t new_cap = builder->nodes_capacity * 2;
        VLNode *new_nodes = realloc(builder->nodes, new_cap * sizeof(VLNode));
        if (!new_nodes) return VL_INVALID_NODE;
        builder->nodes = new_nodes;
        builder->nodes_capacity = new_cap;
    }

    idx = (uint32_t)builder->num_nodes;
    VLNode *node = &builder->nodes[idx];
    node->osm_id = osm_id;
    node->coord.lat = (int32_t)(lat * 1e7);
    node->coord.lon = (int32_t)(lon * 1e7);
    node->edge_start = 0;
    node->edge_count = 0;

    if (node_map_insert(&builder->node_map, osm_id, idx) != VL_OK) {
        return VL_INVALID_NODE;
    }

    builder->num_nodes++;
    return idx;
}

static VLStatus add_temp_edge(VLGraphBuilder *builder, uint32_t source, uint32_t target,
                              uint32_t distance, uint16_t duration, uint16_t flags)
{
    if (builder->num_temp_edges >= builder->temp_edges_capacity) {
        size_t new_cap = builder->temp_edges_capacity * 2;
        VLTempEdge *new_edges = realloc(builder->temp_edges, new_cap * sizeof(VLTempEdge));
        if (!new_edges) return VL_ERROR_OUT_OF_MEMORY;
        builder->temp_edges = new_edges;
        builder->temp_edges_capacity = new_cap;
    }

    VLTempEdge *edge = &builder->temp_edges[builder->num_temp_edges++];
    edge->source = source;
    edge->target = target;
    edge->distance = distance;
    edge->duration = duration;
    edge->flags = flags;

    return VL_OK;
}

/* ============================================================================
 * Build Graph from PBF Context
 * ============================================================================ */

typedef struct {
    int64_t id;
    size_t index;
} VLOSMNodeIndex;

static int compare_osm_node_index(const void *a, const void *b)
{
    const VLOSMNodeIndex *ia = a;
    const VLOSMNodeIndex *ib = b;
    if (ia->id < ib->id) return -1;
    if (ia->id > ib->id) return 1;
    return 0;
}

static size_t find_osm_node_indexed(const VLOSMNodeIndex *index, size_t count, int64_t id)
{
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (index[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < count && index[lo].id == id) {
        return index[lo].index;
    }
    return (size_t)-1;
}

VLStatus vl_graph_build_from_pbf(VLGraphBuilder *builder, const VLPBFContext *ctx)
{
    if (!builder || !ctx) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    VLOSMNodeIndex *node_index = malloc(ctx->num_nodes * sizeof(VLOSMNodeIndex));
    if (!node_index) return VL_ERROR_OUT_OF_MEMORY;

    /* Build index in parallel */
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < ctx->num_nodes; i++) {
        node_index[i].id = ctx->nodes[i].id;
        node_index[i].index = i;
    }
    qsort(node_index, ctx->num_nodes, sizeof(VLOSMNodeIndex), compare_osm_node_index);

    /* Process each way */
    for (size_t w = 0; w < ctx->num_ways; w++) {
        const VLOSMWay *way = &ctx->ways[w];
        if (way->num_refs < 2) continue;

        uint32_t prev_idx = VL_INVALID_NODE;
        VLCoord prev_coord = {0};

        for (int i = 0; i < way->num_refs; i++) {
            int64_t node_id = way->node_refs[i];

            size_t osm_idx = find_osm_node_indexed(node_index, ctx->num_nodes, node_id);
            if (osm_idx == (size_t)-1) {
                prev_idx = VL_INVALID_NODE;
                continue;
            }

            double lat = ctx->nodes[osm_idx].lat;
            double lon = ctx->nodes[osm_idx].lon;

            uint32_t curr_idx = get_or_create_node(builder, node_id, lat, lon);
            if (curr_idx == VL_INVALID_NODE) {
                free(node_index);
                return VL_ERROR_OUT_OF_MEMORY;
            }

            VLCoord curr_coord = {lat, lon};

            if (prev_idx != VL_INVALID_NODE) {
                double dist_m = vl_haversine(prev_coord, curr_coord);
                double dist_mm_d = dist_m * 1000.0;
                uint32_t dist_mm = (dist_mm_d > UINT32_MAX) ? UINT32_MAX : (uint32_t)dist_mm_d;

                int speed_kmh = way->max_speed > 0 ? way->max_speed :
                                vl_default_speed(way->highway_type);
                double time_s = vl_travel_time(dist_m, speed_kmh);
                double duration_ds_d = time_s * 10.0;
                uint16_t duration_ds = (duration_ds_d > UINT16_MAX) ? UINT16_MAX : (uint16_t)duration_ds_d;
                if (duration_ds == 0 && dist_mm > 0) duration_ds = 1;

                uint16_t flags = (uint16_t)way->highway_type;
                if (way->oneway != 0) flags |= VL_EDGE_ONEWAY;
                flags |= way->access_flags;  /* Add OSM access restrictions */

                if (way->oneway >= 0) {
                    VLStatus status = add_temp_edge(builder, prev_idx, curr_idx,
                                                    dist_mm, duration_ds, flags);
                    if (status != VL_OK) {
                        free(node_index);
                        return status;
                    }
                }

                if (way->oneway <= 0) {
                    VLStatus status = add_temp_edge(builder, curr_idx, prev_idx,
                                                    dist_mm, duration_ds, flags);
                    if (status != VL_OK) {
                        free(node_index);
                        return status;
                    }
                }
            }

            prev_idx = curr_idx;
            prev_coord = curr_coord;
        }
    }

    free(node_index);
    return VL_OK;
}

/* ============================================================================
 * Reverse Graph Index
 * ============================================================================ */

VLStatus vl_graph_build_reverse_index(VLGraph *graph)
{
    if (!graph || graph->num_nodes == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* Free existing reverse index if any */
    free(graph->rev_edge_start);
    free(graph->rev_edge_count);
    free(graph->rev_edges);
    free(graph->rev_edge_idx);

    /* Allocate reverse index arrays */
    graph->rev_edge_start = calloc(graph->num_nodes, sizeof(uint32_t));
    graph->rev_edge_count = calloc(graph->num_nodes, sizeof(uint32_t));
    graph->rev_edges = malloc(graph->num_edges * sizeof(uint32_t));
    graph->rev_edge_idx = malloc(graph->num_edges * sizeof(uint32_t));

    if (!graph->rev_edge_start || !graph->rev_edge_count ||
        !graph->rev_edges || !graph->rev_edge_idx) {
        free(graph->rev_edge_start);
        free(graph->rev_edge_count);
        free(graph->rev_edges);
        free(graph->rev_edge_idx);
        graph->rev_edge_start = NULL;
        graph->rev_edge_count = NULL;
        graph->rev_edges = NULL;
        graph->rev_edge_idx = NULL;
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Count incoming edges per node (parallel) */
    #pragma omp parallel for schedule(static)
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        graph->rev_edge_count[i] = 0;
    }

    /* Count in parallel using atomic increments */
    #pragma omp parallel for schedule(static)
    for (uint32_t e = 0; e < graph->num_edges; e++) {
        uint32_t target = graph->edges[e].target;
        #pragma omp atomic
        graph->rev_edge_count[target]++;
    }

    /* Calculate offsets (prefix sum) */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        graph->rev_edge_start[i] = offset;
        offset += graph->rev_edge_count[i];
        graph->rev_edge_count[i] = 0;  /* Reset for filling */
    }

    /* Fill reverse edges */
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        const VLNode *node = &graph->nodes[i];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            uint32_t edge_idx = node->edge_start + e;
            uint32_t target = graph->edges[edge_idx].target;
            uint32_t rev_idx = graph->rev_edge_start[target] + graph->rev_edge_count[target];
            graph->rev_edges[rev_idx] = i;  /* Source node */
            graph->rev_edge_idx[rev_idx] = edge_idx;  /* Original edge index */
            graph->rev_edge_count[target]++;
        }
    }

    return VL_OK;
}

/* ============================================================================
 * Grid Spatial Index
 * ============================================================================ */

VLStatus vl_graph_build_grid_index(VLGraph *graph)
{
    if (!graph || graph->num_nodes == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* Free existing grid index */
    if (graph->grid_index) {
        free(graph->grid_index->cell_nodes);
        free(graph->grid_index->cell_offsets);
        free(graph->grid_index);
        graph->grid_index = NULL;
    }

    VLGridIndex *grid = calloc(1, sizeof(VLGridIndex));
    if (!grid) return VL_ERROR_OUT_OF_MEMORY;

    /* Calculate grid parameters */
    grid->lat_min = graph->bbox_min.lat;
    grid->lon_min = graph->bbox_min.lon;
    double lat_range = graph->bbox_max.lat - graph->bbox_min.lat;
    double lon_range = graph->bbox_max.lon - graph->bbox_min.lon;

    /* Avoid division by zero */
    if (lat_range < 1e-6) lat_range = 1e-6;
    if (lon_range < 1e-6) lon_range = 1e-6;

    grid->cell_lat = lat_range / VL_GRID_SIZE;
    grid->cell_lon = lon_range / VL_GRID_SIZE;

    size_t num_cells = (size_t)VL_GRID_SIZE * VL_GRID_SIZE;

    /* Allocate cell offsets and temporary count array */
    grid->cell_offsets = calloc(num_cells + 1, sizeof(uint32_t));
    uint32_t *cell_count = calloc(num_cells, sizeof(uint32_t));
    grid->cell_nodes = malloc(graph->num_nodes * sizeof(uint32_t));

    if (!grid->cell_offsets || !cell_count || !grid->cell_nodes) {
        free(grid->cell_offsets);
        free(cell_count);
        free(grid->cell_nodes);
        free(grid);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Count nodes per cell (parallel) */
    #pragma omp parallel for schedule(static)
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        VLCoord c = VL_FIXED_TO_COORD(graph->nodes[i].coord);
        int row = (int)((c.lat - grid->lat_min) / grid->cell_lat);
        int col = (int)((c.lon - grid->lon_min) / grid->cell_lon);

        /* Clamp to valid range */
        if (row < 0) row = 0;
        if (row >= VL_GRID_SIZE) row = VL_GRID_SIZE - 1;
        if (col < 0) col = 0;
        if (col >= VL_GRID_SIZE) col = VL_GRID_SIZE - 1;

        size_t cell = (size_t)row * VL_GRID_SIZE + col;
        #pragma omp atomic
        cell_count[cell]++;
    }

    /* Calculate offsets (prefix sum) */
    uint32_t total = 0;
    for (size_t i = 0; i < num_cells; i++) {
        grid->cell_offsets[i] = total;
        total += cell_count[i];
        cell_count[i] = 0;  /* Reset for filling */
    }
    grid->cell_offsets[num_cells] = total;

    /* Fill cell_nodes array */
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        VLCoord c = VL_FIXED_TO_COORD(graph->nodes[i].coord);
        int row = (int)((c.lat - grid->lat_min) / grid->cell_lat);
        int col = (int)((c.lon - grid->lon_min) / grid->cell_lon);

        if (row < 0) row = 0;
        if (row >= VL_GRID_SIZE) row = VL_GRID_SIZE - 1;
        if (col < 0) col = 0;
        if (col >= VL_GRID_SIZE) col = VL_GRID_SIZE - 1;

        size_t cell = (size_t)row * VL_GRID_SIZE + col;
        uint32_t idx = grid->cell_offsets[cell] + cell_count[cell];
        grid->cell_nodes[idx] = i;
        cell_count[cell]++;
    }

    free(cell_count);
    graph->grid_index = grid;

    return VL_OK;
}

/*
 * Find nearest node using grid index - O(1) average case.
 */
uint32_t vl_graph_nearest_node_grid(const VLGraph *graph, VLCoord coord)
{
    if (!graph || !graph->grid_index || graph->num_nodes == 0) {
        return vl_graph_nearest_node(graph, coord);  /* Fallback */
    }

    const VLGridIndex *grid = graph->grid_index;

    int center_row = (int)((coord.lat - grid->lat_min) / grid->cell_lat);
    int center_col = (int)((coord.lon - grid->lon_min) / grid->cell_lon);

    /* Clamp */
    if (center_row < 0) center_row = 0;
    if (center_row >= VL_GRID_SIZE) center_row = VL_GRID_SIZE - 1;
    if (center_col < 0) center_col = 0;
    if (center_col >= VL_GRID_SIZE) center_col = VL_GRID_SIZE - 1;

    uint32_t best_idx = VL_INVALID_NODE;
    double best_dist = VL_INF;

    /* Search expanding rings until we find a node */
    for (int radius = 0; radius < VL_GRID_SIZE && best_idx == VL_INVALID_NODE; radius++) {
        for (int dr = -radius; dr <= radius; dr++) {
            for (int dc = -radius; dc <= radius; dc++) {
                /* Only check cells on the ring perimeter */
                if (radius > 0 && abs(dr) != radius && abs(dc) != radius) continue;

                int row = center_row + dr;
                int col = center_col + dc;
                if (row < 0 || row >= VL_GRID_SIZE || col < 0 || col >= VL_GRID_SIZE) continue;

                size_t cell = (size_t)row * VL_GRID_SIZE + col;
                uint32_t start = grid->cell_offsets[cell];
                uint32_t end = grid->cell_offsets[cell + 1];

                for (uint32_t i = start; i < end; i++) {
                    uint32_t node_idx = grid->cell_nodes[i];
                    VLCoord node_coord = VL_FIXED_TO_COORD(graph->nodes[node_idx].coord);

                    /* Quick squared distance check first */
                    double dlat = coord.lat - node_coord.lat;
                    double dlon = coord.lon - node_coord.lon;
                    double sq_dist = dlat * dlat + dlon * dlon;

                    if (sq_dist < best_dist * 1e-10) {  /* Rough threshold */
                        double dist = vl_haversine(coord, node_coord);
                        if (dist < best_dist) {
                            best_dist = dist;
                            best_idx = node_idx;
                        }
                    }
                }
            }
        }
    }

    /* If still not found, full search (shouldn't happen) */
    if (best_idx == VL_INVALID_NODE) {
        return vl_graph_nearest_node(graph, coord);
    }

    /* Verify with neighboring cells to ensure we have the true nearest */
    int search_radius = (int)(best_dist / 111000.0 / grid->cell_lat) + 1;
    if (search_radius > 5) search_radius = 5;

    for (int dr = -search_radius; dr <= search_radius; dr++) {
        for (int dc = -search_radius; dc <= search_radius; dc++) {
            int row = center_row + dr;
            int col = center_col + dc;
            if (row < 0 || row >= VL_GRID_SIZE || col < 0 || col >= VL_GRID_SIZE) continue;

            size_t cell = (size_t)row * VL_GRID_SIZE + col;
            uint32_t start = grid->cell_offsets[cell];
            uint32_t end = grid->cell_offsets[cell + 1];

            /* SIMD-friendly loop */
            #pragma omp simd
            for (uint32_t i = start; i < end; i++) {
                uint32_t node_idx = grid->cell_nodes[i];
                VLCoord node_coord = VL_FIXED_TO_COORD(graph->nodes[node_idx].coord);
                double dist = vl_haversine(coord, node_coord);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_idx = node_idx;
                }
            }
        }
    }

    return best_idx;
}

/* ============================================================================
 * Finalize Graph (Convert to CSR)
 * ============================================================================ */

static int compare_temp_edges(const void *a, const void *b)
{
    const VLTempEdge *ea = a;
    const VLTempEdge *eb = b;
    if (ea->source < eb->source) return -1;
    if (ea->source > eb->source) return 1;
    if (ea->target < eb->target) return -1;
    if (ea->target > eb->target) return 1;
    return 0;
}

VLGraph *vl_graph_finalize(VLGraphBuilder *builder)
{
    if (!builder || builder->num_nodes == 0) {
        return NULL;
    }

    VLGraph *graph = calloc(1, sizeof(VLGraph));
    if (!graph) return NULL;

    /* Sort edges by source node */
    qsort(builder->temp_edges, builder->num_temp_edges,
          sizeof(VLTempEdge), compare_temp_edges);

    /* Remove duplicate edges (keep shortest) */
    size_t unique_edges = 0;
    for (size_t i = 0; i < builder->num_temp_edges; i++) {
        if (unique_edges > 0 &&
            builder->temp_edges[i].source == builder->temp_edges[unique_edges - 1].source &&
            builder->temp_edges[i].target == builder->temp_edges[unique_edges - 1].target) {
            if (builder->temp_edges[i].distance < builder->temp_edges[unique_edges - 1].distance) {
                builder->temp_edges[unique_edges - 1] = builder->temp_edges[i];
            }
        } else {
            builder->temp_edges[unique_edges++] = builder->temp_edges[i];
        }
    }

    /* Allocate final arrays */
    graph->num_nodes = (uint32_t)builder->num_nodes;
    graph->nodes = malloc(graph->num_nodes * sizeof(VLNode));
    if (!graph->nodes) {
        free(graph);
        return NULL;
    }

    graph->num_edges = (uint32_t)unique_edges;
    graph->edges = malloc(graph->num_edges * sizeof(VLEdge));
    if (!graph->edges) {
        free(graph->nodes);
        free(graph);
        return NULL;
    }

    graph->owns_memory = 1;

    /* Copy nodes */
    memcpy(graph->nodes, builder->nodes, graph->num_nodes * sizeof(VLNode));

    /* Reset edge counts (parallel) */
    #pragma omp parallel for schedule(static)
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        graph->nodes[i].edge_start = 0;
        graph->nodes[i].edge_count = 0;
    }

    /* Count edges per node */
    for (size_t i = 0; i < unique_edges; i++) {
        graph->nodes[builder->temp_edges[i].source].edge_count++;
    }

    /* Calculate edge_start offsets */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        graph->nodes[i].edge_start = offset;
        offset += graph->nodes[i].edge_count;
        graph->nodes[i].edge_count = 0;
    }

    /* Fill edge array */
    for (size_t i = 0; i < unique_edges; i++) {
        VLTempEdge *te = &builder->temp_edges[i];
        VLNode *node = &graph->nodes[te->source];
        uint32_t edge_idx = node->edge_start + node->edge_count;

        graph->edges[edge_idx].target = te->target;
        graph->edges[edge_idx].distance = te->distance;
        graph->edges[edge_idx].duration = te->duration;
        graph->edges[edge_idx].flags = te->flags;

        node->edge_count++;
    }

    /* Calculate bounding box (parallel reduction) */
    double min_lat = 90.0, max_lat = -90.0;
    double min_lon = 180.0, max_lon = -180.0;

    #pragma omp parallel for reduction(min:min_lat,min_lon) reduction(max:max_lat,max_lon)
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        VLCoord c = VL_FIXED_TO_COORD(graph->nodes[i].coord);
        if (c.lat < min_lat) min_lat = c.lat;
        if (c.lat > max_lat) max_lat = c.lat;
        if (c.lon < min_lon) min_lon = c.lon;
        if (c.lon > max_lon) max_lon = c.lon;
    }

    graph->bbox_min.lat = min_lat;
    graph->bbox_min.lon = min_lon;
    graph->bbox_max.lat = max_lat;
    graph->bbox_max.lon = max_lon;

    /* Build reverse graph index */
    VLStatus status = vl_graph_build_reverse_index(graph);
    if (status != VL_OK) {
        fprintf(stderr, "velo: Warning: Failed to build reverse index\n");
    }

    /* Build grid spatial index */
    status = vl_graph_build_grid_index(graph);
    if (status != VL_OK) {
        fprintf(stderr, "velo: Warning: Failed to build grid index\n");
    }

    return graph;
}

/* ============================================================================
 * Degree-2 Node Contraction
 * ============================================================================ */

/*
 * Check if a node has exactly 2 edges (in + out combined) and can be contracted.
 * Returns 1 if contractable, 0 otherwise.
 */
static int is_contractable(const VLGraph *graph, uint32_t node)
{
    uint32_t out_degree = graph->nodes[node].edge_count;
    uint32_t in_degree = graph->rev_edge_count ? graph->rev_edge_count[node] : 0;

    /* Only contract nodes with exactly 2 total connections */
    /* This handles: degree-2 on undirected road, or 1-in + 1-out on directed */
    if (out_degree + in_degree != 2) return 0;

    /* Don't contract nodes at intersections (multiple ways meeting) */
    if (out_degree > 1 || in_degree > 1) return 0;

    return 1;
}

/*
 * Find the edge from 'from' to 'to', returns edge index or UINT32_MAX if not found.
 */
static uint32_t find_edge(const VLGraph *graph, uint32_t from, uint32_t to)
{
    const VLNode *node = &graph->nodes[from];
    for (uint32_t e = 0; e < node->edge_count; e++) {
        uint32_t idx = node->edge_start + e;
        if (graph->edges[idx].target == to) {
            return idx;
        }
    }
    return UINT32_MAX;
}

VLStatus vl_graph_contract_degree2(VLGraph *graph)
{
    if (!graph || !graph->nodes || !graph->edges) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* Need reverse index for contraction */
    if (!graph->rev_edge_start) {
        VLStatus status = vl_graph_build_reverse_index(graph);
        if (status != VL_OK) return status;
    }

    uint32_t num_nodes = graph->num_nodes;
    uint32_t num_edges = graph->num_edges;

    /* Mark nodes to be contracted */
    uint8_t *contracted = calloc(num_nodes, sizeof(uint8_t));
    if (!contracted) return VL_ERROR_OUT_OF_MEMORY;

    /* Count contractable nodes */
    uint32_t num_contractable = 0;
    for (uint32_t i = 0; i < num_nodes; i++) {
        if (is_contractable(graph, i)) {
            contracted[i] = 1;
            num_contractable++;
        }
    }

    if (num_contractable == 0) {
        free(contracted);
        return VL_OK;  /* Nothing to contract */
    }

    printf("velo: Contracting %u degree-2 nodes (%.1f%% of graph)\n",
           num_contractable, 100.0 * num_contractable / num_nodes);

    /* Create mapping from old node IDs to new node IDs */
    uint32_t *old_to_new = malloc(num_nodes * sizeof(uint32_t));
    uint32_t *new_to_old = malloc((num_nodes - num_contractable) * sizeof(uint32_t));
    if (!old_to_new || !new_to_old) {
        free(contracted);
        free(old_to_new);
        free(new_to_old);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    uint32_t new_node_count = 0;
    for (uint32_t i = 0; i < num_nodes; i++) {
        if (!contracted[i]) {
            old_to_new[i] = new_node_count;
            new_to_old[new_node_count] = i;
            new_node_count++;
        } else {
            old_to_new[i] = UINT32_MAX;  /* Contracted away */
        }
    }

    /* Build new edges, skipping contracted nodes and merging chains */
    /* First pass: count new edges */
    uint32_t new_edge_count = 0;
    for (uint32_t i = 0; i < num_nodes; i++) {
        if (contracted[i]) continue;

        const VLNode *node = &graph->nodes[i];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            uint32_t target = graph->edges[node->edge_start + e].target;

            /* Follow chain of contracted nodes */
            while (contracted[target]) {
                /* Find next node in chain */
                const VLNode *t = &graph->nodes[target];
                if (t->edge_count == 0) break;
                target = graph->edges[t->edge_start].target;
            }

            /* Only count edge if target is not contracted */
            if (!contracted[target]) {
                new_edge_count++;
            }
        }
    }

    /* Allocate new arrays */
    VLNode *new_nodes = calloc(new_node_count, sizeof(VLNode));
    VLEdge *new_edges = calloc(new_edge_count, sizeof(VLEdge));

    /* For path unpacking: store intermediate nodes for each edge */
    /* Estimate: average chain length ~2-3, so intermediates ~ num_contractable */
    uint32_t *intermediates = malloc(num_contractable * sizeof(uint32_t));
    uint32_t *edge_offsets = malloc((new_edge_count + 1) * sizeof(uint32_t));

    if (!new_nodes || !new_edges || !intermediates || !edge_offsets) {
        free(contracted);
        free(old_to_new);
        free(new_to_old);
        free(new_nodes);
        free(new_edges);
        free(intermediates);
        free(edge_offsets);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Second pass: build new graph */
    uint32_t edge_idx = 0;
    uint32_t inter_idx = 0;

    for (uint32_t new_i = 0; new_i < new_node_count; new_i++) {
        uint32_t old_i = new_to_old[new_i];

        /* Copy node data */
        new_nodes[new_i] = graph->nodes[old_i];
        new_nodes[new_i].edge_start = edge_idx;
        new_nodes[new_i].edge_count = 0;

        const VLNode *old_node = &graph->nodes[old_i];
        for (uint32_t e = 0; e < old_node->edge_count; e++) {
            uint32_t old_edge_idx = old_node->edge_start + e;
            VLEdge edge = graph->edges[old_edge_idx];
            uint32_t target = edge.target;

            /* Record intermediate nodes offset */
            edge_offsets[edge_idx] = inter_idx;

            /* Follow and accumulate chain */
            uint32_t chain_dist = edge.distance;
            uint32_t chain_dur = edge.duration;

            while (contracted[target]) {
                /* Store intermediate node */
                if (inter_idx < num_contractable) {
                    intermediates[inter_idx++] = target;
                }

                /* Accumulate distance/duration */
                const VLNode *t = &graph->nodes[target];
                if (t->edge_count == 0) break;

                VLEdge next_edge = graph->edges[t->edge_start];
                chain_dist += next_edge.distance;
                chain_dur += next_edge.duration;
                target = next_edge.target;
            }

            /* Skip if target was contracted away (dead end) */
            if (contracted[target]) continue;

            /* Add contracted edge */
            new_edges[edge_idx].target = old_to_new[target];
            new_edges[edge_idx].distance = chain_dist;
            new_edges[edge_idx].duration = (chain_dur > 65535) ? 65535 : (uint16_t)chain_dur;
            new_edges[edge_idx].flags = edge.flags;

            edge_idx++;
            new_nodes[new_i].edge_count++;
        }
    }
    edge_offsets[edge_idx] = inter_idx;  /* Final offset */

    /* Create contraction data structure */
    VLContraction *cont = malloc(sizeof(VLContraction));
    if (!cont) {
        free(contracted);
        free(old_to_new);
        free(new_to_old);
        free(new_nodes);
        free(new_edges);
        free(intermediates);
        free(edge_offsets);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    cont->node_to_original = new_to_old;
    cont->num_contracted_nodes = new_node_count;
    cont->edge_intermediates = intermediates;
    cont->edge_intermediate_offset = edge_offsets;
    cont->num_intermediates = inter_idx;

    /* Replace graph data */
    if (graph->owns_memory) {
        free(graph->nodes);
        free(graph->edges);
    }

    graph->nodes = new_nodes;
    graph->num_nodes = new_node_count;
    graph->edges = new_edges;
    graph->num_edges = edge_idx;
    graph->owns_memory = 1;
    graph->contraction = cont;

    /* Clear old indexes - they need to be rebuilt */
    free(graph->rev_edge_start);
    free(graph->rev_edge_count);
    free(graph->rev_edges);
    free(graph->rev_edge_idx);
    graph->rev_edge_start = NULL;
    graph->rev_edge_count = NULL;
    graph->rev_edges = NULL;
    graph->rev_edge_idx = NULL;

    if (graph->grid_index) {
        free(graph->grid_index->cell_nodes);
        free(graph->grid_index->cell_offsets);
        free(graph->grid_index);
        graph->grid_index = NULL;
    }

    free(contracted);
    free(old_to_new);

    printf("velo: Contracted graph: %u nodes, %u edges\n",
           graph->num_nodes, graph->num_edges);

    /* Rebuild indexes */
    vl_graph_build_reverse_index(graph);
    vl_graph_build_grid_index(graph);

    return VL_OK;
}

VLStatus vl_graph_unpack_path(const VLGraph *graph,
                               const uint32_t *node_indices, int num_nodes,
                               uint32_t **out_indices, int *out_num_nodes)
{
    if (!graph || !node_indices || !out_indices || !out_num_nodes) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* If no contraction data, just copy the path */
    if (!graph->contraction) {
        uint32_t *copy = malloc(num_nodes * sizeof(uint32_t));
        if (!copy) return VL_ERROR_OUT_OF_MEMORY;
        memcpy(copy, node_indices, num_nodes * sizeof(uint32_t));
        *out_indices = copy;
        *out_num_nodes = num_nodes;
        return VL_OK;
    }

    VLContraction *cont = graph->contraction;

    /* First pass: count total nodes including intermediates */
    int total_nodes = num_nodes;
    for (int i = 0; i < num_nodes - 1; i++) {
        uint32_t from = node_indices[i];
        uint32_t to = node_indices[i + 1];

        /* Find the edge */
        uint32_t edge_idx = find_edge(graph, from, to);
        if (edge_idx != UINT32_MAX && edge_idx < graph->num_edges) {
            uint32_t start = cont->edge_intermediate_offset[edge_idx];
            uint32_t end = cont->edge_intermediate_offset[edge_idx + 1];
            total_nodes += (end - start);
        }
    }

    /* Allocate result */
    uint32_t *result = malloc(total_nodes * sizeof(uint32_t));
    if (!result) return VL_ERROR_OUT_OF_MEMORY;

    /* Second pass: build unpacked path */
    int out_idx = 0;

    for (int i = 0; i < num_nodes; i++) {
        /* Add original node (mapped back to original ID) */
        uint32_t node = node_indices[i];
        result[out_idx++] = cont->node_to_original[node];

        /* Add intermediates for edge to next node */
        if (i < num_nodes - 1) {
            uint32_t to = node_indices[i + 1];
            uint32_t edge_idx = find_edge(graph, node, to);

            if (edge_idx != UINT32_MAX && edge_idx < graph->num_edges) {
                uint32_t start = cont->edge_intermediate_offset[edge_idx];
                uint32_t end = cont->edge_intermediate_offset[edge_idx + 1];

                for (uint32_t j = start; j < end; j++) {
                    result[out_idx++] = cont->edge_intermediates[j];
                }
            }
        }
    }

    *out_indices = result;
    *out_num_nodes = out_idx;
    return VL_OK;
}

/* ============================================================================
 * Graph Memory Management
 * ============================================================================ */

void vl_graph_free(VLGraph *graph)
{
    if (!graph) return;

    if (graph->owns_memory) {
        free(graph->nodes);
        free(graph->edges);
    }

    /* Free reverse index */
    free(graph->rev_edge_start);
    free(graph->rev_edge_count);
    free(graph->rev_edges);
    free(graph->rev_edge_idx);

    /* Free contraction data */
    if (graph->contraction) {
        free(graph->contraction->node_to_original);
        free(graph->contraction->edge_intermediates);
        free(graph->contraction->edge_intermediate_offset);
        free(graph->contraction);
    }

    /* Free grid index */
    if (graph->grid_index) {
        free(graph->grid_index->cell_nodes);
        free(graph->grid_index->cell_offsets);
        free(graph->grid_index);
    }

    free(graph);
}

/* ============================================================================
 * Binary Save/Load
 * ============================================================================ */

VLStatus vl_graph_save(const VLGraph *graph, const char *filename)
{
    if (!graph || !filename) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        return VL_ERROR_FILE_WRITE;
    }

    VLBinaryHeader header = {0};
    header.magic = VL_BINARY_MAGIC;
    header.version = VL_BINARY_VERSION;
    header.num_nodes = graph->num_nodes;
    header.num_edges = graph->num_edges;
    header.bbox_min_lat = graph->bbox_min.lat;
    header.bbox_min_lon = graph->bbox_min.lon;
    header.bbox_max_lat = graph->bbox_max.lat;
    header.bbox_max_lon = graph->bbox_max.lon;

    if (fwrite(&header, sizeof(header), 1, f) != 1 ||
        fwrite(graph->nodes, sizeof(VLNode), graph->num_nodes, f) != graph->num_nodes ||
        fwrite(graph->edges, sizeof(VLEdge), graph->num_edges, f) != graph->num_edges) {
        fclose(f);
        return VL_ERROR_FILE_WRITE;
    }

    fclose(f);
    return VL_OK;
}

VLGraph *vl_graph_load(const char *filename)
{
    if (!filename) return NULL;

    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    VLBinaryHeader header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        fclose(f);
        return NULL;
    }

    if (header.magic != VL_BINARY_MAGIC) {
        fclose(f);
        return NULL;
    }

    VLGraph *graph = calloc(1, sizeof(VLGraph));
    if (!graph) {
        fclose(f);
        return NULL;
    }

    graph->num_nodes = header.num_nodes;
    graph->num_edges = header.num_edges;
    graph->bbox_min.lat = header.bbox_min_lat;
    graph->bbox_min.lon = header.bbox_min_lon;
    graph->bbox_max.lat = header.bbox_max_lat;
    graph->bbox_max.lon = header.bbox_max_lon;
    graph->owns_memory = 1;

    graph->nodes = malloc(graph->num_nodes * sizeof(VLNode));
    graph->edges = malloc(graph->num_edges * sizeof(VLEdge));

    if (!graph->nodes || !graph->edges ||
        fread(graph->nodes, sizeof(VLNode), graph->num_nodes, f) != graph->num_nodes ||
        fread(graph->edges, sizeof(VLEdge), graph->num_edges, f) != graph->num_edges) {
        vl_graph_free(graph);
        fclose(f);
        return NULL;
    }

    fclose(f);

    /* Build indexes after loading */
    vl_graph_build_reverse_index(graph);
    vl_graph_build_grid_index(graph);

    return graph;
}

#ifndef _WIN32
VLGraph *vl_graph_mmap(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    size_t len = (size_t)st.st_size;
    void *data = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (data == MAP_FAILED) return NULL;

    VLBinaryHeader *header = data;
    if (header->magic != VL_BINARY_MAGIC) {
        munmap(data, len);
        return NULL;
    }

    VLGraph *graph = calloc(1, sizeof(VLGraph));
    if (!graph) {
        munmap(data, len);
        return NULL;
    }

    graph->num_nodes = header->num_nodes;
    graph->num_edges = header->num_edges;
    graph->bbox_min.lat = header->bbox_min_lat;
    graph->bbox_min.lon = header->bbox_min_lon;
    graph->bbox_max.lat = header->bbox_max_lat;
    graph->bbox_max.lon = header->bbox_max_lon;

    graph->nodes = (VLNode *)((uint8_t *)data + sizeof(VLBinaryHeader));
    graph->edges = (VLEdge *)((uint8_t *)graph->nodes + graph->num_nodes * sizeof(VLNode));

    graph->owns_memory = 0;

    /* Build indexes (these are always owned) */
    vl_graph_build_reverse_index(graph);
    vl_graph_build_grid_index(graph);

    return graph;
}
#endif

/* ============================================================================
 * Nearest Node Lookup (fallback linear scan)
 * ============================================================================ */

uint32_t vl_graph_nearest_node(const VLGraph *graph, VLCoord coord)
{
    if (!graph || graph->num_nodes == 0) {
        return VL_INVALID_NODE;
    }

    /* Use grid index if available */
    if (graph->grid_index) {
        return vl_graph_nearest_node_grid(graph, coord);
    }

    uint32_t best_idx = 0;
    double best_dist = VL_INF;

    /* OpenMP parallel reduction for nearest node */
    #pragma omp parallel
    {
        uint32_t local_best_idx = 0;
        double local_best_dist = VL_INF;

        #pragma omp for nowait schedule(static)
        for (uint32_t i = 0; i < graph->num_nodes; i++) {
            VLCoord node_coord = VL_FIXED_TO_COORD(graph->nodes[i].coord);
            double dist = vl_haversine(coord, node_coord);

            if (dist < local_best_dist) {
                local_best_dist = dist;
                local_best_idx = i;
            }
        }

        #pragma omp critical
        {
            if (local_best_dist < best_dist) {
                best_dist = local_best_dist;
                best_idx = local_best_idx;
            }
        }
    }

    return best_idx;
}

uint32_t vl_graph_nearest_node_within(const VLGraph *graph, VLCoord coord, double max_dist)
{
    if (!graph || graph->num_nodes == 0) {
        return VL_INVALID_NODE;
    }

    uint32_t best_idx = VL_INVALID_NODE;
    double best_dist = max_dist;

    double deg_per_m = 1.0 / 111000.0;
    double cos_lat = cos(coord.lat * M_PI / 180.0);
    VLCoord search_min = {
        coord.lat - max_dist * deg_per_m,
        coord.lon - max_dist * deg_per_m / cos_lat
    };
    VLCoord search_max = {
        coord.lat + max_dist * deg_per_m,
        coord.lon + max_dist * deg_per_m / cos_lat
    };

    #pragma omp parallel
    {
        uint32_t local_best_idx = VL_INVALID_NODE;
        double local_best_dist = max_dist;

        #pragma omp for nowait schedule(static)
        for (uint32_t i = 0; i < graph->num_nodes; i++) {
            VLCoord node_coord = VL_FIXED_TO_COORD(graph->nodes[i].coord);

            if (node_coord.lat < search_min.lat || node_coord.lat > search_max.lat ||
                node_coord.lon < search_min.lon || node_coord.lon > search_max.lon) {
                continue;
            }

            double dist = vl_haversine(coord, node_coord);

            if (dist < local_best_dist) {
                local_best_dist = dist;
                local_best_idx = i;
            }
        }

        #pragma omp critical
        {
            if (local_best_dist < best_dist) {
                best_dist = local_best_dist;
                best_idx = local_best_idx;
            }
        }
    }

    return best_idx;
}

/* ============================================================================
 * Graph Statistics
 * ============================================================================ */

void vl_graph_stats(const VLGraph *graph, uint32_t *num_nodes, uint32_t *num_edges,
                    uint32_t *max_out_degree, double *avg_out_degree)
{
    if (!graph) return;

    if (num_nodes) *num_nodes = graph->num_nodes;
    if (num_edges) *num_edges = graph->num_edges;

    if (max_out_degree || avg_out_degree) {
        uint32_t max_deg = 0;
        uint64_t total_deg = 0;

        #pragma omp parallel for reduction(max:max_deg) reduction(+:total_deg)
        for (uint32_t i = 0; i < graph->num_nodes; i++) {
            uint32_t deg = graph->nodes[i].edge_count;
            if (deg > max_deg) max_deg = deg;
            total_deg += deg;
        }

        if (max_out_degree) *max_out_degree = max_deg;
        if (avg_out_degree) {
            *avg_out_degree = graph->num_nodes > 0 ?
                              (double)total_deg / graph->num_nodes : 0.0;
        }
    }
}

/* ============================================================================
 * Hilbert Curve Reordering for Cache Locality
 * ============================================================================ */

/*
 * Convert (x, y) to Hilbert curve index for a 2^order x 2^order grid.
 * This is the standard algorithm from the Hilbert curve literature.
 */
static uint64_t xy_to_hilbert(uint32_t x, uint32_t y, int order)
{
    uint64_t d = 0;
    for (int s = order - 1; s >= 0; s--) {
        uint32_t rx = (x >> s) & 1;
        uint32_t ry = (y >> s) & 1;
        d += (uint64_t)(s > 0 ? ((uint64_t)1 << (2 * s)) : 1) * ((3 * rx) ^ ry);

        /* Rotate quadrant */
        if (ry == 0) {
            if (rx == 1) {
                x = ((uint32_t)1 << s) - 1 - x;
                y = ((uint32_t)1 << s) - 1 - y;
            }
            /* Swap x and y */
            uint32_t t = x;
            x = y;
            y = t;
        }
    }
    return d;
}

/* Node with Hilbert index for sorting */
typedef struct {
    uint32_t original_idx;
    uint64_t hilbert_idx;
} HilbertNode;

/* Comparison function for qsort */
static int compare_hilbert(const void *a, const void *b)
{
    const HilbertNode *ha = (const HilbertNode *)a;
    const HilbertNode *hb = (const HilbertNode *)b;
    if (ha->hilbert_idx < hb->hilbert_idx) return -1;
    if (ha->hilbert_idx > hb->hilbert_idx) return 1;
    return 0;
}

/* Comparison function for sorting edges by target node index */
static int compare_edges_by_target(const void *a, const void *b)
{
    const VLEdge *ea = (const VLEdge *)a;
    const VLEdge *eb = (const VLEdge *)b;
    if (ea->target < eb->target) return -1;
    if (ea->target > eb->target) return 1;
    return 0;
}

VLStatus vl_graph_reorder_hilbert(VLGraph *graph)
{
    if (!graph || graph->num_nodes == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* If graph is mmap'd (read-only), copy to writable memory first */
    if (!graph->owns_memory) {
        printf("velo: Converting mmap'd graph to writable memory...\n");

        VLNode *nodes_copy = malloc(graph->num_nodes * sizeof(VLNode));
        VLEdge *edges_copy = malloc(graph->num_edges * sizeof(VLEdge));

        if (!nodes_copy || !edges_copy) {
            free(nodes_copy);
            free(edges_copy);
            return VL_ERROR_OUT_OF_MEMORY;
        }

        memcpy(nodes_copy, graph->nodes, graph->num_nodes * sizeof(VLNode));
        memcpy(edges_copy, graph->edges, graph->num_edges * sizeof(VLEdge));

        /* Note: We don't munmap the old memory because other code might reference it.
         * In practice, the OS will reclaim it when the process exits.
         * For proper cleanup, we'd need to track the mmap'd pointer and size. */
        graph->nodes = nodes_copy;
        graph->edges = edges_copy;
        graph->owns_memory = 1;
    }

    printf("velo: Reordering nodes by Hilbert curve...\n");

    uint32_t num_nodes = graph->num_nodes;

    /* Determine grid size (use 16-bit precision = 65536 x 65536) */
    const int order = 16;
    const uint32_t grid_size = (uint32_t)1 << order;

    /* Calculate bounding box */
    double lat_min = graph->bbox_min.lat;
    double lat_max = graph->bbox_max.lat;
    double lon_min = graph->bbox_min.lon;
    double lon_max = graph->bbox_max.lon;

    double lat_range = lat_max - lat_min;
    double lon_range = lon_max - lon_min;
    if (lat_range < 1e-9) lat_range = 1e-9;
    if (lon_range < 1e-9) lon_range = 1e-9;

    /* Allocate arrays for sorting */
    HilbertNode *hilbert_nodes = malloc(num_nodes * sizeof(HilbertNode));
    if (!hilbert_nodes) {
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Compute Hilbert index for each node */
    #pragma omp parallel for
    for (uint32_t i = 0; i < num_nodes; i++) {
        double lat = graph->nodes[i].coord.lat * 1e-7;
        double lon = graph->nodes[i].coord.lon * 1e-7;

        /* Map to grid coordinates */
        uint32_t x = (uint32_t)(((lon - lon_min) / lon_range) * (grid_size - 1));
        uint32_t y = (uint32_t)(((lat - lat_min) / lat_range) * (grid_size - 1));

        /* Clamp to valid range */
        if (x >= grid_size) x = grid_size - 1;
        if (y >= grid_size) y = grid_size - 1;

        hilbert_nodes[i].original_idx = i;
        hilbert_nodes[i].hilbert_idx = xy_to_hilbert(x, y, order);
    }

    /* Sort by Hilbert index */
    qsort(hilbert_nodes, num_nodes, sizeof(HilbertNode), compare_hilbert);

    /* Build old-to-new mapping */
    uint32_t *old_to_new = malloc(num_nodes * sizeof(uint32_t));
    if (!old_to_new) {
        free(hilbert_nodes);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    for (uint32_t new_idx = 0; new_idx < num_nodes; new_idx++) {
        old_to_new[hilbert_nodes[new_idx].original_idx] = new_idx;
    }

    /* Reorder nodes */
    VLNode *new_nodes = malloc(num_nodes * sizeof(VLNode));
    if (!new_nodes) {
        free(hilbert_nodes);
        free(old_to_new);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    for (uint32_t new_idx = 0; new_idx < num_nodes; new_idx++) {
        uint32_t old_idx = hilbert_nodes[new_idx].original_idx;
        new_nodes[new_idx] = graph->nodes[old_idx];
    }

    free(hilbert_nodes);

    /* Update edge targets to use new node indices */
    #pragma omp parallel for
    for (uint32_t i = 0; i < graph->num_edges; i++) {
        uint32_t target = graph->edges[i].target;
        if (target < num_nodes) {
            graph->edges[i].target = old_to_new[target];
        }
        /* else: edge points to invalid node, leave it unchanged */
    }

    /* Reorder edges to match new node order */
    /* First, count edges per new node */
    uint32_t *new_edge_counts = calloc(num_nodes, sizeof(uint32_t));
    if (!new_edge_counts) {
        free(new_nodes);
        free(old_to_new);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    for (uint32_t old_idx = 0; old_idx < num_nodes; old_idx++) {
        uint32_t new_idx = old_to_new[old_idx];
        new_edge_counts[new_idx] = graph->nodes[old_idx].edge_count;
    }

    /* Calculate new edge offsets */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < num_nodes; i++) {
        new_nodes[i].edge_start = offset;
        new_nodes[i].edge_count = new_edge_counts[i];
        offset += new_edge_counts[i];
    }

    /* Copy edges in new order */
    VLEdge *new_edges = malloc(graph->num_edges * sizeof(VLEdge));
    if (!new_edges) {
        free(new_nodes);
        free(old_to_new);
        free(new_edge_counts);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    for (uint32_t old_idx = 0; old_idx < num_nodes; old_idx++) {
        uint32_t new_idx = old_to_new[old_idx];
        VLNode *old_node = &graph->nodes[old_idx];
        VLNode *new_node = &new_nodes[new_idx];

        memcpy(&new_edges[new_node->edge_start],
               &graph->edges[old_node->edge_start],
               old_node->edge_count * sizeof(VLEdge));
    }

    /* Sort each node's adjacency list by target node index.
     * This improves cache locality during traversal since nodes with
     * close indices are spatially close after Hilbert reordering. */
    #pragma omp parallel for
    for (uint32_t i = 0; i < num_nodes; i++) {
        VLNode *node = &new_nodes[i];
        if (node->edge_count > 1) {
            qsort(&new_edges[node->edge_start], node->edge_count,
                  sizeof(VLEdge), compare_edges_by_target);
        }
    }

    /* Replace old arrays with new ones */
    free(graph->nodes);
    free(graph->edges);
    graph->nodes = new_nodes;
    graph->edges = new_edges;

    free(old_to_new);
    free(new_edge_counts);

    /* Rebuild reverse index */
    if (graph->rev_edge_start) {
        free(graph->rev_edge_start);
        free(graph->rev_edge_count);
        free(graph->rev_edges);
        free(graph->rev_edge_idx);
        graph->rev_edge_start = NULL;
        graph->rev_edge_count = NULL;
        graph->rev_edges = NULL;
        graph->rev_edge_idx = NULL;

        VLStatus status = vl_graph_build_reverse_index(graph);
        if (status != VL_OK) {
            return status;
        }
    }

    /* Rebuild grid index */
    if (graph->grid_index) {
        free(graph->grid_index->cell_nodes);
        free(graph->grid_index->cell_offsets);
        free(graph->grid_index);
        graph->grid_index = NULL;

        VLStatus status = vl_graph_build_grid_index(graph);
        if (status != VL_OK) {
            return status;
        }
    }

    printf("velo: Hilbert reordering complete\n");

    return VL_OK;
}

/* ============================================================================
 * Graph Validation
 * ============================================================================ */

VLStatus vl_graph_validate(const VLGraph *graph, int *out_errors)
{
    if (!graph) {
        if (out_errors) *out_errors = 1;
        return VL_ERROR_INVALID_ARGUMENT;
    }

    int errors = 0;

    /* Check basic structure */
    if (graph->num_nodes == 0) {
        fprintf(stderr, "velo: validation error: graph has no nodes\n");
        errors++;
    }
    if (graph->num_edges == 0 && graph->num_nodes > 1) {
        fprintf(stderr, "velo: validation error: graph has no edges\n");
        errors++;
    }
    if (!graph->nodes || !graph->edges) {
        fprintf(stderr, "velo: validation error: null node/edge arrays\n");
        errors++;
        if (out_errors) *out_errors = errors;
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* Validate edges */
    uint32_t saturated_distances = 0;
    uint32_t saturated_durations = 0;
    uint32_t zero_distances = 0;
    uint32_t invalid_targets = 0;

    for (uint32_t i = 0; i < graph->num_edges; i++) {
        const VLEdge *edge = &graph->edges[i];

        /* Check target is valid */
        if (edge->target >= graph->num_nodes) {
            invalid_targets++;
        }

        /* Check for saturated values (may indicate overflow during construction) */
        if (edge->distance == UINT32_MAX) {
            saturated_distances++;
        }
        if (edge->duration == UINT16_MAX) {
            saturated_durations++;
        }

        /* Check for zero distance (suspicious unless self-loop) */
        if (edge->distance == 0) {
            zero_distances++;
        }
    }

    if (invalid_targets > 0) {
        fprintf(stderr, "velo: validation error: %u edges have invalid targets\n", invalid_targets);
        errors++;
    }

    /* Warn about saturated values (not errors, but suspicious) */
    if (saturated_distances > 0) {
        fprintf(stderr, "velo: validation warning: %u edges have saturated distance (UINT32_MAX)\n",
                saturated_distances);
    }
    if (saturated_durations > 0) {
        fprintf(stderr, "velo: validation warning: %u edges have saturated duration (UINT16_MAX)\n",
                saturated_durations);
    }

    /* Validate node edge_start/edge_count consistency */
    uint32_t expected_offset = 0;
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        const VLNode *node = &graph->nodes[i];

        if (node->edge_start != expected_offset) {
            fprintf(stderr, "velo: validation error: node %u edge_start mismatch (expected %u, got %u)\n",
                    i, expected_offset, node->edge_start);
            errors++;
            break;  /* Stop after first error to avoid flood */
        }

        if (node->edge_start + node->edge_count > graph->num_edges) {
            fprintf(stderr, "velo: validation error: node %u edges exceed array bounds\n", i);
            errors++;
            break;
        }

        expected_offset += node->edge_count;
    }

    if (expected_offset != graph->num_edges) {
        fprintf(stderr, "velo: validation error: total edge count mismatch (sum=%u, num_edges=%u)\n",
                expected_offset, graph->num_edges);
        errors++;
    }

    if (out_errors) *out_errors = errors;
    return errors == 0 ? VL_OK : VL_ERROR_INVALID_ARGUMENT;
}
