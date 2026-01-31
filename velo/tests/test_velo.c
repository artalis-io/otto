/*
 * test_velo.c - Velo comprehensive test suite
 */

#include "velo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Test Framework
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-50s", #name); \
    fflush(stdout); \
    tests_run++; \
    test_##name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_LT(a, b) ASSERT((a) < (b))
#define ASSERT_LE(a, b) ASSERT((a) <= (b))
#define ASSERT_GT(a, b) ASSERT((a) > (b))
#define ASSERT_GE(a, b) ASSERT((a) >= (b))

#define ASSERT_NEAR(a, b, eps) ASSERT(fabs((a) - (b)) < (eps))

/* ============================================================================
 * Geo Tests
 * ============================================================================ */

TEST(haversine_same_point)
{
    VLCoord a = {47.5, 19.0};
    double dist = vl_haversine(a, a);
    ASSERT_NEAR(dist, 0.0, 0.001);
}

TEST(haversine_known_distance)
{
    /* Budapest to Vienna: ~214 km */
    VLCoord budapest = {47.4979, 19.0402};
    VLCoord vienna = {48.2082, 16.3738};
    double dist = vl_haversine(budapest, vienna);
    ASSERT_GT(dist, 210000);
    ASSERT_LT(dist, 220000);
}

TEST(haversine_antipodal)
{
    /* Points on opposite sides of Earth: ~20000 km */
    VLCoord a = {0, 0};
    VLCoord b = {0, 180};
    double dist = vl_haversine(a, b);
    ASSERT_GT(dist, 19900000);
    ASSERT_LT(dist, 20100000);
}

TEST(coord_valid)
{
    ASSERT(vl_coord_valid((VLCoord){0, 0}));
    ASSERT(vl_coord_valid((VLCoord){90, 180}));
    ASSERT(vl_coord_valid((VLCoord){-90, -180}));
    ASSERT(!vl_coord_valid((VLCoord){91, 0}));
    ASSERT(!vl_coord_valid((VLCoord){0, 181}));
}

TEST(fixed_coord_conversion)
{
    VLCoord c = {47.123456, -19.654321};
    VLCoordFixed f = VL_COORD_TO_FIXED(c);
    VLCoord c2 = VL_FIXED_TO_COORD(f);
    ASSERT_NEAR(c.lat, c2.lat, 1e-6);
    ASSERT_NEAR(c.lon, c2.lon, 1e-6);
}

/* ============================================================================
 * Heap Tests
 * ============================================================================ */

/* Forward declarations for heap functions */
VLHeap *vl_heap_create(size_t num_nodes);
void vl_heap_free(VLHeap *heap);
int vl_heap_empty(const VLHeap *heap);
size_t vl_heap_size(const VLHeap *heap);
VLStatus vl_heap_push(VLHeap *heap, uint32_t node, double priority);
VLStatus vl_heap_pop(VLHeap *heap, VLHeapEntry *entry);
int vl_heap_contains(const VLHeap *heap, uint32_t node);
VLStatus vl_heap_decrease_key(VLHeap *heap, uint32_t node, double new_priority);

TEST(heap_create_free)
{
    VLHeap *heap = vl_heap_create(100);
    ASSERT_NE(heap, NULL);
    ASSERT(vl_heap_empty(heap));
    vl_heap_free(heap);
}

TEST(heap_push_pop)
{
    VLHeap *heap = vl_heap_create(100);

    vl_heap_push(heap, 5, 5.0);
    vl_heap_push(heap, 2, 2.0);
    vl_heap_push(heap, 8, 8.0);
    vl_heap_push(heap, 1, 1.0);
    vl_heap_push(heap, 3, 3.0);

    ASSERT_EQ(vl_heap_size(heap), 5);

    VLHeapEntry entry;

    vl_heap_pop(heap, &entry);
    ASSERT_EQ(entry.node, 1);
    ASSERT_NEAR(entry.priority, 1.0, 0.001);

    vl_heap_pop(heap, &entry);
    ASSERT_EQ(entry.node, 2);

    vl_heap_pop(heap, &entry);
    ASSERT_EQ(entry.node, 3);

    vl_heap_pop(heap, &entry);
    ASSERT_EQ(entry.node, 5);

    vl_heap_pop(heap, &entry);
    ASSERT_EQ(entry.node, 8);

    ASSERT(vl_heap_empty(heap));

    vl_heap_free(heap);
}

TEST(heap_decrease_key)
{
    VLHeap *heap = vl_heap_create(100);

    vl_heap_push(heap, 5, 50.0);
    vl_heap_push(heap, 3, 30.0);
    vl_heap_push(heap, 7, 70.0);

    /* Decrease key of node 7 to make it minimum */
    vl_heap_decrease_key(heap, 7, 10.0);

    VLHeapEntry entry;
    vl_heap_pop(heap, &entry);
    ASSERT_EQ(entry.node, 7);
    ASSERT_NEAR(entry.priority, 10.0, 0.001);

    vl_heap_free(heap);
}

