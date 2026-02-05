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
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include "mongoose.h"
#include "velo.h"
#include "polyline.h"
#include "shared.h"   /* For sh_ratelimit, sh_workqueue, sh_cors, sh_capacity */
#include "sh_httpserver.h"  /* For sh_mg_set_write_timeout */

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    char graph_path[512];
    char save_index_path[512];  /* Path to save binary index (empty = don't save) */
    char listen_addr[64];
    int port;
    int use_landmarks;
    int landmark_count;
    char name[128];
    /* Rate limiting configuration */
    int rate_limit_enabled;   /* 1 = enabled, 0 = disabled */
    double rate_limit_rps;    /* Tokens refilled per second */
    double rate_limit_burst;  /* Maximum burst capacity */
    /* Work queue configuration */
    int work_queue_enabled;      /* 1 = enabled, 0 = disabled */
    size_t work_queue_depth;     /* Max pending requests */
    double work_queue_timeout;   /* Request timeout in seconds */
    int route_workers;           /* Number of route worker threads (0 = auto) */
    /* CORS configuration */
    char cors_origins[512];      /* Comma-separated allowed origins (empty = allow all) */
    /* Adaptive capacity configuration */
    int adaptive_enabled;        /* 1 = enabled, 0 = disabled */
    double target_utilization;   /* Target utilization (0.0-1.0) */
    double client_timeout_ms;    /* Client timeout in milliseconds */
    int burst_requests;          /* Requests in initial burst (for sizing) */
    size_t adaptive_window;      /* Sample window size for percentiles */
    double adaptive_interval;    /* Recalculation interval (requests) */
} RouteServerConfig;

/* Default configuration */
static RouteServerConfig s_config = {
    .graph_path = "",
    .save_index_path = "",
    .listen_addr = "0.0.0.0",
    .port = 8082,
    .use_landmarks = 1,
    .landmark_count = 32,
    .name = "Velo Route Server",
    .rate_limit_enabled = 1,  /* Enabled by default */
    .rate_limit_rps = 10.0,   /* 10 requests per second */
    .rate_limit_burst = 50.0, /* Burst capacity of 50 */
    .work_queue_enabled = 1,  /* Enabled by default */
    .work_queue_depth = 128,  /* Max 128 pending requests */
    .work_queue_timeout = 10.0, /* 10 second timeout (routing can be slow) */
    .route_workers = 0,       /* 0 = auto-detect CPU count */
    /* CORS: empty = allow all origins (*) */
    .cors_origins = "",
    /* Adaptive capacity: disabled by default */
    .adaptive_enabled = 0,
    .target_utilization = 0.7,
    .client_timeout_ms = 10000.0,
    .burst_requests = 10,
    .adaptive_window = 1000,
    .adaptive_interval = 1000
};

/* Global state */
static volatile sig_atomic_t s_signo = 0;
static VLGraph *s_graph = NULL;
static VLLandmarks *s_landmarks = NULL;

/* Rate limiter instance (uses shared library) */
static ShRateLimiter *s_rate_limiter = NULL;

/* Work queue instance (uses shared library) */
static ShWorkQueue *s_work_queue = NULL;

/* CORS configuration (uses shared library) */
static ShCorsConfig s_cors_config;

/* Adaptive capacity tracker (uses shared library) */
static ShAdaptiveTracker *s_adaptive_tracker = NULL;

/* Route work item - passed through the work queue */
typedef struct {
    /* Request parameters */
    double from_lat, from_lon;
    double to_lat, to_lon;
    VLProfile profile;
    VLWeightType weight;
    int include_geometry;

    /* Response (set by worker) */
    VLRoute route;
    VLStatus status;
    int completed;
    char error_msg[128];

    /* Completion signaling */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    volatile int cancelled;  /* Set by HTTP handler on timeout */
} RouteWorkItem;

/* Route worker thread state */
typedef struct {
    int id;
    pthread_t thread;
} RouteWorker;

