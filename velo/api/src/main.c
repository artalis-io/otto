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
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <keel/keel.h>
#include <stddef.h>
#include "velo.h"
#include "vl_api.h"
#include "sh_polyline.h"
#include "shared.h"   /* For sh_ratelimit, sh_workqueue, sh_cors, sh_capacity */
#include "sh_keelserver.h"  /* Keel-backed sh_kl_* helpers */
#include "sh_args.h"        /* For sh_parse_int, sh_parse_double */
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"
#include "sh_json.h"  /* For streaming JSON writer + parser */
#include "sh_query.h" /* For query-string parameter parsing */
#include "sh_arena.h" /* Arena for the JSON body parser */
#include "sh_geo.h"   /* For sh_parse_coord */

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    char graph_path[SH_PATH_MAX];
    char save_index_path[SH_PATH_MAX];  /* Path to save binary index (empty = don't save) */
    char listen_addr[SH_HOSTNAME_MAX];
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
    /* Build mode */
    int build_only;              /* Exit after building/saving index (no HTTP server) */
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
    .adaptive_interval = 1000,
    /* Build mode */
    .build_only = 0
};

/* Global state */
static volatile sig_atomic_t s_signo = 0;
static VLGraph *s_graph = NULL;
static VLLandmarks *s_landmarks = NULL;
static VLAPIContext *s_api_ctx = NULL;  /* Transport-agnostic API context */

/* Rate limiter instance (uses shared library) */
static ShRateLimiter *s_rate_limiter = NULL;

/* Work queue instance (uses shared library) */
/*
 * KlThreadPool exposes no statistics, but /api/v1/stats publishes work-queue
 * counters, so they are tracked here. Every counter is read and written only
 * on the event loop thread (submit, done_fn, on_deadline and the stats handler
 * all run there), so plain integers are sufficient.
 */
typedef struct {
    uint64_t pushed;
    uint64_t popped;    /* completed (done_fn ran) */
    uint64_t dropped;   /* submit rejected: queue full */
    uint64_t expired;   /* deadline exceeded */
} VeloQueueStats;

static KlThreadPool *s_pool = NULL;
static VeloQueueStats s_qstats;

/* CORS configuration (uses shared library) */
static ShCorsConfig s_cors_config;

/* Adaptive capacity tracker (uses shared library) */
static ShAdaptiveTracker *s_adaptive_tracker = NULL;

/* ============================================================================
 * Route Request Context
 *
 * OWNERSHIP / LIFETIME (same rules as Surge and FuelWise):
 * freed in exactly one place -- done_fn (the item ran) or cancel_fn (dropped
 * at pool shutdown before starting). on_cancel and on_deadline never free,
 * because work_fn may still be running on a worker; they only set `detached`,
 * which is read and written solely on the event loop thread.
 * ============================================================================ */

typedef struct {
    KlHttpServer *server;
    KlThreadPool *pool;
} AppCtx;

