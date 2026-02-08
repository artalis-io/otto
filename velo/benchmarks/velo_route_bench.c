/*
 * velo_route_bench.c - Route Quality and Performance Benchmark Tool
 *
 * Automated feedback loop for Velo routing optimization.
 * Validates route correctness, profile compliance, and performance.
 *
 * Usage:
 *   ./velo-route-bench --graph data/hungary.vlg routes/regional/*.json
 *   ./velo-route-bench --suite quick --graph data/hungary.vlg
 *   ./velo-route-bench --compare-algorithms routes/test.json --graph data.vlg
 */

#include "velo.h"
#include "vl_route.h"
#include "vl_graph.h"
#include "vl_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <sys/resource.h>
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define MAX_ROUTES 256
#define MAX_PATH_LEN 512
#define MAX_ALGORITHMS 6
#define MAX_VIOLATIONS 100
#define JSON_BUF_SIZE (1024 * 64)

/* Default tolerances */
#define DEFAULT_DISTANCE_TOLERANCE_PCT 0.1
#define DEFAULT_DURATION_TOLERANCE_PCT 5.0
#define DEFAULT_TIME_LIMIT_MS 10000
#define DEFAULT_LANDMARKS 16

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    char name[128];
    char file[MAX_PATH_LEN];
    char category[32];

    double origin_lat;
    double origin_lon;
    double dest_lat;
    double dest_lon;

    VLProfile profile;
    VLWeightType weight;
    VLAlgorithm algorithm;

    /* Reference values */
    double ref_distance_m;
    double ref_duration_s;
    int ref_waypoints;
    char ref_source[64];

    /* Tolerances */
    double distance_tolerance_pct;
    double duration_tolerance_pct;
    double time_limit_ms;
} BenchRoute;

typedef struct {
    uint32_t edge_idx;
    uint64_t way_id;
    char highway_class[32];
    char restriction[64];
    char reason[128];
} ViolationDetail;

typedef struct {
    int pass;
    double expected_m;
    double actual_m;
    double error_m;
    double error_pct;
} DistanceValidation;

typedef struct {
    int pass;
    double expected_s;
    double actual_s;
    double error_s;
    double error_pct;
} DurationValidation;

typedef struct {
    int pass;
    int edges_checked;
    int violations_found;
    ViolationDetail violations[MAX_VIOLATIONS];
} ProfileValidation;

typedef struct {
    int pass;
    int algorithms_tested;
    VLAlgorithm algorithms[MAX_ALGORITHMS];
    double distances[MAX_ALGORITHMS];
    double times_ms[MAX_ALGORITHMS];
    uint32_t nodes_explored[MAX_ALGORITHMS];
} ConsistencyValidation;

typedef struct {
    int available;           /* 1 if OSRM server was reachable */
    int pass;                /* Distance within tolerance */
    double osrm_distance_m;
    double osrm_duration_s;
    double velo_distance_m;
    double velo_duration_s;
    double distance_diff_m;
    double distance_diff_pct;
    double duration_diff_s;
    double duration_diff_pct;
    double osrm_query_ms;
    char error[128];
} OSRMValidation;

typedef struct {
    int overall_pass;
    DistanceValidation distance;
    DurationValidation duration;
    ProfileValidation profile;
    ConsistencyValidation consistency;
    OSRMValidation osrm;
} RouteValidation;

typedef struct {
    double query_time_ms;
    uint32_t nodes_explored;
    double ms_per_km;
    double nodes_per_km;
    double speedup_vs_dijkstra;
} PerformanceMetrics;

typedef struct {
    BenchRoute route;
    VLStatus status;
    double result_distance_m;
    double result_duration_s;
    int result_waypoints;
    VLAlgorithm algorithm_used;
    RouteValidation validation;
    PerformanceMetrics performance;
    char error_msg[256];
} BenchResult;

typedef struct {
    char graph_file[MAX_PATH_LEN];
    char output_file[MAX_PATH_LEN];
    char suite[32];
    char category[32];
    int verbose;
    int quiet;
    int compare_algorithms;
    int compare_profiles;
    int generate_reference;
    int iterations;
    int warmup;
    int num_landmarks;
    int use_landmarks;
    double distance_tolerance;
    double duration_tolerance;
    double time_limit_ms;
    int strict;
    char format[16];

    /* OSRM comparison */
    char osrm_url[256];      /* e.g., "http://localhost:5000" */
    int compare_osrm;        /* 1 if --osrm-url was specified */

    char *route_files[MAX_ROUTES];
    int num_route_files;
} BenchOptions;

/* ============================================================================
 * Timing Utilities
 * ============================================================================ */

static double get_time_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
#endif
}

/* ============================================================================
 * JSON Parsing (Minimal Implementation)
 * ============================================================================ */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, len, f);
    fclose(f);

    buf[read] = '\0';
    if (out_len) *out_len = read;
    return buf;
}

/* Simple JSON value extraction (no full parser) */
static int json_get_string(const char *json, const char *key, char *buf, size_t buflen)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n')) p++;

    if (*p != '"') return 0;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < buflen - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return 1;
}

static int json_get_double(const char *json, const char *key, double *val)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n')) p++;

    char *end;
    *val = strtod(p, &end);
    return end != p;
}

static int json_get_int(const char *json, const char *key, int *val)
{
    double d;
    if (!json_get_double(json, key, &d)) return 0;
    *val = (int)d;
    return 1;
}

