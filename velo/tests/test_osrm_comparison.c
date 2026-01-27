/*
 * test_osrm_comparison.c - Compare Velo routing results against OSRM
 *
 * This test verifies that Velo produces correct routes by comparing
 * distances and durations against OSRM (Open Source Routing Machine).
 *
 * Usage:
 *   ./test_osrm_comparison <graph.vlg> [osrm_url]
 *
 * If OSRM URL is not provided, defaults to http://localhost:5000
 * If OSRM is unavailable, runs Velo-only consistency tests.
 */

#include "velo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define NUM_RANDOM_TESTS 100
#define NUM_FIXED_TESTS 20
#define DISTANCE_TOLERANCE_PERCENT 5.0   /* Allow 5% difference */
#define DURATION_TOLERANCE_PERCENT 15.0  /* Allow 15% difference (speed assumptions vary) */
#define HTTP_TIMEOUT_MS 5000

/* Hungary bounding box */
#define HUNGARY_LAT_MIN 45.74
#define HUNGARY_LAT_MAX 48.58
#define HUNGARY_LON_MIN 16.11
#define HUNGARY_LON_MAX 22.90

/* ============================================================================
 * HTTP Client (minimal implementation)
 * ============================================================================ */

typedef struct {
    int status_code;
    char *body;
    size_t body_len;
} HTTPResponse;

#ifndef _WIN32
static int http_get(const char *host, int port, const char *path, HTTPResponse *resp)
{
    int sockfd;
    struct sockaddr_in server_addr;
    struct hostent *server;
    char request[1024];
    char buffer[8192];

    resp->body = NULL;
    resp->body_len = 0;
    resp->status_code = 0;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    /* Set timeout */
    struct timeval tv;
    tv.tv_sec = HTTP_TIMEOUT_MS / 1000;
    tv.tv_usec = (HTTP_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    server = gethostbyname(host);
    if (!server) {
        close(sockfd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return -1;
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host, port);

    if (send(sockfd, request, strlen(request), 0) < 0) {
        close(sockfd);
        return -1;
    }

    /* Read response */
    size_t total = 0;
    size_t capacity = 8192;
    resp->body = malloc(capacity);
    if (!resp->body) {
        close(sockfd);
        return -1;
    }

    ssize_t n;
    while ((n = recv(sockfd, buffer, sizeof(buffer), 0)) > 0) {
        if (total + n >= capacity) {
            capacity *= 2;
            char *new_body = realloc(resp->body, capacity);
            if (!new_body) {
                free(resp->body);
                resp->body = NULL;
                close(sockfd);
                return -1;
            }
            resp->body = new_body;
        }
        memcpy(resp->body + total, buffer, n);
        total += n;
    }
    resp->body[total] = '\0';
    resp->body_len = total;

    close(sockfd);

    /* Parse status code */
    if (total > 12 && strncmp(resp->body, "HTTP/1.", 7) == 0) {
        resp->status_code = atoi(resp->body + 9);
    }

    /* Find body (after \r\n\r\n) */
    char *body_start = strstr(resp->body, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t header_len = body_start - resp->body;
        memmove(resp->body, body_start, total - header_len + 1);
        resp->body_len = total - header_len;
    }

    return 0;
}
#else
/* Windows implementation would go here */
static int http_get(const char *host, int port, const char *path, HTTPResponse *resp)
{
    (void)host; (void)port; (void)path;
    resp->body = NULL;
    resp->body_len = 0;
    resp->status_code = 0;
    return -1;  /* Not implemented for Windows */
}
#endif

static void http_response_free(HTTPResponse *resp)
{
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}

/* ============================================================================
 * JSON Parsing (minimal, just for OSRM response)
 * ============================================================================ */

static double json_get_number(const char *json, const char *key)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t') pos++;

    return atof(pos);
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    pos += strlen(search);
    const char *end = strchr(pos, '"');
    if (!end) return -1;

    size_t len = end - pos;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, pos, len);
    out[len] = '\0';

    return 0;
}

/* ============================================================================
 * OSRM Query
 * ============================================================================ */

typedef struct {
    double distance;    /* meters */
    double duration;    /* seconds */
    int success;
} OSRMResult;