TEST(heap_contains)
{
    VLHeap *heap = vl_heap_create(100);

    ASSERT(!vl_heap_contains(heap, 5));

    vl_heap_push(heap, 5, 5.0);
    ASSERT(vl_heap_contains(heap, 5));
    ASSERT(!vl_heap_contains(heap, 3));

    VLHeapEntry entry;
    vl_heap_pop(heap, &entry);
    ASSERT(!vl_heap_contains(heap, 5));

    vl_heap_free(heap);
}

/* ============================================================================
 * Protobuf Tests (using shared library)
 * ============================================================================ */

#include "sh_protobuf.h"

TEST(pb_varint_small)
{
    uint8_t buf[] = {0x01};
    uint64_t value;
    int n = sh_pb_read_varint(buf, sizeof(buf), &value);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(value, 1);
}

TEST(pb_varint_300)
{
    /* 300 = 0xAC 0x02 */
    uint8_t buf[] = {0xAC, 0x02};
    uint64_t value;
    int n = sh_pb_read_varint(buf, sizeof(buf), &value);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(value, 300);
}

TEST(pb_varint_large)
{
    /* 150 = 0x96 0x01 */
    uint8_t buf[] = {0x96, 0x01};
    uint64_t value;
    int n = sh_pb_read_varint(buf, sizeof(buf), &value);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(value, 150);
}

TEST(pb_svarint_positive)
{
    /* zigzag(1) = 2 */
    uint8_t buf[] = {0x02};
    int64_t value;
    int n = sh_pb_read_svarint(buf, sizeof(buf), &value);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(value, 1);
}

TEST(pb_svarint_negative)
{
    /* zigzag(-1) = 1 */
    uint8_t buf[] = {0x01};
    int64_t value;
    int n = sh_pb_read_svarint(buf, sizeof(buf), &value);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(value, -1);
}

TEST(pb_svarint_larger)
{
    /* zigzag(-2) = 3 */
    uint8_t buf[] = {0x03};
    int64_t value;
    int n = sh_pb_read_svarint(buf, sizeof(buf), &value);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(value, -2);
}

TEST(pb_tag)
{
    /* Field 1, wire type 0 (varint) = 0x08 */
    uint8_t buf[] = {0x08};
    uint32_t field, wire;
    int n = sh_pb_read_tag(buf, sizeof(buf), &field, &wire);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(field, 1);
    ASSERT_EQ(wire, 0);
}

TEST(pb_tag_field2_string)
{
    /* Field 2, wire type 2 (length-delimited) = 0x12 */
    uint8_t buf[] = {0x12};
    uint32_t field, wire;
    int n = sh_pb_read_tag(buf, sizeof(buf), &field, &wire);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(field, 2);
    ASSERT_EQ(wire, 2);
}

TEST(pb_delta_decode)
{
    int64_t arr[] = {10, 5, -3, 7};
    sh_pb_delta_decode_i64(arr, 4);
    ASSERT_EQ(arr[0], 10);
    ASSERT_EQ(arr[1], 15);
    ASSERT_EQ(arr[2], 12);
    ASSERT_EQ(arr[3], 19);
}

/* ============================================================================
 * Graph Tests
 * ============================================================================ */

