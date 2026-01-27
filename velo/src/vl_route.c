/*
 * vl_route.c - Routing algorithms
 *
 * Implements:
 * - Unidirectional Dijkstra
 * - Bidirectional Dijkstra (using reverse graph index)
 * - Unidirectional A* with haversine heuristic
 * - Bidirectional A* with consistent potential
 *
 * Optimizations:
 * - Lazy initialization with timestamps (avoids O(V) init)
 * - Reverse graph index for O(degree) backward expansion
 * - Query context for memory reuse
 * - OpenMP SIMD for tight loops
 */

#include "vl_types.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef _WIN32
#include <windows.h>
static double get_time_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <sys/time.h>
static double get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
#endif

/* Forward declarations */
double vl_haversine(VLCoord a, VLCoord b);
double vl_haversine_fixed(VLCoordFixed a, VLCoordFixed b);

VLHeap *vl_heap_create(size_t num_nodes);
void vl_heap_free(VLHeap *heap);
void vl_heap_clear(VLHeap *heap);
int vl_heap_empty(const VLHeap *heap);
int vl_heap_contains(const VLHeap *heap, uint32_t node);
VLStatus vl_heap_push(VLHeap *heap, uint32_t node, double priority);
VLStatus vl_heap_pop(VLHeap *heap, VLHeapEntry *entry);
VLStatus vl_heap_peek(const VLHeap *heap, VLHeapEntry *entry);
VLStatus vl_heap_decrease_key(VLHeap *heap, uint32_t node, double new_priority);

/* Bucket heap forward declarations */
typedef struct VLBucketHeap VLBucketHeap;
VLBucketHeap *vl_bucket_heap_create(uint32_t num_nodes);
void vl_bucket_heap_free(VLBucketHeap *heap);
int vl_bucket_heap_empty(const VLBucketHeap *heap);
VLStatus vl_bucket_heap_push(VLBucketHeap *heap, uint32_t node, double priority);
VLStatus vl_bucket_heap_pop(VLBucketHeap *heap, VLHeapEntry *entry);

uint32_t vl_graph_nearest_node(const VLGraph *graph, VLCoord coord);
void vl_query_context_free(VLQueryContext *ctx);

/* ============================================================================
 * Route Options
 * ============================================================================ */

void vl_default_options(VLRouteOptions *opts)
{
    if (!opts) return;
    opts->algorithm = VL_ALGORITHM_ASTAR_BIDIR;
    opts->weight = VL_WEIGHT_DURATION;
    opts->include_geometry = 1;
    opts->max_distance = 0;
    opts->max_duration = 0;
}

/* ============================================================================
 * Query Context (for memory reuse and lazy initialization)
 * ============================================================================ */

VLQueryContext *vl_query_context_create(const VLGraph *graph)
{
    if (!graph) return NULL;

    VLQueryContext *ctx = calloc(1, sizeof(VLQueryContext));
    if (!ctx) return NULL;

    size_t n = graph->num_nodes;
    ctx->num_nodes = (uint32_t)n;

    ctx->dist_fwd = malloc(n * sizeof(double));
    ctx->dist_bwd = malloc(n * sizeof(double));
    ctx->parent_fwd = malloc(n * sizeof(uint32_t));
    ctx->parent_bwd = malloc(n * sizeof(uint32_t));
    ctx->timestamp_fwd = calloc(n, sizeof(uint32_t));
    ctx->timestamp_bwd = calloc(n, sizeof(uint32_t));
    ctx->settled_timestamp_fwd = calloc(n, sizeof(uint32_t));
    ctx->settled_timestamp_bwd = calloc(n, sizeof(uint32_t));
    ctx->heap_fwd = vl_heap_create(n);
    ctx->heap_bwd = vl_heap_create(n);

    if (!ctx->dist_fwd || !ctx->dist_bwd || !ctx->parent_fwd || !ctx->parent_bwd ||
        !ctx->timestamp_fwd || !ctx->timestamp_bwd ||
        !ctx->settled_timestamp_fwd || !ctx->settled_timestamp_bwd ||
        !ctx->heap_fwd || !ctx->heap_bwd) {
        vl_query_context_free(ctx);
        return NULL;
    }

    ctx->current_timestamp = 0;
    return ctx;
}

void vl_query_context_free(VLQueryContext *ctx)
{
    if (!ctx) return;
    free(ctx->dist_fwd);
    free(ctx->dist_bwd);
    free(ctx->parent_fwd);
    free(ctx->parent_bwd);
    free(ctx->timestamp_fwd);
    free(ctx->timestamp_bwd);
    free(ctx->settled_timestamp_fwd);
    free(ctx->settled_timestamp_bwd);
    vl_heap_free(ctx->heap_fwd);
    vl_heap_free(ctx->heap_bwd);
    free(ctx);
}

