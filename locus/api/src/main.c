/*
 * Locus API Server
 *
 * REST API for geocoding operations.
 * Uses Keel for HTTP serving.
 *
 * Features:
 * - Rate limiting (per-IP token bucket)
 * - Work queue with backpressure
 * - Adaptive capacity tuning
 * - Socket timeout protection
 * - Structured logging
 * - Distributed tracing
 * - Prometheus metrics
 */

#include "locus.h"
#include "lc_serialize.h"
#include "lc_mmap.h"
#include "lc_api.h"
#include <keel/keel.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <math.h>

/* Shared library includes */
#include "shared.h"
#include "sh_httpserver.h"
#include "sh_httpasync.h"
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"
#include "sh_completion.h"
#include "sh_worker_pool.h"
#include "sh_json.h"
#include "sh_query.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

/*
 * Locus-specific configuration (extends ShServerConfig).
 */
typedef struct {
    /* Common server config (uses sh_args) */
    ShServerConfig server;

    /* Locus-specific */
    char data_file[512];
    char save_path[512];
    int num_workers;  /* Geocode worker threads (0 = auto) */
} LocusServerConfig;

/* Default configuration */
static LocusServerConfig s_config;

/* CORS configuration (uses sh_cors) */
static ShCorsConfig s_cors;

/* ============================================================================
 * Global State
 *
 * DESIGN NOTE: Thread Safety
 *
 * g_index:
 *   - Set once during initialization (before any threads start)
 *   - Read-only after initialization
 *   - All search operations are read-only on the index
 *   - Thread-safe: no concurrent writes
 *
 * Queue counters:
 *   - Read and written only on the event loop thread (submit, done_fn,
 *     on_deadline and the stats handler all run there)
 * ============================================================================ */

static LCIndex *g_index = NULL;
static LCAPIContext *g_api_ctx = NULL;

/* Rate limiter instance (uses shared library) */
static ShRateLimiter *s_rate_limiter = NULL;

/*
 * KlThreadPool exposes no statistics, but /api/v1/stats publishes work-queue
 * counters, so they are tracked here.
 */
/* Geocode thread pool */
static KlThreadPool *s_pool = NULL;
static ShHttpAsyncStats s_qstats;

/* Adaptive capacity tracker (uses shared library) */
static ShAdaptiveTracker *s_adaptive_tracker = NULL;

typedef struct {
    ShHttpAsync async;   /* server, pool, cors, timeout, stats */
} AppCtx;

/* JSON building is handled by sh_json.h (ShJsonWriter + ShJsonBuf) */

/* ============================================================================
 * Configuration Loading
 * ============================================================================ */

/* Initialize Locus-specific defaults */
static void init_locus_defaults(LocusServerConfig *cfg) {
    /* Initialize common server config using sh_args */
    sh_args_init(&cfg->server);

    /* Override defaults for Locus */
    cfg->server.port = 8083;
    cfg->server.rate_limit_rps = 50.0;      /* Geocoding is fast */
    cfg->server.rate_limit_burst = 100.0;
    cfg->server.work_queue_depth = 128;
    cfg->server.work_queue_timeout = 5.0;

    /* Locus-specific defaults */
    cfg->data_file[0] = '\0';
    cfg->save_path[0] = '\0';
    cfg->num_workers = 0;  /* Auto-detect */
}