static VLGraph *create_test_graph(void)
{
    /*
     * Create a simple test graph:
     *
     *     0 --10-- 1 --15-- 2
     *     |        |        |
     *    20       25       30
     *     |        |        |
     *     3 --35-- 4 --40-- 5
     *
     * Distances in km, durations proportional
     */

    VLGraph *graph = calloc(1, sizeof(VLGraph));
    if (!graph) return NULL;

    graph->num_nodes = 6;
    graph->nodes = calloc(6, sizeof(VLNode));
    if (!graph->nodes) {
        free(graph);
        return NULL;
    }

    /* Set coordinates (arbitrary but consistent) */
    double coords[6][2] = {
        {47.5, 19.0},  /* 0 */
        {47.5, 19.1},  /* 1 */
        {47.5, 19.2},  /* 2 */
        {47.4, 19.0},  /* 3 */
        {47.4, 19.1},  /* 4 */
        {47.4, 19.2}   /* 5 */
    };

    for (int i = 0; i < 6; i++) {
        graph->nodes[i].coord.lat = (int32_t)(coords[i][0] * 1e7);
        graph->nodes[i].coord.lon = (int32_t)(coords[i][1] * 1e7);
        graph->nodes[i].osm_id = i + 1;
    }

    /* Count edges: 14 (7 bidirectional edges) */
    graph->num_edges = 14;
    graph->edges = calloc(14, sizeof(VLEdge));
    if (!graph->edges) {
        free(graph->nodes);
        free(graph);
        return NULL;
    }

    /* Edge definitions: {from, to, dist_km, speed_kmh} */
    int edge_defs[][4] = {
        {0, 1, 10, 50}, {1, 0, 10, 50},
        {1, 2, 15, 50}, {2, 1, 15, 50},
        {0, 3, 20, 40}, {3, 0, 20, 40},
        {1, 4, 25, 40}, {4, 1, 25, 40},
        {2, 5, 30, 40}, {5, 2, 30, 40},
        {3, 4, 35, 30}, {4, 3, 35, 30},
        {4, 5, 40, 30}, {5, 4, 40, 30}
    };

    /* Count edges per node first */
    for (int i = 0; i < 14; i++) {
        graph->nodes[edge_defs[i][0]].edge_count++;
    }

    /* Calculate offsets */
    uint32_t offset = 0;
    for (int i = 0; i < 6; i++) {
        graph->nodes[i].edge_start = offset;
        offset += graph->nodes[i].edge_count;
        graph->nodes[i].edge_count = 0;  /* Reset for filling */
    }

    /* Fill edges */
    for (int i = 0; i < 14; i++) {
        int from = edge_defs[i][0];
        int to = edge_defs[i][1];
        int dist_km = edge_defs[i][2];
        int speed = edge_defs[i][3];

        uint32_t idx = graph->nodes[from].edge_start + graph->nodes[from].edge_count;
        graph->edges[idx].target = (uint32_t)to;
        graph->edges[idx].distance = (uint32_t)(dist_km * 1000 * 1000);  /* km to mm */
        graph->edges[idx].duration = (uint16_t)((dist_km * 36.0) / speed);  /* deciseconds */
        graph->edges[idx].flags = 0;
        graph->nodes[from].edge_count++;
    }

    graph->owns_memory = 1;
    return graph;
}

TEST(graph_create)
{
    VLGraph *graph = create_test_graph();
    ASSERT_NE(graph, NULL);
    ASSERT_EQ(graph->num_nodes, 6);
    ASSERT_EQ(graph->num_edges, 14);
    vl_graph_free(graph);
}

TEST(graph_nearest_node)
{
    VLGraph *graph = create_test_graph();

    /* Query near node 0 */
    VLCoord query = {47.5001, 19.0001};
    uint32_t nearest = vl_graph_nearest_node(graph, query);
    ASSERT_EQ(nearest, 0);

    /* Query near node 5 */
    query = (VLCoord){47.4001, 19.1999};
    nearest = vl_graph_nearest_node(graph, query);
    ASSERT_EQ(nearest, 5);

    vl_graph_free(graph);
}

/* ============================================================================
 * Routing Tests
 * ============================================================================ */