/* Lazy initialization macros */
#define LAZY_INIT_FWD(ctx, node) do { \
    if ((ctx)->timestamp_fwd[node] != (ctx)->current_timestamp) { \
        (ctx)->dist_fwd[node] = VL_INF; \
        (ctx)->parent_fwd[node] = VL_INVALID_NODE; \
        (ctx)->timestamp_fwd[node] = (ctx)->current_timestamp; \
    } \
} while(0)

#define LAZY_INIT_BWD(ctx, node) do { \
    if ((ctx)->timestamp_bwd[node] != (ctx)->current_timestamp) { \
        (ctx)->dist_bwd[node] = VL_INF; \
        (ctx)->parent_bwd[node] = VL_INVALID_NODE; \
        (ctx)->timestamp_bwd[node] = (ctx)->current_timestamp; \
    } \
} while(0)

#define IS_SETTLED_FWD(ctx, node) \
    ((ctx)->settled_timestamp_fwd[node] == (ctx)->current_timestamp)

#define IS_SETTLED_BWD(ctx, node) \
    ((ctx)->settled_timestamp_bwd[node] == (ctx)->current_timestamp)

#define SET_SETTLED_FWD(ctx, node) \
    ((ctx)->settled_timestamp_fwd[node] = (ctx)->current_timestamp)

#define SET_SETTLED_BWD(ctx, node) \
    ((ctx)->settled_timestamp_bwd[node] = (ctx)->current_timestamp)

/* ============================================================================
 * Route Result Management
 * ============================================================================ */

void vl_free_route(VLRoute *route)
{
    if (!route) return;
    free(route->coords);
    free(route->node_indices);
    memset(route, 0, sizeof(VLRoute));
}

/* ============================================================================
 * Edge Weight Calculation
 * ============================================================================ */

static inline double edge_weight(const VLEdge *edge, VLWeightType weight)
{
    if (weight == VL_WEIGHT_DISTANCE) {
        return edge->distance / 1000.0;
    } else {
        return edge->duration / 10.0;
    }
}

/* ============================================================================
 * Path Reconstruction
 * ============================================================================ */

static VLStatus reconstruct_path(const VLGraph *graph, const uint32_t *parent,
                                 uint32_t source, uint32_t target,
                                 VLRoute *route, int include_geometry)
{
    int path_len = 0;
    uint32_t node = target;
    while (node != source && node != VL_INVALID_NODE) {
        path_len++;
        node = parent[node];
    }
    if (node == VL_INVALID_NODE) {
        return VL_ERROR_NO_ROUTE;
    }
    path_len++;

    route->node_indices = malloc(path_len * sizeof(uint32_t));
    if (!route->node_indices) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    route->num_nodes = path_len;

    if (include_geometry) {
        route->coords = malloc(path_len * sizeof(VLCoord));
        if (!route->coords) {
            free(route->node_indices);
            route->node_indices = NULL;
            return VL_ERROR_OUT_OF_MEMORY;
        }
        route->num_coords = path_len;
    }

    node = target;
    for (int i = path_len - 1; i >= 0; i--) {
        route->node_indices[i] = node;
        if (include_geometry) {
            route->coords[i] = VL_FIXED_TO_COORD(graph->nodes[node].coord);
        }
        if (i > 0) {
            node = parent[node];
        }
    }

    return VL_OK;
}

static VLStatus reconstruct_bidir_path(const VLGraph *graph,
                                       const uint32_t *parent_fwd,
                                       const uint32_t *parent_bwd,
                                       uint32_t source, uint32_t target,
                                       uint32_t meeting_node,
                                       VLRoute *route, int include_geometry)
{
    int fwd_len = 0;
    uint32_t node = meeting_node;
    while (node != source && node != VL_INVALID_NODE) {
        fwd_len++;
        node = parent_fwd[node];
    }
    if (node == VL_INVALID_NODE) {
        return VL_ERROR_NO_ROUTE;
    }
    fwd_len++;

    int bwd_len = 0;
    node = meeting_node;
    if (parent_bwd[node] != VL_INVALID_NODE) {
        node = parent_bwd[node];
        while (node != target && node != VL_INVALID_NODE) {
            bwd_len++;
            node = parent_bwd[node];
        }
        if (node == VL_INVALID_NODE) {
            return VL_ERROR_NO_ROUTE;
        }
        bwd_len++;
    }

    int path_len = fwd_len + bwd_len;

    route->node_indices = malloc(path_len * sizeof(uint32_t));
    if (!route->node_indices) {
        return VL_ERROR_OUT_OF_MEMORY;
    }
    route->num_nodes = path_len;

    if (include_geometry) {
        route->coords = malloc(path_len * sizeof(VLCoord));
        if (!route->coords) {
            free(route->node_indices);
            route->node_indices = NULL;
            return VL_ERROR_OUT_OF_MEMORY;
        }
        route->num_coords = path_len;
    }

    node = meeting_node;
    for (int i = fwd_len - 1; i >= 0; i--) {
        route->node_indices[i] = node;
        if (include_geometry) {
            route->coords[i] = VL_FIXED_TO_COORD(graph->nodes[node].coord);
        }
        if (i > 0) {
            node = parent_fwd[node];
        }
    }

    if (bwd_len > 0) {
        node = parent_bwd[meeting_node];
        for (int i = 0; i < bwd_len; i++) {
            route->node_indices[fwd_len + i] = node;
            if (include_geometry) {
                route->coords[fwd_len + i] = VL_FIXED_TO_COORD(graph->nodes[node].coord);
            }
            node = parent_bwd[node];
        }
    }

    return VL_OK;
}