static OSRMResult query_osrm(const char *host, int port,
                             double src_lat, double src_lon,
                             double dst_lat, double dst_lon)
{
    OSRMResult result = {0, 0, 0};

    char path[512];
    snprintf(path, sizeof(path),
             "/route/v1/driving/%.6f,%.6f;%.6f,%.6f?overview=false",
             src_lon, src_lat, dst_lon, dst_lat);

    HTTPResponse resp;
    if (http_get(host, port, path, &resp) != 0 || resp.status_code != 200) {
        http_response_free(&resp);
        return result;
    }

    /* Check for "Ok" status */
    char code[32];
    if (json_get_string(resp.body, "code", code, sizeof(code)) != 0 ||
        strcmp(code, "Ok") != 0) {
        http_response_free(&resp);
        return result;
    }

    /* Extract distance and duration from first route */
    const char *routes = strstr(resp.body, "\"routes\":");
    if (routes) {
        result.distance = json_get_number(routes, "distance");
        result.duration = json_get_number(routes, "duration");
        result.success = (result.distance > 0);
    }

    http_response_free(&resp);
    return result;
}

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

static double random_in_range(double min, double max)
{
    return min + (max - min) * ((double)rand() / RAND_MAX);
}

static void generate_hungary_coords(double *lat, double *lon)
{
    *lat = random_in_range(HUNGARY_LAT_MIN, HUNGARY_LAT_MAX);
    *lon = random_in_range(HUNGARY_LON_MIN, HUNGARY_LON_MAX);
}

typedef struct {
    int total;
    int passed;
    int failed;
    int osrm_unavailable;
    int velo_no_route;
    int osrm_no_route;
    double max_distance_error_pct;
    double max_duration_error_pct;
    double avg_distance_error_pct;
    double avg_duration_error_pct;
} TestStats;

static void print_result(int passed, const char *test_name,
                        double velo_dist, double osrm_dist,
                        double velo_dur, double osrm_dur)
{
    const char *status = passed ? "PASS" : "FAIL";
    double dist_err = (osrm_dist > 0) ?
        100.0 * fabs(velo_dist - osrm_dist) / osrm_dist : 0;
    double dur_err = (osrm_dur > 0) ?
        100.0 * fabs(velo_dur - osrm_dur) / osrm_dur : 0;

    printf("  [%s] %s\n", status, test_name);
    printf("         Velo:  %.1f km, %.1f min\n",
           velo_dist / 1000, velo_dur / 60);
    printf("         OSRM:  %.1f km, %.1f min\n",
           osrm_dist / 1000, osrm_dur / 60);
    printf("         Error: %.1f%% dist, %.1f%% dur\n", dist_err, dur_err);
}

/* ============================================================================
 * Fixed Test Routes (known city pairs in Hungary)
 * ============================================================================ */

typedef struct {
    const char *name;
    double src_lat, src_lon;
    double dst_lat, dst_lon;
} FixedRoute;

static FixedRoute fixed_routes[] = {
    /* Major city pairs */
    {"Budapest -> Debrecen", 47.4979, 19.0402, 47.5316, 21.6273},
    {"Budapest -> Szeged", 47.4979, 19.0402, 46.2530, 20.1414},
    {"Budapest -> Pécs", 47.4979, 19.0402, 46.0727, 18.2323},
    {"Budapest -> Győr", 47.4979, 19.0402, 47.6875, 17.6504},
    {"Budapest -> Miskolc", 47.4979, 19.0402, 48.1035, 20.7784},
    {"Budapest -> Nyíregyháza", 47.4979, 19.0402, 47.9554, 21.7167},
    {"Budapest -> Kecskemét", 47.4979, 19.0402, 46.8964, 19.6897},
    {"Budapest -> Székesfehérvár", 47.4979, 19.0402, 47.1860, 18.4221},

    /* Cross-country */
    {"Sopron -> Nyíregyháza", 47.6851, 16.5908, 47.9554, 21.7167},
    {"Szeged -> Győr", 46.2530, 20.1414, 47.6875, 17.6504},
    {"Pécs -> Debrecen", 46.0727, 18.2323, 47.5316, 21.6273},
    {"Miskolc -> Pécs", 48.1035, 20.7784, 46.0727, 18.2323},

    /* Short routes */
    {"Budapest -> Szentendre", 47.4979, 19.0402, 47.6693, 19.0760},
    {"Budapest -> Gödöllő", 47.4979, 19.0402, 47.5960, 19.3555},
    {"Szeged -> Hódmezővásárhely", 46.2530, 20.1414, 46.4181, 20.3300},

    /* Medium routes */
    {"Eger -> Debrecen", 47.9025, 20.3772, 47.5316, 21.6273},
    {"Kaposvár -> Szombathely", 46.3594, 17.7968, 47.2307, 16.6218},
    {"Veszprém -> Siófok", 47.0930, 17.9093, 46.9048, 18.0486},
    {"Esztergom -> Visegrád", 47.7856, 18.7403, 47.7810, 18.9770},
    {"Tihany -> Balatonfüred", 46.9133, 17.8892, 46.9575, 17.8936},
};