TEST(route_same_node)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 0, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 0.0, 0.001);
    ASSERT_NEAR(route.duration_s, 0.0, 0.001);
    ASSERT_EQ(route.num_nodes, 1);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_adjacent_nodes)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 1, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 10000.0, 100.0);  /* 10 km */
    ASSERT_EQ(route.num_nodes, 2);
    ASSERT_EQ(route.node_indices[0], 0);
    ASSERT_EQ(route.node_indices[1], 1);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_shortest_path)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.weight = VL_WEIGHT_DISTANCE;

    /* Route from 0 to 5 */
    /* Shortest by distance: 0 -> 1 -> 2 -> 5 (10 + 15 + 30 = 55 km) */
    /* vs 0 -> 3 -> 4 -> 5 (20 + 35 + 40 = 95 km) */
    /* vs 0 -> 1 -> 4 -> 5 (10 + 25 + 40 = 75 km) */

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 55000.0, 100.0);
    ASSERT_EQ(route.num_nodes, 4);
    ASSERT_EQ(route.node_indices[0], 0);
    ASSERT_EQ(route.node_indices[1], 1);
    ASSERT_EQ(route.node_indices[2], 2);
    ASSERT_EQ(route.node_indices[3], 5);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_dijkstra_bidir)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA_BIDIR;
    opts.weight = VL_WEIGHT_DISTANCE;

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 55000.0, 100.0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_astar)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_ASTAR;
    opts.weight = VL_WEIGHT_DISTANCE;

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 55000.0, 100.0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_astar_bidir)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_ASTAR_BIDIR;
    opts.weight = VL_WEIGHT_DISTANCE;

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 55000.0, 100.0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_duration_weight)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.weight = VL_WEIGHT_DURATION;

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    /* Duration-optimal path may differ from distance-optimal */
    ASSERT_GT(route.duration_s, 0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_with_geometry)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.include_geometry = 1;

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NE(route.coords, NULL);
    ASSERT_EQ(route.num_coords, route.num_nodes);

    /* Verify first and last coords */
    VLCoord first = VL_FIXED_TO_COORD(graph->nodes[0].coord);
    VLCoord last = VL_FIXED_TO_COORD(graph->nodes[5].coord);
    ASSERT_NEAR(route.coords[0].lat, first.lat, 0.0001);
    ASSERT_NEAR(route.coords[route.num_coords - 1].lat, last.lat, 0.0001);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_no_geometry)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.include_geometry = 0;

    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_EQ(route.coords, NULL);
    ASSERT_EQ(route.num_coords, 0);
    ASSERT_NE(route.node_indices, NULL);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(route_coords_wrapper)
{
    VLGraph *graph = create_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);

    VLCoord origin = {47.5001, 19.0001};      /* Near node 0 */
    VLCoord destination = {47.4001, 19.1999}; /* Near node 5 */

    VLRoute route;
    VLStatus status = vl_route_coords(graph, origin, destination, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_GT(route.distance_m, 0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

/* ============================================================================
 * API Tests
 * ============================================================================ */

TEST(version)
{
    const char *ver = vl_version();
    ASSERT_NE(ver, NULL);
    ASSERT_EQ(strcmp(ver, VL_VERSION_STRING), 0);
}

TEST(status_strings)
{
    ASSERT_NE(vl_status_string(VL_OK), NULL);
    ASSERT_NE(vl_status_string(VL_ERROR_NO_ROUTE), NULL);
    ASSERT_NE(vl_status_string(VL_ERROR_OUT_OF_MEMORY), NULL);
    ASSERT_NE(vl_status_string((VLStatus)999), NULL);  /* Unknown */
}

TEST(default_options)
{
    VLRouteOptions opts;
    vl_default_options(&opts);

    ASSERT_EQ(opts.algorithm, VL_ALGORITHM_ASTAR_BIDIR);
    ASSERT_EQ(opts.weight, VL_WEIGHT_DURATION);
    ASSERT_EQ(opts.include_geometry, 1);
    ASSERT_EQ(opts.profile, VL_PROFILE_CAR);
}

/* ============================================================================
 * Vehicle Profile Tests
 * ============================================================================ */

/*
 * Create a test graph with mixed road types and access flags for profile testing:
 *
 *     0 ---(motorway)--- 1 ---(trunk)--- 2
 *     |                  |               |
 * (primary)         (secondary)     (tertiary)
 *     |                  |               |
 *     3 --(residential)- 4 --(service)-- 5
 *          [hgv=no]         [hgv=no]
 *
 * This allows testing that profiles correctly use both road types and access flags.
 */
static VLGraph *create_profile_test_graph(void)
{
    VLGraph *graph = calloc(1, sizeof(VLGraph));
    if (!graph) return NULL;

    graph->num_nodes = 6;
    graph->nodes = calloc(6, sizeof(VLNode));
    if (!graph->nodes) {
        free(graph);
        return NULL;
    }

    /* Set coordinates */
    double coords[6][2] = {
        {47.5, 19.0}, {47.5, 19.1}, {47.5, 19.2},
        {47.4, 19.0}, {47.4, 19.1}, {47.4, 19.2}
    };
    for (int i = 0; i < 6; i++) {
        graph->nodes[i].coord.lat = (int32_t)(coords[i][0] * 1e7);
        graph->nodes[i].coord.lon = (int32_t)(coords[i][1] * 1e7);
        graph->nodes[i].osm_id = i + 1;
    }

    /* 14 edges (7 bidirectional), each with specific road type and access flags */
    graph->num_edges = 14;
    graph->edges = calloc(14, sizeof(VLEdge));
    if (!graph->edges) {
        free(graph->nodes);
        free(graph);
        return NULL;
    }

    /* Edge definitions: {from, to, dist_km, flags (road_type | access)} */
    struct { int from, to, dist; uint16_t flags; } edge_defs[] = {
        {0, 1, 10, VL_EDGE_MOTORWAY},    {1, 0, 10, VL_EDGE_MOTORWAY},
        {1, 2, 15, VL_EDGE_TRUNK},       {2, 1, 15, VL_EDGE_TRUNK},
        {0, 3, 20, VL_EDGE_PRIMARY},     {3, 0, 20, VL_EDGE_PRIMARY},
        {1, 4, 25, VL_EDGE_SECONDARY},   {4, 1, 25, VL_EDGE_SECONDARY},
        {2, 5, 30, VL_EDGE_TERTIARY},    {5, 2, 30, VL_EDGE_TERTIARY},
        /* Residential with hgv=no */
        {3, 4, 35, VL_EDGE_RESIDENTIAL | VL_ACCESS_NO_TRUCK},
        {4, 3, 35, VL_EDGE_RESIDENTIAL | VL_ACCESS_NO_TRUCK},
        /* Service with hgv=no */
        {4, 5, 40, VL_EDGE_SERVICE | VL_ACCESS_NO_TRUCK},
        {5, 4, 40, VL_EDGE_SERVICE | VL_ACCESS_NO_TRUCK}
    };

    /* Count edges per node */
    for (int i = 0; i < 14; i++) {
        graph->nodes[edge_defs[i].from].edge_count++;
    }

    /* Calculate offsets */
    uint32_t offset = 0;
    for (int i = 0; i < 6; i++) {
        graph->nodes[i].edge_start = offset;
        offset += graph->nodes[i].edge_count;
        graph->nodes[i].edge_count = 0;
    }

    /* Fill edges */
    for (int i = 0; i < 14; i++) {
        int from = edge_defs[i].from;
        uint32_t idx = graph->nodes[from].edge_start + graph->nodes[from].edge_count;
        graph->edges[idx].target = (uint32_t)edge_defs[i].to;
        graph->edges[idx].distance = (uint32_t)(edge_defs[i].dist * 1000 * 1000);
        graph->edges[idx].duration = (uint16_t)(edge_defs[i].dist * 10);
        graph->edges[idx].flags = edge_defs[i].flags;
        graph->nodes[from].edge_count++;
    }

    graph->owns_memory = 1;
    return graph;
}

TEST(profile_car_all_roads)
{
    VLGraph *graph = create_profile_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.weight = VL_WEIGHT_DISTANCE;
    opts.profile = VL_PROFILE_CAR;

    /* Cars can use all roads, so shortest path 0->5 via motorway+trunk+tertiary */
    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    /* Shortest path: 0 -> 1 -> 2 -> 5 (10 + 15 + 30 = 55 km) */
    ASSERT_NEAR(route.distance_m, 55000.0, 100.0);
    ASSERT_EQ(route.num_nodes, 4);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(profile_truck_respects_hgv_no)
{
    VLGraph *graph = create_profile_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.weight = VL_WEIGHT_DISTANCE;
    opts.profile = VL_PROFILE_TRUCK;

    /* Trucks must avoid edges with VL_ACCESS_NO_TRUCK flag (hgv=no from OSM) */
    /* 3-4 and 4-5 have hgv=no, so trucks cannot go 3->4->5 */
    /* Must go 3->0->1->2->5 (primary+motorway+trunk+tertiary = 20+10+15+30 = 75 km) */
    VLRoute route;
    VLStatus status = vl_route(graph, 3, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 75000.0, 100.0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(profile_bike_avoids_motorway_trunk)
{
    VLGraph *graph = create_profile_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.weight = VL_WEIGHT_DISTANCE;
    opts.profile = VL_PROFILE_BIKE;

    /* Bikes must avoid motorway (0-1) and trunk (1-2) */
    /* From 0 to 2: cannot go 0->1->2 (motorway+trunk) */
    /* Must go via bottom: 0->3->4->5->2 (primary+residential+service+tertiary = 20+35+40+30 = 125 km) */
    VLRoute route;
    VLStatus status = vl_route(graph, 0, 2, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 125000.0, 100.0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(profile_foot_avoids_motorway_trunk)
{
    VLGraph *graph = create_profile_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.weight = VL_WEIGHT_DISTANCE;
    opts.profile = VL_PROFILE_FOOT;

    /* Pedestrians avoid motorway (0-1) and trunk (1-2) */
    /* From 0 to 2: cannot go 0->1->2 (motorway+trunk blocked) */
    /* Must go via bottom: 0->3->4->5->2 (primary+residential+service+tertiary = 20+35+40+30 = 125 km) */
    VLRoute route;
    VLStatus status = vl_route(graph, 0, 2, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 125000.0, 100.0);

    vl_free_route(&route);

    /* 3 to 5 should also work (residential + service) */
    status = vl_route(graph, 3, 5, &opts, &route);
    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 75000.0, 100.0);  /* 3->4->5 = 35+40 = 75 km */

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(profile_any_no_filtering)
{
    VLGraph *graph = create_profile_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.weight = VL_WEIGHT_DISTANCE;
    opts.profile = VL_PROFILE_ANY;

    /* ANY profile should behave same as CAR - all roads accessible */
    VLRoute route;
    VLStatus status = vl_route(graph, 0, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 55000.0, 100.0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

TEST(profile_with_astar)
{
    VLGraph *graph = create_profile_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_ASTAR;
    opts.weight = VL_WEIGHT_DISTANCE;
    opts.profile = VL_PROFILE_TRUCK;

    /* Same truck test (hgv=no) with A* algorithm */
    VLRoute route;
    VLStatus status = vl_route(graph, 3, 5, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, 75000.0, 100.0);

    vl_free_route(&route);
    vl_graph_free(graph);
}

/* ============================================================================
 * Shortest vs Fastest Routing Tests
 *
 * These tests verify that:
 * 1. Shortest (distance) routing finds the path with minimum distance
 * 2. Fastest (duration) routing finds the path with minimum time
 * 3. These can produce different results when road speeds vary
 * ============================================================================ */

/*
 * Create a test graph where shortest and fastest paths differ:
 *
 *     0 =====(highway, 10km, 100km/h)=====  1
 *     |                                     |
 * (local, 5km, 30km/h)                  (local, 5km, 30km/h)
 *     |                                     |
 *     2 -----(local, 5km, 30km/h)---------- 3
 *
 * Shortest 0->3: 0->2->3 = 10km (but slow: 20 min)
 * Fastest 0->3:  0->1->3 = 15km (but fast: 16 min)
 */
static VLGraph *create_speed_test_graph(void)
{
    VLGraph *graph = calloc(1, sizeof(VLGraph));
    if (!graph) return NULL;

    graph->num_nodes = 4;
    graph->nodes = calloc(4, sizeof(VLNode));
    if (!graph->nodes) {
        free(graph);
        return NULL;
    }

    double coords[4][2] = {
        {47.5, 19.0}, {47.5, 19.15},  /* top row */
        {47.4, 19.0}, {47.4, 19.15}   /* bottom row */
    };
    for (int i = 0; i < 4; i++) {
        graph->nodes[i].coord.lat = (int32_t)(coords[i][0] * 1e7);
        graph->nodes[i].coord.lon = (int32_t)(coords[i][1] * 1e7);
        graph->nodes[i].osm_id = i + 1;
    }

    /* 8 edges (4 bidirectional) */
    graph->num_edges = 8;
    graph->edges = calloc(8, sizeof(VLEdge));
    if (!graph->edges) {
        free(graph->nodes);
        free(graph);
        return NULL;
    }

    /* Edge definitions: {from, to, dist_km, speed_kmh, flags} */
    struct { int from, to, dist, speed; uint16_t flags; } edge_defs[] = {
        /* Highway 0-1: 10km at 100km/h = 6 min */
        {0, 1, 10, 100, VL_EDGE_MOTORWAY}, {1, 0, 10, 100, VL_EDGE_MOTORWAY},
        /* Local roads */
        {0, 2, 5, 30, VL_EDGE_RESIDENTIAL}, {2, 0, 5, 30, VL_EDGE_RESIDENTIAL},
        {1, 3, 5, 30, VL_EDGE_RESIDENTIAL}, {3, 1, 5, 30, VL_EDGE_RESIDENTIAL},
        {2, 3, 5, 30, VL_EDGE_RESIDENTIAL}, {3, 2, 5, 30, VL_EDGE_RESIDENTIAL}
    };

    /* Count edges per node */
    for (int i = 0; i < 8; i++) {
        graph->nodes[edge_defs[i].from].edge_count++;
    }

    /* Calculate offsets */
    uint32_t offset = 0;
    for (int i = 0; i < 4; i++) {
        graph->nodes[i].edge_start = offset;
        offset += graph->nodes[i].edge_count;
        graph->nodes[i].edge_count = 0;
    }

    /* Fill edges */
    for (int i = 0; i < 8; i++) {
        int from = edge_defs[i].from;
        uint32_t idx = graph->nodes[from].edge_start + graph->nodes[from].edge_count;
        graph->edges[idx].target = (uint32_t)edge_defs[i].to;
        graph->edges[idx].distance = (uint32_t)(edge_defs[i].dist * 1000 * 1000);  /* km to mm */
        /* duration in deciseconds: (dist_km / speed_kmh) * 3600 * 10 */
        graph->edges[idx].duration = (uint16_t)((edge_defs[i].dist * 36000) / edge_defs[i].speed);
        graph->edges[idx].flags = edge_defs[i].flags;
        graph->nodes[from].edge_count++;
    }

    graph->owns_memory = 1;
    return graph;
}

TEST(shortest_vs_fastest_different_paths)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;

    VLRoute shortest_route, fastest_route;

    /* Shortest path (by distance) */
    opts.weight = VL_WEIGHT_DISTANCE;
    VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
    ASSERT_EQ(status, VL_OK);

    /* Fastest path (by duration) */
    opts.weight = VL_WEIGHT_DURATION;
    status = vl_route(graph, 0, 3, &opts, &fastest_route);
    ASSERT_EQ(status, VL_OK);

    /* Shortest path: 0->2->3 = 10km */
    ASSERT_NEAR(shortest_route.distance_m, 10000.0, 100.0);

    /* Fastest path: 0->1->3 = 15km */
    ASSERT_NEAR(fastest_route.distance_m, 15000.0, 100.0);

    /* Verify shortest has less distance */
    ASSERT_LT(shortest_route.distance_m, fastest_route.distance_m);

    /* Verify fastest has less duration */
    ASSERT_LT(fastest_route.duration_s, shortest_route.duration_s);

    vl_free_route(&shortest_route);
    vl_free_route(&fastest_route);
    vl_graph_free(graph);
}

TEST(shortest_vs_fastest_car_profile)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.profile = VL_PROFILE_CAR;

    VLRoute shortest_route, fastest_route;

    /* Car can use all roads */
    opts.weight = VL_WEIGHT_DISTANCE;
    VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
    ASSERT_EQ(status, VL_OK);

    opts.weight = VL_WEIGHT_DURATION;
    status = vl_route(graph, 0, 3, &opts, &fastest_route);
    ASSERT_EQ(status, VL_OK);

    /* Verify different paths were found */
    ASSERT_LT(shortest_route.distance_m, fastest_route.distance_m);
    ASSERT_LT(fastest_route.duration_s, shortest_route.duration_s);

    vl_free_route(&shortest_route);
    vl_free_route(&fastest_route);
    vl_graph_free(graph);
}

TEST(shortest_vs_fastest_bike_profile)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.profile = VL_PROFILE_BIKE;

    VLRoute shortest_route, fastest_route;

    /* Bike avoids motorway, so both modes use same path: 0->2->3 */
    opts.weight = VL_WEIGHT_DISTANCE;
    VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
    ASSERT_EQ(status, VL_OK);

    opts.weight = VL_WEIGHT_DURATION;
    status = vl_route(graph, 0, 3, &opts, &fastest_route);
    ASSERT_EQ(status, VL_OK);

    /* Both should take same path since motorway is blocked */
    ASSERT_NEAR(shortest_route.distance_m, fastest_route.distance_m, 100.0);
    ASSERT_NEAR(shortest_route.duration_s, fastest_route.duration_s, 1.0);

    /* Path should be 0->2->3 = 10km */
    ASSERT_NEAR(shortest_route.distance_m, 10000.0, 100.0);

    vl_free_route(&shortest_route);
    vl_free_route(&fastest_route);
    vl_graph_free(graph);
}

TEST(shortest_vs_fastest_foot_profile)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;
    opts.profile = VL_PROFILE_FOOT;

    VLRoute shortest_route, fastest_route;

    /* Foot avoids motorway, so both modes use same path: 0->2->3 */
    opts.weight = VL_WEIGHT_DISTANCE;
    VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
    ASSERT_EQ(status, VL_OK);

    opts.weight = VL_WEIGHT_DURATION;
    status = vl_route(graph, 0, 3, &opts, &fastest_route);
    ASSERT_EQ(status, VL_OK);

    /* Both should take same path since motorway is blocked */
    ASSERT_NEAR(shortest_route.distance_m, fastest_route.distance_m, 100.0);

    vl_free_route(&shortest_route);
    vl_free_route(&fastest_route);
    vl_graph_free(graph);
}

TEST(fastest_always_faster_or_equal)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;

    /* Test all profiles */
    VLProfile profiles[] = {VL_PROFILE_CAR, VL_PROFILE_TRUCK, VL_PROFILE_BIKE, VL_PROFILE_FOOT, VL_PROFILE_ANY};

    for (int p = 0; p < 5; p++) {
        opts.profile = profiles[p];

        VLRoute shortest_route, fastest_route;

        opts.weight = VL_WEIGHT_DISTANCE;
        VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
        if (status != VL_OK) continue;  /* Skip if no route for this profile */

        opts.weight = VL_WEIGHT_DURATION;
        status = vl_route(graph, 0, 3, &opts, &fastest_route);
        if (status != VL_OK) {
            vl_free_route(&shortest_route);
            continue;
        }

        /* Fastest route should have duration <= shortest route's duration */
        ASSERT_LE(fastest_route.duration_s, shortest_route.duration_s + 0.1);

        /* Shortest route should have distance <= fastest route's distance */
        ASSERT_LE(shortest_route.distance_m, fastest_route.distance_m + 100.0);

        vl_free_route(&shortest_route);
        vl_free_route(&fastest_route);
    }

    vl_graph_free(graph);
}

TEST(shortest_always_shorter_or_equal)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA;

    VLRoute shortest_route, fastest_route;

    opts.weight = VL_WEIGHT_DISTANCE;
    opts.profile = VL_PROFILE_CAR;
    VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
    ASSERT_EQ(status, VL_OK);

    opts.weight = VL_WEIGHT_DURATION;
    status = vl_route(graph, 0, 3, &opts, &fastest_route);
    ASSERT_EQ(status, VL_OK);

    /* Shortest route must have distance <= fastest route distance */
    ASSERT_LE(shortest_route.distance_m, fastest_route.distance_m + 100.0);

    vl_free_route(&shortest_route);
    vl_free_route(&fastest_route);
    vl_graph_free(graph);
}

TEST(shortest_vs_fastest_with_astar)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_ASTAR;
    opts.profile = VL_PROFILE_CAR;

    VLRoute shortest_route, fastest_route;

    opts.weight = VL_WEIGHT_DISTANCE;
    VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
    ASSERT_EQ(status, VL_OK);

    opts.weight = VL_WEIGHT_DURATION;
    status = vl_route(graph, 0, 3, &opts, &fastest_route);
    ASSERT_EQ(status, VL_OK);

    /* A* should produce same optimal results as Dijkstra */
    ASSERT_NEAR(shortest_route.distance_m, 10000.0, 100.0);
    ASSERT_NEAR(fastest_route.distance_m, 15000.0, 100.0);

    vl_free_route(&shortest_route);
    vl_free_route(&fastest_route);
    vl_graph_free(graph);
}

TEST(shortest_vs_fastest_bidir_dijkstra)
{
    VLGraph *graph = create_speed_test_graph();

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_DIJKSTRA_BIDIR;
    opts.profile = VL_PROFILE_CAR;

    VLRoute shortest_route, fastest_route;

    opts.weight = VL_WEIGHT_DISTANCE;
    VLStatus status = vl_route(graph, 0, 3, &opts, &shortest_route);
    ASSERT_EQ(status, VL_OK);

    opts.weight = VL_WEIGHT_DURATION;
    status = vl_route(graph, 0, 3, &opts, &fastest_route);
    ASSERT_EQ(status, VL_OK);

    /* Bidirectional Dijkstra should produce optimal results */
    ASSERT_NEAR(shortest_route.distance_m, 10000.0, 100.0);
    ASSERT_NEAR(fastest_route.distance_m, 15000.0, 100.0);

    vl_free_route(&shortest_route);
    vl_free_route(&fastest_route);
    vl_graph_free(graph);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("Velo Test Suite\n");
    printf("================\n\n");

    printf("Geo Tests:\n");
    RUN_TEST(haversine_same_point);
    RUN_TEST(haversine_known_distance);
    RUN_TEST(haversine_antipodal);
    RUN_TEST(coord_valid);
    RUN_TEST(fixed_coord_conversion);
    printf("\n");

    printf("Heap Tests:\n");
    RUN_TEST(heap_create_free);
    RUN_TEST(heap_push_pop);
    RUN_TEST(heap_decrease_key);
    RUN_TEST(heap_contains);
    printf("\n");

    printf("Protobuf Tests:\n");
    RUN_TEST(pb_varint_small);
    RUN_TEST(pb_varint_300);
    RUN_TEST(pb_varint_large);
    RUN_TEST(pb_svarint_positive);
    RUN_TEST(pb_svarint_negative);
    RUN_TEST(pb_svarint_larger);
    RUN_TEST(pb_tag);
    RUN_TEST(pb_tag_field2_string);
    RUN_TEST(pb_delta_decode);
    printf("\n");

    printf("Graph Tests:\n");
    RUN_TEST(graph_create);
    RUN_TEST(graph_nearest_node);
    printf("\n");

    printf("Routing Tests:\n");
    RUN_TEST(route_same_node);
    RUN_TEST(route_adjacent_nodes);
    RUN_TEST(route_shortest_path);
    RUN_TEST(route_dijkstra_bidir);
    RUN_TEST(route_astar);
    RUN_TEST(route_astar_bidir);
    RUN_TEST(route_duration_weight);
    RUN_TEST(route_with_geometry);
    RUN_TEST(route_no_geometry);
    RUN_TEST(route_coords_wrapper);
    printf("\n");

    printf("API Tests:\n");
    RUN_TEST(version);
    RUN_TEST(status_strings);
    RUN_TEST(default_options);
    printf("\n");

    printf("Vehicle Profile Tests:\n");
    RUN_TEST(profile_car_all_roads);
    RUN_TEST(profile_truck_respects_hgv_no);
    RUN_TEST(profile_bike_avoids_motorway_trunk);
    RUN_TEST(profile_foot_avoids_motorway_trunk);
    RUN_TEST(profile_any_no_filtering);
    RUN_TEST(profile_with_astar);
    printf("\n");

    printf("Shortest vs Fastest Routing Tests:\n");
    RUN_TEST(shortest_vs_fastest_different_paths);
    RUN_TEST(shortest_vs_fastest_car_profile);
    RUN_TEST(shortest_vs_fastest_bike_profile);
    RUN_TEST(shortest_vs_fastest_foot_profile);
    RUN_TEST(fastest_always_faster_or_equal);
    RUN_TEST(shortest_always_shorter_or_equal);
    RUN_TEST(shortest_vs_fastest_with_astar);
    RUN_TEST(shortest_vs_fastest_bidir_dijkstra);
    printf("\n");

    printf("================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}