/* Load Locus-specific environment variables */
static void load_locus_env(LocusServerConfig *cfg) {
    const char *val;

    /* Load common config using sh_args (handles LOCUS_ prefix) */
    sh_args_load_env(&cfg->server, SH_API_LOCUS);

    /* Locus-specific environment variables */
    if ((val = getenv("LOCUS_DATA_FILE"))) {
        strncpy(cfg->data_file, val, sizeof(cfg->data_file) - 1);
        cfg->data_file[sizeof(cfg->data_file) - 1] = '\0';
    }
    if ((val = getenv("LOCUS_NUM_WORKERS"))) {
        cfg->num_workers = sh_parse_int(val, 0, 0, 256);
    }

    /* CORS configuration */
    if ((val = getenv("LOCUS_CORS_ORIGINS"))) {
        sh_cors_parse_origins(&s_cors, val);
    }
    if ((val = getenv("LOCUS_CORS_METHODS"))) {
        sh_cors_set_methods(&s_cors, val);
    }
    if ((val = getenv("LOCUS_CORS_HEADERS"))) {
        sh_cors_set_headers(&s_cors, val);
    }
    if ((val = getenv("LOCUS_CORS_CREDENTIALS"))) {
        s_cors.allow_credentials = (sh_parse_int(val, 0, 0, 1) != 0);
    }
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

/*
 * Extract origin from request.
 * Thread-safe using thread-local storage for origin buffer.
 */
static const char *get_origin_from_request(const KlHttpRequest *req) {
    return sh_http_origin(req);
}


/* HTTP response helpers - use shared implementation */
static void send_json_cors(KlHttpResponse *res, const KlHttpRequest *req,
                           int status, const char *json) {
    sh_http_reply_json(res, status, &s_cors, get_origin_from_request(req), json);
}

static void send_error_cors(KlHttpResponse *res, const KlHttpRequest *req,
                            int status, const char *message) {
    sh_http_reply_error(res, status, &s_cors, get_origin_from_request(req), message);
}

/* ============================================================================
 * Geocode Work Queue Functions
 * ============================================================================ */

/* Initialize a geocode work item */
static void record_metrics(ShMetricsTimer timer, const char *endpoint) {
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", endpoint, NULL);
    sh_metrics_timer_observe(timer, "http_request_duration_ms", endpoint, NULL);
}

/*
 * The geocode handler is lc_api_handle() in liblocus. This wrapper adds the
 * one thing that is genuinely the server's job and not the library's: the
 * adaptive-capacity feedback that retunes this server's rate limiter.
 *
 * What used to be here was a second implementation of lc_api_handle -- its
 * own routing, its own query parsing, its own JSON building (GeoCtx,
 * process_search, process_autocomplete, process_reverse). A third lived in
 * wasm/src/lc_wasm_api.c. Three copies of five endpoints, and they disagreed:
 * see docs/roadmaps/transport.md for the table.
 */
static int locus_metered_handler(void *ctx, const ShApiRequest *req,
                                 ShApiResponse *resp)
{
    struct timeval t0, t1;
    int rc;

    gettimeofday(&t0, NULL);
    rc = lc_api_handle(ctx, req, resp);
    gettimeofday(&t1, NULL);

    if (s_adaptive_tracker) {
        ShCapacityParams np;
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (t1.tv_usec - t0.tv_usec) / 1000.0;
        sh_adaptive_record(s_adaptive_tracker, ms);
        if (sh_adaptive_update(s_adaptive_tracker, &np) && s_rate_limiter) {
            sh_ratelimit_update_rate(s_rate_limiter,
                                     np.rate_limit_rps,
                                     np.rate_limit_burst);
        }
    }

    return rc;
}


/* ============================================================================
 * Request Handlers
 * ============================================================================ */

static void handle_health(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    sh_http_handle_health(res, &s_cors, get_origin_from_request(req),
                        "locus-geocoder", lc_version());
    record_metrics(timer, "endpoint:health");
    sh_trace_clear();
}

static void handle_stats(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    if (!g_index) {
        send_error_cors(res, req, 503, "Index not loaded");
        return;
    }

    SHBBox bounds = lc_index_bounds(g_index);

    /* Get rate limiter stats */
    ShRateLimitStats rl_stats = {0};
    if (s_rate_limiter) {
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
    }

    /* Get work queue stats */

    /* Get adaptive capacity stats */
    ShAdaptiveStats adaptive_stats = {0};
    if (s_adaptive_tracker) {
        sh_adaptive_stats(s_adaptive_tracker, &adaptive_stats);
    }

    /* Build JSON response using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);

    sh_json_write_key(&jw, "entities");
    sh_json_write_int(&jw, lc_index_entity_count(g_index));
    sh_json_write_key(&jw, "memory_mb");
    sh_json_write_double(&jw, (double)lc_index_memory_usage(g_index) / (1024.0 * 1024.0));

    /* bounds object */
    sh_json_write_key(&jw, "bounds");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "min_lat");
    sh_json_write_double(&jw, bounds.min_lat);
    sh_json_write_key(&jw, "min_lon");
    sh_json_write_double(&jw, bounds.min_lon);
    sh_json_write_key(&jw, "max_lat");
    sh_json_write_double(&jw, bounds.max_lat);
    sh_json_write_key(&jw, "max_lon");
    sh_json_write_double(&jw, bounds.max_lon);
    sh_json_write_object_end(&jw);

    /* rate_limit object */
    sh_json_write_key(&jw, "rate_limit");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "enabled");
    sh_json_write_bool(&jw, s_rate_limiter != NULL);
    sh_json_write_key(&jw, "rps");
    sh_json_write_double(&jw, s_config.server.rate_limit_rps);
    sh_json_write_key(&jw, "burst");
    sh_json_write_double(&jw, s_config.server.rate_limit_burst);
    sh_json_write_key(&jw, "allowed");
    sh_json_write_int(&jw, (int64_t)rl_stats.requests_allowed);
    sh_json_write_key(&jw, "denied");
    sh_json_write_int(&jw, (int64_t)rl_stats.requests_denied);
    sh_json_write_object_end(&jw);

    /* work_queue object */
    sh_json_write_key(&jw, "work_queue");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "enabled");
    sh_json_write_bool(&jw, s_pool != NULL);
    sh_json_write_key(&jw, "depth");
    sh_json_write_int(&jw, (int64_t)(s_qstats.pushed - s_qstats.popped));
    sh_json_write_key(&jw, "capacity");
    sh_json_write_int(&jw, (int64_t)s_config.server.work_queue_depth);
    sh_json_write_key(&jw, "pushed");
    sh_json_write_int(&jw, (int64_t)s_qstats.pushed);
    sh_json_write_key(&jw, "popped");
    sh_json_write_int(&jw, (int64_t)s_qstats.popped);
    sh_json_write_key(&jw, "dropped");
    sh_json_write_int(&jw, (int64_t)s_qstats.dropped);
    sh_json_write_key(&jw, "expired");
    sh_json_write_int(&jw, (int64_t)s_qstats.expired);
    sh_json_write_object_end(&jw);

    /* adaptive object */
    sh_json_write_key(&jw, "adaptive");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "enabled");
    sh_json_write_bool(&jw, s_adaptive_tracker != NULL);
    sh_json_write_key(&jw, "samples");
    sh_json_write_int(&jw, (int64_t)adaptive_stats.sample_count);
    sh_json_write_key(&jw, "response_ms");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "p50");
    sh_json_write_double(&jw, adaptive_stats.p50_ms);
    sh_json_write_key(&jw, "p90");
    sh_json_write_double(&jw, adaptive_stats.p90_ms);
    sh_json_write_key(&jw, "p99");
    sh_json_write_double(&jw, adaptive_stats.p99_ms);
    sh_json_write_object_end(&jw);
    sh_json_write_object_end(&jw);

    sh_json_write_object_end(&jw);

    char *json = sh_json_buf_take(&jb);
    record_metrics(timer, "endpoint:stats");
    send_json_cors(res, req, 200, json);
    free(json);
}