/* ============================================================================
 * OSRM Comparison
 * ============================================================================ */

/*
 * Query OSRM server and compare results.
 * OSRM API: GET /route/v1/{profile}/{lon1},{lat1};{lon2},{lat2}?overview=false
 * Response: {"routes":[{"distance":123.4,"duration":56.7}],"code":"Ok"}
 *
 * Uses curl via popen() - similar to Ralph's GLPK comparison via glpsol.
 */
static void query_osrm(const char *osrm_url, BenchRoute *route,
                       double velo_distance, double velo_duration,
                       double tolerance_pct, OSRMValidation *v)
{
    memset(v, 0, sizeof(*v));

    /* Map Velo profile to OSRM profile */
    const char *osrm_profile = "driving";
    switch (route->profile) {
        case VL_PROFILE_CAR:
        case VL_PROFILE_TRUCK:
        case VL_PROFILE_ANY:
            osrm_profile = "driving";
            break;
        case VL_PROFILE_BIKE:
            osrm_profile = "cycling";
            break;
        case VL_PROFILE_FOOT:
            osrm_profile = "walking";
            break;
    }

    /* Build OSRM URL: /route/v1/{profile}/{lon},{lat};{lon},{lat}?overview=false */
    char url[1024];
    snprintf(url, sizeof(url),
             "%s/route/v1/%s/%.6f,%.6f;%.6f,%.6f?overview=false",
             osrm_url, osrm_profile,
             route->origin_lon, route->origin_lat,
             route->dest_lon, route->dest_lat);

    /* Query using curl */
    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             "curl -s --connect-timeout 5 --max-time 10 '%s' 2>/dev/null", url);

    double start = get_time_ms();
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        snprintf(v->error, sizeof(v->error), "Failed to run curl");
        return;
    }

    /* Read response */
    char response[8192];
    size_t total = 0;
    size_t n;
    while ((n = fread(response + total, 1, sizeof(response) - total - 1, pipe)) > 0) {
        total += n;
    }
    response[total] = '\0';

    int ret = pclose(pipe);
    v->osrm_query_ms = get_time_ms() - start;

    if (ret != 0 || total == 0) {
        snprintf(v->error, sizeof(v->error), "OSRM server not reachable");
        return;
    }

    /* Check for OSRM error response */
    char code[32] = {0};
    if (json_get_string(response, "code", code, sizeof(code))) {
        if (strcmp(code, "Ok") != 0) {
            snprintf(v->error, sizeof(v->error), "OSRM error: %s", code);
            return;
        }
    } else {
        snprintf(v->error, sizeof(v->error), "Invalid OSRM response");
        return;
    }

    /* Parse distance and duration from routes[0] */
    const char *routes = strstr(response, "\"routes\"");
    if (!routes) {
        snprintf(v->error, sizeof(v->error), "No routes in OSRM response");
        return;
    }

    if (!json_get_double(routes, "distance", &v->osrm_distance_m) ||
        !json_get_double(routes, "duration", &v->osrm_duration_s)) {
        snprintf(v->error, sizeof(v->error), "Failed to parse OSRM distance/duration");
        return;
    }

    v->available = 1;
    v->velo_distance_m = velo_distance;
    v->velo_duration_s = velo_duration;

    /* Compute differences */
    v->distance_diff_m = velo_distance - v->osrm_distance_m;
    v->duration_diff_s = velo_duration - v->osrm_duration_s;

    if (v->osrm_distance_m > 0) {
        v->distance_diff_pct = (v->distance_diff_m / v->osrm_distance_m) * 100.0;
    }
    if (v->osrm_duration_s > 0) {
        v->duration_diff_pct = (v->duration_diff_s / v->osrm_duration_s) * 100.0;
    }

    /* Pass if distance within tolerance (use absolute value) */
    v->pass = (fabs(v->distance_diff_pct) <= tolerance_pct);
}

/*
 * Check if curl is available (called once at startup)
 */
static int osrm_check_curl(void)
{
    return system("which curl >/dev/null 2>&1") == 0;
}

/*
 * Check if OSRM server is reachable
 */
static int osrm_check_server(const char *osrm_url, int verbose)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "curl -s --connect-timeout 2 '%s/health' >/dev/null 2>&1 || "
             "curl -s --connect-timeout 2 '%s/' >/dev/null 2>&1",
             osrm_url, osrm_url);

    int ret = system(cmd);
    if (ret != 0 && verbose) {
        fprintf(stderr, "Warning: OSRM server at %s not reachable\n", osrm_url);
    }
    return ret == 0;
}

/* ============================================================================
 * Route Loading
 * ============================================================================ */

static VLProfile parse_profile(const char *str)
{
    if (strcmp(str, "car") == 0) return VL_PROFILE_CAR;
    if (strcmp(str, "truck") == 0) return VL_PROFILE_TRUCK;
    if (strcmp(str, "bike") == 0) return VL_PROFILE_BIKE;
    if (strcmp(str, "foot") == 0) return VL_PROFILE_FOOT;
    return VL_PROFILE_ANY;
}

static VLWeightType parse_weight(const char *str)
{
    if (strcmp(str, "distance") == 0) return VL_WEIGHT_DISTANCE;
    if (strcmp(str, "duration") == 0) return VL_WEIGHT_DURATION;
    return VL_WEIGHT_DURATION;  /* Default: fastest */
}