static void calculate_metrics(const VLGraph *graph, VLRoute *route)
{
    route->distance_m = 0;
    route->duration_s = 0;

    for (int i = 0; i < route->num_nodes - 1; i++) {
        uint32_t from = route->node_indices[i];
        uint32_t to = route->node_indices[i + 1];

        const VLNode *node = &graph->nodes[from];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            const VLEdge *edge = &graph->edges[node->edge_start + e];
            if (edge->target == to) {
                route->distance_m += edge->distance / 1000.0;
                route->duration_s += edge->duration / 10.0;
                break;
            }
        }
    }
}

/* ============================================================================
 * Heuristic Function
 * ============================================================================ */

static inline double heuristic(const VLGraph *graph, uint32_t node, uint32_t target,
                               VLWeightType weight)
{
    double dist_m = vl_haversine_fixed(graph->nodes[node].coord,
                                        graph->nodes[target].coord);

    if (weight == VL_WEIGHT_DISTANCE) {
        return dist_m;
    } else {
        double speed_mps = VL_SPEED_MOTORWAY * (1000.0 / 3600.0);
        return dist_m / speed_mps;
    }
}

/* ============================================================================
 * Unidirectional Dijkstra (with context)
 * ============================================================================ */

static VLStatus dijkstra_ctx(const VLGraph *graph, VLQueryContext *ctx,
                             uint32_t source, uint32_t target,
                             const VLRouteOptions *opts, VLRoute *route)
{
    double start_time = get_time_ms();

    /* Increment timestamp for lazy init */
    ctx->current_timestamp++;
    if (ctx->current_timestamp == 0) ctx->current_timestamp = 1;

    vl_heap_clear(ctx->heap_fwd);

    LAZY_INIT_FWD(ctx, source);
    ctx->dist_fwd[source] = 0;
    vl_heap_push(ctx->heap_fwd, source, 0);

    uint32_t nodes_explored = 0;
    VLStatus status = VL_ERROR_NO_ROUTE;

    while (!vl_heap_empty(ctx->heap_fwd)) {
        VLHeapEntry entry;
        vl_heap_pop(ctx->heap_fwd, &entry);

        uint32_t u = entry.node;

        LAZY_INIT_FWD(ctx, u);
        if (entry.priority > ctx->dist_fwd[u]) continue;

        nodes_explored++;

        if (u == target) {
            status = VL_OK;
            break;
        }

        if (opts->max_distance > 0 && ctx->dist_fwd[u] > opts->max_distance) break;
        if (opts->max_duration > 0 && ctx->dist_fwd[u] > opts->max_duration) break;

        const VLNode *node = &graph->nodes[u];

        /* SIMD hint for edge relaxation */
        #pragma omp simd
        for (uint32_t e = 0; e < node->edge_count; e++) {
            const VLEdge *edge = &graph->edges[node->edge_start + e];
            uint32_t v = edge->target;
            double w = edge_weight(edge, opts->weight);
            double new_dist = ctx->dist_fwd[u] + w;

            LAZY_INIT_FWD(ctx, v);
            if (new_dist < ctx->dist_fwd[v]) {
                ctx->dist_fwd[v] = new_dist;
                ctx->parent_fwd[v] = u;
                vl_heap_push(ctx->heap_fwd, v, new_dist);
            }
        }
    }

    route->nodes_explored = nodes_explored;
    route->search_time_ms = get_time_ms() - start_time;

    if (status == VL_OK) {
        status = reconstruct_path(graph, ctx->parent_fwd, source, target, route,
                                  opts->include_geometry);
        if (status == VL_OK) {
            calculate_metrics(graph, route);
        }
    }

    route->status = status;
    return status;
}

/* ============================================================================
 * Bidirectional Dijkstra (with reverse index)
 * ============================================================================ */