/*
 * The three geocode endpoints. Each one now does the same three things:
 * marshal a ShApiRequest, hand it to the shared dispatcher, record metrics.
 * Validation, parsing and routing all moved into locus_api_handler(), and the
 * suspend/pool/resume protocol into sh_http_async_dispatch().
 */
static void handle_geocode(KlHttpRequest *req, KlHttpResponse *res, void *ud,
                           const char *path, const char *endpoint) {
    AppCtx *app = (AppCtx *)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();

    char query_buf[512];
    size_t qlen = req->query_len < sizeof(query_buf) - 1 ? req->query_len
                                                         : sizeof(query_buf) - 1;
    if (req->query && qlen > 0) memcpy(query_buf, req->query, qlen);
    query_buf[req->query && qlen > 0 ? qlen : 0] = '\0';

    ShApiRequest api_req;
    memset(&api_req, 0, sizeof(api_req));
    api_req.method = "GET";
    api_req.path   = path;
    api_req.query  = query_buf;

    sh_http_async_dispatch(&app->async, req, res, locus_metered_handler,
                           g_api_ctx, &api_req);

    record_metrics(timer, endpoint);
    sh_trace_clear();
}

static void handle_search(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    handle_geocode(req, res, ud, "/api/v1/search", "endpoint:search");
}