static RouteWorker *s_route_workers = NULL;
static int s_num_route_workers = 0;

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
    /* Rate limiting configuration */
    if ((val = getenv("VELO_RATE_LIMIT_ENABLED"))) {
        cfg->rate_limit_enabled = (atoi(val) != 0);
    }
    if ((val = getenv("VELO_RATE_LIMIT_RPS"))) {
        cfg->rate_limit_rps = atof(val);
        if (cfg->rate_limit_rps <= 0) cfg->rate_limit_rps = 10.0;
    }
    if ((val = getenv("VELO_RATE_LIMIT_BURST"))) {
        cfg->rate_limit_burst = atof(val);
        if (cfg->rate_limit_burst <= 0) cfg->rate_limit_burst = 50.0;
    }
    /* Work queue configuration */
    if ((val = getenv("VELO_WORK_QUEUE_ENABLED"))) {
        cfg->work_queue_enabled = (atoi(val) != 0);
    }
    if ((val = getenv("VELO_WORK_QUEUE_DEPTH"))) {
        cfg->work_queue_depth = (size_t)atol(val);
        if (cfg->work_queue_depth < 1) cfg->work_queue_depth = 128;
    }
    if ((val = getenv("VELO_WORK_QUEUE_TIMEOUT"))) {
        cfg->work_queue_timeout = atof(val);
        if (cfg->work_queue_timeout <= 0) cfg->work_queue_timeout = 10.0;
    }
    if ((val = getenv("VELO_ROUTE_WORKERS"))) {
        cfg->route_workers = atoi(val);
    }
    /* CORS configuration */
    if ((val = getenv("VELO_CORS_ORIGINS"))) {
        strncpy(cfg->cors_origins, val, sizeof(cfg->cors_origins) - 1);
        cfg->cors_origins[sizeof(cfg->cors_origins) - 1] = '\0';
    }
    /* Adaptive capacity configuration */
    if ((val = getenv("VELO_ADAPTIVE_ENABLED"))) {
        cfg->adaptive_enabled = (atoi(val) != 0);
    }
    if ((val = getenv("VELO_TARGET_UTILIZATION"))) {
        cfg->target_utilization = atof(val);
        if (cfg->target_utilization <= 0 || cfg->target_utilization > 1.0) {
            cfg->target_utilization = 0.7;
        }
    }
    if ((val = getenv("VELO_CLIENT_TIMEOUT"))) {
        cfg->client_timeout_ms = atof(val);
        if (cfg->client_timeout_ms <= 0) cfg->client_timeout_ms = 10000.0;
    }
    if ((val = getenv("VELO_BURST_REQUESTS"))) {
        cfg->burst_requests = atoi(val);
        if (cfg->burst_requests < 1) cfg->burst_requests = 10;
    }
    if ((val = getenv("VELO_ADAPTIVE_WINDOW"))) {
        cfg->adaptive_window = (size_t)atol(val);
        if (cfg->adaptive_window < 10) cfg->adaptive_window = 1000;
    }
    if ((val = getenv("VELO_ADAPTIVE_INTERVAL"))) {
        cfg->adaptive_interval = atof(val);
        if (cfg->adaptive_interval < 1) cfg->adaptive_interval = 1000;
    }
}

/* ============================================================================
 * Route Work Queue Functions
 * ============================================================================ */

/* Initialize a route work item */
static void route_work_item_init(RouteWorkItem *item) {
    memset(item, 0, sizeof(*item));
    item->completed = 0;
    item->status = VL_ERROR_INVALID_ARGUMENT;
    pthread_mutex_init(&item->mutex, NULL);
    pthread_cond_init(&item->cond, NULL);
}

/* Clean up a route work item */
static void route_work_item_cleanup(RouteWorkItem *item) {
    pthread_mutex_destroy(&item->mutex);
    pthread_cond_destroy(&item->cond);
    vl_free_route(&item->route);
}