static VLStatus dijkstra_bidir_ctx(const VLGraph *graph, VLQueryContext *ctx,
                                   uint32_t source, uint32_t target,
                                   const VLRouteOptions *opts, VLRoute *route)
{
    /* Check if reverse index exists */
    if (!graph->rev_edge_start || !graph->rev_edges) {
        /* Fall back to unidirectional */
        return dijkstra_ctx(graph, ctx, source, target, opts, route);
    }

    double start_time = get_time_ms();

    ctx->current_timestamp++;
    if (ctx->current_timestamp == 0) ctx->current_timestamp = 1;

    vl_heap_clear(ctx->heap_fwd);
    vl_heap_clear(ctx->heap_bwd);

    LAZY_INIT_FWD(ctx, source);
    LAZY_INIT_BWD(ctx, target);

    ctx->dist_fwd[source] = 0;
    ctx->dist_bwd[target] = 0;
    vl_heap_push(ctx->heap_fwd, source, 0);
    vl_heap_push(ctx->heap_bwd, target, 0);

    uint32_t nodes_explored = 0;
    double best_path = VL_INF;
    uint32_t meeting_node = VL_INVALID_NODE;

    while (!vl_heap_empty(ctx->heap_fwd) || !vl_heap_empty(ctx->heap_bwd)) {
        /* Peek both heaps to determine which to expand */
        double min_fwd = VL_INF, min_bwd = VL_INF;
        VLHeapEntry peek_fwd, peek_bwd;

        if (!vl_heap_empty(ctx->heap_fwd)) {
            vl_heap_peek(ctx->heap_fwd, &peek_fwd);
            min_fwd = peek_fwd.priority;
        }
        if (!vl_heap_empty(ctx->heap_bwd)) {
            vl_heap_peek(ctx->heap_bwd, &peek_bwd);
            min_bwd = peek_bwd.priority;
        }

        /* Termination: if sum of minimum distances exceeds best path, we're done */
        if (min_fwd + min_bwd >= best_path) {
            break;
        }

        /* Expand from the direction with smaller minimum */
        if (min_fwd <= min_bwd) {
            /* Pop from forward heap */
            vl_heap_pop(ctx->heap_fwd, &peek_fwd);
            uint32_t u = peek_fwd.node;
            LAZY_INIT_FWD(ctx, u);

            if (!IS_SETTLED_FWD(ctx, u)) {
                SET_SETTLED_FWD(ctx, u);
                nodes_explored++;

                /* Check if backward search settled this node */
                if (IS_SETTLED_BWD(ctx, u)) {
                    LAZY_INIT_BWD(ctx, u);
                    double path_len = ctx->dist_fwd[u] + ctx->dist_bwd[u];
                    if (path_len < best_path) {
                        best_path = path_len;
                        meeting_node = u;
                    }
                }

                /* Relax forward edges */
                const VLNode *node = &graph->nodes[u];
                for (uint32_t e = 0; e < node->edge_count; e++) {
                    const VLEdge *edge = &graph->edges[node->edge_start + e];
                    uint32_t v = edge->target;
                    if (IS_SETTLED_FWD(ctx, v)) continue;

                    double w = edge_weight(edge, opts->weight);
                    LAZY_INIT_FWD(ctx, v);
                    double new_dist = ctx->dist_fwd[u] + w;

                    if (new_dist < ctx->dist_fwd[v]) {
                        ctx->dist_fwd[v] = new_dist;
                        ctx->parent_fwd[v] = u;
                        vl_heap_push(ctx->heap_fwd, v, new_dist);

                        /* Check for meeting point */
                        LAZY_INIT_BWD(ctx, v);
                        if (ctx->dist_bwd[v] < VL_INF) {
                            double path_len = new_dist + ctx->dist_bwd[v];
                            if (path_len < best_path) {
                                best_path = path_len;
                                meeting_node = v;
                            }
                        }
                    }
                }
            }
        } else {
            /* Pop from backward heap */
            vl_heap_pop(ctx->heap_bwd, &peek_bwd);
            uint32_t u = peek_bwd.node;
            LAZY_INIT_BWD(ctx, u);

            if (!IS_SETTLED_BWD(ctx, u)) {
                SET_SETTLED_BWD(ctx, u);
                nodes_explored++;

                /* Check if forward search settled this node */
                if (IS_SETTLED_FWD(ctx, u)) {
                    LAZY_INIT_FWD(ctx, u);
                    double path_len = ctx->dist_fwd[u] + ctx->dist_bwd[u];
                    if (path_len < best_path) {
                        best_path = path_len;
                        meeting_node = u;
                    }
                }

                /* Relax backward edges using reverse index - O(degree) */
                uint32_t rev_start = graph->rev_edge_start[u];
                uint32_t rev_count = graph->rev_edge_count[u];

                for (uint32_t r = 0; r < rev_count; r++) {
                    uint32_t v = graph->rev_edges[rev_start + r];
                    uint32_t edge_idx = graph->rev_edge_idx[rev_start + r];
                    const VLEdge *edge = &graph->edges[edge_idx];

                    if (IS_SETTLED_BWD(ctx, v)) continue;

                    double w = edge_weight(edge, opts->weight);
                    LAZY_INIT_BWD(ctx, v);
                    double new_dist = ctx->dist_bwd[u] + w;

                    if (new_dist < ctx->dist_bwd[v]) {
                        ctx->dist_bwd[v] = new_dist;
                        ctx->parent_bwd[v] = u;
                        vl_heap_push(ctx->heap_bwd, v, new_dist);

                        /* Check for meeting point */
                        LAZY_INIT_FWD(ctx, v);
                        if (ctx->dist_fwd[v] < VL_INF) {
                            double path_len = ctx->dist_fwd[v] + new_dist;
                            if (path_len < best_path) {
                                best_path = path_len;
                                meeting_node = v;
                            }
                        }
                    }
                }
            }
        }
    }

    route->nodes_explored = nodes_explored;
    route->search_time_ms = get_time_ms() - start_time;

    VLStatus status = VL_ERROR_NO_ROUTE;
    if (meeting_node != VL_INVALID_NODE) {
        status = reconstruct_bidir_path(graph, ctx->parent_fwd, ctx->parent_bwd,
                                        source, target, meeting_node,
                                        route, opts->include_geometry);
        if (status == VL_OK) {
            calculate_metrics(graph, route);
        }
    }

    route->status = status;
    return status;
}