typedef struct {
    KlAsyncOp op;
    AppCtx *app;
    const KlHttpRequest *req;   /* lives inside the conn; valid while suspended */

    /* Request parameters */
    double from_lat, from_lon;
    double to_lat, to_lon;
    VLProfile profile;
    VLWeightType weight;
    int include_geometry;

    /* Result (set by the worker) */
    VLRoute route;
    VLStatus status;
    char *response_json;        /* owned */
    int status_code;
    char error_msg[128];

    int detached;
    ShMetricsTimer timer;
} RouteCtx;


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
        cfg->rate_limit_enabled = sh_parse_int(val, cfg->rate_limit_enabled, 0, 1);
    }
    if ((val = getenv("VELO_RATE_LIMIT_RPS"))) {
        cfg->rate_limit_rps = sh_parse_double(val, cfg->rate_limit_rps, 0.1, 10000.0);
    }
    if ((val = getenv("VELO_RATE_LIMIT_BURST"))) {
        cfg->rate_limit_burst = sh_parse_double(val, cfg->rate_limit_burst, 1.0, 100000.0);
    }
    /* Work queue configuration */
    if ((val = getenv("VELO_WORK_QUEUE_ENABLED"))) {
        cfg->work_queue_enabled = sh_parse_int(val, cfg->work_queue_enabled, 0, 1);
    }
    if ((val = getenv("VELO_WORK_QUEUE_DEPTH"))) {
        cfg->work_queue_depth = (size_t)sh_parse_int(val, (int)cfg->work_queue_depth, 1, 100000);
    }
    if ((val = getenv("VELO_WORK_QUEUE_TIMEOUT"))) {
        cfg->work_queue_timeout = sh_parse_double(val, cfg->work_queue_timeout, 0.1, 3600.0);
    }
    if ((val = getenv("VELO_ROUTE_WORKERS"))) {
        cfg->route_workers = sh_parse_int(val, cfg->route_workers, 1, 256);
    }
    /* CORS configuration */
    if ((val = getenv("VELO_CORS_ORIGINS"))) {
        strncpy(cfg->cors_origins, val, sizeof(cfg->cors_origins) - 1);
        cfg->cors_origins[sizeof(cfg->cors_origins) - 1] = '\0';
    }
    /* Adaptive capacity configuration */
    if ((val = getenv("VELO_ADAPTIVE_ENABLED"))) {
        cfg->adaptive_enabled = sh_parse_int(val, cfg->adaptive_enabled, 0, 1);
    }
    if ((val = getenv("VELO_TARGET_UTILIZATION"))) {
        cfg->target_utilization = sh_parse_double(val, cfg->target_utilization, 0.01, 1.0);
    }
    if ((val = getenv("VELO_CLIENT_TIMEOUT"))) {
        cfg->client_timeout_ms = sh_parse_double(val, cfg->client_timeout_ms, 100.0, 600000.0);
    }
    if ((val = getenv("VELO_BURST_REQUESTS"))) {
        cfg->burst_requests = sh_parse_int(val, cfg->burst_requests, 1, 10000);
    }
    if ((val = getenv("VELO_ADAPTIVE_WINDOW"))) {
        cfg->adaptive_window = (size_t)sh_parse_int(val, (int)cfg->adaptive_window, 10, 100000);
    }
    if ((val = getenv("VELO_ADAPTIVE_INTERVAL"))) {
        cfg->adaptive_interval = sh_parse_double(val, cfg->adaptive_interval, 1.0, 100000.0);
    }
}

/* ============================================================================
 * Route Context Helpers
 * ============================================================================ */

static void route_ctx_free(RouteCtx *ctx) {
    if (!ctx) return;
    vl_free_route(&ctx->route);
    free(ctx->response_json);
    free(ctx);
}

static void record_metrics(ShMetricsTimer timer, const char *endpoint) {
    sh_metrics_counter_inc("http_requests_total", 1,
        "endpoint", endpoint, "service", "velo", NULL);
    sh_metrics_timer_observe(timer, "http_request_duration_ms",
        "endpoint", endpoint, "service", "velo", NULL);
}

