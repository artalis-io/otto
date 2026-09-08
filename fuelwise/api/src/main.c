/*
 * FuelWise REST API Server
 *
 * A lightweight HTTP API for fuel optimization, served by Keel (MIT).
 * Features rate limiting, a bounded solve queue for CPU-intensive operations,
 * and configuration through CLI args and environment variables.
 *
 * Uses the transport-agnostic fw_api_handle() for core processing.
 *
 * Solves run on a Keel thread pool with the connection suspended via
 * KlAsyncOp, so the event loop keeps serving while a solve is in flight,
 * rather than serializing every request behind the running solve. See
 * solve_on_resume() for the one non-obvious part of the Keel async contract.
 *
 * Endpoints:
 *   GET  /api/v1/health         - Health check (bypasses queue + rate limit)
 *   GET  /api/v1/stats          - Server statistics (bypasses queue + rate limit)
 *   POST /api/v1/filter         - Filter stations to route
 *   POST /api/v1/solve          - Solve refueling problem
 *   POST /api/v1/optimize       - Full optimization pipeline
 */

#include <keel/keel.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fuelwise.h"
#include "fw_api.h"  /* Transport-agnostic API handler */
#include "shared.h"  /* For sh_ratelimit, sh_args */
#include "sh_httpserver.h"
#include "sh_httpasync.h"
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"
#include "sh_json.h"  /* For JSON building */

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Rate limiter instance */
static ShRateLimiter *s_rate_limiter = NULL;

/* Server configuration (from sh_args) */
static ShServerConfig s_config;

/* CORS configuration */
static ShCorsConfig s_cors;

/*
 * Request body cap. Keel's default is 1 MB; station lists can be larger.
 */
#define FW_MAX_BODY_SIZE (16u * 1024u * 1024u)

/* ============================================================================
 * Solve Queue
 *
 * KlThreadPool replaces ShWorkQueue + ShWorkerPool + ShCompletion. It exposes
 * no statistics of its own, but /api/v1/stats publishes work-queue counters
 * that test_api.sh asserts on, so sh_http_async_dispatch() maintains them in
 * the ShHttpAsyncStats below.
 * ============================================================================ */

static KlThreadPool *s_pool = NULL;
static ShHttpAsyncStats s_qstats;

typedef struct {
    ShHttpAsync async;   /* server, pool, cors, timeout, stats */
} AppCtx;

/* ============================================================================
 * Response Helpers
 * ============================================================================ */

static void send_error(KlHttpResponse *res, int status, const char *message) {
    sh_http_reply_error(res, status, &s_cors, NULL, message);
}

static void send_json_status(KlHttpResponse *res, int status, const char *json) {
    sh_http_reply_json(res, status, &s_cors, NULL, json);
}

static void send_json(KlHttpResponse *res, const char *json) {
    send_json_status(res, 200, json);
}

static void record_metrics(ShMetricsTimer timer, const char *endpoint) {
    sh_metrics_counter_inc("http_requests_total", 1,
        "endpoint", endpoint, "service", "fuelwise", NULL);
    sh_metrics_timer_observe(timer, "http_request_duration_ms",
        "endpoint", endpoint, "service", "fuelwise", NULL);
}

/* ============================================================================
 * Solve Pipeline
 * ============================================================================ */

/*
 * Route a CPU-intensive endpoint through the solve pool, or run it inline
 * when the queue is disabled (--queue-off), matching the old behaviour.
 */