/* ============================================================================
 * A* (with context)
 * ============================================================================ */

static VLStatus astar_ctx(const VLGraph *graph, VLQueryContext *ctx,
                          uint32_t source, uint32_t target,
                          const VLRouteOptions *opts, VLRoute *route)
{
    double start_time = get_time_ms();

    ctx->current_timestamp++;
    if (ctx->current_timestamp == 0) ctx->current_timestamp = 1;

    vl_heap_clear(ctx->heap_fwd);

    LAZY_INIT_FWD(ctx, source);
    ctx->dist_fwd[source] = 0;
    double h = heuristic(graph, source, target, opts->weight);
    vl_heap_push(ctx->heap_fwd, source, h);

    uint32_t nodes_explored = 0;
    VLStatus status = VL_ERROR_NO_ROUTE;

    while (!vl_heap_empty(ctx->heap_fwd)) {
        VLHeapEntry entry;
        vl_heap_pop(ctx->heap_fwd, &entry);

        uint32_t u = entry.node;
        nodes_explored++;

        if (u == target) {
            status = VL_OK;
            break;
        }

        LAZY_INIT_FWD(ctx, u);
        if (entry.priority > ctx->dist_fwd[u] + heuristic(graph, u, target, opts->weight) + 0.001) {
            continue;
        }

        const VLNode *node = &graph->nodes[u];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            const VLEdge *edge = &graph->edges[node->edge_start + e];
            uint32_t v = edge->target;
            double w = edge_weight(edge, opts->weight);
            double tentative_g = ctx->dist_fwd[u] + w;

            LAZY_INIT_FWD(ctx, v);
            if (tentative_g < ctx->dist_fwd[v]) {
                ctx->dist_fwd[v] = tentative_g;
                ctx->parent_fwd[v] = u;
                double f = tentative_g + heuristic(graph, v, target, opts->weight);
                vl_heap_push(ctx->heap_fwd, v, f);
            }
        }
    }

    route->nodes_explored = nodes_explored;
    route->search_time_ms = get_time_ms() - start_time;

    if (status == VL_OK) {
        status = reconstruct_path(graph, ctx->parent_fwd, source, target, route,
                                  opts->include_geometry);
        if (status == VL_OK) {
            calculate_metrics(graph, route);
        }
    }

    route->status = status;
    return status;
}

/* ============================================================================
 * Bidirectional A* (with reverse index)
 * ============================================================================ */

static inline double potential_fwd(const VLGraph *graph, uint32_t node,
                                   uint32_t source, uint32_t target, VLWeightType weight)
{
    double h_to_target = heuristic(graph, node, target, weight);
    double h_to_source = heuristic(graph, node, source, weight);
    return (h_to_target - h_to_source) / 2.0;
}

