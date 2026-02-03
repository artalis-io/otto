/*
 * Velo Route Server
 *
 * A lightweight routing API server that provides route planning
 * between coordinates using the velo routing engine.
 *
 * Endpoints:
 *   GET  /api/v1/health           - Health check
 *   GET  /api/v1/stats            - Graph statistics
 *   GET  /api/v1/route            - Calculate route
 *   POST /api/v1/route            - Calculate route (JSON body)
 *
 * Route Parameters:
 *   from      - Origin coordinates (lat,lon)
 *   to        - Destination coordinates (lat,lon)
 *   profile   - Vehicle profile: car, truck, bike, foot (default: car)
 *   mode      - Optimization: fastest, shortest (default: fastest)
 *   geometry  - Include path: true, false (default: true)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include "mongoose.h"
#include "velo.h"
#include "polyline.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    char graph_path[512];
    char listen_addr[64];
    int port;
    int use_landmarks;
    int landmark_count;
    char name[128];
} RouteServerConfig;

/* Default configuration */
static RouteServerConfig s_config = {
    .graph_path = "",
    .listen_addr = "0.0.0.0",
    .port = 8082,
    .use_landmarks = 1,
    .landmark_count = 32,
    .name = "Velo Route Server"
};

/* Global state */
static int s_signo = 0;
static VLGraph *s_graph = NULL;
static VLLandmarks *s_landmarks = NULL;

static void signal_handler(int signo) {
    s_signo = signo;
}

/* ============================================================================
 * Configuration Loading
 * ============================================================================ */

static char *trim(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str);
    while (end > str && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return str;
}

/*
 * Safe integer parsing with error detection.
 * Returns 1 on success, 0 on failure (invalid input or out of range).
 */
static int safe_parse_int(const char *str, int *out) {
    if (!str || !*str) return 0;
    char *endptr;
    long val = strtol(str, &endptr, 10);
    if (endptr == str || *endptr != '\0') return 0;
    if (val < INT_MIN || val > INT_MAX) return 0;
    *out = (int)val;
    return 1;
}

static int load_config_file(const char *filename, RouteServerConfig *cfg) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (*trimmed == '#' || *trimmed == '\0') continue;

        char *colon = strchr(trimmed, ':');
        if (!colon) continue;

        *colon = '\0';
        char *key = trim(trimmed);
        char *value = trim(colon + 1);

        /* Remove quotes */
        size_t vlen = strlen(value);
        if (vlen >= 2 && ((value[0] == '"' && value[vlen-1] == '"') ||
                          (value[0] == '\'' && value[vlen-1] == '\''))) {
            value[vlen-1] = '\0';
            value++;
        }

        if (strcmp(key, "graph_path") == 0 || strcmp(key, "graph") == 0) {
            strncpy(cfg->graph_path, value, sizeof(cfg->graph_path) - 1);
            cfg->graph_path[sizeof(cfg->graph_path) - 1] = '\0';
        } else if (strcmp(key, "listen") == 0 || strcmp(key, "host") == 0) {
            strncpy(cfg->listen_addr, value, sizeof(cfg->listen_addr) - 1);
            cfg->listen_addr[sizeof(cfg->listen_addr) - 1] = '\0';
        } else if (strcmp(key, "port") == 0) {
            safe_parse_int(value, &cfg->port);
        } else if (strcmp(key, "landmarks") == 0) {
            safe_parse_int(value, &cfg->use_landmarks);
        } else if (strcmp(key, "landmark_count") == 0) {
            safe_parse_int(value, &cfg->landmark_count);
        } else if (strcmp(key, "name") == 0) {
            strncpy(cfg->name, value, sizeof(cfg->name) - 1);
            cfg->name[sizeof(cfg->name) - 1] = '\0';
        }
    }

    fclose(f);
    return 0;
}