static void handle_via_queue(KlHttpRequest *req, KlHttpResponse *res, void *ud,
                             const char *path, const char *endpoint) {
    AppCtx *app = (AppCtx *)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();

    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;

    ShApiRequest api_req;
    memset(&api_req, 0, sizeof(api_req));
    api_req.method   = "POST";
    api_req.path     = path;
    api_req.body     = (br && br->len > 0) ? br->data : NULL;
    api_req.body_len = (br && br->len > 0) ? br->len : 0;

    /* Everything the suspend/pool/resume protocol used to do by hand here --
     * the context, the body copy, on_resume/on_cancel/on_deadline, the 503 on
     * a full queue and the 504 on a deadline -- is sh_http_async_dispatch's
     * job now. See shared/src/sh_httpasync.c. */
    sh_http_async_dispatch(&app->async, req, res, fw_api_handle, NULL, &api_req);

    record_metrics(timer, endpoint);
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

static void handle_health(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    sh_http_handle_health(res, &s_cors, NULL, "fuelwise-api", fw_version());
    record_metrics(timer, "health");
    sh_trace_clear();
}

static void handle_stats(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();

    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "service");
    sh_json_write_string(&jw, "fuelwise-api");
    sh_json_write_key(&jw, "version");
    sh_json_write_string(&jw, fw_version());

    /* Work queue stats (now backed by the Keel thread pool) */
    sh_json_write_key(&jw, "work_queue");
    sh_json_write_object_start(&jw);
    if (s_pool) {
        sh_json_write_key(&jw, "enabled");
        sh_json_write_bool(&jw, true);
        sh_json_write_key(&jw, "depth");
        sh_json_write_int(&jw, (int64_t)(s_qstats.pushed - s_qstats.popped));
        sh_json_write_key(&jw, "capacity");
        sh_json_write_int(&jw, (int64_t)s_config.work_queue_depth);
        sh_json_write_key(&jw, "pushed");
        sh_json_write_int(&jw, (int64_t)s_qstats.pushed);
        sh_json_write_key(&jw, "popped");
        sh_json_write_int(&jw, (int64_t)s_qstats.popped);
        sh_json_write_key(&jw, "dropped");
        sh_json_write_int(&jw, (int64_t)s_qstats.dropped);
        sh_json_write_key(&jw, "expired");
        sh_json_write_int(&jw, (int64_t)s_qstats.expired);
        sh_json_write_key(&jw, "timeout_sec");
        sh_json_write_double(&jw, s_config.work_queue_timeout);
    } else {
        sh_json_write_key(&jw, "enabled");
        sh_json_write_bool(&jw, false);
    }
    sh_json_write_object_end(&jw);

    /* Rate limit stats */
    sh_json_write_key(&jw, "rate_limit");
    sh_json_write_object_start(&jw);
    if (s_rate_limiter) {
        ShRateLimitStats rl_stats;
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
        sh_json_write_key(&jw, "enabled");
        sh_json_write_bool(&jw, true);
        sh_json_write_key(&jw, "rps");
        sh_json_write_double(&jw, s_config.rate_limit_rps);
        sh_json_write_key(&jw, "burst");
        sh_json_write_double(&jw, s_config.rate_limit_burst);
        sh_json_write_key(&jw, "allowed");
        sh_json_write_int(&jw, (int64_t)rl_stats.requests_allowed);
        sh_json_write_key(&jw, "denied");
        sh_json_write_int(&jw, (int64_t)rl_stats.requests_denied);
        sh_json_write_key(&jw, "active_entries");
        sh_json_write_int(&jw, (int64_t)rl_stats.active_entries);
        sh_json_write_key(&jw, "evictions");
        sh_json_write_int(&jw, (int64_t)rl_stats.evictions);
    } else {
        sh_json_write_key(&jw, "enabled");
        sh_json_write_bool(&jw, false);
    }
    sh_json_write_object_end(&jw);

    sh_json_write_object_end(&jw);

    char *json = sh_json_buf_take(&jb);
    send_json(res, json);
    free(json);

    record_metrics(timer, "stats");
    sh_trace_clear();
}

static void handle_metrics(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    sh_http_handle_metrics(res);
    sh_trace_clear();
}

static void handle_solve(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    handle_via_queue(req, res, ud, "/api/v1/solve", "solve");
    sh_trace_clear();
}

static void handle_filter(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    handle_via_queue(req, res, ud, "/api/v1/filter", "filter");
    sh_trace_clear();
}

static void handle_optimize(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    handle_via_queue(req, res, ud, "/api/v1/optimize", "optimize");
    sh_trace_clear();
}

/* ============================================================================
 * Middleware
 * ============================================================================ */