/* Process a single route request */
static void process_route_request(RouteCtx *item) {
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

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

/* Compatibility wrappers for simple calls (uses wildcard origin) */
static void send_json(KlHttpResponse *res, int status, const char *json) {
    sh_kl_reply_json(res, status, &s_cors_config, NULL, json);
}

static void send_error(KlHttpResponse *res, int status, const char *message) {
    sh_kl_reply_error(res, status, &s_cors_config, NULL, message);
}

/* Note: json_escape_polyline removed - ShJsonWriter handles escaping */

/* ============================================================================
 * Query Parameter Parsing
 * ============================================================================ */

static int parse_coord(const char *str, double *lat, double *lon) {
    if (!str || !*str) return -1;

    /* Use shared library coordinate parser */
    SHCoord coord;
    if (sh_parse_coord(str, &coord) != 0) return -1;
    *lat = coord.lat;
    *lon = coord.lon;
    return 0;
}

static VLProfile parse_profile(const char *str) {
    if (!str || !*str) return VL_PROFILE_CAR;

    if (strcmp(str, "car") == 0) return VL_PROFILE_CAR;
    if (strcmp(str, "truck") == 0) return VL_PROFILE_TRUCK;
    if (strcmp(str, "bike") == 0) return VL_PROFILE_BIKE;
    if (strcmp(str, "bicycle") == 0) return VL_PROFILE_BIKE;
    if (strcmp(str, "foot") == 0) return VL_PROFILE_FOOT;
    if (strcmp(str, "pedestrian") == 0) return VL_PROFILE_FOOT;
    if (strcmp(str, "walk") == 0) return VL_PROFILE_FOOT;

    return VL_PROFILE_CAR;
}

static VLWeightType parse_mode(const char *str) {
    if (!str || !*str) return VL_WEIGHT_DURATION;

    if (strcmp(str, "fastest") == 0) return VL_WEIGHT_DURATION;
    if (strcmp(str, "shortest") == 0) return VL_WEIGHT_DISTANCE;
    if (strcmp(str, "duration") == 0) return VL_WEIGHT_DURATION;
    if (strcmp(str, "distance") == 0) return VL_WEIGHT_DISTANCE;

    return VL_WEIGHT_DURATION;
}

static int parse_bool(const char *str, int default_val) {
    if (!str || !*str) return default_val;

    if (strcmp(str, "true") == 0) return 1;
    if (strcmp(str, "1") == 0) return 1;
    if (strcmp(str, "yes") == 0) return 1;
    if (strcmp(str, "false") == 0) return 0;
    if (strcmp(str, "0") == 0) return 0;
    if (strcmp(str, "no") == 0) return 0;

    return default_val;
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

static void handle_health(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    sh_kl_handle_health(res, &s_cors_config, NULL, "velo-route-server", vl_version());
    record_metrics(timer, "health");
    sh_trace_clear();
}

static void handle_stats(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    if (!s_graph) {
        send_error(res, 503, "Graph not loaded");
        return;
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

    /* Build response using streaming JSON writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);

    /* Graph info */
    sh_json_write_kv_string(&w, "graph_path", s_config.graph_path);
    sh_json_write_kv_int(&w, "num_nodes", (int64_t)s_graph->num_nodes);
    sh_json_write_kv_int(&w, "num_edges", (int64_t)s_graph->num_edges);
    sh_json_write_kv_bool(&w, "landmarks_enabled", s_landmarks != NULL);
    sh_json_write_kv_int(&w, "landmark_count", s_landmarks ? s_config.landmark_count : 0);

    /* Bounding box */
    sh_json_write_key(&w, "bbox");
    sh_json_write_object_start(&w);
    sh_json_write_kv_double_fmt(&w, "min_lat", s_graph->bbox_min.lat, 6);
    sh_json_write_kv_double_fmt(&w, "min_lon", s_graph->bbox_min.lon, 6);
    sh_json_write_kv_double_fmt(&w, "max_lat", s_graph->bbox_max.lat, 6);
    sh_json_write_kv_double_fmt(&w, "max_lon", s_graph->bbox_max.lon, 6);
    sh_json_write_object_end(&w);

    /* Work queue stats */
    sh_json_write_key(&w, "work_queue");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "enabled", s_pool != NULL);
    sh_json_write_kv_int(&w, "depth", (int64_t)(s_qstats.pushed - s_qstats.popped));
    sh_json_write_kv_int(&w, "capacity", (int64_t)s_config.work_queue_depth);
    sh_json_write_kv_int(&w, "pushed", (int64_t)s_qstats.pushed);
    sh_json_write_kv_int(&w, "popped", (int64_t)s_qstats.popped);
    sh_json_write_kv_int(&w, "dropped", (int64_t)s_qstats.dropped);
    sh_json_write_kv_int(&w, "expired", (int64_t)s_qstats.expired);
    sh_json_write_object_end(&w);

    /* Rate limiter stats */
    sh_json_write_key(&w, "rate_limit");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "enabled", s_rate_limiter != NULL);
    sh_json_write_kv_double_fmt(&w, "rps", s_config.rate_limit_rps, 1);
    sh_json_write_kv_double_fmt(&w, "burst", s_config.rate_limit_burst, 0);
    sh_json_write_kv_int(&w, "allowed", (int64_t)rl_stats.requests_allowed);
    sh_json_write_kv_int(&w, "denied", (int64_t)rl_stats.requests_denied);
    sh_json_write_object_end(&w);

    /* Adaptive capacity stats */
    sh_json_write_key(&w, "adaptive");
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "enabled", s_adaptive_tracker != NULL);
    sh_json_write_kv_int(&w, "sample_count", (int64_t)adaptive_stats.sample_count);
    sh_json_write_kv_int(&w, "recalc_count", (int64_t)adaptive_stats.recalc_count);

    /* Add percentile stats if we have samples */
    if (s_adaptive_tracker && adaptive_stats.sample_count > 0) {
        sh_json_write_kv_double_fmt(&w, "p50_ms", adaptive_stats.p50_ms, 2);
        sh_json_write_kv_double_fmt(&w, "p90_ms", adaptive_stats.p90_ms, 2);
        sh_json_write_kv_double_fmt(&w, "p99_ms", adaptive_stats.p99_ms, 2);
        sh_json_write_kv_double_fmt(&w, "avg_ms", adaptive_stats.avg_ms, 2);
        sh_json_write_kv_double_fmt(&w, "ema_ms", adaptive_stats.ema_ms, 2);
    }

    /* Add calculated params if available */
    if (has_adaptive_params) {
        sh_json_write_kv_double_fmt(&w, "calc_rps", adaptive_params.rate_limit_rps, 2);
        sh_json_write_kv_double_fmt(&w, "calc_burst", adaptive_params.rate_limit_burst, 0);
    }

    sh_json_write_object_end(&w);  /* Close adaptive */
    sh_json_write_object_end(&w);  /* Close root */

    /* Send response */
    if (!sh_json_writer_error(&w) && jb.buf) {
        send_json(res, 200, jb.buf);
    } else {
        send_error(res, 500, "Failed to generate response");
    }

    sh_json_buf_free(&jb);
    record_metrics(timer, "stats");
    sh_trace_clear();
}