static VLStatus astar_bidir_ctx(const VLGraph *graph, VLQueryContext *ctx,
                                uint32_t source, uint32_t target,
                                const VLRouteOptions *opts, VLRoute *route)
{
    if (!graph->rev_edge_start || !graph->rev_edges) {
        return astar_ctx(graph, ctx, source, target, opts, route);
    }

    double start_time = get_time_ms();

    ctx->current_timestamp++;
    if (ctx->current_timestamp == 0) ctx->current_timestamp = 1;

    vl_heap_clear(ctx->heap_fwd);
    vl_heap_clear(ctx->heap_bwd);

    LAZY_INIT_FWD(ctx, source);
    LAZY_INIT_BWD(ctx, target);

    ctx->dist_fwd[source] = 0;
    ctx->dist_bwd[target] = 0;

    double p_source = potential_fwd(graph, source, source, target, opts->weight);
    double p_target = -potential_fwd(graph, target, source, target, opts->weight);

    vl_heap_push(ctx->heap_fwd, source, p_source);
    vl_heap_push(ctx->heap_bwd, target, p_target);

    uint32_t nodes_explored = 0;
    double best_path = VL_INF;
    uint32_t meeting_node = VL_INVALID_NODE;

    while (!vl_heap_empty(ctx->heap_fwd) || !vl_heap_empty(ctx->heap_bwd)) {
        /* Peek both heaps to determine which to expand */
        double min_fwd = VL_INF, min_bwd = VL_INF;
        VLHeapEntry peek_fwd, peek_bwd;

        if (!vl_heap_empty(ctx->heap_fwd)) {
            vl_heap_peek(ctx->heap_fwd, &peek_fwd);
            min_fwd = peek_fwd.priority;
        }
        if (!vl_heap_empty(ctx->heap_bwd)) {
            vl_heap_peek(ctx->heap_bwd, &peek_bwd);
            min_bwd = peek_bwd.priority;
        }

        /* Termination: if sum of min f-values exceeds best path, we're done
         * For A*, this is conservative but correct (may explore a few extra nodes) */
        if (min_fwd + min_bwd >= best_path) {
            break;
        }

        /* Expand from the direction with smaller f-value */
        if (min_fwd <= min_bwd) {
            vl_heap_pop(ctx->heap_fwd, &peek_fwd);
            uint32_t u = peek_fwd.node;
            LAZY_INIT_FWD(ctx, u);

            if (!IS_SETTLED_FWD(ctx, u)) {
                SET_SETTLED_FWD(ctx, u);
                nodes_explored++;

                if (IS_SETTLED_BWD(ctx, u)) {
                    LAZY_INIT_BWD(ctx, u);
                    double path_len = ctx->dist_fwd[u] + ctx->dist_bwd[u];
                    if (path_len < best_path) {
                        best_path = path_len;
                        meeting_node = u;
                    }
                }

                const VLNode *node = &graph->nodes[u];
                for (uint32_t e = 0; e < node->edge_count; e++) {
                    const VLEdge *edge = &graph->edges[node->edge_start + e];
                    uint32_t v = edge->target;
                    if (IS_SETTLED_FWD(ctx, v)) continue;

                    double w = edge_weight(edge, opts->weight);
                    LAZY_INIT_FWD(ctx, v);
                    double tentative_g = ctx->dist_fwd[u] + w;

                    if (tentative_g < ctx->dist_fwd[v]) {
                        ctx->dist_fwd[v] = tentative_g;
                        ctx->parent_fwd[v] = u;
                        double p = potential_fwd(graph, v, source, target, opts->weight);
                        vl_heap_push(ctx->heap_fwd, v, tentative_g + p);

                        LAZY_INIT_BWD(ctx, v);
                        if (ctx->dist_bwd[v] < VL_INF) {
                            double path_len = tentative_g + ctx->dist_bwd[v];
                            if (path_len < best_path) {
                                best_path = path_len;
                                meeting_node = v;
                            }
                        }
                    }
                }
            }
        } else {
            vl_heap_pop(ctx->heap_bwd, &peek_bwd);
            uint32_t u = peek_bwd.node;
            LAZY_INIT_BWD(ctx, u);

            if (!IS_SETTLED_BWD(ctx, u)) {
                SET_SETTLED_BWD(ctx, u);
                nodes_explored++;

                if (IS_SETTLED_FWD(ctx, u)) {
                    LAZY_INIT_FWD(ctx, u);
                    double path_len = ctx->dist_fwd[u] + ctx->dist_bwd[u];
                    if (path_len < best_path) {
                        best_path = path_len;
                        meeting_node = u;
                    }
                }

                /* Use reverse index for backward expansion */
                uint32_t rev_start = graph->rev_edge_start[u];
                uint32_t rev_count = graph->rev_edge_count[u];

                for (uint32_t r = 0; r < rev_count; r++) {
                    uint32_t v = graph->rev_edges[rev_start + r];
                    uint32_t edge_idx = graph->rev_edge_idx[rev_start + r];
                    const VLEdge *edge = &graph->edges[edge_idx];

                    if (IS_SETTLED_BWD(ctx, v)) continue;

                    double w = edge_weight(edge, opts->weight);
                    LAZY_INIT_BWD(ctx, v);
                    double tentative_g = ctx->dist_bwd[u] + w;

                    if (tentative_g < ctx->dist_bwd[v]) {
                        ctx->dist_bwd[v] = tentative_g;
                        ctx->parent_bwd[v] = u;
                        double p = -potential_fwd(graph, v, source, target, opts->weight);
                        vl_heap_push(ctx->heap_bwd, v, tentative_g + p);

                        LAZY_INIT_FWD(ctx, v);
                        if (ctx->dist_fwd[v] < VL_INF) {
                            double path_len = ctx->dist_fwd[v] + tentative_g;
                            if (path_len < best_path) {
                                best_path = path_len;
                                meeting_node = v;
                            }
                        }
                    }
                }
            }
        }
    }

    route->nodes_explored = nodes_explored;
    route->search_time_ms = get_time_ms() - start_time;

    VLStatus status = VL_ERROR_NO_ROUTE;
    if (meeting_node != VL_INVALID_NODE) {
        status = reconstruct_bidir_path(graph, ctx->parent_fwd, ctx->parent_bwd,
                                        source, target, meeting_node,
                                        route, opts->include_geometry);
        if (status == VL_OK) {
            calculate_metrics(graph, route);
        }
    }

    route->status = status;
    return status;
}