static void handle_autocomplete(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    handle_geocode(req, res, ud, "/api/v1/autocomplete", "endpoint:autocomplete");
}

static void handle_reverse(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    handle_geocode(req, res, ud, "/api/v1/reverse", "endpoint:reverse");
}

/* Handle /metrics endpoint for Prometheus */
static void handle_metrics(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    sh_http_handle_metrics(res);
    sh_trace_clear();
}

/* ============================================================================
 * Request Router
 * ============================================================================ */

/* ============================================================================
 * Middleware
 * ============================================================================ */

/* CORS preflight, before rate limiting. */
static int mw_preflight(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_trace_from_headers(sh_http_trace_header_getter, req);
    sh_http_reply_preflight(res, &s_cors, get_origin_from_request(req));
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/*
 * Rate limit every request before routing, except the monitoring endpoints --
 * a health/metrics probe must not consume the request budget. Middleware
 * patterns have no alternation, so the exemptions are checked here.
 */
static int mw_rate_limit(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_trace_from_headers(sh_http_trace_header_getter, req);

    static const char *exempt[] = {
        "/api/v1/health", "/api/v1/stats", "/metrics"
    };
    for (size_t i = 0; i < sizeof(exempt) / sizeof(exempt[0]); i++) {
        size_t len = strlen(exempt[i]);
        if (req->path_len == len && memcmp(req->path, exempt[i], len) == 0)
            return 0;
    }

    if (!sh_http_check_rate_limit(req, res, s_rate_limiter, &s_cors,
                                get_origin_from_request(req))) {
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:429", "endpoint:ratelimit", NULL);
        sh_trace_clear();
        return 1;  /* short-circuit */
    }
    return 0;
}

/*
 * Anything the route table would not match.
 *
 * Keel route patterns have no wildcard ('*' is only special in middleware
 * patterns), so a catch-all route is not expressible, and Keel's built-in 404
 * is text/plain with no CORS headers.
 */
static int mw_not_found(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    AppCtx *app = (AppCtx *)ud;
    KlHttpRoute *matched = NULL;
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS];
    int num_params = 0;

    int rc = kl_http_router_match(&app->async.server->router,
                                  req->method, req->method_len,
                                  req->path, req->path_len,
                                  &matched, params, &num_params);
    if (rc == 200) return 0;

    if (rc == 405) {
        send_error_cors(res, req, 405, "Method not allowed");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:405", "endpoint:unknown", NULL);
    } else {
        send_error_cors(res, req, 404, "Not found");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:404", "endpoint:unknown", NULL);
    }
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Locus Geocoder Server\n\n");
    printf("Usage: %s [options] <pbf-or-idx-file>\n\n", prog);

    /* Common options from sh_args */
    sh_args_usage(prog, "<pbf-or-idx-file>");

    printf("Locus-specific options:\n");
    printf("  -S, --save PATH      Save index to binary file after building\n");
    printf("  --workers N          Geocode worker threads (default: auto)\n");
    printf("\n");
    printf("Locus-specific environment variables:\n");
    printf("  LOCUS_DATA_FILE      Path to data file (PBF or index)\n");
    printf("  LOCUS_NUM_WORKERS    Worker thread count (0 = auto)\n");
    printf("\n");
    printf("CORS configuration:\n");
    printf("  LOCUS_CORS_ORIGINS      Comma-separated allowed origins (empty = allow all)\n");
    printf("  LOCUS_CORS_METHODS      Allowed HTTP methods (default: GET, POST, OPTIONS)\n");
    printf("  LOCUS_CORS_HEADERS      Allowed request headers\n");
    printf("  LOCUS_CORS_CREDENTIALS  Allow credentials (default: 0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s data/monaco-latest.osm.pbf               # Build from PBF\n", prog);
    printf("  %s -S monaco.idx data/monaco-latest.osm.pbf # Build and save\n", prog);
    printf("  %s monaco.idx                              # Load from binary\n", prog);
}