static VLAlgorithm parse_algorithm(const char *str)
{
    if (strcmp(str, "dijkstra") == 0) return VL_ALGORITHM_DIJKSTRA;
    if (strcmp(str, "dijkstra_bidir") == 0) return VL_ALGORITHM_DIJKSTRA_BIDIR;
    if (strcmp(str, "astar") == 0) return VL_ALGORITHM_ASTAR;
    if (strcmp(str, "astar_bidir") == 0) return VL_ALGORITHM_ASTAR_BIDIR;
    return VL_ALGORITHM_ASTAR_BIDIR;  /* Default */
}

static int load_route(const char *path, BenchRoute *route, BenchOptions *opts)
{
    memset(route, 0, sizeof(*route));

    char *json = read_file(path, NULL);
    if (!json) {
        fprintf(stderr, "Failed to read: %s\n", path);
        return 0;
    }

    /* Extract basic info */
    json_get_string(json, "name", route->name, sizeof(route->name));
    strncpy(route->file, path, sizeof(route->file) - 1);

    json_get_string(json, "category", route->category, sizeof(route->category));

    /* Origin */
    const char *origin = strstr(json, "\"origin\"");
    if (origin) {
        json_get_double(origin, "lat", &route->origin_lat);
        json_get_double(origin, "lon", &route->origin_lon);
    }

    /* Destination */
    const char *dest = strstr(json, "\"destination\"");
    if (dest) {
        json_get_double(dest, "lat", &route->dest_lat);
        json_get_double(dest, "lon", &route->dest_lon);
    }

    /* Options */
    const char *options = strstr(json, "\"options\"");
    if (options) {
        char buf[32];
        if (json_get_string(options, "profile", buf, sizeof(buf))) {
            route->profile = parse_profile(buf);
        }
        if (json_get_string(options, "weight", buf, sizeof(buf))) {
            route->weight = parse_weight(buf);
        }
        if (json_get_string(options, "algorithm", buf, sizeof(buf))) {
            route->algorithm = parse_algorithm(buf);
        }
    }

    /* Reference */
    const char *ref = strstr(json, "\"reference\"");
    if (ref) {
        json_get_string(ref, "source", route->ref_source, sizeof(route->ref_source));
        json_get_double(ref, "distance_m", &route->ref_distance_m);
        json_get_double(ref, "duration_s", &route->ref_duration_s);
        json_get_int(ref, "waypoint_count", &route->ref_waypoints);
    }

    /* Tolerances (use defaults if not specified) */
    const char *tol = strstr(json, "\"tolerances\"");
    if (tol) {
        if (!json_get_double(tol, "distance_pct", &route->distance_tolerance_pct)) {
            route->distance_tolerance_pct = opts->distance_tolerance;
        }
        if (!json_get_double(tol, "duration_pct", &route->duration_tolerance_pct)) {
            route->duration_tolerance_pct = opts->duration_tolerance;
        }
        if (!json_get_double(tol, "time_ms_max", &route->time_limit_ms)) {
            route->time_limit_ms = opts->time_limit_ms;
        }
    } else {
        route->distance_tolerance_pct = opts->distance_tolerance;
        route->duration_tolerance_pct = opts->duration_tolerance;
        route->time_limit_ms = opts->time_limit_ms;
    }

    free(json);
    return 1;
}

/* ============================================================================
 * Validation
 * ============================================================================ */

static void validate_distance(BenchResult *result)
{
    DistanceValidation *v = &result->validation.distance;

    v->expected_m = result->route.ref_distance_m;
    v->actual_m = result->result_distance_m;
    v->error_m = fabs(v->actual_m - v->expected_m);

    if (v->expected_m > 0) {
        v->error_pct = (v->error_m / v->expected_m) * 100.0;
    } else {
        v->error_pct = 0;
    }

    v->pass = (v->error_pct <= result->route.distance_tolerance_pct);
}

static void validate_duration(BenchResult *result)
{
    DurationValidation *v = &result->validation.duration;

    v->expected_s = result->route.ref_duration_s;
    v->actual_s = result->result_duration_s;
    v->error_s = fabs(v->actual_s - v->expected_s);

    if (v->expected_s > 0) {
        v->error_pct = (v->error_s / v->expected_s) * 100.0;
    } else {
        v->error_pct = 0;
    }

    v->pass = (v->error_pct <= result->route.duration_tolerance_pct);
}

static void validate_profile(VLGraph *graph, VLRoute *route, VLProfile profile,
                             ProfileValidation *v)
{
    v->pass = 1;
    v->edges_checked = 0;
    v->violations_found = 0;

    if (!route->node_indices || route->num_nodes < 2) return;

    /* Walk through route and check each edge */
    for (uint32_t i = 0; i < (uint32_t)route->num_nodes - 1; i++) {
        uint32_t from = route->node_indices[i];
        uint32_t to = route->node_indices[i + 1];

        if (from >= graph->num_nodes || to >= graph->num_nodes) continue;

        VLNode *node = &graph->nodes[from];
        for (uint32_t e = 0; e < node->edge_count; e++) {
            uint32_t edge_idx = node->edge_start + e;
            VLEdge *edge = &graph->edges[edge_idx];

            if (edge->target == to) {
                v->edges_checked++;

                /* Check if edge is allowed for this profile */
                int allowed = 1;
                const char *reason = NULL;

                /* Profile-specific checks based on edge flags */
                if (profile == VL_PROFILE_TRUCK) {
                    /* Check for truck restrictions */
                    if (edge->flags & VL_ACCESS_NO_TRUCK) {
                        allowed = 0;
                        reason = "hgv=no";
                    }
                    /* Could add more checks: maxweight, residential avoidance, etc. */
                }

                if (!allowed && v->violations_found < MAX_VIOLATIONS) {
                    ViolationDetail *vd = &v->violations[v->violations_found++];
                    vd->edge_idx = edge_idx;
                    vd->way_id = 0;  /* VLEdge doesn't store way_id */
                    if (reason) strncpy(vd->reason, reason, sizeof(vd->reason) - 1);
                    v->pass = 0;
                }
                break;
            }
        }
    }
}