static void load_config_env(RouteServerConfig *cfg) {
    const char *val;

    if ((val = getenv("ROUTE_GRAPH_PATH")) || (val = getenv("GRAPH_PATH"))) {
        strncpy(cfg->graph_path, val, sizeof(cfg->graph_path) - 1);
        cfg->graph_path[sizeof(cfg->graph_path) - 1] = '\0';
    }
    if ((val = getenv("ROUTE_PORT")) || (val = getenv("PORT"))) {
        safe_parse_int(val, &cfg->port);
    }
    if ((val = getenv("ROUTE_HOST")) || (val = getenv("HOST"))) {
        strncpy(cfg->listen_addr, val, sizeof(cfg->listen_addr) - 1);
        cfg->listen_addr[sizeof(cfg->listen_addr) - 1] = '\0';
    }
    if ((val = getenv("ROUTE_LANDMARKS"))) {
        safe_parse_int(val, &cfg->use_landmarks);
    }
    if ((val = getenv("ROUTE_LANDMARK_COUNT"))) {
        safe_parse_int(val, &cfg->landmark_count);
    }
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

static void send_json(struct mg_connection *c, int status, const char *json) {
    mg_http_reply(c, status,
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n",
        "%s", json);
}

static void send_error(struct mg_connection *c, int status, const char *message) {
    mg_http_reply(c, status,
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n",
        "{\"error\": \"%s\"}\n", message);
}

/* Escape backslashes in polyline for JSON output */
static char *json_escape_polyline(const char *polyline) {
    if (!polyline) return NULL;

    /* Count backslashes */
    size_t len = strlen(polyline);
    size_t backslashes = 0;
    for (size_t i = 0; i < len; i++) {
        if (polyline[i] == '\\') backslashes++;
    }

    /* Allocate escaped string (check for overflow) */
    if (len > SIZE_MAX - backslashes - 1) return NULL;
    char *escaped = malloc(len + backslashes + 1);
    if (!escaped) return NULL;

    /* Copy with escaping */
    char *dst = escaped;
    for (size_t i = 0; i < len; i++) {
        if (polyline[i] == '\\') {
            *dst++ = '\\';
        }
        *dst++ = polyline[i];
    }
    *dst = '\0';

    return escaped;
}

/* ============================================================================
 * Query Parameter Parsing
 * ============================================================================ */

static int parse_coord(struct mg_str str, double *lat, double *lon) {
    char buf[64];
    if (str.len == 0 || str.len >= sizeof(buf)) return -1;
    memcpy(buf, str.buf, str.len);
    buf[str.len] = '\0';

    char *comma = strchr(buf, ',');
    if (!comma) return -1;
    *comma = '\0';

    *lat = atof(buf);
    *lon = atof(comma + 1);

    /* Reject inf/NaN from malformed input like "1e1000" */
    if (!isfinite(*lat) || !isfinite(*lon)) return -1;
    if (*lat < -90 || *lat > 90 || *lon < -180 || *lon > 180) return -1;
    return 0;
}

static VLProfile parse_profile(struct mg_str str) {
    if (str.len == 0) return VL_PROFILE_CAR;

    if (mg_match(str, mg_str("car"), NULL)) return VL_PROFILE_CAR;
    if (mg_match(str, mg_str("truck"), NULL)) return VL_PROFILE_TRUCK;
    if (mg_match(str, mg_str("bike"), NULL)) return VL_PROFILE_BIKE;
    if (mg_match(str, mg_str("bicycle"), NULL)) return VL_PROFILE_BIKE;
    if (mg_match(str, mg_str("foot"), NULL)) return VL_PROFILE_FOOT;
    if (mg_match(str, mg_str("pedestrian"), NULL)) return VL_PROFILE_FOOT;
    if (mg_match(str, mg_str("walk"), NULL)) return VL_PROFILE_FOOT;

    return VL_PROFILE_CAR;
}

static VLWeightType parse_mode(struct mg_str str) {
    if (str.len == 0) return VL_WEIGHT_DURATION;

    if (mg_match(str, mg_str("fastest"), NULL)) return VL_WEIGHT_DURATION;
    if (mg_match(str, mg_str("shortest"), NULL)) return VL_WEIGHT_DISTANCE;
    if (mg_match(str, mg_str("duration"), NULL)) return VL_WEIGHT_DURATION;
    if (mg_match(str, mg_str("distance"), NULL)) return VL_WEIGHT_DISTANCE;

    return VL_WEIGHT_DURATION;
}

static int parse_bool(struct mg_str str, int default_val) {
    if (str.len == 0) return default_val;

    if (mg_match(str, mg_str("true"), NULL)) return 1;
    if (mg_match(str, mg_str("1"), NULL)) return 1;
    if (mg_match(str, mg_str("yes"), NULL)) return 1;
    if (mg_match(str, mg_str("false"), NULL)) return 0;
    if (mg_match(str, mg_str("0"), NULL)) return 0;
    if (mg_match(str, mg_str("no"), NULL)) return 0;

    return default_val;
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

static void handle_health(struct mg_connection *c) {
    char response[512];
    snprintf(response, sizeof(response),
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"velo-route-server\",\n"
        "  \"version\": \"%s\"\n"
        "}\n",
        vl_version());
    send_json(c, 200, response);
}

static void handle_stats(struct mg_connection *c) {
    if (!s_graph) {
        send_error(c, 503, "Graph not loaded");
        return;
    }

    char response[2048];
    snprintf(response, sizeof(response),
        "{\n"
        "  \"graph_path\": \"%s\",\n"
        "  \"num_nodes\": %u,\n"
        "  \"num_edges\": %u,\n"
        "  \"landmarks_enabled\": %s,\n"
        "  \"landmark_count\": %d,\n"
        "  \"bbox\": {\n"
        "    \"min_lat\": %.6f,\n"
        "    \"min_lon\": %.6f,\n"
        "    \"max_lat\": %.6f,\n"
        "    \"max_lon\": %.6f\n"
        "  }\n"
        "}\n",
        s_config.graph_path,
        s_graph->num_nodes,
        s_graph->num_edges,
        s_landmarks ? "true" : "false",
        s_landmarks ? s_config.landmark_count : 0,
        s_graph->bbox_min.lat,
        s_graph->bbox_min.lon,
        s_graph->bbox_max.lat,
        s_graph->bbox_max.lon);
    send_json(c, 200, response);
}

static void handle_route(struct mg_connection *c, struct mg_http_message *hm) {
    if (!s_graph) {
        send_error(c, 503, "Graph not loaded");
        return;
    }

    /* Parse parameters */
    double from_lat = 0, from_lon = 0, to_lat = 0, to_lon = 0;
    VLProfile profile = VL_PROFILE_CAR;
    VLWeightType weight = VL_WEIGHT_DURATION;
    int include_geometry = 1;

    if (mg_match(hm->method, mg_str("POST"), NULL)) {
        /* Parse JSON body */
        struct mg_str body = hm->body;

        /* Parse from */
        char *from_str = mg_json_get_str(body, "$.from");
        if (from_str) {
            struct mg_str from_val = mg_str(from_str);
            if (parse_coord(from_val, &from_lat, &from_lon) != 0) {
                free(from_str);
                send_error(c, 400, "Invalid 'from' coordinate");
                return;
            }
            free(from_str);
        }

        /* Parse to */
        char *to_str = mg_json_get_str(body, "$.to");
        if (to_str) {
            struct mg_str to_val = mg_str(to_str);
            if (parse_coord(to_val, &to_lat, &to_lon) != 0) {
                free(to_str);
                send_error(c, 400, "Invalid 'to' coordinate");
                return;
            }
            free(to_str);
        }

        /* Parse profile */
        char *profile_str = mg_json_get_str(body, "$.profile");
        if (profile_str) {
            profile = parse_profile(mg_str(profile_str));
            free(profile_str);
        }

        /* Parse mode */
        char *mode_str = mg_json_get_str(body, "$.mode");
        if (mode_str) {
            weight = parse_mode(mg_str(mode_str));
            free(mode_str);
        }

        /* Parse geometry */
        bool geom;
        if (mg_json_get_bool(body, "$.geometry", &geom)) {
            include_geometry = geom ? 1 : 0;
        }
    } else {
        /* GET request - parse query string */
        struct mg_str from_val = mg_http_var(hm->query, mg_str("from"));
        struct mg_str to_val = mg_http_var(hm->query, mg_str("to"));
        struct mg_str profile_val = mg_http_var(hm->query, mg_str("profile"));
        struct mg_str mode_val = mg_http_var(hm->query, mg_str("mode"));
        struct mg_str geom_val = mg_http_var(hm->query, mg_str("geometry"));

        if (from_val.len == 0) {
            send_error(c, 400, "Missing 'from' parameter");
            return;
        }
        if (to_val.len == 0) {
            send_error(c, 400, "Missing 'to' parameter");
            return;
        }

        if (parse_coord(from_val, &from_lat, &from_lon) != 0) {
            send_error(c, 400, "Invalid 'from' coordinate (format: lat,lon)");
            return;
        }
        if (parse_coord(to_val, &to_lat, &to_lon) != 0) {
            send_error(c, 400, "Invalid 'to' coordinate (format: lat,lon)");
            return;
        }

        profile = parse_profile(profile_val);
        weight = parse_mode(mode_val);
        include_geometry = parse_bool(geom_val, 1);
    }

    /* Validate coordinates are within graph bounds */
    if (from_lat < s_graph->bbox_min.lat || from_lat > s_graph->bbox_max.lat ||
        from_lon < s_graph->bbox_min.lon || from_lon > s_graph->bbox_max.lon) {
        send_error(c, 400, "Origin coordinate outside graph bounds");
        return;
    }
    if (to_lat < s_graph->bbox_min.lat || to_lat > s_graph->bbox_max.lat ||
        to_lon < s_graph->bbox_min.lon || to_lon > s_graph->bbox_max.lon) {
        send_error(c, 400, "Destination coordinate outside graph bounds");
        return;
    }

    /* Set up route options */
    VLRouteOptions opts = {0};
    opts.algorithm = VL_ALGORITHM_ASTAR_BIDIR;
    opts.weight = weight;
    opts.profile = profile;
    opts.include_geometry = include_geometry;

    /* Calculate route */
    VLCoord from = {from_lat, from_lon};
    VLCoord to = {to_lat, to_lon};

    VLRoute route;
    VLStatus status;

    if (s_landmarks) {
        /* Use landmarks for faster routing - need to find nearest nodes first */
        uint32_t from_node = vl_graph_nearest_node_grid(s_graph, from);
        uint32_t to_node = vl_graph_nearest_node_grid(s_graph, to);

        if (from_node == VL_INVALID_NODE) {
            send_error(c, 400, "Could not find road near origin");
            return;
        }
        if (to_node == VL_INVALID_NODE) {
            send_error(c, 400, "Could not find road near destination");
            return;
        }

        status = vl_route_astar_landmarks_bidir(s_graph, s_landmarks, from_node, to_node, &opts, &route);
    } else {
        /* Use vl_route_coords which handles nearest-node lookup internally */
        status = vl_route_coords(s_graph, from, to, &opts, &route);
    }

    if (status != VL_OK) {
        const char *msg = "Routing failed";
        switch (status) {
            case VL_ERROR_NO_ROUTE: msg = "No route found"; break;
            case VL_ERROR_NODE_NOT_FOUND: msg = "Could not find road near coordinate"; break;
            case VL_ERROR_INVALID_ARGUMENT: msg = "Invalid argument"; break;
            default: break;
        }
        send_error(c, 404, msg);
        return;
    }

    /* Encode polyline if geometry requested */
    char *polyline = NULL;
    if (include_geometry && route.num_coords > 0) {
        size_t max_len = polyline_max_encoded_size(route.num_coords);
        polyline = malloc(max_len);
        if (polyline) {
            /* Convert VLCoord array to double array (check for overflow first) */
            double *coords = NULL;
            if ((size_t)route.num_coords <= SIZE_MAX / (2 * sizeof(double))) {
                size_t coord_size = (size_t)route.num_coords * 2 * sizeof(double);
                coords = malloc(coord_size);
            }
            if (coords) {
                for (int i = 0; i < route.num_coords; i++) {
                    coords[i * 2] = route.coords[i].lat;
                    coords[i * 2 + 1] = route.coords[i].lon;
                }
                polyline_encode(coords, route.num_coords, 5, polyline, max_len);
                free(coords);
            } else {
                free(polyline);
                polyline = NULL;
            }
        }
    }

    /* Build JSON response (double polyline size for potential backslash escaping) */
    size_t resp_capacity = 4096 + (polyline ? strlen(polyline) * 2 : 0);
    char *response = malloc(resp_capacity);
    if (!response) {
        if (polyline) free(polyline);
        vl_free_route(&route);
        send_error(c, 500, "Out of memory");
        return;
    }

    const char *profile_str = "car";
    switch (profile) {
        case VL_PROFILE_TRUCK: profile_str = "truck"; break;
        case VL_PROFILE_BIKE: profile_str = "bike"; break;
        case VL_PROFILE_FOOT: profile_str = "foot"; break;
        default: break;
    }

    const char *mode_str = weight == VL_WEIGHT_DISTANCE ? "shortest" : "fastest";

    size_t n = 0;
    int written = snprintf(response, resp_capacity,
        "{\n"
        "  \"status\": \"ok\",\n"
        "  \"route\": {\n"
        "    \"distance\": %.2f,\n"
        "    \"duration\": %.2f,\n"
        "    \"profile\": \"%s\",\n"
        "    \"mode\": \"%s\",\n"
        "    \"from\": [%.6f, %.6f],\n"
        "    \"to\": [%.6f, %.6f]",
        route.distance_m,
        route.duration_s,
        profile_str,
        mode_str,
        from_lat, from_lon,
        to_lat, to_lon);
    if (written > 0) n = (size_t)written;

    if (polyline && n < resp_capacity) {
        char *escaped = json_escape_polyline(polyline);
        if (escaped) {
            written = snprintf(response + n, resp_capacity - n,
                ",\n    \"geometry\": \"%s\"",
                escaped);
            if (written > 0) n += (size_t)written;
            free(escaped);
        }
    }

    if (n < resp_capacity) {
        written = snprintf(response + n, resp_capacity - n,
            "\n  },\n"
            "  \"meta\": {\n"
            "    \"nodes_explored\": %u,\n"
            "    \"search_time_ms\": %.2f\n"
            "  }\n"
            "}\n",
            route.nodes_explored,
            route.search_time_ms);
        if (written > 0) n += (size_t)written;
    }

    send_json(c, 200, response);

    free(response);
    if (polyline) free(polyline);
    vl_free_route(&route);
}

/* ============================================================================
 * Main Event Handler
 * ============================================================================ */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 204,
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Access-Control-Max-Age: 86400\r\n",
                "");
            return;
        }

        /* Route requests */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c);
        } else if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
            handle_stats(c);
        } else if (mg_match(hm->uri, mg_str("/api/v1/route"), NULL)) {
            handle_route(c, hm);
        } else {
            send_error(c, 404, "Not found");
        }
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Velo Route Server\n\n");
    printf("Usage: %s [options] <graph-file>\n\n", prog);
    printf("Options:\n");
    printf("  -p, --port PORT      Port to listen on (default: 8082)\n");
    printf("  -h, --host HOST      Host to bind to (default: 0.0.0.0)\n");
    printf("  -c, --config FILE    Configuration file (YAML format)\n");
    printf("  --no-landmarks       Disable ALT landmarks\n");
    printf("  --landmarks N        Number of landmarks (default: 32)\n");
    printf("  --help               Show this help\n");
    printf("\n");
    printf("Graph file can be:\n");
    printf("  - OSM PBF file (.osm.pbf)\n");
    printf("  - Velo binary graph (.vlg)\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  ROUTE_GRAPH_PATH     Path to graph file\n");
    printf("  ROUTE_PORT           Server port\n");
    printf("  ROUTE_HOST           Server host\n");
    printf("  ROUTE_LANDMARKS      Enable landmarks (0/1)\n");
    printf("  ROUTE_LANDMARK_COUNT Number of landmarks\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -p 8082 hungary-latest.osm.pbf\n", prog);
    printf("  curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=car&mode=fastest'\n");
}