/* Wait for route work item completion with timeout */
static int route_work_item_wait(RouteWorkItem *item, double timeout_sec) {
    struct timespec abstime;
    struct timeval tv;
    gettimeofday(&tv, NULL);

    abstime.tv_sec = tv.tv_sec + (time_t)timeout_sec;
    abstime.tv_nsec = tv.tv_usec * 1000 +
                      (long)((timeout_sec - (time_t)timeout_sec) * 1e9);
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&item->mutex);
    while (!item->completed) {
        int rc = pthread_cond_timedwait(&item->cond, &item->mutex, &abstime);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&item->mutex);
            return 0;  /* Timeout */
        }
    }
    pthread_mutex_unlock(&item->mutex);
    return 1;  /* Completed */
}

/* Signal that route work item is completed */
static void route_work_item_complete(RouteWorkItem *item) {
    pthread_mutex_lock(&item->mutex);
    item->completed = 1;
    pthread_cond_signal(&item->cond);
    pthread_mutex_unlock(&item->mutex);
}

/* Process a single route request */
static void process_route_request(RouteWorkItem *item) {
    VLRouteOptions opts = {0};
    opts.algorithm = VL_ALGORITHM_ASTAR_BIDIR;
    opts.weight = item->weight;
    opts.profile = item->profile;
    opts.include_geometry = item->include_geometry;

    VLCoord from = {item->from_lat, item->from_lon};
    VLCoord to = {item->to_lat, item->to_lon};

    if (s_landmarks) {
        /* Use landmarks for faster routing */
        uint32_t from_node = vl_graph_nearest_node_grid(s_graph, from);
        uint32_t to_node = vl_graph_nearest_node_grid(s_graph, to);

        if (from_node == VL_INVALID_NODE) {
            item->status = VL_ERROR_NODE_NOT_FOUND;
            strncpy(item->error_msg, "Could not find road near origin",
                    sizeof(item->error_msg) - 1);
            item->error_msg[sizeof(item->error_msg) - 1] = '\0';
            return;
        }
        if (to_node == VL_INVALID_NODE) {
            item->status = VL_ERROR_NODE_NOT_FOUND;
            strncpy(item->error_msg, "Could not find road near destination",
                    sizeof(item->error_msg) - 1);
            item->error_msg[sizeof(item->error_msg) - 1] = '\0';
            return;
        }

        item->status = vl_route_astar_landmarks_bidir(s_graph, s_landmarks,
                                                       from_node, to_node,
                                                       &opts, &item->route);
    } else {
        item->status = vl_route_coords(s_graph, from, to, &opts, &item->route);
    }

    if (item->status != VL_OK) {
        switch (item->status) {
            case VL_ERROR_NO_ROUTE:
                strncpy(item->error_msg, "No route found", sizeof(item->error_msg) - 1);
                break;
            case VL_ERROR_NODE_NOT_FOUND:
                strncpy(item->error_msg, "Could not find road near coordinate",
                        sizeof(item->error_msg) - 1);
                break;
            default:
                strncpy(item->error_msg, "Routing failed", sizeof(item->error_msg) - 1);
                break;
        }
        item->error_msg[sizeof(item->error_msg) - 1] = '\0';
    }
}