static void validate_consistency(VLGraph *graph, VLLandmarks *landmarks,
                                 BenchRoute *route, ConsistencyValidation *v)
{
    v->pass = 1;
    v->algorithms_tested = 0;

    VLCoord origin = {route->origin_lat, route->origin_lon};
    VLCoord dest = {route->dest_lat, route->dest_lon};

    uint32_t source = vl_graph_nearest_node(graph, origin);
    uint32_t target = vl_graph_nearest_node(graph, dest);

    VLAlgorithm algs[] = {
        VL_ALGORITHM_DIJKSTRA,
        VL_ALGORITHM_DIJKSTRA_BIDIR,
        VL_ALGORITHM_ASTAR,
        VL_ALGORITHM_ASTAR_BIDIR
    };
    int num_algs = sizeof(algs) / sizeof(algs[0]);

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.weight = route->weight;
    opts.profile = route->profile;
    opts.include_geometry = 0;

    for (int i = 0; i < num_algs; i++) {
        opts.algorithm = algs[i];

        VLRoute r;
        double start = get_time_ms();
        VLStatus status = vl_route(graph, source, target, &opts, &r);
        double elapsed = get_time_ms() - start;

        if (status == VL_OK) {
            v->algorithms[v->algorithms_tested] = algs[i];
            v->distances[v->algorithms_tested] = r.distance_m;
            v->times_ms[v->algorithms_tested] = elapsed;
            v->nodes_explored[v->algorithms_tested] = r.nodes_explored;
            v->algorithms_tested++;
            vl_free_route(&r);
        }
    }

    /* Check all distances are consistent */
    if (v->algorithms_tested > 1) {
        double first_dist = v->distances[0];
        for (int i = 1; i < v->algorithms_tested; i++) {
            double diff = fabs(v->distances[i] - first_dist);
            if (diff > 1.0) {  /* Allow 1m tolerance */
                v->pass = 0;
                break;
            }
        }
    }
}

/* ============================================================================
 * Benchmark Execution
 * ============================================================================ */

static void run_single_route(VLGraph *graph, VLLandmarks *landmarks,
                             BenchRoute *route, BenchResult *result,
                             BenchOptions *opts)
{
    memset(result, 0, sizeof(*result));
    result->route = *route;

    VLCoord origin = {route->origin_lat, route->origin_lon};
    VLCoord dest = {route->dest_lat, route->dest_lon};

    uint32_t source = vl_graph_nearest_node(graph, origin);
    uint32_t target = vl_graph_nearest_node(graph, dest);

    if (source == (uint32_t)-1 || target == (uint32_t)-1) {
        result->status = VL_ERROR_NO_ROUTE;
        snprintf(result->error_msg, sizeof(result->error_msg),
                 "Could not find graph nodes for coordinates");
        return;
    }

    VLRouteOptions ropts;
    vl_default_options(&ropts);
    ropts.algorithm = route->algorithm;
    ropts.weight = route->weight;
    ropts.profile = route->profile;
    ropts.include_geometry = 1;

    /* Warmup */
    for (int i = 0; i < opts->warmup; i++) {
        VLRoute r;
        VLStatus s = vl_route(graph, source, target, &ropts, &r);
        if (s == VL_OK) vl_free_route(&r);
    }

    /* Timed run(s) */
    double total_time = 0;
    uint32_t total_nodes = 0;
    VLRoute final_route;
    memset(&final_route, 0, sizeof(final_route));

    for (int i = 0; i < opts->iterations; i++) {
        VLRoute r;
        double start = get_time_ms();
        VLStatus status = vl_route(graph, source, target, &ropts, &r);
        double elapsed = get_time_ms() - start;

        if (status != VL_OK) {
            result->status = status;
            snprintf(result->error_msg, sizeof(result->error_msg),
                     "Routing failed: %s", vl_status_string(status));
            return;
        }

        total_time += elapsed;
        total_nodes += r.nodes_explored;

        if (i == opts->iterations - 1) {
            final_route = r;  /* Keep last route */
        } else {
            vl_free_route(&r);
        }
    }

    result->status = VL_OK;
    result->result_distance_m = final_route.distance_m;
    result->result_duration_s = final_route.duration_s;
    result->result_waypoints = final_route.num_nodes;
    result->algorithm_used = route->algorithm;

    /* Performance metrics */
    result->performance.query_time_ms = total_time / opts->iterations;
    result->performance.nodes_explored = total_nodes / opts->iterations;

    double distance_km = final_route.distance_m / 1000.0;
    if (distance_km > 0) {
        result->performance.ms_per_km = result->performance.query_time_ms / distance_km;
        result->performance.nodes_per_km = result->performance.nodes_explored / distance_km;
    }

    /* Validation */
    if (route->ref_distance_m > 0) {
        validate_distance(result);
    } else {
        result->validation.distance.pass = 1;  /* No reference to compare */
    }

    if (route->ref_duration_s > 0) {
        validate_duration(result);
    } else {
        result->validation.duration.pass = 1;
    }

    validate_profile(graph, &final_route, route->profile, &result->validation.profile);

    if (opts->compare_algorithms) {
        validate_consistency(graph, landmarks, route, &result->validation.consistency);
    } else {
        result->validation.consistency.pass = 1;
    }

    /* OSRM comparison */
    if (opts->compare_osrm && opts->osrm_url[0]) {
        query_osrm(opts->osrm_url, route,
                   result->result_distance_m, result->result_duration_s,
                   route->distance_tolerance_pct,
                   &result->validation.osrm);
    } else {
        result->validation.osrm.pass = 1;  /* Skip if no OSRM URL */
    }

    result->validation.overall_pass =
        result->validation.distance.pass &&
        result->validation.duration.pass &&
        result->validation.profile.pass &&
        result->validation.consistency.pass &&
        (!opts->compare_osrm || result->validation.osrm.pass);

    vl_free_route(&final_route);
}