/* GET /metrics - Prometheus metrics endpoint, uses shared helper */
static void handle_metrics(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    sh_kl_handle_metrics(res);
    sh_trace_clear();
}

/*
 * Runs on a pool worker thread: route, encode geometry, render the JSON
 * response. Touches only this context plus the read-only graph/landmarks.
 */
static void route_render(RouteCtx *ctx) {
    struct timeval route_start, route_end;
    gettimeofday(&route_start, NULL);

    process_route_request(ctx);

    gettimeofday(&route_end, NULL);
    double route_ms = (route_end.tv_sec - route_start.tv_sec) * 1000.0 +
                      (route_end.tv_usec - route_start.tv_usec) / 1000.0;

    /* Record response time for adaptive capacity */
    if (s_adaptive_tracker) {
        sh_adaptive_record(s_adaptive_tracker, route_ms);

        ShCapacityParams new_params;
        if (sh_adaptive_update(s_adaptive_tracker, &new_params)) {
            if (s_rate_limiter) {
                sh_ratelimit_update_rate(s_rate_limiter,
                                         new_params.rate_limit_rps,
                                         new_params.rate_limit_burst);
            }
        }
    }

    if (ctx->status != VL_OK) {
        ctx->status_code = 404;
        return;
    }

    /* Encode polyline if geometry requested */
    char *polyline = NULL;
    if (ctx->include_geometry && ctx->route.num_coords > 0) {
        size_t max_len = sh_polyline_max_encoded_size(ctx->route.num_coords);
        polyline = malloc(max_len);
        if (polyline) {
            /* Convert VLCoord array to double array (check for overflow first) */
            double *coords = NULL;
            if ((size_t)ctx->route.num_coords <= SIZE_MAX / (2 * sizeof(double))) {
                size_t coord_size = (size_t)ctx->route.num_coords * 2 * sizeof(double);
                coords = malloc(coord_size);
            }
            if (coords) {
                for (int i = 0; i < ctx->route.num_coords; i++) {
                    coords[i * 2] = ctx->route.coords[i].lat;
                    coords[i * 2 + 1] = ctx->route.coords[i].lon;
                }
                sh_polyline_encode(coords, ctx->route.num_coords, 5, polyline, max_len);
                free(coords);
            } else {
                free(polyline);
                polyline = NULL;
            }
        }
    }

    /* Build JSON response using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    const char *profile_str = "car";
    switch (ctx->profile) {
        case VL_PROFILE_TRUCK: profile_str = "truck"; break;
        case VL_PROFILE_BIKE: profile_str = "bike"; break;
        case VL_PROFILE_FOOT: profile_str = "foot"; break;
        default: break;
    }

    const char *mode_str = ctx->weight == VL_WEIGHT_DISTANCE ? "shortest" : "fastest";

    sh_json_write_object_start(&jw);
    sh_json_write_kv_string(&jw, "status", "ok");

    sh_json_write_key(&jw, "route");
    sh_json_write_object_start(&jw);
    sh_json_write_kv_double_fmt(&jw, "distance", ctx->route.distance_m, 2);
    sh_json_write_kv_double_fmt(&jw, "duration", ctx->route.duration_s, 2);
    sh_json_write_kv_string(&jw, "profile", profile_str);
    sh_json_write_kv_string(&jw, "mode", mode_str);

    /* from: [lat, lon] */
    sh_json_write_key(&jw, "from");
    sh_json_write_array_start(&jw);
    sh_json_write_double_fmt(&jw, ctx->from_lat, 6);
    sh_json_write_double_fmt(&jw, ctx->from_lon, 6);
    sh_json_write_array_end(&jw);

    /* to: [lat, lon] */
    sh_json_write_key(&jw, "to");
    sh_json_write_array_start(&jw);
    sh_json_write_double_fmt(&jw, ctx->to_lat, 6);
    sh_json_write_double_fmt(&jw, ctx->to_lon, 6);
    sh_json_write_array_end(&jw);

    /* geometry (optional) - ShJsonWriter handles escaping */
    if (polyline) {
        sh_json_write_kv_string(&jw, "geometry", polyline);
    }

    sh_json_write_object_end(&jw);  /* Close route */

    /* meta object */
    sh_json_write_key(&jw, "meta");
    sh_json_write_object_start(&jw);
    sh_json_write_kv_int(&jw, "nodes_explored", (int64_t)ctx->route.nodes_explored);
    sh_json_write_kv_double_fmt(&jw, "search_time_ms", ctx->route.search_time_ms, 2);
    sh_json_write_object_end(&jw);

    sh_json_write_object_end(&jw);  /* Close root */

    if (polyline) free(polyline);

    if (!sh_json_writer_error(&jw) && jb.buf) {
        ctx->response_json = sh_json_buf_take(&jb);
        ctx->status_code = 200;
    } else {
        ctx->status_code = 500;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Failed to generate response");
    }
    sh_json_buf_free(&jb);
}