/* CORS preflight, before rate limiting. */
static int mw_preflight(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_trace_from_headers(sh_http_trace_header_getter, req);
    sh_http_reply_preflight(res, &s_cors, NULL);
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/*
 * Rate limit everything except health, stats and metrics.
 *
 * Middleware patterns support a trailing slash-star prefix but not
 * alternation, so the exemptions are checked here rather than by registering
 * this on several patterns.
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

    if (!sh_http_check_rate_limit(req, res, s_rate_limiter, &s_cors, NULL)) {
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
        send_error(res, 405, "Method not allowed");
        sh_metrics_counter_inc("http_requests_total", 1,
            "endpoint", "method_not_allowed", "service", "fuelwise", NULL);
    } else {
        send_error(res, 404, "Not found");
        sh_metrics_counter_inc("http_requests_total", 1,
            "endpoint", "not_found", "service", "fuelwise", NULL);
    }
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/* Body reader factory: kl_http_body_reader_buffer() reads max_size from the
   route's user_data, so wrap it to keep the cap explicit. */
static KlHttpBodyReader *api_body_reader(KlAllocator *alloc,
                                         const KlHttpRequest *req,
                                         void *user_data) {
    (void)user_data;
    return kl_http_body_reader_buffer(alloc, req,
                                      (void *)(size_t)FW_MAX_BODY_SIZE);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    sh_args_usage(prog,
        "<no-data-file>\n\n"
        "FuelWise API server for fuel optimization.\n\n"
        "Example:\n"
        "  %s -p 8080                    # Start on port 8080\n"
        "  %s --rate-limit-off           # Disable rate limiting\n"
        "  %s --queue-off                # Disable work queue\n"
    );
}

int main(int argc, char *argv[]) {
    /* Initialize config with defaults */
    sh_args_init(&s_config);
    sh_cors_init(&s_cors);

    /* FuelWise-specific defaults */
    s_config.port = 8080;
    s_config.rate_limit_rps = 10.0;
    s_config.rate_limit_burst = 50.0;
    s_config.work_queue_depth = 100;
    s_config.work_queue_timeout = 10.0;
    s_config.worker_threads = 4;

    /* Load from environment first */
    sh_args_load_env(&s_config, SH_API_FUELWISE);

    /* Parse command line (overrides env) */
    /*
     * sh_args_parse() returns -2 for --help and -1 for a parse error.
     * Treating both as an error made --help exit 1 and left the -h/--help
     * loop below unreachable.
     */
    int first_arg = sh_args_parse(&s_config, argc, argv);
    if (first_arg == -2) {
        print_usage(argv[0]);
        return 0;
    }
    if (first_arg < 0) {
        print_usage(argv[0]);
        return 1;
    }

    /* -h is not handled by sh_args_parse */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Initialize logging */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "fuelwise";
    log_cfg.version = fw_version();
    sh_log_init(&log_cfg);

    /* Initialize metrics */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "fuelwise";
    sh_metrics_init(&metrics_cfg);

    SH_LOG_INFO("Starting fuelwise API server", "version", fw_version(), NULL);

    /* Initialize rate limiter */
    if (s_config.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(
            s_config.rate_limit_rps,
            s_config.rate_limit_burst,
            s_config.rate_limit_buckets > 0 ? s_config.rate_limit_buckets : 4096
        );
        if (!s_rate_limiter) {
            fprintf(stderr, "Error: Failed to create rate limiter\n");
            return 1;
        }
    }

    /* Initialize HTTP server */
    KlHttpServer server;
    KlHttpServerConfig http_cfg = {
        .port = s_config.port,
        .bind_addr = s_config.host[0] ? s_config.host : "0.0.0.0",
        .install_signal_handlers = 1,
        .max_body_size = FW_MAX_BODY_SIZE,
        .drain_timeout_ms = 5000,
    };

    if (kl_http_server_init(&server, &http_cfg) < 0) {
        fprintf(stderr, "Error: Cannot listen on %s:%d\n",
            s_config.host[0] ? s_config.host : "0.0.0.0", s_config.port);
        sh_ratelimit_free(s_rate_limiter);
        return 1;
    }

    /* Solve pool. queue_capacity gives the backpressure ShWorkQueue used to. */
    if (s_config.work_queue_enabled) {
        KlThreadPoolConfig pool_cfg = {
            .num_workers = s_config.worker_threads > 0 ? s_config.worker_threads : 0,
            .queue_capacity = (int)s_config.work_queue_depth,
        };
        s_pool = kl_thread_pool_create(kl_http_server_event_ctx(&server), &pool_cfg);
        if (!s_pool) {
            fprintf(stderr, "Error: Failed to create solve thread pool\n");
            kl_http_server_free(&server);
            sh_ratelimit_free(s_rate_limiter);
            return 1;
        }
    }

    AppCtx app;
    memset(&app, 0, sizeof(app));
    app.async.server    = &server;
    app.async.pool      = s_pool;
    app.async.cors      = &s_cors;
    app.async.timeout_s = s_config.work_queue_timeout;
    app.async.stats     = &s_qstats;

    /*
     * Routes are method-specific, so a wrong method (e.g. GET /api/v1/solve)
     * makes kl_http_router_match() return 405 and mw_not_found answers with
     * "Method not allowed".
     */
    kl_http_server_route(&server, "GET",  "/api/v1/health",   handle_health,   NULL, NULL);
    kl_http_server_route(&server, "GET",  "/api/v1/stats",    handle_stats,    NULL, NULL);
    kl_http_server_route(&server, "GET",  "/metrics",         handle_metrics,  NULL, NULL);
    kl_http_server_route(&server, "POST", "/api/v1/solve",    handle_solve,    &app, api_body_reader);
    kl_http_server_route(&server, "POST", "/api/v1/filter",   handle_filter,   &app, api_body_reader);
    kl_http_server_route(&server, "POST", "/api/v1/optimize", handle_optimize, &app, api_body_reader);

    /*
     * Middleware runs in registration order, before routing. Preflight first
     * (it must not be rate limited), then the limiter, then the fallback,
     * which must be last because it short-circuits unmatched requests.
     */
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_rate_limit, NULL);
    kl_http_server_use(&server, "*", "/*", mw_not_found, &app);

    /* Print startup message */
    printf("FuelWise API Server v%s\n", fw_version());
    printf("Listening on http://%s:%d\n",
        s_config.host[0] ? s_config.host : "0.0.0.0", s_config.port);
    printf("\n");
    printf("Configuration:\n");
    printf("  Rate limiting: %s", s_config.rate_limit_enabled ? "enabled" : "disabled");
    if (s_config.rate_limit_enabled) {
        printf(" (%.1f RPS, burst %.0f)", s_config.rate_limit_rps, s_config.rate_limit_burst);
    }
    printf("\n");
    printf("  Work queue: %s", s_config.work_queue_enabled ? "enabled" : "disabled");
    if (s_pool) {
        printf(" (depth %zu, timeout %.1fs, %d workers)",
            s_config.work_queue_depth, s_config.work_queue_timeout,
            s_config.worker_threads > 0 ? s_config.worker_threads : 0);
    }
    printf("\n");
    printf("\n");
    printf("Endpoints:\n");
    printf("  GET  /api/v1/health    - Health check\n");
    printf("  GET  /api/v1/stats     - Server statistics\n");
    printf("  POST /api/v1/solve     - Solve refueling problem\n");
    printf("  POST /api/v1/filter    - Filter stations to route\n");
    printf("  POST /api/v1/optimize  - Full optimization pipeline\n");
    printf("\nPress Ctrl+C to stop.\n\n");
    fflush(stdout);

    /* Event loop: blocks until SIGINT/SIGTERM. */
    kl_http_server_run(&server);

    printf("\nShutting down...\n");

    /* Pool first: drains in-flight work, fires cancel_fn for queued items. */
    if (s_pool) kl_thread_pool_free(s_pool);

    kl_http_server_free(&server);
    sh_ratelimit_free(s_rate_limiter);

    return 0;
}