/* ============================================================================
 * JSON Output
 * ============================================================================ */

static const char *algorithm_name(VLAlgorithm alg)
{
    switch (alg) {
        case VL_ALGORITHM_DIJKSTRA: return "dijkstra";
        case VL_ALGORITHM_DIJKSTRA_BIDIR: return "dijkstra_bidir";
        case VL_ALGORITHM_ASTAR: return "astar";
        case VL_ALGORITHM_ASTAR_BIDIR: return "astar_bidir";
        default: return "unknown";
    }
}

static const char *profile_name(VLProfile p)
{
    switch (p) {
        case VL_PROFILE_CAR: return "car";
        case VL_PROFILE_TRUCK: return "truck";
        case VL_PROFILE_BIKE: return "bike";
        case VL_PROFILE_FOOT: return "foot";
        case VL_PROFILE_ANY: return "any";
        default: return "unknown";
    }
}

static const char *weight_name(VLWeightType w)
{
    switch (w) {
        case VL_WEIGHT_DISTANCE: return "distance";
        case VL_WEIGHT_DURATION: return "duration";
        default: return "unknown";
    }
}

static void output_result_json(BenchResult *result, VLGraph *graph, FILE *out)
{
    fprintf(out, "  {\n");
    fprintf(out, "    \"route\": {\n");
    fprintf(out, "      \"name\": \"%s\",\n", result->route.name);
    fprintf(out, "      \"file\": \"%s\",\n", result->route.file);
    fprintf(out, "      \"category\": \"%s\",\n", result->route.category);
    fprintf(out, "      \"origin\": {\"lat\": %.6f, \"lon\": %.6f},\n",
            result->route.origin_lat, result->route.origin_lon);
    fprintf(out, "      \"destination\": {\"lat\": %.6f, \"lon\": %.6f},\n",
            result->route.dest_lat, result->route.dest_lon);
    fprintf(out, "      \"profile\": \"%s\",\n", profile_name(result->route.profile));
    fprintf(out, "      \"weight\": \"%s\"\n", weight_name(result->route.weight));
    fprintf(out, "    },\n");

    if (result->route.ref_distance_m > 0) {
        fprintf(out, "    \"reference\": {\n");
        fprintf(out, "      \"source\": \"%s\",\n", result->route.ref_source);
        fprintf(out, "      \"distance_m\": %.1f,\n", result->route.ref_distance_m);
        fprintf(out, "      \"duration_s\": %.1f,\n", result->route.ref_duration_s);
        fprintf(out, "      \"waypoints\": %d\n", result->route.ref_waypoints);
        fprintf(out, "    },\n");
    }

    fprintf(out, "    \"result\": {\n");
    fprintf(out, "      \"status\": \"%s\",\n",
            result->status == VL_OK ? "ok" : vl_status_string(result->status));
    if (result->status == VL_OK) {
        fprintf(out, "      \"algorithm\": \"%s\",\n", algorithm_name(result->algorithm_used));
        fprintf(out, "      \"distance_m\": %.1f,\n", result->result_distance_m);
        fprintf(out, "      \"duration_s\": %.1f,\n", result->result_duration_s);
        fprintf(out, "      \"waypoints\": %d\n", result->result_waypoints);
    } else {
        fprintf(out, "      \"error\": \"%s\"\n", result->error_msg);
    }
    fprintf(out, "    },\n");

    fprintf(out, "    \"validation\": {\n");
    fprintf(out, "      \"overall\": \"%s\",\n",
            result->validation.overall_pass ? "pass" : "fail");

    fprintf(out, "      \"distance\": {\n");
    fprintf(out, "        \"pass\": %s,\n", result->validation.distance.pass ? "true" : "false");
    fprintf(out, "        \"expected_m\": %.1f,\n", result->validation.distance.expected_m);
    fprintf(out, "        \"actual_m\": %.1f,\n", result->validation.distance.actual_m);
    fprintf(out, "        \"error_m\": %.1f,\n", result->validation.distance.error_m);
    fprintf(out, "        \"error_pct\": %.4f,\n", result->validation.distance.error_pct);
    fprintf(out, "        \"tolerance_pct\": %.2f\n", result->route.distance_tolerance_pct);
    fprintf(out, "      },\n");

    fprintf(out, "      \"duration\": {\n");
    fprintf(out, "        \"pass\": %s,\n", result->validation.duration.pass ? "true" : "false");
    fprintf(out, "        \"expected_s\": %.1f,\n", result->validation.duration.expected_s);
    fprintf(out, "        \"actual_s\": %.1f,\n", result->validation.duration.actual_s);
    fprintf(out, "        \"error_s\": %.1f,\n", result->validation.duration.error_s);
    fprintf(out, "        \"error_pct\": %.4f,\n", result->validation.duration.error_pct);
    fprintf(out, "        \"tolerance_pct\": %.2f\n", result->route.duration_tolerance_pct);
    fprintf(out, "      },\n");

    fprintf(out, "      \"profile\": {\n");
    fprintf(out, "        \"pass\": %s,\n", result->validation.profile.pass ? "true" : "false");
    fprintf(out, "        \"edges_checked\": %d,\n", result->validation.profile.edges_checked);
    fprintf(out, "        \"violations\": %d\n", result->validation.profile.violations_found);
    fprintf(out, "      },\n");

    fprintf(out, "      \"consistency\": {\n");
    fprintf(out, "        \"pass\": %s,\n", result->validation.consistency.pass ? "true" : "false");
    fprintf(out, "        \"algorithms_tested\": %d\n", result->validation.consistency.algorithms_tested);
    fprintf(out, "      },\n");

    /* OSRM comparison (if available) */
    fprintf(out, "      \"osrm\": {\n");
    fprintf(out, "        \"available\": %s,\n", result->validation.osrm.available ? "true" : "false");
    if (result->validation.osrm.available) {
        fprintf(out, "        \"pass\": %s,\n", result->validation.osrm.pass ? "true" : "false");
        fprintf(out, "        \"osrm_distance_m\": %.1f,\n", result->validation.osrm.osrm_distance_m);
        fprintf(out, "        \"osrm_duration_s\": %.1f,\n", result->validation.osrm.osrm_duration_s);
        fprintf(out, "        \"velo_distance_m\": %.1f,\n", result->validation.osrm.velo_distance_m);
        fprintf(out, "        \"velo_duration_s\": %.1f,\n", result->validation.osrm.velo_duration_s);
        fprintf(out, "        \"distance_diff_m\": %.1f,\n", result->validation.osrm.distance_diff_m);
        fprintf(out, "        \"distance_diff_pct\": %.2f,\n", result->validation.osrm.distance_diff_pct);
        fprintf(out, "        \"duration_diff_s\": %.1f,\n", result->validation.osrm.duration_diff_s);
        fprintf(out, "        \"duration_diff_pct\": %.2f,\n", result->validation.osrm.duration_diff_pct);
        fprintf(out, "        \"osrm_query_ms\": %.1f\n", result->validation.osrm.osrm_query_ms);
    } else if (result->validation.osrm.error[0]) {
        fprintf(out, "        \"error\": \"%s\"\n", result->validation.osrm.error);
    } else {
        fprintf(out, "        \"error\": \"not configured\"\n");
    }
    fprintf(out, "      }\n");
    fprintf(out, "    },\n");

    fprintf(out, "    \"performance\": {\n");
    fprintf(out, "      \"query_time_ms\": %.3f,\n", result->performance.query_time_ms);
    fprintf(out, "      \"nodes_explored\": %u,\n", result->performance.nodes_explored);
    fprintf(out, "      \"ms_per_km\": %.4f,\n", result->performance.ms_per_km);
    fprintf(out, "      \"nodes_per_km\": %.1f\n", result->performance.nodes_per_km);
    fprintf(out, "    }\n");

    fprintf(out, "  }");
}