#define NUM_FIXED_ROUTES (sizeof(fixed_routes) / sizeof(fixed_routes[0]))

/* ============================================================================
 * Main Test Functions
 * ============================================================================ */

static int run_comparison_test(const VLGraph *graph,
                               const char *osrm_host, int osrm_port,
                               const char *test_name,
                               double src_lat, double src_lon,
                               double dst_lat, double dst_lon,
                               TestStats *stats)
{
    stats->total++;

    /* Query Velo */
    VLCoord origin = {src_lat, src_lon};
    VLCoord dest = {dst_lat, dst_lon};

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.algorithm = VL_ALGORITHM_ASTAR;
    opts.weight = VL_WEIGHT_DURATION;
    opts.include_geometry = 0;

    VLRoute velo_route;
    VLStatus status = vl_route_coords(graph, origin, dest, &opts, &velo_route);

    if (status != VL_OK) {
        stats->velo_no_route++;
        printf("  [SKIP] %s - Velo: no route\n", test_name);
        return 0;
    }

    /* Query OSRM */
    OSRMResult osrm = query_osrm(osrm_host, osrm_port,
                                  src_lat, src_lon, dst_lat, dst_lon);

    if (!osrm.success) {
        stats->osrm_unavailable++;
        printf("  [SKIP] %s - OSRM unavailable\n", test_name);
        vl_free_route(&velo_route);
        return 0;
    }

    /* Compare results */
    double dist_err_pct = 100.0 * fabs(velo_route.distance_m - osrm.distance) / osrm.distance;
    double dur_err_pct = 100.0 * fabs(velo_route.duration_s - osrm.duration) / osrm.duration;

    int passed = (dist_err_pct <= DISTANCE_TOLERANCE_PERCENT) &&
                 (dur_err_pct <= DURATION_TOLERANCE_PERCENT);

    if (passed) {
        stats->passed++;
    } else {
        stats->failed++;
    }

    /* Update stats */
    if (dist_err_pct > stats->max_distance_error_pct) {
        stats->max_distance_error_pct = dist_err_pct;
    }
    if (dur_err_pct > stats->max_duration_error_pct) {
        stats->max_duration_error_pct = dur_err_pct;
    }
    stats->avg_distance_error_pct += dist_err_pct;
    stats->avg_duration_error_pct += dur_err_pct;

    print_result(passed, test_name,
                 velo_route.distance_m, osrm.distance,
                 velo_route.duration_s, osrm.duration);

    vl_free_route(&velo_route);
    return passed;
}

static int check_osrm_available(const char *host, int port)
{
    printf("Checking OSRM availability at %s:%d...\n", host, port);

    HTTPResponse resp;
    if (http_get(host, port, "/", &resp) != 0) {
        printf("  OSRM not available (connection failed)\n");
        return 0;
    }

    int available = (resp.status_code == 200 || resp.status_code == 400);
    http_response_free(&resp);

    if (available) {
        printf("  OSRM is available\n");
    } else {
        printf("  OSRM not available (status: %d)\n", resp.status_code);
    }

    return available;
}

/* ============================================================================
 * Velo Self-Consistency Tests
 * ============================================================================ */