/* ============================================================================
 * Async Plumbing
 * ============================================================================ */

static void route_work_fn(void *user_data) {
    route_render((RouteCtx *)user_data);
}

static void route_done_fn(void *user_data) {
    RouteCtx *ctx = (RouteCtx *)user_data;

    s_qstats.popped++;

    if (ctx->detached) {
        route_ctx_free(ctx);
        return;
    }

    KlHttpResponse *res = kl_http_conn_response(ctx->op.conn);
    if (ctx->status_code == 200 && ctx->response_json) {
        send_json(res, 200, ctx->response_json);
    } else if (ctx->status_code == 404) {
        send_error(res, 404, ctx->error_msg[0] ? ctx->error_msg : "Routing failed");
    } else {
        send_error(res, 500, ctx->error_msg[0] ? ctx->error_msg
                                               : "Failed to generate response");
    }
    record_metrics(ctx->timer, "route");

    kl_async_complete(ctx->app->server, &ctx->op);
    route_ctx_free(ctx);
}

/* Pool shutdown dropped the item before it started. */
static void route_cancel_fn(void *user_data) {
    route_ctx_free((RouteCtx *)user_data);
}

/*
 * Declare the send. kl_async_complete() re-arms the fd but leaves the
 * connection SUSPENDED unless on_resume says what happens next; without this
 * the response is never written and the client hangs. Keel's
 * examples/thread_pool and examples/async_thread_pool leave this a no-op and
 * hang for exactly that reason; tests/smoke_iouring_async.c is the correct
 * reference, and kl_http_request_send_response() is its public equivalent.
 */
static void route_on_resume(KlAsyncOp *op, void *ud) {
    (void)ud;
    RouteCtx *ctx = (RouteCtx *)((char *)op - offsetof(RouteCtx, op));
    kl_http_request_send_response(ctx->req);
}

/* Connection died while suspended; the worker may still be running. */
static void route_on_cancel(KlAsyncOp *op, void *ud) {
    (void)ud;
    ((RouteCtx *)((char *)op - offsetof(RouteCtx, op)))->detached = 1;
}

/* Deadline exceeded: reply 504 now, let done_fn free the context later. */
static void route_on_deadline(KlAsyncOp *op, void *ud) {
    (void)ud;
    RouteCtx *ctx = (RouteCtx *)((char *)op - offsetof(RouteCtx, op));

    if (ctx->detached) return;
    ctx->detached = 1;
    s_qstats.expired++;

    send_error(kl_http_conn_response(op->conn), 504, "Request timeout");
    record_metrics(ctx->timer, "route");

    kl_async_complete(ctx->app->server, op);
}

/* ============================================================================
 * Route Handler
 * ============================================================================ */