/* ============================================================================
 * Suite Definitions
 * ============================================================================ */

static void collect_routes_from_dir(const char *dir, char **files, int *count, int max)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && *count < max) {
        if (entry->d_name[0] == '.') continue;

        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISREG(st.st_mode) && strstr(entry->d_name, ".json")) {
                files[*count] = strdup(path);
                (*count)++;
            } else if (S_ISDIR(st.st_mode)) {
                collect_routes_from_dir(path, files, count, max);
            }
        }
    }
    closedir(d);
}

static void load_suite(const char *suite, BenchOptions *opts)
{
    char base_dir[MAX_PATH_LEN];

    /* Find routes directory relative to executable or cwd */
    const char *search_paths[] = {
        "benchmarks/routes",
        "velo/benchmarks/routes",
        "../benchmarks/routes",
        "routes"
    };

    int found = 0;
    for (int i = 0; i < 4; i++) {
        struct stat st;
        if (stat(search_paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(base_dir, search_paths[i], sizeof(base_dir) - 1);
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "Could not find routes directory\n");
        return;
    }

    if (strcmp(suite, "smoke") == 0) {
        /* Just a few routes for quick test */
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/regional", base_dir);
        collect_routes_from_dir(path, opts->route_files, &opts->num_route_files, 3);
    } else if (strcmp(suite, "quick") == 0) {
        /* Urban + regional */
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/urban", base_dir);
        collect_routes_from_dir(path, opts->route_files, &opts->num_route_files, MAX_ROUTES);
        snprintf(path, sizeof(path), "%s/regional", base_dir);
        collect_routes_from_dir(path, opts->route_files, &opts->num_route_files, MAX_ROUTES);
    } else if (strcmp(suite, "full") == 0) {
        /* All routes */
        collect_routes_from_dir(base_dir, opts->route_files, &opts->num_route_files, MAX_ROUTES);
    } else {
        fprintf(stderr, "Unknown suite: %s\n", suite);
    }
}

/* ============================================================================
 * CLI
 * ============================================================================ */

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] [ROUTE_FILES...]\n", prog);
    printf("\n");
    printf("Velo Route Quality and Performance Benchmark\n");
    printf("\n");
    printf("OPTIONS:\n");
    printf("  -g, --graph FILE         Graph file (.vlg or .osm.pbf) [required]\n");
    printf("  -o, --output FILE        Output JSON file (default: stdout)\n");
    printf("  -v, --verbose            Print progress to stderr\n");
    printf("  -q, --quiet              Suppress all output except errors\n");
    printf("\n");
    printf("ROUTE SELECTION:\n");
    printf("  --suite SUITE            Run predefined suite (smoke/quick/full)\n");
    printf("  --category CAT           Run all routes in category\n");
    printf("\n");
    printf("ALGORITHM OPTIONS:\n");
    printf("  --compare-algorithms     Run all algorithms and compare\n");
    printf("  --landmarks N            Use N landmarks (default: %d)\n", DEFAULT_LANDMARKS);
    printf("  --no-landmarks           Disable landmarks\n");
    printf("\n");
    printf("VALIDATION OPTIONS:\n");
    printf("  --distance-tolerance PCT Distance tolerance (default: %.1f%%)\n",
           DEFAULT_DISTANCE_TOLERANCE_PCT);
    printf("  --duration-tolerance PCT Duration tolerance (default: %.1f%%)\n",
           DEFAULT_DURATION_TOLERANCE_PCT);
    printf("  --strict                 Fail on any warning\n");
    printf("\n");
    printf("PERFORMANCE OPTIONS:\n");
    printf("  --iterations N           Run each route N times (default: 1)\n");
    printf("  --warmup N               Warmup iterations (default: 0)\n");
    printf("\n");
    printf("REFERENCE OPTIONS:\n");
    printf("  --generate-reference     Compute reference using Dijkstra\n");
    printf("  --osrm-url URL           Compare against OSRM server (e.g., http://localhost:5000)\n");
    printf("\n");
    printf("UTILITY:\n");
    printf("  --list-routes            List available benchmark routes\n");
    printf("  --help                   Show this help\n");
    printf("\n");
    printf("EXAMPLES:\n");
    printf("  %s --graph data/hungary.vlg --suite smoke\n", prog);
    printf("  %s --graph data/hungary.vlg routes/regional/*.json\n", prog);
    printf("  %s --graph data/hungary.vlg --compare-algorithms routes/test.json\n", prog);
}