/* ============================================================================
 * Main Routing Function (with context)
 * ============================================================================ */

VLStatus vl_route_with_context(const VLGraph *graph, VLQueryContext *ctx,
                               uint32_t source, uint32_t target,
                               const VLRouteOptions *opts, VLRoute *route)
{
    if (!graph || !ctx || !route) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    if (source >= graph->num_nodes || target >= graph->num_nodes) {
        return VL_ERROR_NODE_NOT_FOUND;
    }

    memset(route, 0, sizeof(VLRoute));

    if (source == target) {
        route->status = VL_OK;
        route->distance_m = 0;
        route->duration_s = 0;
        route->num_nodes = 1;
        route->node_indices = malloc(sizeof(uint32_t));
        if (route->node_indices) {
            route->node_indices[0] = source;
        }
        if (opts && opts->include_geometry) {
            route->num_coords = 1;
            route->coords = malloc(sizeof(VLCoord));
            if (route->coords) {
                route->coords[0] = VL_FIXED_TO_COORD(graph->nodes[source].coord);
            }
        }
        return VL_OK;
    }

    VLRouteOptions default_opts;
    if (!opts) {
        vl_default_options(&default_opts);
        opts = &default_opts;
    }

    switch (opts->algorithm) {
    case VL_ALGORITHM_DIJKSTRA:
        return dijkstra_ctx(graph, ctx, source, target, opts, route);

    case VL_ALGORITHM_DIJKSTRA_BIDIR:
        return dijkstra_bidir_ctx(graph, ctx, source, target, opts, route);

    case VL_ALGORITHM_ASTAR:
        return astar_ctx(graph, ctx, source, target, opts, route);

    case VL_ALGORITHM_ASTAR_BIDIR:
        return astar_bidir_ctx(graph, ctx, source, target, opts, route);

    default:
        return VL_ERROR_INVALID_ARGUMENT;
    }
}

/* ============================================================================
 * Main Routing Function (allocates temporary context)
 * ============================================================================ */

VLStatus vl_route(const VLGraph *graph, uint32_t source, uint32_t target,
                  const VLRouteOptions *opts, VLRoute *route)
{
    if (!graph || !route) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    VLQueryContext *ctx = vl_query_context_create(graph);
    if (!ctx) {
        return VL_ERROR_OUT_OF_MEMORY;
    }

    VLStatus status = vl_route_with_context(graph, ctx, source, target, opts, route);

    vl_query_context_free(ctx);
    return status;
}

VLStatus vl_route_coords(const VLGraph *graph, VLCoord origin, VLCoord destination,
                         const VLRouteOptions *opts, VLRoute *route)
{
    if (!graph || !route) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    uint32_t source = vl_graph_nearest_node(graph, origin);
    uint32_t target = vl_graph_nearest_node(graph, destination);

    if (source == VL_INVALID_NODE || target == VL_INVALID_NODE) {
        return VL_ERROR_NODE_NOT_FOUND;
    }

    return vl_route(graph, source, target, opts, route);
}

/* ============================================================================
 * A* with Landmarks (ALT algorithm)
 * ============================================================================ */

/* External landmark heuristic function */
double vl_landmarks_heuristic(const VLLandmarks *lm, uint32_t from, uint32_t to);

VLStatus vl_route_astar_landmarks(const VLGraph *graph, const VLLandmarks *lm,
                                   uint32_t source, uint32_t target,
                                   const VLRouteOptions *opts, VLRoute *route)
{
    if (!graph || !lm || !route) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    if (source >= graph->num_nodes || target >= graph->num_nodes) {
        return VL_ERROR_NODE_NOT_FOUND;
    }

    memset(route, 0, sizeof(VLRoute));

    /* Handle same source and target */
    if (source == target) {
        route->status = VL_OK;
        route->distance_m = 0;
        route->duration_s = 0;
        route->num_nodes = 1;
        route->node_indices = malloc(sizeof(uint32_t));
        if (route->node_indices) {
            route->node_indices[0] = source;
        }
        return VL_OK;
    }

    double start_time = get_time_ms();

    /* Use default options if not provided */
    VLRouteOptions default_opts;
    if (!opts) {
        vl_default_options(&default_opts);
        opts = &default_opts;
    }

    /* Allocate temporary arrays */
    double *dist = malloc(graph->num_nodes * sizeof(double));
    uint32_t *parent = malloc(graph->num_nodes * sizeof(uint32_t));
    uint8_t *visited = calloc(graph->num_nodes, sizeof(uint8_t));

    if (!dist || !parent || !visited) {
        free(dist);
        free(parent);
        free(visited);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Initialize */
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        dist[i] = VL_INF;
        parent[i] = VL_INVALID_NODE;
    }
    dist[source] = 0;

    /* Create heap */
    VLHeap *heap = vl_heap_create(graph->num_nodes);
    if (!heap) {
        free(dist);
        free(parent);
        free(visited);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Initial heuristic using landmarks */
    double h = vl_landmarks_heuristic(lm, source, target);
    vl_heap_push(heap, source, h);

    uint32_t nodes_explored = 0;
    VLStatus status = VL_ERROR_NO_ROUTE;

    while (!vl_heap_empty(heap)) {
        VLHeapEntry entry;
        vl_heap_pop(heap, &entry);

        uint32_t u = entry.node;

        if (visited[u]) continue;
        visited[u] = 1;
        nodes_explored++;

        if (u == target) {
            status = VL_OK;
            break;
        }

        const VLNode *node = &graph->nodes[u];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            const VLEdge *edge = &graph->edges[node->edge_start + e];
            uint32_t v = edge->target;

            if (visited[v]) continue;

            double w = (opts->weight == VL_WEIGHT_DURATION) ?
                       (edge->duration * 0.1) : (edge->distance * 0.001);
            double tentative_g = dist[u] + w;

            if (tentative_g < dist[v]) {
                dist[v] = tentative_g;
                parent[v] = u;

                /* Use landmark heuristic for f-value */
                double h_v = vl_landmarks_heuristic(lm, v, target);
                double f = tentative_g + h_v;
                vl_heap_push(heap, v, f);
            }
        }
    }

    route->nodes_explored = nodes_explored;
    route->search_time_ms = get_time_ms() - start_time;

    if (status == VL_OK) {
        status = reconstruct_path(graph, parent, source, target, route,
                                  opts->include_geometry);
        if (status == VL_OK) {
            calculate_metrics(graph, route);
        }
    }

    route->status = status;

    vl_heap_free(heap);
    free(dist);
    free(parent);
    free(visited);

    return status;
}