static void handle_route(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    AppCtx *app = (AppCtx *)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();

    if (!s_graph) {
        send_error(res, 503, "Graph not loaded");
        return;
    }

    /* Parse parameters */
    double from_lat = 0, from_lon = 0, to_lat = 0, to_lon = 0;
    VLProfile profile = VL_PROFILE_CAR;
    VLWeightType weight = VL_WEIGHT_DURATION;
    int include_geometry = 1;

    int is_post = (req->method_len == 4 && memcmp(req->method, "POST", 4) == 0);

    if (is_post) {
        /* Parse JSON body */
        KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
        if (!br || br->len == 0) {
            send_error(res, 400, "Empty request body");
            return;
        }

        /* Arena-backed parse; released before we return. */
        SHArena *arena = sh_arena_create(br->len * 4 + 4096);
        if (!arena) {
            send_error(res, 500, "Out of memory");
            return;
        }

        ShJsonValue *root = NULL;
        if (sh_json_parse(br->data, br->len, arena, &root) != SH_JSON_OK) {
            sh_arena_free(arena);
            send_error(res, 400, "Invalid JSON body");
            return;
        }

        const char *from_str = sh_json_as_string(sh_json_get_path(root, "from"), NULL);
        if (from_str && parse_coord(from_str, &from_lat, &from_lon) != 0) {
            sh_arena_free(arena);
            send_error(res, 400, "Invalid 'from' coordinate");
            return;
        }

        const char *to_str = sh_json_as_string(sh_json_get_path(root, "to"), NULL);
        if (to_str && parse_coord(to_str, &to_lat, &to_lon) != 0) {
            sh_arena_free(arena);
            send_error(res, 400, "Invalid 'to' coordinate");
            return;
        }

        profile = parse_profile(sh_json_as_string(sh_json_get_path(root, "profile"), NULL));
        weight = parse_mode(sh_json_as_string(sh_json_get_path(root, "mode"), NULL));

        ShJsonValue *geom = sh_json_get_path(root, "geometry");
        if (geom) include_geometry = sh_json_as_bool(geom, true) ? 1 : 0;

        sh_arena_free(arena);
    } else {
        /* GET request - parse query string */
        char query[1024];
        size_t qlen = req->query_len < sizeof(query) - 1 ? req->query_len
                                                         : sizeof(query) - 1;
        if (req->query && qlen > 0) memcpy(query, req->query, qlen);
        query[req->query && qlen > 0 ? qlen : 0] = '\0';

        char from_val[128] = "", to_val[128] = "";
        char profile_val[32] = "", mode_val[32] = "", geom_val[16] = "";

        sh_query_get_str(query, "from", from_val, sizeof(from_val));
        sh_query_get_str(query, "to", to_val, sizeof(to_val));
        sh_query_get_str(query, "profile", profile_val, sizeof(profile_val));
        sh_query_get_str(query, "mode", mode_val, sizeof(mode_val));
        sh_query_get_str(query, "geometry", geom_val, sizeof(geom_val));

        if (from_val[0] == '\0') {
            send_error(res, 400, "Missing 'from' parameter");
            return;
        }
        if (to_val[0] == '\0') {
            send_error(res, 400, "Missing 'to' parameter");
            return;
        }
        if (parse_coord(from_val, &from_lat, &from_lon) != 0) {
            send_error(res, 400, "Invalid 'from' coordinate (format: lat,lon)");
            return;
        }
        if (parse_coord(to_val, &to_lat, &to_lon) != 0) {
            send_error(res, 400, "Invalid 'to' coordinate (format: lat,lon)");
            return;
        }

        profile = parse_profile(profile_val);
        weight = parse_mode(mode_val);
        include_geometry = parse_bool(geom_val, 1);
    }

    /* Validate coordinates are within graph bounds */
    if (from_lat < s_graph->bbox_min.lat || from_lat > s_graph->bbox_max.lat ||
        from_lon < s_graph->bbox_min.lon || from_lon > s_graph->bbox_max.lon) {
        send_error(res, 400, "Origin coordinate outside graph bounds");
        return;
    }
    if (to_lat < s_graph->bbox_min.lat || to_lat > s_graph->bbox_max.lat ||
        to_lon < s_graph->bbox_min.lon || to_lon > s_graph->bbox_max.lon) {
        send_error(res, 400, "Destination coordinate outside graph bounds");
        return;
    }

    RouteCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        send_error(res, 500, "Out of memory");
        return;
    }
    ctx->status = VL_ERROR_INVALID_ARGUMENT;
    ctx->from_lat = from_lat;
    ctx->from_lon = from_lon;
    ctx->to_lat = to_lat;
    ctx->to_lon = to_lon;
    ctx->profile = profile;
    ctx->weight = weight;
    ctx->include_geometry = include_geometry;
    ctx->timer = timer;

    /* Work queue disabled: route inline on the event loop, as before. */
    if (!s_pool) {
        route_render(ctx);
        if (ctx->status_code == 200 && ctx->response_json) {
            send_json(res, 200, ctx->response_json);
        } else if (ctx->status_code == 404) {
            send_error(res, 404, ctx->error_msg[0] ? ctx->error_msg : "Routing failed");
        } else {
            send_error(res, 500, "Failed to generate response");
        }
        record_metrics(timer, "route");
        route_ctx_free(ctx);
        return;
    }

    ctx->app = app;
    ctx->req = req;
    ctx->op.on_resume = route_on_resume;
    ctx->op.on_cancel = route_on_cancel;
    ctx->op.on_deadline = route_on_deadline;
    if (s_config.work_queue_timeout > 0.0) {
        ctx->op.deadline_ms = kl_monotonic_ms() +
            (uint64_t)(s_config.work_queue_timeout * 1000.0);
    }

    if (kl_async_suspend(app->server, kl_http_request_conn(req), &ctx->op) < 0) {
        route_ctx_free(ctx);
        send_error(res, 500, "Failed to suspend request");
        return;
    }

    KlWorkItem item = {
        .work_fn   = route_work_fn,
        .done_fn   = route_done_fn,
        .cancel_fn = route_cancel_fn,
        .user_data = ctx,
    };

    if (kl_thread_pool_submit(app->pool, &item) < 0) {
        /* Queue full - backpressure, same 503 as the old work queue. */
        s_qstats.dropped++;
        ctx->detached = 1;
        send_error(res, 503, "Server busy, try again later");
        record_metrics(timer, "route");
        kl_async_complete(app->server, &ctx->op);
        route_ctx_free(ctx);
        return;
    }

    s_qstats.pushed++;
}