static int parse_args(int argc, char *argv[], BenchOptions *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->iterations = 1;
    opts->warmup = 0;
    opts->num_landmarks = DEFAULT_LANDMARKS;
    opts->use_landmarks = 1;
    opts->distance_tolerance = DEFAULT_DISTANCE_TOLERANCE_PCT;
    opts->duration_tolerance = DEFAULT_DURATION_TOLERANCE_PCT;
    opts->time_limit_ms = DEFAULT_TIME_LIMIT_MS;
    strcpy(opts->format, "json");

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "-g") == 0 || strcmp(arg, "--graph") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            strncpy(opts->graph_file, argv[i], sizeof(opts->graph_file) - 1);
        } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            strncpy(opts->output_file, argv[i], sizeof(opts->output_file) - 1);
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            opts->verbose = 1;
        } else if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
            opts->quiet = 1;
        } else if (strcmp(arg, "--suite") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            strncpy(opts->suite, argv[i], sizeof(opts->suite) - 1);
        } else if (strcmp(arg, "--category") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            strncpy(opts->category, argv[i], sizeof(opts->category) - 1);
        } else if (strcmp(arg, "--compare-algorithms") == 0) {
            opts->compare_algorithms = 1;
        } else if (strcmp(arg, "--landmarks") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            opts->num_landmarks = atoi(argv[i]);
        } else if (strcmp(arg, "--no-landmarks") == 0) {
            opts->use_landmarks = 0;
        } else if (strcmp(arg, "--distance-tolerance") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            opts->distance_tolerance = atof(argv[i]);
        } else if (strcmp(arg, "--duration-tolerance") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            opts->duration_tolerance = atof(argv[i]);
        } else if (strcmp(arg, "--strict") == 0) {
            opts->strict = 1;
        } else if (strcmp(arg, "--iterations") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            opts->iterations = atoi(argv[i]);
        } else if (strcmp(arg, "--warmup") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            opts->warmup = atoi(argv[i]);
        } else if (strcmp(arg, "--generate-reference") == 0) {
            opts->generate_reference = 1;
        } else if (strcmp(arg, "--osrm-url") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing argument for %s\n", arg); return 0; }
            strncpy(opts->osrm_url, argv[i], sizeof(opts->osrm_url) - 1);
            opts->compare_osrm = 1;
        } else if (strcmp(arg, "--list-routes") == 0) {
            /* TODO: List routes */
            printf("Available routes in benchmarks/routes/:\n");
            exit(0);
        } else if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return 0;
        } else {
            /* Route file */
            if (opts->num_route_files < MAX_ROUTES) {
                opts->route_files[opts->num_route_files++] = strdup(arg);
            }
        }
    }

    if (!opts->graph_file[0]) {
        fprintf(stderr, "Error: --graph is required\n");
        return 0;
    }

    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[])
{
    BenchOptions opts;
    if (!parse_args(argc, argv, &opts)) {
        return 1;
    }

    /* Load suite if specified */
    if (opts.suite[0]) {
        load_suite(opts.suite, &opts);
    }

    if (opts.num_route_files == 0) {
        fprintf(stderr, "No route files specified. Use --suite or provide route files.\n");
        return 1;
    }

    /* Load graph */
    if (opts.verbose) {
        fprintf(stderr, "Loading graph: %s\n", opts.graph_file);
    }

    VLGraph *graph = NULL;
    size_t len = strlen(opts.graph_file);
    if (len > 4 && strcmp(opts.graph_file + len - 4, ".vlg") == 0) {
        graph = vl_load_binary(opts.graph_file);
    } else {
        graph = vl_load_pbf(opts.graph_file);
    }

    if (!graph) {
        fprintf(stderr, "Failed to load graph: %s\n", opts.graph_file);
        return 1;
    }

    if (opts.verbose) {
        fprintf(stderr, "Graph loaded: %u nodes, %u edges\n",
                graph->num_nodes, graph->num_edges);
    }

    /* Check OSRM if enabled */
    if (opts.compare_osrm) {
        if (!osrm_check_curl()) {
            fprintf(stderr, "Error: curl not found. Install curl to use --osrm-url.\n");
            vl_graph_free(graph);
            return 1;
        }
        if (opts.verbose) {
            fprintf(stderr, "Checking OSRM server: %s\n", opts.osrm_url);
        }
        if (!osrm_check_server(opts.osrm_url, opts.verbose)) {
            fprintf(stderr, "Warning: OSRM server not responding, comparison may fail\n");
        }
    }

    /* Create landmarks if enabled */
    VLLandmarks *landmarks = NULL;
    if (opts.use_landmarks && opts.num_landmarks > 0) {
        if (opts.verbose) {
            fprintf(stderr, "Creating %d landmarks...\n", opts.num_landmarks);
        }
        double start = get_time_ms();
        landmarks = vl_landmarks_create(graph, opts.num_landmarks);
        if (opts.verbose && landmarks) {
            fprintf(stderr, "Landmarks created in %.1f ms\n", get_time_ms() - start);
        }
    }

    /* Open output */
    FILE *out = stdout;
    if (opts.output_file[0]) {
        out = fopen(opts.output_file, "w");
        if (!out) {
            fprintf(stderr, "Failed to open output: %s\n", opts.output_file);
            vl_graph_free(graph);
            return 1;
        }
    }

    /* Run benchmarks */
    fprintf(out, "{\n");
    fprintf(out, "  \"benchmark\": {\n");
    fprintf(out, "    \"tool_version\": \"1.1.0\",\n");
    fprintf(out, "    \"graph_file\": \"%s\",\n", opts.graph_file);
    fprintf(out, "    \"graph_nodes\": %u,\n", graph->num_nodes);
    fprintf(out, "    \"graph_edges\": %u,\n", graph->num_edges);
    if (opts.compare_osrm) {
        fprintf(out, "    \"osrm_url\": \"%s\"\n", opts.osrm_url);
    } else {
        fprintf(out, "    \"osrm_url\": null\n");
    }
    fprintf(out, "  },\n");
    fprintf(out, "  \"routes\": [\n");

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < opts.num_route_files; i++) {
        BenchRoute route;
        if (!load_route(opts.route_files[i], &route, &opts)) {
            if (opts.verbose) {
                fprintf(stderr, "Skipping invalid route: %s\n", opts.route_files[i]);
            }
            continue;
        }

        if (opts.verbose) {
            fprintf(stderr, "Running: %s... ", route.name);
            fflush(stderr);
        }

        BenchResult result;
        run_single_route(graph, landmarks, &route, &result, &opts);

        if (result.validation.overall_pass) {
            passed++;
            if (opts.verbose) fprintf(stderr, "PASS (%.1f ms)\n", result.performance.query_time_ms);
        } else {
            failed++;
            if (opts.verbose) fprintf(stderr, "FAIL\n");
        }

        if (i > 0) fprintf(out, ",\n");
        output_result_json(&result, graph, out);
    }

    fprintf(out, "\n  ],\n");
    fprintf(out, "  \"summary\": {\n");
    fprintf(out, "    \"total\": %d,\n", passed + failed);
    fprintf(out, "    \"passed\": %d,\n", passed);
    fprintf(out, "    \"failed\": %d\n", failed);
    fprintf(out, "  }\n");
    fprintf(out, "}\n");

    if (out != stdout) fclose(out);

    /* Cleanup */
    for (int i = 0; i < opts.num_route_files; i++) {
        free(opts.route_files[i]);
    }
    if (landmarks) vl_landmarks_free(landmarks);
    vl_graph_free(graph);

    if (opts.verbose) {
        fprintf(stderr, "\nSummary: %d/%d passed\n", passed, passed + failed);
    }

    return failed > 0 ? 1 : 0;
}
