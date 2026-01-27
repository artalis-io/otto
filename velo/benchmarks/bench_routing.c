/*
 * bench_routing.c - Routing algorithm benchmarks
 *
 * Compares performance of different routing algorithms.
 */

#include "velo.h"
#include "vl_route.h"
#include "vl_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

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

/* ============================================================================
 * Generate Test Graph
 * ============================================================================ */

/*
 * Create a grid graph for benchmarking.
 * Grid of rows x cols nodes with edges to 4 neighbors.
 */
static VLGraph *create_grid_graph(int rows, int cols)
{
    VLGraph *graph = calloc(1, sizeof(VLGraph));
    if (!graph) return NULL;

    graph->num_nodes = (uint32_t)(rows * cols);
    graph->nodes = calloc(graph->num_nodes, sizeof(VLNode));
    if (!graph->nodes) {
        free(graph);
        return NULL;
    }

    /* Count edges: each interior node has 4, edges have 3, corners have 2 */
    int edge_count = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (r > 0) edge_count++;
            if (r < rows - 1) edge_count++;
            if (c > 0) edge_count++;
            if (c < cols - 1) edge_count++;
        }
    }

    graph->num_edges = (uint32_t)edge_count;
    graph->edges = calloc(graph->num_edges, sizeof(VLEdge));
    if (!graph->edges) {
        free(graph->nodes);
        free(graph);
        return NULL;
    }

    /* Set up nodes with coordinates */
    double lat_step = 0.01;  /* ~1km */
    double lon_step = 0.01;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            graph->nodes[idx].coord.lat = (int32_t)((47.0 + r * lat_step) * 1e7);
            graph->nodes[idx].coord.lon = (int32_t)((19.0 + c * lon_step) * 1e7);
            graph->nodes[idx].osm_id = idx + 1;
        }
    }

    /* Count edges per node */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (r > 0) graph->nodes[idx].edge_count++;
            if (r < rows - 1) graph->nodes[idx].edge_count++;
            if (c > 0) graph->nodes[idx].edge_count++;
            if (c < cols - 1) graph->nodes[idx].edge_count++;
        }
    }

    /* Calculate offsets */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < graph->num_nodes; i++) {
        graph->nodes[i].edge_start = offset;
        offset += graph->nodes[i].edge_count;
        graph->nodes[i].edge_count = 0;
    }

    /* Fill edges */
    uint32_t dist = 1000000;  /* 1km in mm */
    uint16_t duration = 720;   /* 72 seconds at 50 km/h */

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            VLNode *node = &graph->nodes[idx];

            if (r > 0) {
                uint32_t edge_idx = node->edge_start + node->edge_count++;
                graph->edges[edge_idx].target = (uint32_t)((r - 1) * cols + c);
                graph->edges[edge_idx].distance = dist;
                graph->edges[edge_idx].duration = duration;
            }
            if (r < rows - 1) {
                uint32_t edge_idx = node->edge_start + node->edge_count++;
                graph->edges[edge_idx].target = (uint32_t)((r + 1) * cols + c);
                graph->edges[edge_idx].distance = dist;
                graph->edges[edge_idx].duration = duration;
            }
            if (c > 0) {
                uint32_t edge_idx = node->edge_start + node->edge_count++;
                graph->edges[edge_idx].target = (uint32_t)(r * cols + (c - 1));
                graph->edges[edge_idx].distance = dist;
                graph->edges[edge_idx].duration = duration;
            }
            if (c < cols - 1) {
                uint32_t edge_idx = node->edge_start + node->edge_count++;
                graph->edges[edge_idx].target = (uint32_t)(r * cols + (c + 1));
                graph->edges[edge_idx].distance = dist;
                graph->edges[edge_idx].duration = duration;
            }
        }
    }

    graph->owns_memory = 1;
    return graph;
}

/* ============================================================================
 * Benchmark Runner
 * ============================================================================ */

typedef struct {
    const char *name;
    VLAlgorithm algorithm;
} AlgorithmDef;