/* ============================================================================
 * Dijkstra with Bucket Heap (O(1) amortized operations)
 * ============================================================================ */

VLStatus vl_route_dijkstra_bucket(const VLGraph *graph, uint32_t source, uint32_t target,
                                   const VLRouteOptions *opts, VLRoute *route)
{
    if (!graph || !route) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    if (source >= graph->num_nodes || target >= graph->num_nodes) {
        return VL_ERROR_NODE_NOT_FOUND;
    }

    memset(route, 0, sizeof(VLRoute));

    /* Handle same source and target */
    if (source == target) {
        route->status = VL_OK;
        route->distance_m = 0;
        route->duration_s = 0;
        route->num_nodes = 1;
        route->node_indices = malloc(sizeof(uint32_t));
        if (route->node_indices) {
            route->node_indices[0] = source;
        }
        return VL_OK;
    }

    double start_time = get_time_ms();

    /* Use default options if not provided */
    VLRouteOptions default_opts;
    if (!opts) {
        vl_default_options(&default_opts);
        opts = &default_opts;
    }

    /* Allocate temporary arrays */
    double *dist = malloc(graph->num_nodes * sizeof(double));
    uint32_t *parent = malloc(graph->num_nodes * sizeof(uint32_t));
    uint8_t *visited = calloc(graph->num_nodes, sizeof(uint8_t));

    if (!dist || !parent || !visited) {
        free(dist);
        free(parent);
        free(visited);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    /* Initialize */
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        dist[i] = VL_INF;
        parent[i] = VL_INVALID_NODE;
    }
    dist[source] = 0;

    /* Create bucket heap */
    VLBucketHeap *heap = vl_bucket_heap_create(graph->num_nodes);
    if (!heap) {
        free(dist);
        free(parent);
        free(visited);
        return VL_ERROR_OUT_OF_MEMORY;
    }

    vl_bucket_heap_push(heap, source, 0);

    uint32_t nodes_explored = 0;
    VLStatus status = VL_ERROR_NO_ROUTE;

    while (!vl_bucket_heap_empty(heap)) {
        VLHeapEntry entry;
        vl_bucket_heap_pop(heap, &entry);

        uint32_t u = entry.node;

        if (visited[u]) continue;
        visited[u] = 1;
        nodes_explored++;

        if (u == target) {
            status = VL_OK;
            break;
        }

        const VLNode *node = &graph->nodes[u];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            const VLEdge *edge = &graph->edges[node->edge_start + e];
            uint32_t v = edge->target;

            if (visited[v]) continue;

            double w = (opts->weight == VL_WEIGHT_DURATION) ?
                       (edge->duration * 0.1) : (edge->distance * 0.001);
            double tentative = dist[u] + w;

            if (tentative < dist[v]) {
                dist[v] = tentative;
                parent[v] = u;
                vl_bucket_heap_push(heap, v, tentative);
            }
        }
    }

    route->nodes_explored = nodes_explored;
    route->search_time_ms = get_time_ms() - start_time;

    if (status == VL_OK) {
        status = reconstruct_path(graph, parent, source, target, route,
                                  opts->include_geometry);
        if (status == VL_OK) {
            calculate_metrics(graph, route);
        }
    }

    route->status = status;

    vl_bucket_heap_free(heap);
    free(dist);
    free(parent);
    free(visited);

    return status;
}