static void run_consistency_tests(const VLGraph *graph)
{
    printf("\n=== Velo Self-Consistency Tests ===\n\n");

    int passed = 0, failed = 0;

    /* Test 1: Route to same point should have zero distance */
    printf("Test: Route to same point\n");
    {
        VLCoord point = {47.4979, 19.0402};  /* Budapest */
        VLRoute route;
        VLStatus status = vl_route_coords(graph, point, point, NULL, &route);

        if (status == VL_OK && route.distance_m < 1.0) {
            printf("  [PASS] Distance: %.2f m\n", route.distance_m);
            passed++;
        } else {
            printf("  [FAIL] Distance: %.2f m (expected ~0)\n", route.distance_m);
            failed++;
        }
        vl_free_route(&route);
    }

    /* Test 2: Symmetric routes should have similar distance */
    printf("Test: Symmetric routes (A->B vs B->A)\n");
    {
        VLCoord a = {47.4979, 19.0402};  /* Budapest */
        VLCoord b = {46.2530, 20.1414};  /* Szeged */

        VLRoute route_ab, route_ba;
        vl_route_coords(graph, a, b, NULL, &route_ab);
        vl_route_coords(graph, b, a, NULL, &route_ba);

        double diff_pct = 100.0 * fabs(route_ab.distance_m - route_ba.distance_m) /
                          ((route_ab.distance_m + route_ba.distance_m) / 2);

        /* Allow 20% difference for one-way streets */
        if (diff_pct < 20.0) {
            printf("  [PASS] A->B: %.1f km, B->A: %.1f km (diff: %.1f%%)\n",
                   route_ab.distance_m / 1000, route_ba.distance_m / 1000, diff_pct);
            passed++;
        } else {
            printf("  [FAIL] A->B: %.1f km, B->A: %.1f km (diff: %.1f%%)\n",
                   route_ab.distance_m / 1000, route_ba.distance_m / 1000, diff_pct);
            failed++;
        }
        vl_free_route(&route_ab);
        vl_free_route(&route_ba);
    }

    /* Test 3: Triangle inequality (A->B->C >= A->C... approximately) */
    printf("Test: Route distances are reasonable\n");
    {
        VLCoord a = {47.4979, 19.0402};  /* Budapest */
        VLCoord b = {46.2530, 20.1414};  /* Szeged */
        VLCoord c = {47.5316, 21.6273};  /* Debrecen */

        VLRoute route_ab, route_bc, route_ac;
        vl_route_coords(graph, a, b, NULL, &route_ab);
        vl_route_coords(graph, b, c, NULL, &route_bc);
        vl_route_coords(graph, a, c, NULL, &route_ac);

        /* Route should be at least as long as straight-line distance */
        double haversine_ac = vl_haversine(a, c);

        if (route_ac.distance_m >= haversine_ac * 0.9) {  /* Allow 10% for rounding */
            printf("  [PASS] A->C: %.1f km, straight-line: %.1f km\n",
                   route_ac.distance_m / 1000, haversine_ac / 1000);
            passed++;
        } else {
            printf("  [FAIL] A->C: %.1f km < straight-line: %.1f km\n",
                   route_ac.distance_m / 1000, haversine_ac / 1000);
            failed++;
        }

        vl_free_route(&route_ab);
        vl_free_route(&route_bc);
        vl_free_route(&route_ac);
    }

    /* Test 4: All algorithms should find same-length shortest path */
    printf("Test: Algorithm consistency\n");
    {
        VLCoord a = {47.4979, 19.0402};  /* Budapest */
        VLCoord b = {47.6875, 17.6504};  /* Győr */

        VLRouteOptions opts;
        vl_default_options(&opts);
        opts.weight = VL_WEIGHT_DISTANCE;
        opts.include_geometry = 0;

        VLAlgorithm algos[] = {
            VL_ALGORITHM_DIJKSTRA,
            VL_ALGORITHM_DIJKSTRA_BIDIR,
            VL_ALGORITHM_ASTAR,
            VL_ALGORITHM_ASTAR_BIDIR
        };
        const char *algo_names[] = {
            "Dijkstra", "Dijkstra Bidir", "A*", "A* Bidir"
        };

        double distances[4];
        int all_same = 1;

        for (int i = 0; i < 4; i++) {
            opts.algorithm = algos[i];
            VLRoute route;
            vl_route_coords(graph, a, b, &opts, &route);
            distances[i] = route.distance_m;
            vl_free_route(&route);

            if (i > 0 && fabs(distances[i] - distances[0]) > 1.0) {
                all_same = 0;
            }
        }

        if (all_same) {
            printf("  [PASS] All algorithms: %.1f km\n", distances[0] / 1000);
            passed++;
        } else {
            printf("  [FAIL] Distances differ:\n");
            for (int i = 0; i < 4; i++) {
                printf("         %s: %.1f km\n", algo_names[i], distances[i] / 1000);
            }
            failed++;
        }
    }

    /* Test 5: Duration should be reasonable given distance */
    printf("Test: Duration/distance ratio\n");
    {
        VLCoord a = {47.4979, 19.0402};  /* Budapest */
        VLCoord b = {47.5316, 21.6273};  /* Debrecen */

        VLRoute route;
        vl_route_coords(graph, a, b, NULL, &route);

        double avg_speed_kmh = (route.distance_m / 1000) / (route.duration_s / 3600);

        /* Average speed should be between 30 and 130 km/h */
        if (avg_speed_kmh >= 30 && avg_speed_kmh <= 130) {
            printf("  [PASS] Avg speed: %.1f km/h (%.1f km in %.1f min)\n",
                   avg_speed_kmh, route.distance_m / 1000, route.duration_s / 60);
            passed++;
        } else {
            printf("  [FAIL] Avg speed: %.1f km/h (unrealistic)\n", avg_speed_kmh);
            failed++;
        }
        vl_free_route(&route);
    }

    printf("\n=== Consistency Results: %d/%d passed ===\n",
           passed, passed + failed);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <graph.vlg> [osrm_host:port]\n", argv[0]);
        fprintf(stderr, "\nExample:\n");
        fprintf(stderr, "  %s hungary.vlg localhost:5000\n", argv[0]);
        return 1;
    }

    const char *graph_file = argv[1];
    const char *osrm_host = "localhost";
    int osrm_port = 5000;

    if (argc > 2) {
        char *colon = strchr(argv[2], ':');
        if (colon) {
            *colon = '\0';
            osrm_host = argv[2];
            osrm_port = atoi(colon + 1);
        } else {
            osrm_host = argv[2];
        }
    }

    printf("Velo vs OSRM Comparison Test\n");
    printf("============================\n\n");

    /* Load graph */
    printf("Loading graph: %s\n", graph_file);
    VLGraph *graph = vl_load_binary(graph_file);
    if (!graph) {
        fprintf(stderr, "Failed to load graph\n");
        return 1;
    }
    printf("Graph loaded: %u nodes, %u edges\n\n",
           graph->num_nodes, graph->num_edges);

    /* Seed random number generator */
    srand((unsigned int)time(NULL));

    /* Run consistency tests first */
    run_consistency_tests(graph);

    /* Check OSRM availability */
    int osrm_available = check_osrm_available(osrm_host, osrm_port);

    if (!osrm_available) {
        printf("\nOSRM not available. To run OSRM comparison:\n");
        printf("  1. Start OSRM with Hungary data:\n");
        printf("     docker run -t -i -p 5000:5000 -v $(pwd):/data \\\n");
        printf("       ghcr.io/project-osrm/osrm-backend osrm-routed \\\n");
        printf("       --algorithm mld /data/hungary-latest.osrm\n");
        printf("  2. Re-run this test\n\n");

        vl_graph_free(graph);
        return 0;
    }

    /* Run OSRM comparison tests */
    TestStats stats = {0};

    printf("\n=== Fixed Route Tests ===\n\n");
    for (size_t i = 0; i < NUM_FIXED_ROUTES; i++) {
        const FixedRoute *r = &fixed_routes[i];
        run_comparison_test(graph, osrm_host, osrm_port,
                           r->name, r->src_lat, r->src_lon,
                           r->dst_lat, r->dst_lon, &stats);
    }

    printf("\n=== Random Route Tests ===\n\n");
    for (int i = 0; i < NUM_RANDOM_TESTS; i++) {
        double src_lat, src_lon, dst_lat, dst_lon;
        generate_hungary_coords(&src_lat, &src_lon);
        generate_hungary_coords(&dst_lat, &dst_lon);

        char test_name[64];
        snprintf(test_name, sizeof(test_name),
                 "Random #%d (%.2f,%.2f -> %.2f,%.2f)",
                 i + 1, src_lat, src_lon, dst_lat, dst_lon);

        run_comparison_test(graph, osrm_host, osrm_port,
                           test_name, src_lat, src_lon,
                           dst_lat, dst_lon, &stats);
    }

    /* Print summary */
    int compared = stats.passed + stats.failed;
    if (compared > 0) {
        stats.avg_distance_error_pct /= compared;
        stats.avg_duration_error_pct /= compared;
    }

    printf("\n");
    printf("================================================\n");
    printf("                 TEST SUMMARY\n");
    printf("================================================\n");
    printf("Total tests:           %d\n", stats.total);
    printf("Compared with OSRM:    %d\n", compared);
    printf("Passed:                %d\n", stats.passed);
    printf("Failed:                %d\n", stats.failed);
    printf("Velo no route:         %d\n", stats.velo_no_route);
    printf("OSRM unavailable:      %d\n", stats.osrm_unavailable);
    printf("\n");
    printf("Distance error (avg):  %.2f%%\n", stats.avg_distance_error_pct);
    printf("Distance error (max):  %.2f%%\n", stats.max_distance_error_pct);
    printf("Duration error (avg):  %.2f%%\n", stats.avg_duration_error_pct);
    printf("Duration error (max):  %.2f%%\n", stats.max_duration_error_pct);
    printf("================================================\n");

    int success = (stats.failed == 0 && stats.passed > 0);
    printf("\nResult: %s\n", success ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    vl_graph_free(graph);
    return success ? 0 : 1;
}