/* Route worker thread function */
static void *route_worker_fn(void *arg) {
    RouteWorker *w = (RouteWorker *)arg;
    (void)w;  /* Worker ID for debugging if needed */

    while (s_signo == 0) {
        /* Pop work item with timeout (100ms to check for shutdown) */
        ShWorkItem *queue_item = sh_workqueue_pop_timeout(s_work_queue, 100);
        if (!queue_item) continue;

        RouteWorkItem *item = (RouteWorkItem *)queue_item->user_ctx;
        if (!item) {
            sh_workqueue_item_free(queue_item);
            continue;
        }

        /* Check if request has expired or was cancelled by HTTP handler timeout */
        if (sh_workqueue_item_expired(s_work_queue, queue_item) || item->cancelled) {
            item->status = VL_ERROR_INTERNAL;  /* Timeout */
            strncpy(item->error_msg, "Request timeout", sizeof(item->error_msg) - 1);
            item->error_msg[sizeof(item->error_msg) - 1] = '\0';
            route_work_item_complete(item);
            sh_workqueue_item_free(queue_item);
            continue;
        }

        /* Track timing for adaptive capacity */
        struct timeval route_start, route_end;
        gettimeofday(&route_start, NULL);

        /* Process the route request */
        process_route_request(item);

        /* Record response time for adaptive capacity */
        gettimeofday(&route_end, NULL);
        double route_ms = (route_end.tv_sec - route_start.tv_sec) * 1000.0 +
                          (route_end.tv_usec - route_start.tv_usec) / 1000.0;

        if (s_adaptive_tracker) {
            sh_adaptive_record(s_adaptive_tracker, route_ms);

            /* Check if rate limiter should be updated */
            ShCapacityParams new_params;
            if (sh_adaptive_update(s_adaptive_tracker, &new_params)) {
                /* Update rate limiter with new parameters */
                if (s_rate_limiter) {
                    sh_ratelimit_update_rate(s_rate_limiter,
                                             new_params.rate_limit_rps,
                                             new_params.rate_limit_burst);
                }
            }
        }

        /* Signal completion */
        route_work_item_complete(item);
        sh_workqueue_item_free(queue_item);
    }

    return NULL;
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

static void send_json_with_cors(struct mg_connection *c, int status,
                                 const char *json, const char *origin) {
    char cors_hdrs[512];
    sh_cors_headers(&s_cors_config, origin, cors_hdrs, sizeof(cors_hdrs));
    char headers[600];
    snprintf(headers, sizeof(headers), "Content-Type: application/json\r\n%s", cors_hdrs);
    mg_http_reply(c, status, headers, "%s", json);
}

static void send_error_with_cors(struct mg_connection *c, int status,
                                  const char *message, const char *origin) {
    char cors_hdrs[512];
    sh_cors_headers(&s_cors_config, origin, cors_hdrs, sizeof(cors_hdrs));
    char headers[600];
    snprintf(headers, sizeof(headers), "Content-Type: application/json\r\n%s", cors_hdrs);
    mg_http_reply(c, status, headers, "{\"error\": \"%s\"}\n", message);
}

/* Compatibility wrappers for simple calls (uses wildcard origin) */
static void send_json(struct mg_connection *c, int status, const char *json) {
    send_json_with_cors(c, status, json, NULL);
}

static void send_error(struct mg_connection *c, int status, const char *message) {
    send_error_with_cors(c, status, message, NULL);
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

    /* Get work queue stats */
    ShWorkQueueStats wq_stats = {0};
    if (s_work_queue) {
        sh_workqueue_stats(s_work_queue, &wq_stats);
    }

    /* Get rate limiter stats */
    ShRateLimitStats rl_stats = {0};
    if (s_rate_limiter) {
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
    }

    /* Get adaptive capacity stats */
    ShAdaptiveStats adaptive_stats = {0};
    ShCapacityParams adaptive_params = {0};
    int has_adaptive_params = 0;
    if (s_adaptive_tracker) {
        sh_adaptive_stats(s_adaptive_tracker, &adaptive_stats);
        has_adaptive_params = sh_adaptive_get_params(s_adaptive_tracker, &adaptive_params);
    }

    char response[6144];
    int n = snprintf(response, sizeof(response),
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
        "  },\n"
        "  \"work_queue\": {\n"
        "    \"enabled\": %s,\n"
        "    \"depth\": %zu,\n"
        "    \"capacity\": %zu,\n"
        "    \"pushed\": %lu,\n"
        "    \"popped\": %lu,\n"
        "    \"dropped\": %lu,\n"
        "    \"expired\": %lu\n"
        "  },\n"
        "  \"rate_limit\": {\n"
        "    \"enabled\": %s,\n"
        "    \"rps\": %.1f,\n"
        "    \"burst\": %.0f,\n"
        "    \"allowed\": %lu,\n"
        "    \"denied\": %lu\n"
        "  },\n"
        "  \"adaptive\": {\n"
        "    \"enabled\": %s,\n"
        "    \"sample_count\": %lu,\n"
        "    \"recalc_count\": %lu",
        s_config.graph_path,
        s_graph->num_nodes,
        s_graph->num_edges,
        s_landmarks ? "true" : "false",
        s_landmarks ? s_config.landmark_count : 0,
        s_graph->bbox_min.lat,
        s_graph->bbox_min.lon,
        s_graph->bbox_max.lat,
        s_graph->bbox_max.lon,
        s_work_queue ? "true" : "false",
        wq_stats.current_depth, wq_stats.max_capacity,
        (unsigned long)wq_stats.total_pushed, (unsigned long)wq_stats.total_popped,
        (unsigned long)wq_stats.total_dropped, (unsigned long)wq_stats.total_expired,
        s_rate_limiter ? "true" : "false",
        s_config.rate_limit_rps, s_config.rate_limit_burst,
        (unsigned long)rl_stats.requests_allowed, (unsigned long)rl_stats.requests_denied,
        s_adaptive_tracker ? "true" : "false",
        (unsigned long)adaptive_stats.sample_count, (unsigned long)adaptive_stats.recalc_count);

    /* Add percentile stats if we have samples */
    if (s_adaptive_tracker && adaptive_stats.sample_count > 0 && n > 0 && (size_t)n < sizeof(response)) {
        n += snprintf(response + n, sizeof(response) - (size_t)n,
            ",\n    \"p50_ms\": %.2f,\n"
            "    \"p90_ms\": %.2f,\n"
            "    \"p99_ms\": %.2f,\n"
            "    \"avg_ms\": %.2f,\n"
            "    \"ema_ms\": %.2f",
            adaptive_stats.p50_ms, adaptive_stats.p90_ms, adaptive_stats.p99_ms,
            adaptive_stats.avg_ms, adaptive_stats.ema_ms);
    }

    /* Add calculated params if available */
    if (has_adaptive_params && n > 0 && (size_t)n < sizeof(response)) {
        n += snprintf(response + n, sizeof(response) - (size_t)n,
            ",\n    \"calc_rps\": %.2f,\n"
            "    \"calc_burst\": %.0f",
            adaptive_params.rate_limit_rps, adaptive_params.rate_limit_burst);
    }

    /* Close adaptive section and response */
    if (n > 0 && (size_t)n < sizeof(response)) {
        snprintf(response + n, sizeof(response) - (size_t)n, "\n  }\n}\n");
    }

    send_json(c, 200, response);
}

static void handle_route(struct mg_connection *c, struct mg_http_message *hm) {
    if (!s_graph) {
        send_error(c, 503, "Graph not loaded");
        return;
    }

    /* Extract Origin header for CORS */
    struct mg_str *origin_hdr = mg_http_get_header(hm, "Origin");
    char origin[256] = "";
    if (origin_hdr && origin_hdr->len > 0 && origin_hdr->len < sizeof(origin)) {
        memcpy(origin, origin_hdr->buf, origin_hdr->len);
        origin[origin_hdr->len] = '\0';
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

    VLRoute route;
    VLStatus status;

    /* Use work queue if enabled */
    if (s_work_queue) {
        RouteWorkItem item;
        route_work_item_init(&item);
        item.from_lat = from_lat;
        item.from_lon = from_lon;
        item.to_lat = to_lat;
        item.to_lon = to_lon;
        item.profile = profile;
        item.weight = weight;
        item.include_geometry = include_geometry;

        /* Create queue item */
        ShWorkItem queue_item = {
            .data = NULL,
            .data_len = 0,
            .user_ctx = &item
        };

        /* Try to push to queue */
        double pressure;
        if (!sh_workqueue_try_push(s_work_queue, &queue_item, &pressure)) {
            route_work_item_cleanup(&item);
            char cors_hdrs[512];
            sh_cors_headers(&s_cors_config, origin, cors_hdrs, sizeof(cors_hdrs));
            char headers[600];
            snprintf(headers, sizeof(headers),
                     "Content-Type: text/plain\r\n"
                     "Retry-After: 1\r\n%s", cors_hdrs);
            mg_http_reply(c, 503, headers, "Server busy, try again later\n");
            return;
        }

        /* Wait for completion with timeout */
        if (!route_work_item_wait(&item, s_config.work_queue_timeout)) {
            /* Mark item as cancelled so worker can skip if not started */
            item.cancelled = 1;
            route_work_item_cleanup(&item);
            char cors_hdrs[512];
            sh_cors_headers(&s_cors_config, origin, cors_hdrs, sizeof(cors_hdrs));
            char headers[600];
            snprintf(headers, sizeof(headers),
                     "Content-Type: text/plain\r\n%s", cors_hdrs);
            mg_http_reply(c, 504, headers, "Request timeout\n");
            return;
        }

        status = item.status;
        if (status != VL_OK) {
            send_error(c, 404, item.error_msg);
            route_work_item_cleanup(&item);
            return;
        }

        /* Move route from item to local variable */
        route = item.route;
        memset(&item.route, 0, sizeof(item.route));  /* Prevent double-free */
        route_work_item_cleanup(&item);
    } else {
        /* Direct routing (work queue disabled) */
        VLRouteOptions opts = {0};
        opts.algorithm = VL_ALGORITHM_ASTAR_BIDIR;
        opts.weight = weight;
        opts.profile = profile;
        opts.include_geometry = include_geometry;

        VLCoord from = {from_lat, from_lon};
        VLCoord to = {to_lat, to_lon};

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
    /* Set socket write timeout on new connections to protect against slow clients */
    if (ev == MG_EV_ACCEPT) {
        sh_mg_set_write_timeout(c, 5000);  /* 5 second write timeout */
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* Extract Origin header for CORS */
        struct mg_str *origin_hdr = mg_http_get_header(hm, "Origin");
        char origin[256] = "";
        if (origin_hdr && origin_hdr->len > 0 && origin_hdr->len < sizeof(origin)) {
            memcpy(origin, origin_hdr->buf, origin_hdr->len);
            origin[origin_hdr->len] = '\0';
        }

        /* Rate limiting check (supports both IPv4 and IPv6) */
        if (s_rate_limiter) {
            ShRateLimitAddr client_addr;
            if (c->rem.is_ip6) {
                sh_ratelimit_addr_ipv6(&client_addr,
                                       c->rem.addr.ip6[0], c->rem.addr.ip6[1]);
            } else {
                sh_ratelimit_addr_ipv4(&client_addr, c->rem.addr.ip4);
            }
            if (!sh_ratelimit_check(s_rate_limiter, &client_addr)) {
                char cors_hdrs[512];
                sh_cors_headers(&s_cors_config, origin, cors_hdrs, sizeof(cors_hdrs));
                char headers[600];
                snprintf(headers, sizeof(headers),
                         "Content-Type: text/plain\r\n"
                         "Retry-After: 1\r\n%s", cors_hdrs);
                mg_http_reply(c, 429, headers, "Rate limit exceeded\n");
                return;
            }
        }

        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            char cors_hdrs[512];
            sh_cors_preflight_headers(&s_cors_config, origin, cors_hdrs, sizeof(cors_hdrs));
            mg_http_reply(c, 204, cors_hdrs, "");
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
    printf("  --save-index FILE    Save binary index to FILE (for faster future startup)\n");
    printf("  --help               Show this help\n");
    printf("\n");
    printf("Graph file can be:\n");
    printf("  - OSM PBF file (.osm.pbf)\n");
    printf("  - Velo binary graph (.vlg) - much faster to load\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  ROUTE_GRAPH_PATH         Path to graph file\n");
    printf("  ROUTE_PORT               Server port\n");
    printf("  ROUTE_HOST               Server host\n");
    printf("  ROUTE_LANDMARKS          Enable landmarks (0/1)\n");
    printf("  ROUTE_LANDMARK_COUNT     Number of landmarks\n");
    printf("\n");
    printf("  Rate limiting:\n");
    printf("  VELO_RATE_LIMIT_ENABLED  Enable rate limiting (default: 1)\n");
    printf("  VELO_RATE_LIMIT_RPS      Requests per second (default: 10)\n");
    printf("  VELO_RATE_LIMIT_BURST    Burst capacity (default: 50)\n");
    printf("\n");
    printf("  Work queue:\n");
    printf("  VELO_WORK_QUEUE_ENABLED  Enable work queue (default: 1)\n");
    printf("  VELO_WORK_QUEUE_DEPTH    Max pending requests (default: 128)\n");
    printf("  VELO_WORK_QUEUE_TIMEOUT  Request timeout in seconds (default: 10)\n");
    printf("  VELO_ROUTE_WORKERS       Route worker count (0 = auto)\n");
    printf("\n");
    printf("  CORS:\n");
    printf("  VELO_CORS_ORIGINS        Comma-separated allowed origins (empty = allow all)\n");
    printf("\n");
    printf("  Adaptive capacity:\n");
    printf("  VELO_ADAPTIVE_ENABLED    Enable adaptive capacity (default: 0)\n");
    printf("  VELO_TARGET_UTILIZATION  Target utilization 0.0-1.0 (default: 0.7)\n");
    printf("  VELO_CLIENT_TIMEOUT      Client timeout in ms (default: 10000)\n");
    printf("  VELO_BURST_REQUESTS      Requests in initial burst (default: 10)\n");
    printf("  VELO_ADAPTIVE_WINDOW     Sample window size (default: 1000)\n");
    printf("  VELO_ADAPTIVE_INTERVAL   Recalculation interval (default: 1000)\n");
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
        } else if (strcmp(argv[i], "--save-index") == 0 || strcmp(argv[i], "-s") == 0) {
            if (++i < argc) {
                strncpy(s_config.save_index_path, argv[i], sizeof(s_config.save_index_path) - 1);
                s_config.save_index_path[sizeof(s_config.save_index_path) - 1] = '\0';
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

    /* Save binary index if requested */
    if (s_config.save_index_path[0] != '\0') {
        printf("Saving binary index to: %s\n", s_config.save_index_path);
        VLStatus save_status = vl_save_binary(s_graph, s_config.save_index_path);
        if (save_status == VL_OK) {
            printf("Index saved successfully\n");
            /* Exit after saving - this is typically a pre-build step */
            vl_graph_free(s_graph);
            return 0;
        } else {
            fprintf(stderr, "Warning: Failed to save index\n");
        }
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

    /* Initialize rate limiter (uses shared library) */
    if (s_config.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(s_config.rate_limit_rps,
                                             s_config.rate_limit_burst, 4096);
        if (s_rate_limiter) {
            printf("Rate limit: %.0f RPS, burst %.0f (IPv4 + IPv6)\n",
                   s_config.rate_limit_rps, s_config.rate_limit_burst);
        } else {
            fprintf(stderr, "Warning: Failed to create rate limiter\n");
        }
    } else {
        printf("Rate limit: disabled\n");
    }

    /* Initialize work queue and route workers */
    if (s_config.work_queue_enabled) {
        s_work_queue = sh_workqueue_create(s_config.work_queue_depth,
                                           s_config.work_queue_timeout);
        if (s_work_queue) {
            /* Determine number of route workers */
            int num_route_workers = s_config.route_workers;
            if (num_route_workers <= 0) {
#ifdef _SC_NPROCESSORS_ONLN
                long n = sysconf(_SC_NPROCESSORS_ONLN);
                num_route_workers = (n > 0) ? (int)n : 4;
#else
                num_route_workers = 4;
#endif
            }
            if (num_route_workers > 64) num_route_workers = 64;

            /* Allocate route workers */
            s_route_workers = calloc(num_route_workers, sizeof(RouteWorker));
            if (s_route_workers) {
                s_num_route_workers = num_route_workers;
                for (int i = 0; i < num_route_workers; i++) {
                    s_route_workers[i].id = i;
                    if (pthread_create(&s_route_workers[i].thread, NULL,
                                       route_worker_fn, &s_route_workers[i]) != 0) {
                        fprintf(stderr, "Error: Failed to create route worker %d\n", i);
                        s_num_route_workers = i;
                        break;
                    }
                }
                printf("Work queue: depth %zu, timeout %.1fs, %d route workers\n",
                       s_config.work_queue_depth, s_config.work_queue_timeout,
                       s_num_route_workers);
            } else {
                fprintf(stderr, "Warning: Failed to allocate route workers\n");
                sh_workqueue_free(s_work_queue);
                s_work_queue = NULL;
            }
        } else {
            fprintf(stderr, "Warning: Failed to create work queue\n");
        }
    } else {
        printf("Work queue: disabled\n");
    }

    /* Initialize CORS configuration */
    sh_cors_init(&s_cors_config);
    sh_cors_set_methods(&s_cors_config, "GET, POST, OPTIONS");
    sh_cors_set_headers(&s_cors_config, "Content-Type, Authorization");
    if (s_config.cors_origins[0] != '\0') {
        int added = sh_cors_parse_origins(&s_cors_config, s_config.cors_origins);
        printf("CORS: %d allowed origin%s\n", added, added == 1 ? "" : "s");
    } else {
        printf("CORS: allowing all origins (*)\n");
    }

    /* Initialize adaptive capacity tracker */
    if (s_config.adaptive_enabled) {
        ShAdaptiveConfig adaptive_cfg;
        sh_adaptive_config_init(&adaptive_cfg);
        adaptive_cfg.num_workers = s_num_route_workers > 0 ? s_num_route_workers : 4;
        adaptive_cfg.target_utilization = s_config.target_utilization;
        adaptive_cfg.client_timeout_ms = s_config.client_timeout_ms;
        adaptive_cfg.burst_tiles = s_config.burst_requests;
        adaptive_cfg.window_size = s_config.adaptive_window;
        adaptive_cfg.recalc_interval = s_config.adaptive_interval;

        s_adaptive_tracker = sh_adaptive_create(&adaptive_cfg);
        if (s_adaptive_tracker) {
            printf("Adaptive capacity: enabled (window=%zu, interval=%.0f, util=%.0f%%)\n",
                   s_config.adaptive_window, s_config.adaptive_interval,
                   s_config.target_utilization * 100.0);
        } else {
            fprintf(stderr, "Warning: Failed to create adaptive tracker\n");
        }
    } else {
        printf("Adaptive capacity: disabled\n");
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
        /* Cleanup work queue */
        if (s_work_queue) {
            sh_workqueue_shutdown(s_work_queue);
            for (int i = 0; i < s_num_route_workers; i++) {
                pthread_join(s_route_workers[i].thread, NULL);
            }
            free(s_route_workers);
            sh_workqueue_free(s_work_queue);
        }
        sh_adaptive_free(s_adaptive_tracker);
        sh_ratelimit_free(s_rate_limiter);
        if (s_landmarks) vl_landmarks_free(s_landmarks);
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

    /* Shutdown work queue and wait for route workers */
    if (s_work_queue) {
        sh_workqueue_shutdown(s_work_queue);
        for (int i = 0; i < s_num_route_workers; i++) {
            pthread_join(s_route_workers[i].thread, NULL);
        }

        /* Print work queue stats */
        ShWorkQueueStats wq_stats;
        sh_workqueue_stats(s_work_queue, &wq_stats);
        printf("Work queue: %lu pushed, %lu popped, %lu dropped, %lu expired\n",
               (unsigned long)wq_stats.total_pushed,
               (unsigned long)wq_stats.total_popped,
               (unsigned long)wq_stats.total_dropped,
               (unsigned long)wq_stats.total_expired);
    }

    mg_mgr_free(&mgr);
    free(s_route_workers);
    sh_workqueue_free(s_work_queue);
    sh_adaptive_free(s_adaptive_tracker);
    sh_ratelimit_free(s_rate_limiter);
    if (s_landmarks) vl_landmarks_free(s_landmarks);
    vl_graph_free(s_graph);

    return 0;
}