/* ============================================================================
 * Middleware
 * ============================================================================ */

/* CORS preflight, before rate limiting (as in the mongoose server). */
static int mw_preflight(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_trace_from_headers(sh_kl_trace_header_getter, req);
    sh_kl_reply_preflight(res, &s_cors_config, sh_kl_origin(req));
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/* Rate limit every request before routing. */
static int mw_rate_limit(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_trace_from_headers(sh_kl_trace_header_getter, req);
    if (!sh_kl_check_rate_limit(req, res, s_rate_limiter, &s_cors_config,
                                sh_kl_origin(req))) {
        sh_metrics_counter_inc("http_requests_total", 1,
            "endpoint", "rate_limited", "status", "429", NULL);
        sh_trace_clear();
        return 1;  /* short-circuit */
    }
    return 0;
}

/*
 * Anything the route table would not match.
 *
 * Keel route patterns have no wildcard -- '*' is only special in middleware
 * patterns -- so a catch-all route is not expressible, and Keel's built-in 404
 * is text/plain with no CORS headers.
 */
static int mw_not_found(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    KlHttpServer *server = (KlHttpServer *)ud;
    KlHttpRoute *matched = NULL;
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS];
    int num_params = 0;

    int rc = kl_http_router_match(&server->router, req->method, req->method_len,
                                  req->path, req->path_len,
                                  &matched, params, &num_params);
    if (rc == 200) return 0;

    if (rc == 405) {
        send_error(res, 405, "Method not allowed");
        sh_metrics_counter_inc("http_requests_total", 1,
            "endpoint", "method_not_allowed", "service", "velo", NULL);
    } else {
        send_error(res, 404, "Not found");
        sh_metrics_counter_inc("http_requests_total", 1,
            "endpoint", "not_found", "service", "velo", NULL);
    }
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/* Body reader factory: kl_http_body_reader_buffer() reads max_size from the
   route's user_data, so wrap it to keep the cap explicit. */
#define VELO_MAX_BODY_SIZE (1u * 1024u * 1024u)
static KlHttpBodyReader *route_body_reader(KlAllocator *alloc,
                                           const KlHttpRequest *req,
                                           void *user_data) {
    (void)user_data;
    return kl_http_body_reader_buffer(alloc, req,
                                      (void *)(size_t)VELO_MAX_BODY_SIZE);
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
    printf("  --build-only         Exit after building/saving index (no HTTP server)\n");
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
        } else if (strcmp(argv[i], "--build-only") == 0) {
            s_config.build_only = 1;
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
        } else {
            fprintf(stderr, "Warning: Failed to save index\n");
        }

        /* Exit if --build-only was specified */
        if (s_config.build_only) {
            printf("Build complete (--build-only specified)\n");
            vl_graph_free(s_graph);
            return 0;
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

    /* Create transport-agnostic API context */
    {
        VLAPIConfig api_cfg;
        vl_api_config_init(&api_cfg);
        api_cfg.graph_path = s_config.graph_path;
        api_cfg.name = s_config.name;
        api_cfg.landmark_count = s_landmarks ? s_config.landmark_count : 0;
        s_api_ctx = vl_api_create(s_graph, s_landmarks, &api_cfg);
        if (!s_api_ctx) {
            fprintf(stderr, "Warning: Failed to create API context\n");
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
        int num_workers = s_config.route_workers > 0 ? s_config.route_workers : 4;
        adaptive_cfg.num_workers = num_workers > 0 ? num_workers : 4;
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

    /* Initialize logging */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "velo";
    log_cfg.version = vl_version();
    sh_log_init(&log_cfg);

    /* Initialize metrics */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "velo";
    sh_metrics_init(&metrics_cfg);

    SH_LOG_INFO("Starting velo route server", "version", vl_version(), NULL);

    /* Initialize HTTP server */
    KlHttpServer server;
    KlHttpServerConfig http_cfg = {
        .port = s_config.port,
        .bind_addr = s_config.listen_addr,
        .install_signal_handlers = 1,
        .max_body_size = VELO_MAX_BODY_SIZE,
        .drain_timeout_ms = 5000,
    };

    if (kl_http_server_init(&server, &http_cfg) < 0) {
        fprintf(stderr, "Error: Cannot listen on %s:%d\n",
                s_config.listen_addr, s_config.port);
        sh_adaptive_free(s_adaptive_tracker);
        sh_ratelimit_free(s_rate_limiter);
        vl_api_free(s_api_ctx);
        if (s_landmarks) vl_landmarks_free(s_landmarks);
        vl_graph_free(s_graph);
        return 1;
    }

    /* Solve pool. queue_capacity gives the backpressure ShWorkQueue used to. */
    if (s_config.work_queue_enabled) {
        KlThreadPoolConfig pool_cfg = {
            .num_workers = s_config.route_workers,
            .queue_capacity = (int)s_config.work_queue_depth,
        };
        s_pool = kl_thread_pool_create(kl_http_server_event_ctx(&server), &pool_cfg);
        if (s_pool) {
            printf("Route pool: depth %zu, timeout %.1fs, %d route workers\n",
                   s_config.work_queue_depth, s_config.work_queue_timeout,
                   s_config.route_workers);
        } else {
            fprintf(stderr, "Warning: Failed to create route thread pool\n");
        }
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

    AppCtx app = { .server = &server, .pool = s_pool };

    /* Routes. POST /api/v1/route needs a body reader; GET does not. */
    kl_http_server_route(&server, "GET",  "/api/v1/health", handle_health,  NULL, NULL);
    kl_http_server_route(&server, "GET",  "/api/v1/stats",  handle_stats,   NULL, NULL);
    kl_http_server_route(&server, "GET",  "/metrics",       handle_metrics, NULL, NULL);
    kl_http_server_route(&server, "GET",  "/api/v1/route",  handle_route,   &app, NULL);
    kl_http_server_route(&server, "POST", "/api/v1/route",  handle_route,   &app,
                         route_body_reader);

    /*
     * Middleware runs in registration order, before routing. Preflight first
     * (it must not be rate limited), then the limiter, then the fallback,
     * which must be last because it short-circuits unmatched requests.
     */
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_rate_limit, NULL);
    kl_http_server_use(&server, "*", "/*", mw_not_found, &server);

    /* Event loop: blocks until SIGINT/SIGTERM. */
    kl_http_server_run(&server);

    printf("\nShutting down...\n");

    /* Pool first: drains in-flight work, fires cancel_fn for queued items. */
    if (s_pool) kl_thread_pool_free(s_pool);

    printf("Route queue: %lu pushed, %lu popped, %lu dropped, %lu expired\n",
           (unsigned long)s_qstats.pushed, (unsigned long)s_qstats.popped,
           (unsigned long)s_qstats.dropped, (unsigned long)s_qstats.expired);

    kl_http_server_free(&server);
    sh_adaptive_free(s_adaptive_tracker);
    sh_ratelimit_free(s_rate_limiter);
    vl_api_free(s_api_ctx);
    if (s_landmarks) vl_landmarks_free(s_landmarks);
    vl_graph_free(s_graph);

    return 0;
}