static void run_benchmark(VLGraph *graph, const char *graph_name,
                          uint32_t source, uint32_t target, int iterations)
{
    AlgorithmDef algorithms[] = {
        {"Dijkstra       ", VL_ALGORITHM_DIJKSTRA},
        {"Dijkstra Bidir ", VL_ALGORITHM_DIJKSTRA_BIDIR},
        {"A*             ", VL_ALGORITHM_ASTAR},
        {"A* Bidir       ", VL_ALGORITHM_ASTAR_BIDIR}
    };
    int num_algos = sizeof(algorithms) / sizeof(algorithms[0]);

    printf("\nBenchmark: %s (%u nodes, %u edges)\n",
           graph_name, graph->num_nodes, graph->num_edges);
    printf("Route: node %u -> node %u, %d iterations\n",
           source, target, iterations);
    printf("%-20s %12s %12s %12s\n",
           "Algorithm", "Time (ms)", "Nodes", "Distance");
    printf("%-20s %12s %12s %12s\n",
           "-------------------", "------------", "------------", "------------");

    for (int a = 0; a < num_algos; a++) {
        VLRouteOptions opts;
        vl_default_options(&opts);
        opts.algorithm = algorithms[a].algorithm;
        opts.weight = VL_WEIGHT_DISTANCE;
        opts.include_geometry = 0;

        double total_time = 0;
        uint32_t total_nodes = 0;
        double distance = 0;

        for (int i = 0; i < iterations; i++) {
            VLRoute route;
            double start = get_time_ms();
            VLStatus status = vl_route(graph, source, target, &opts, &route);
            double end = get_time_ms();

            if (status == VL_OK) {
                total_time += (end - start);
                total_nodes += route.nodes_explored;
                distance = route.distance_m;
                vl_free_route(&route);
            } else {
                printf("%-20s FAILED: %s\n", algorithms[a].name, vl_status_string(status));
                break;
            }
        }

        double avg_time = total_time / iterations;
        uint32_t avg_nodes = total_nodes / (uint32_t)iterations;

        printf("%-20s %12.3f %12u %12.0f\n",
               algorithms[a].name, avg_time, avg_nodes, distance);
    }

    /* Also test bucket heap Dijkstra */
    {
        VLRouteOptions opts;
        vl_default_options(&opts);
        opts.weight = VL_WEIGHT_DISTANCE;
        opts.include_geometry = 0;

        double total_time = 0;
        uint32_t total_nodes = 0;
        double distance = 0;

        for (int i = 0; i < iterations; i++) {
            VLRoute route;
            double start = get_time_ms();
            VLStatus status = vl_route_dijkstra_bucket(graph, source, target, &opts, &route);
            double end = get_time_ms();

            if (status == VL_OK) {
                total_time += (end - start);
                total_nodes += route.nodes_explored;
                distance = route.distance_m;
                vl_free_route(&route);
            } else {
                printf("%-20s FAILED: %s\n", "Dijkstra Bucket", vl_status_string(status));
                break;
            }
        }

        double avg_time = total_time / iterations;
        uint32_t avg_nodes = total_nodes / (uint32_t)iterations;

        printf("%-20s %12.3f %12u %12.0f\n",
               "Dijkstra Bucket ", avg_time, avg_nodes, distance);
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[])
{
    printf("Velo Routing Benchmarks\n");
    printf("=======================\n");

    /* Small grid (100x100 = 10,000 nodes) */
    VLGraph *small_grid = create_grid_graph(100, 100);
    if (small_grid) {
        run_benchmark(small_grid, "100x100 Grid", 0, 9999, 100);
        vl_graph_free(small_grid);
    }

    /* Medium grid (300x300 = 90,000 nodes) */
    VLGraph *medium_grid = create_grid_graph(300, 300);
    if (medium_grid) {
        run_benchmark(medium_grid, "300x300 Grid", 0, 89999, 10);
        vl_graph_free(medium_grid);
    }

    /* Large grid (500x500 = 250,000 nodes) */
    VLGraph *large_grid = create_grid_graph(500, 500);
    if (large_grid) {
        run_benchmark(large_grid, "500x500 Grid", 0, 249999, 5);
        vl_graph_free(large_grid);
    }

    /* If a graph file is provided, benchmark with real data */
    if (argc > 1) {
        const char *filename = argv[1];
        int use_landmarks = 0;
        int num_landmarks = VL_DEFAULT_LANDMARKS;
        int use_hilbert = 0;

        /* Parse command-line options */
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--landmarks") == 0) {
                use_landmarks = 1;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    num_landmarks = atoi(argv[++i]);
                    if (num_landmarks <= 0 || num_landmarks > VL_MAX_LANDMARKS) {
                        num_landmarks = VL_DEFAULT_LANDMARKS;
                    }
                }
            } else if (strcmp(argv[i], "--hilbert") == 0) {
                use_hilbert = 1;
            }
        }

        printf("\n\nLoading real graph from: %s\n", filename);

        VLGraph *real_graph = NULL;
        size_t len = strlen(filename);

        /* Detect file type by extension */
        if (len > 4 && strcmp(filename + len - 4, ".vlg") == 0) {
            real_graph = vl_load_binary(filename);
        } else {
            real_graph = vl_load_pbf(filename);
        }

        if (real_graph && real_graph->num_nodes > 0) {
            printf("Graph loaded: %u nodes, %u edges\n",
                   real_graph->num_nodes, real_graph->num_edges);

            /* Apply Hilbert curve reordering if requested */
            if (use_hilbert) {
                printf("\nApplying Hilbert curve reordering...\n");
                double hilbert_start = get_time_ms();
                VLStatus status = vl_graph_reorder_hilbert(real_graph);
                double hilbert_time = get_time_ms() - hilbert_start;
                if (status == VL_OK) {
                    printf("Hilbert reordering: %.1f ms\n", hilbert_time);
                } else {
                    printf("Hilbert reordering failed: %s\n", vl_status_string(status));
                }
            }

            /* Use coordinate-based routing for fair comparison */
            /* Budapest -> Szeged */
            VLCoord budapest = {47.4979, 19.0402};
            VLCoord szeged = {46.2530, 20.1414};
            uint32_t source = vl_graph_nearest_node(real_graph, budapest);
            uint32_t target = vl_graph_nearest_node(real_graph, szeged);

            printf("Budapest node: %u, Szeged node: %u\n\n", source, target);
            run_benchmark(real_graph, "Budapest -> Szeged", source, target, 10);

            /* Sopron -> Nyíregyháza (cross-country) */
            VLCoord sopron = {47.6851, 16.5908};
            VLCoord nyiregyhaza = {47.9554, 21.7167};
            source = vl_graph_nearest_node(real_graph, sopron);
            target = vl_graph_nearest_node(real_graph, nyiregyhaza);
            run_benchmark(real_graph, "Sopron -> Nyíregyháza", source, target, 5);

            /* Pécs -> Debrecen (diagonal) */
            VLCoord pecs = {46.0727, 18.2323};
            VLCoord debrecen = {47.5316, 21.6273};
            source = vl_graph_nearest_node(real_graph, pecs);
            target = vl_graph_nearest_node(real_graph, debrecen);
            run_benchmark(real_graph, "Pécs -> Debrecen", source, target, 5);

            /* Landmarks benchmark */
            if (use_landmarks) {
                printf("\n\nALT (A* with Landmarks) Benchmark\n");
                printf("==================================\n");
                printf("Creating %d landmarks...\n", num_landmarks);

                double lm_start = get_time_ms();
                VLLandmarks *landmarks = vl_landmarks_create(real_graph, num_landmarks);
                double lm_time = get_time_ms() - lm_start;

                if (landmarks) {
                    printf("Landmark preprocessing: %.1f ms\n\n", lm_time);

                    VLRouteOptions opts;
                    vl_default_options(&opts);
                    opts.weight = VL_WEIGHT_DISTANCE;
                    opts.include_geometry = 0;

                    /* Route test cases */
                    struct {
                        const char *name;
                        VLCoord origin;
                        VLCoord dest;
                        int iterations;
                    } routes[] = {
                        {"Budapest -> Szeged", {47.4979, 19.0402}, {46.2530, 20.1414}, 10},
                        {"Sopron -> Nyíregyháza", {47.6851, 16.5908}, {47.9554, 21.7167}, 5},
                        {"Pécs -> Debrecen", {46.0727, 18.2323}, {47.5316, 21.6273}, 5}
                    };
                    int num_routes = sizeof(routes) / sizeof(routes[0]);

                    printf("%-25s %8s %8s %12s %12s\n",
                           "Route", "A* (ms)", "ALT (ms)", "Speedup", "Distance");
                    printf("%-25s %8s %8s %12s %12s\n",
                           "-------------------------", "--------", "--------",
                           "------------", "------------");

                    for (int r = 0; r < num_routes; r++) {
                        source = vl_graph_nearest_node(real_graph, routes[r].origin);
                        target = vl_graph_nearest_node(real_graph, routes[r].dest);

                        /* Standard A* */
                        opts.algorithm = VL_ALGORITHM_ASTAR_BIDIR;
                        double astar_time = 0;
                        double distance = 0;
                        for (int i = 0; i < routes[r].iterations; i++) {
                            VLRoute route;
                            double start = get_time_ms();
                            VLStatus status = vl_route(real_graph, source, target, &opts, &route);
                            astar_time += get_time_ms() - start;
                            if (status == VL_OK) {
                                distance = route.distance_m;
                                vl_free_route(&route);
                            }
                        }
                        astar_time /= routes[r].iterations;

                        /* ALT */
                        double alt_time = 0;
                        double alt_distance = 0;
                        for (int i = 0; i < routes[r].iterations; i++) {
                            VLRoute route;
                            double start = get_time_ms();
                            VLStatus status = vl_route_astar_landmarks(
                                real_graph, landmarks, source, target, &opts, &route);
                            alt_time += get_time_ms() - start;
                            if (status == VL_OK) {
                                alt_distance = route.distance_m;
                                vl_free_route(&route);
                            }
                        }
                        alt_time /= routes[r].iterations;

                        double speedup = astar_time / alt_time;
                        printf("%-25s %8.1f %8.1f %11.2fx %12.0f\n",
                               routes[r].name, astar_time, alt_time, speedup, distance);

                        /* Verify distances match */
                        if (fabs(distance - alt_distance) > 1.0) {
                            printf("  WARNING: distance mismatch! A*=%.0f ALT=%.0f\n",
                                   distance, alt_distance);
                        }
                    }

                    vl_landmarks_free(landmarks);
                } else {
                    printf("Failed to create landmarks.\n");
                }
            }

            vl_graph_free(real_graph);
        } else {
            printf("Failed to load graph.\n");
        }
    }

    printf("\nBenchmarks complete.\n");
    return 0;
}