int main(int argc, char *argv[]) {
    /* Initialize logging first (reads SH_LOG_LEVEL, SH_LOG_FORMAT from env) */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "locus";
    log_cfg.version = lc_version();
    sh_log_init(&log_cfg);

    /* Initialize defaults */
    init_locus_defaults(&s_config);
    sh_cors_init(&s_cors);

    /* Load config from environment */
    load_locus_env(&s_config);

    /* Parse command line arguments using sh_args for common options */
    int arg_index = sh_args_parse(&s_config.server, argc, argv);
    if (arg_index == -2) {
        /* --help was passed to sh_args */
        print_usage(argv[0]);
        return 0;
    }

    /* Parse Locus-specific arguments */
    for (int i = (arg_index > 0 ? arg_index : 1); i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--save") == 0) {
            if (++i < argc) {
                strncpy(s_config.save_path, argv[i], sizeof(s_config.save_path) - 1);
                s_config.save_path[sizeof(s_config.save_path) - 1] = '\0';
            }
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (++i < argc) s_config.num_workers = sh_parse_int(argv[i], 0, 0, 256);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            /* Positional argument - data file */
            strncpy(s_config.data_file, argv[i], sizeof(s_config.data_file) - 1);
            s_config.data_file[sizeof(s_config.data_file) - 1] = '\0';
        }
    }

    /* Validate config */
    if (s_config.data_file[0] == '\0') {
        fprintf(stderr, "Error: No data file specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    struct timeval load_start, load_end;
    gettimeofday(&load_start, NULL);

    /* Check if input is a binary index or PBF */
    if (lc_is_binary_index(s_config.data_file)) {
        /* Load via mmap (v4 zero-copy) */
        printf("Loading binary index: %s\n", s_config.data_file);
        g_index = lc_index_mmap(s_config.data_file);
        if (!g_index) {
            fprintf(stderr, "Error: Failed to load binary index\n");
            return 1;
        }
        printf("Index loaded via mmap (fast startup)\n");
    } else {
        /* Build from PBF */
        printf("Building index from: %s\n", s_config.data_file);
        g_index = lc_index_create();
        if (!g_index) {
            fprintf(stderr, "Error: Failed to create index\n");
            return 1;
        }

        LCStatus status = lc_index_build_from_pbf(g_index, s_config.data_file, NULL);
        if (status != LC_OK) {
            fprintf(stderr, "Error: Failed to load PBF: %s\n", lc_status_string(status));
            lc_index_free(g_index);
            return 1;
        }

        /* Save if requested */
        if (s_config.save_path[0] != '\0') {
            printf("Saving to: %s\n", s_config.save_path);
            status = lc_index_save(g_index, s_config.save_path);
            if (status != LC_OK) {
                fprintf(stderr, "Warning: Failed to save index: %s\n", lc_status_string(status));
            } else {
                printf("Saved binary index\n");
            }

            /* Exit if --build-only was specified */
            if (s_config.server.build_only) {
                printf("Build complete (--build-only specified)\n");
                lc_index_free(g_index);
                sh_log_shutdown();
                return 0;
            }
        }
    }

    gettimeofday(&load_end, NULL);
    double load_time = (load_end.tv_sec - load_start.tv_sec) +
                       (load_end.tv_usec - load_start.tv_usec) / 1e6;

    printf("Loaded: %u entities (%.1f MB) in %.3fs\n",
            lc_index_entity_count(g_index),
            (double)lc_index_memory_usage(g_index) / (1024.0 * 1024.0),
            load_time);

    /* The handler runs against this, on pool workers. It is created before
     * the listener opens and freed after the loop stops, so its lifetime
     * strictly contains every request. */
    {
        LCAPIConfig api_cfg;
        lc_api_config_init(&api_cfg);
        api_cfg.name = "locus-geocoder";
        g_api_ctx = lc_api_create(g_index, &api_cfg);
        if (!g_api_ctx) {
            fprintf(stderr, "Error: Failed to create API context\n");
            lc_index_free(g_index);
            return 1;
        }
    }

    /* Initialize rate limiter (uses shared library) */
    if (s_config.server.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(s_config.server.rate_limit_rps,
                                             s_config.server.rate_limit_burst, 4096);
        if (s_rate_limiter) {
            printf("Rate limit: %.0f RPS, burst %.0f (IPv4 + IPv6)\n",
                   s_config.server.rate_limit_rps, s_config.server.rate_limit_burst);
        } else {
            fprintf(stderr, "Warning: Failed to create rate limiter\n");
        }
    } else {
        printf("Rate limit: disabled\n");
    }

    /* Geocode pool is created after the HTTP server (it needs the event ctx). */

    /* Initialize adaptive capacity tracker */
    if (s_config.server.adaptive_enabled) {
        ShAdaptiveConfig adaptive_cfg;
        sh_adaptive_config_init(&adaptive_cfg);
        int num_workers = s_config.num_workers > 0 ? s_config.num_workers : 4;
        adaptive_cfg.num_workers = num_workers > 0 ? num_workers : 4;
        adaptive_cfg.target_utilization = s_config.server.target_utilization;
        adaptive_cfg.client_timeout_ms = s_config.server.client_timeout_ms;
        adaptive_cfg.burst_tiles = s_config.server.burst_tiles;
        adaptive_cfg.window_size = s_config.server.adaptive_window;
        adaptive_cfg.recalc_interval = s_config.server.adaptive_interval;

        s_adaptive_tracker = sh_adaptive_create(&adaptive_cfg);
        if (s_adaptive_tracker) {
            printf("Adaptive capacity: enabled (window=%zu, interval=%.0f, util=%.0f%%)\n",
                   s_config.server.adaptive_window, s_config.server.adaptive_interval,
                   s_config.server.target_utilization * 100.0);
        } else {
            fprintf(stderr, "Warning: Failed to create adaptive tracker\n");
        }
    } else {
        printf("Adaptive capacity: disabled\n");
    }

    /* Initialize metrics (reads SH_METRICS_STATSD_HOST from env) */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "locus";
    if (sh_metrics_init(&metrics_cfg) == 0) {
        const char *statsd_host = getenv("SH_METRICS_STATSD_HOST");
        if (statsd_host && statsd_host[0]) {
            printf("Metrics: StatsD enabled (%s:%d)\n", statsd_host,
                   metrics_cfg.statsd_port ? metrics_cfg.statsd_port : 8125);
        } else {
            printf("Metrics: Prometheus endpoint at /metrics\n");
        }
    }

    SH_LOG_INFO("Server initializing",
                "data_file", s_config.data_file,
                "port", s_config.server.host);


    /* Initialize HTTP server */
    KlHttpServer server;
    KlHttpServerConfig http_cfg = {
        .port = s_config.server.port,
        .bind_addr = s_config.server.host,
        .install_signal_handlers = 1,
        .drain_timeout_ms = 5000,
    };

    if (kl_http_server_init(&server, &http_cfg) < 0) {
        fprintf(stderr, "Error: Failed to listen on %s:%d\n",
                s_config.server.host, s_config.server.port);
        sh_ratelimit_free(s_rate_limiter);
        sh_adaptive_free(s_adaptive_tracker);
        lc_api_free(g_api_ctx);
        lc_index_free(g_index);
        return 1;
    }

    /* Geocode pool. queue_capacity gives the backpressure ShWorkQueue used to. */
    if (s_config.server.work_queue_enabled) {
        KlThreadPoolConfig pool_cfg = {
            .num_workers = s_config.num_workers,
            .queue_capacity = (int)s_config.server.work_queue_depth,
        };
        s_pool = kl_thread_pool_create(kl_http_server_event_ctx(&server), &pool_cfg);
        if (s_pool) {
            printf("Geocode pool: depth %zu, timeout %.1fs, %d workers\n",
                   s_config.server.work_queue_depth,
                   s_config.server.work_queue_timeout, s_config.num_workers);
        } else {
            fprintf(stderr, "Warning: Failed to create geocode thread pool\n");
        }
    }

    AppCtx app;
    memset(&app, 0, sizeof(app));
    app.async.server    = &server;
    app.async.pool      = s_pool;
    app.async.cors      = &s_cors;
    app.async.timeout_s = s_config.server.work_queue_timeout;
    app.async.stats     = &s_qstats;

    /* Routes. Every handler that suspends must be a route, not middleware. */
    kl_http_server_route(&server, "GET", "/api/v1/health",       handle_health,       NULL, NULL);
    kl_http_server_route(&server, "GET", "/api/v1/stats",        handle_stats,        NULL, NULL);
    kl_http_server_route(&server, "GET", "/metrics",             handle_metrics,      NULL, NULL);
    kl_http_server_route(&server, "GET", "/api/v1/search",       handle_search,       &app, NULL);
    kl_http_server_route(&server, "GET", "/api/v1/autocomplete", handle_autocomplete, &app, NULL);
    kl_http_server_route(&server, "GET", "/api/v1/reverse",      handle_reverse,      &app, NULL);

    /* Middleware runs in registration order, before routing. */
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_rate_limit, NULL);
    kl_http_server_use(&server, "*", "/*", mw_not_found, &app);

    printf("\nLocus Geocoder Server v%s\n", lc_version());
    printf("Listening on http://%s:%d\n", s_config.server.host, s_config.server.port);
    printf("\nEndpoints:\n");
    printf("  GET  /api/v1/health\n");
    printf("  GET  /api/v1/stats\n");
    printf("  GET  /api/v1/search?q=<query>&limit=<n>\n");
    printf("  GET  /api/v1/autocomplete?q=<prefix>&limit=<n>\n");
    printf("  GET  /api/v1/reverse?lat=<lat>&lon=<lon>\n");
    printf("  GET  /metrics\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop: blocks until SIGINT/SIGTERM. */
    kl_http_server_run(&server);

    printf("\nShutting down...\n");

    /* Pool first: drains in-flight work, fires cancel_fn for queued items. */
    if (s_pool) kl_thread_pool_free(s_pool);
    kl_http_server_free(&server);

    printf("Geocode queue: %lu pushed, %lu popped, %lu dropped, %lu expired\n",
           (unsigned long)s_qstats.pushed, (unsigned long)s_qstats.popped,
           (unsigned long)s_qstats.dropped, (unsigned long)s_qstats.expired);

    /* Print rate limiter stats */
    if (s_rate_limiter) {
        ShRateLimitStats rl_stats;
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
        printf("Rate limiter: %lu allowed, %lu denied\n",
               (unsigned long)rl_stats.requests_allowed,
               (unsigned long)rl_stats.requests_denied);
    }

    /* Cleanup */
    sh_ratelimit_free(s_rate_limiter);
    sh_adaptive_free(s_adaptive_tracker);
    lc_api_free(g_api_ctx);
    lc_index_free(g_index);

    SH_LOG_INFO("Server shutdown complete");
    sh_metrics_shutdown();
    sh_log_shutdown();

    return 0;
}