int main(int argc, char *argv[]) {
    /* Load config from environment first */
    load_config_env(&s_config);

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (++i < argc) safe_parse_int(argv[i], &s_config.port);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) {
            if (++i < argc) strncpy(s_config.listen_addr, argv[i], sizeof(s_config.listen_addr) - 1);
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (++i < argc) {
                if (load_config_file(argv[i], &s_config) != 0) {
                    fprintf(stderr, "Warning: Could not load config file: %s\n", argv[i]);
                }
            }
        } else if (strcmp(argv[i], "--no-landmarks") == 0) {
            s_config.use_landmarks = 0;
        } else if (strcmp(argv[i], "--landmarks") == 0) {
            if (++i < argc) {
                safe_parse_int(argv[i], &s_config.landmark_count);
                s_config.use_landmarks = 1;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            strncpy(s_config.graph_path, argv[i], sizeof(s_config.graph_path) - 1);
        }
    }

    /* Validate config */
    if (s_config.graph_path[0] == '\0') {
        fprintf(stderr, "Error: No graph file specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Load graph */
    printf("Loading graph: %s\n", s_config.graph_path);

    const char *ext = strrchr(s_config.graph_path, '.');
    if (ext && strcmp(ext, ".vlg") == 0) {
        /* Load binary graph */
        s_graph = vl_load_binary(s_config.graph_path);
    } else {
        /* Load from PBF */
        s_graph = vl_load_pbf(s_config.graph_path);
    }

    if (!s_graph) {
        fprintf(stderr, "Error: Failed to load graph: %s\n", s_config.graph_path);
        return 1;
    }

    printf("Loaded: %u nodes, %u edges\n", s_graph->num_nodes, s_graph->num_edges);
    printf("Bounds: [%.4f, %.4f] to [%.4f, %.4f]\n",
           s_graph->bbox_min.lon, s_graph->bbox_min.lat,
           s_graph->bbox_max.lon, s_graph->bbox_max.lat);

    /* Build spatial index if not present */
    if (!s_graph->grid_index) {
        printf("Building spatial index...\n");
        vl_graph_build_grid_index(s_graph);
    }

    /* Build reverse index for bidirectional search */
    if (!s_graph->rev_edges) {
        printf("Building reverse index...\n");
        vl_graph_build_reverse_index(s_graph);
    }

    /* Create landmarks for ALT heuristic */
    if (s_config.use_landmarks && s_config.landmark_count > 0) {
        printf("Creating %d landmarks...\n", s_config.landmark_count);
        s_landmarks = vl_landmarks_create(s_graph, s_config.landmark_count);
        if (s_landmarks) {
            printf("Landmarks created successfully\n");
        } else {
            printf("Warning: Failed to create landmarks, using basic A*\n");
        }
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize mongoose */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* Build listen address */
    char listen_url[128];
    snprintf(listen_url, sizeof(listen_url), "http://%s:%d",
             s_config.listen_addr, s_config.port);

    /* Start listening */
    struct mg_connection *c = mg_http_listen(&mgr, listen_url, ev_handler, NULL);
    if (c == NULL) {
        fprintf(stderr, "Error: Cannot listen on %s\n", listen_url);
        vl_graph_free(s_graph);
        return 1;
    }

    printf("\nVelo Route Server v%s\n", vl_version());
    printf("Listening on http://%s:%d\n", s_config.listen_addr, s_config.port);
    printf("\nEndpoints:\n");
    printf("  GET  /api/v1/health  - Health check\n");
    printf("  GET  /api/v1/stats   - Graph statistics\n");
    printf("  GET  /api/v1/route   - Calculate route\n");
    printf("  POST /api/v1/route   - Calculate route (JSON)\n");
    printf("\nRoute parameters:\n");
    printf("  from=lat,lon         - Origin coordinates\n");
    printf("  to=lat,lon           - Destination coordinates\n");
    printf("  profile=car|truck|bike|foot\n");
    printf("  mode=fastest|shortest\n");
    printf("  geometry=true|false\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop */
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 1000);
    }

    printf("\nShutting down...\n");
    mg_mgr_free(&mgr);
    if (s_landmarks) vl_landmarks_free(s_landmarks);
    vl_graph_free(s_graph);

    return 0;
}
