/*
 * Surge API Server
 *
 * REST API for VRP/PDPTW solving.
 * Uses Keel (MIT) for HTTP serving.
 *
 * Features:
 * - Rate limiting (per-IP token bucket)
 * - Bounded solve queue with backpressure
 * - Read/body timeout protection
 * - Structured logging
 * - Distributed tracing
 * - Prometheus metrics
 *
 * Architecture:
 *   Each solve request is fully self-contained: JSON in -> SGContext build ->
 *   solve -> JSON out. No shared mutable state between requests.
 *
 *   Solves run on a Keel thread pool. The connection is suspended via
 *   KlAsyncOp for the duration, so the event loop keeps accepting and serving
 *   other requests while a solve is in flight, rather than serializing every
 *   request behind the running solve.
 */

#include "surge.h"
#include "sg_api.h"

#include <keel/keel.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared library includes */
#include "shared.h"
#include "sh_httpserver.h"
#include "sh_httpasync.h"
#include "sh_args.h"
#include "sh_cors.h"
#include "sh_json.h"
#include "sh_log.h"
#include "sh_metrics.h"
#include "sh_ratelimit.h"
#include "sh_trace.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    ShServerConfig server;
    int num_workers;  /* Solve worker threads (0 = auto) */
} SurgeServerConfig;

static SurgeServerConfig s_config;

/* CORS configuration */
static ShCorsConfig s_cors;

/*
 * Maximum accepted solve payload. Keel's default body limit is 1 MB, which is
 * well under a realistic large VRP instance, so raise it explicitly.
 */
#define SURGE_MAX_BODY_SIZE (32u * 1024u * 1024u)

/* ============================================================================
 * Global State
 *
 * DESIGN NOTE: Thread Safety
 *
 * Unlike Locus/Carta, Surge has no shared read-only index. Each solve
 * request creates its own SGContext, so there is no shared mutable state
 * between requests. The only shared resource is the rate limiter, which is
 * internally thread-safe.
 * ============================================================================ */

/* Transport-agnostic API context */
static SGAPIContext *s_api_ctx = NULL;

/* Rate limiter instance */
static ShRateLimiter *s_rate_limiter = NULL;

/* ============================================================================
 * Application Context
 * ============================================================================ */

typedef struct {
    ShHttpAsync async;   /* server, pool, cors, timeout, stats */
} AppCtx;

/* ============================================================================
 * Configuration Loading
 * ============================================================================ */

static void init_surge_defaults(SurgeServerConfig *cfg) {
    sh_args_init(&cfg->server);

    /* Surge-specific defaults */
    cfg->server.port = 8085;
    cfg->server.rate_limit_rps = 10.0;       /* VRP solving is expensive */
    cfg->server.rate_limit_burst = 20.0;
    cfg->server.work_queue_depth = 64;
    cfg->server.work_queue_timeout = 60.0;   /* Solves can take time */

    cfg->num_workers = 0;  /* Auto-detect */
}

static void load_surge_env(SurgeServerConfig *cfg) {
    const char *val;

    sh_args_load_env(&cfg->server, SH_API_SURGE);

    if ((val = getenv("SURGE_NUM_WORKERS"))) {
        cfg->num_workers = sh_parse_int(val, 0, 0, 256);
    }

    /* CORS configuration */
    if ((val = getenv("SURGE_CORS_ORIGINS"))) {
        sh_cors_parse_origins(&s_cors, val);
    }
    if ((val = getenv("SURGE_CORS_METHODS"))) {
        sh_cors_set_methods(&s_cors, val);
    }
    if ((val = getenv("SURGE_CORS_HEADERS"))) {
        sh_cors_set_headers(&s_cors, val);
    }
    if ((val = getenv("SURGE_CORS_CREDENTIALS"))) {
        s_cors.allow_credentials = (sh_parse_int(val, 0, 0, 1) != 0);
    }
}

/* ============================================================================
 * Middleware
 * ============================================================================ */

/* Rate limit every request before routing. */
static int mw_rate_limit(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;

    /* Extract or generate trace ID for this request. */
    sh_trace_from_headers(sh_http_trace_header_getter, req);

    if (!sh_http_check_rate_limit(req, res, s_rate_limiter, &s_cors,
                                sh_http_origin(req))) {
        SH_LOG_WARN("Rate limit exceeded", "status", "429");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:429", "endpoint:ratelimit", NULL);
        sh_trace_clear();
        return 1;  /* short-circuit */
    }
    return 0;
}

/* CORS preflight. */
static int mw_preflight(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_http_reply_preflight(res, &s_cors, sh_http_origin(req));
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/*
 * Unmatched paths.
 *
 * Keel route patterns have no wildcard -- '*' is only special in middleware
 * patterns -- so a catch-all route is not expressible, and an unmatched path
 * would otherwise fall through to Keel's built-in text/plain 404, which
 * carries no CORS headers (a browser would see an opaque CORS failure rather
 * than a clean 404). Ask the router what it would do and answer in the same
 * JSON+CORS shape.
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
    if (rc == 200) return 0;  /* a route will handle this */

    if (rc == 405) {
        sh_http_reply_error(res, 405, &s_cors, sh_http_origin(req),
                          "Method not allowed");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:405", "endpoint:unknown", NULL);
    } else {
        sh_http_reply_error(res, 404, &s_cors, sh_http_origin(req), "Not found");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:404", "endpoint:unknown", NULL);
    }
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/* ============================================================================
 * Request Handlers
 * ============================================================================ */

static void handle_health(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_http_handle_health(res, &s_cors, sh_http_origin(req), "surge", sg_version());
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", "endpoint:health", NULL);
    sh_trace_clear();
}

static void handle_version(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    size_t out_len;
    char *json = sg_api_version(&out_len);
    if (json) {
        sh_http_reply_json(res, 200, &s_cors, sh_http_origin(req), json);
        free(json);
    } else {
        sh_http_reply_error(res, 500, &s_cors, sh_http_origin(req),
                          "Failed to generate version response");
    }
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", "endpoint:version", NULL);
    sh_trace_clear();
}

static void handle_stats(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    ShApiRequest api_req = { .path = "/api/v1/stats" };
    ShApiResponse resp = {0};

    if (sg_api_handle(s_api_ctx, &api_req, &resp) == 0 && resp.body) {
        sh_http_reply_json(res, resp.status_code, &s_cors, sh_http_origin(req),
                         (const char *)resp.body);
    } else {
        sh_http_reply_error(res, 500, &s_cors, sh_http_origin(req),
                          "Failed to generate stats response");
    }
    sh_api_response_free(&resp);

    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", "endpoint:stats", NULL);
    sh_trace_clear();
}

static void handle_metrics(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    sh_http_handle_metrics(res);
    sh_trace_clear();
}

static void handle_solve(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    AppCtx *app = (AppCtx *)ud;
    const char *origin = sh_http_origin(req);

    /* Registered for "*" so a non-POST gets 405 rather than the catch-all 404. */
    if (req->method_len != 4 || memcmp(req->method, "POST", 4) != 0) {
        sh_http_reply_error(res, 405, &s_cors, origin,
                          "Method not allowed. Use POST.");
        sh_trace_clear();
        return;
    }

    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        sh_http_reply_error(res, 400, &s_cors, origin, "Empty request body");
        sh_trace_clear();
        return;
    }

    ShApiRequest api_req;
    memset(&api_req, 0, sizeof(api_req));
    api_req.method   = "POST";
    api_req.path     = "/api/v1/solve";
    api_req.body     = br->data;
    api_req.body_len = br->len;

    /* The context, the body copy, on_resume/on_cancel/on_deadline, the 503 on
     * a full queue and the 504 on a deadline all live in
     * sh_http_async_dispatch() now. See shared/src/sh_httpasync.c. */
    sh_http_async_dispatch(&app->async, req, res, sg_api_handle, s_api_ctx,
                           &api_req);

    sh_trace_clear();
}

/* ============================================================================
 * Body Reader
 * ============================================================================ */

/*
 * kl_http_body_reader_buffer() reads its max_size from the route's user_data,
 * which we need for the app context. Wrap it so the cap stays explicit.
 */
static KlHttpBodyReader *solve_body_reader(KlAllocator *alloc,
                                           const KlHttpRequest *req,
                                           void *user_data) {
    (void)user_data;
    return kl_http_body_reader_buffer(alloc, req,
                                      (void *)(size_t)SURGE_MAX_BODY_SIZE);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Surge VRP Solver Server\n\n");
    printf("Usage: %s [options]\n\n", prog);

    sh_args_usage(prog, NULL);

    printf("Surge-specific options:\n");
    printf("  --workers N          Solve worker threads (default: auto)\n");
    printf("\n");
    printf("Surge-specific environment variables:\n");
    printf("  SURGE_NUM_WORKERS    Worker thread count (0 = auto)\n");
    printf("\n");
    printf("CORS configuration:\n");
    printf("  SURGE_CORS_ORIGINS      Comma-separated allowed origins (empty = allow all)\n");
    printf("  SURGE_CORS_METHODS      Allowed HTTP methods (default: GET, POST, OPTIONS)\n");
    printf("  SURGE_CORS_HEADERS      Allowed request headers\n");
    printf("  SURGE_CORS_CREDENTIALS  Allow credentials (default: 0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                           # Start on port 8085\n", prog);
    printf("  %s -p 9000                   # Start on custom port\n", prog);
    printf("  %s --workers 4               # Use 4 solve workers\n", prog);
}

int main(int argc, char *argv[]) {
    /* Initialize logging */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "surge";
    log_cfg.version = sg_version();
    sh_log_init(&log_cfg);

    /* Initialize defaults */
    init_surge_defaults(&s_config);
    sh_cors_init(&s_cors);

    /* Load config from environment */
    load_surge_env(&s_config);

    /* Parse command line arguments */
    int arg_index = sh_args_parse(&s_config.server, argc, argv);
    if (arg_index == -2) {
        print_usage(argv[0]);
        return 0;
    }

    /* Parse Surge-specific arguments */
    for (int i = (arg_index > 0 ? arg_index : 1); i < argc; i++) {
        if (strcmp(argv[i], "--workers") == 0) {
            if (++i < argc) s_config.num_workers = sh_parse_int(argv[i], 0, 0, 256);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Initialize API context */
    s_api_ctx = sg_api_create();
    if (!s_api_ctx) {
        fprintf(stderr, "Error: Failed to create API context\n");
        return 1;
    }

    /* Initialize rate limiter */
    if (s_config.server.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(s_config.server.rate_limit_rps,
                                             s_config.server.rate_limit_burst, 4096);
        if (s_rate_limiter) {
            printf("Rate limit: %.0f RPS, burst %.0f\n",
                   s_config.server.rate_limit_rps, s_config.server.rate_limit_burst);
        } else {
            fprintf(stderr, "Warning: Failed to create rate limiter\n");
        }
    } else {
        printf("Rate limit: disabled\n");
    }

    /* Initialize metrics */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "surge";
    if (sh_metrics_init(&metrics_cfg) == 0) {
        const char *statsd_host = getenv("SH_METRICS_STATSD_HOST");
        if (statsd_host && statsd_host[0]) {
            printf("Metrics: StatsD enabled (%s:%d)\n", statsd_host,
                   metrics_cfg.statsd_port ? metrics_cfg.statsd_port : 8125);
        } else {
            printf("Metrics: Prometheus endpoint at /metrics\n");
        }
    }

    /* Initialize HTTP server */
    KlHttpServer server;
    KlHttpServerConfig http_cfg = {
        .port = s_config.server.port,
        .install_signal_handlers = 1,
        .max_body_size = SURGE_MAX_BODY_SIZE,
        .drain_timeout_ms = 5000,
    };

    if (kl_http_server_init(&server, &http_cfg) < 0) {
        fprintf(stderr, "Error: Failed to initialize HTTP server on port %d\n",
                s_config.server.port);
        sg_api_free(s_api_ctx);
        return 1;
    }

    /*
     * Solve thread pool. queue_capacity provides the same backpressure the old
     * ShWorkQueue did; a full queue yields 503.
     */
    KlThreadPoolConfig pool_cfg = {
        .num_workers = s_config.num_workers,
        .queue_capacity = (int)s_config.server.work_queue_depth,
    };
    KlThreadPool *pool = kl_thread_pool_create(kl_http_server_event_ctx(&server),
                                               &pool_cfg);
    if (!pool) {
        fprintf(stderr, "Error: Failed to create solve thread pool\n");
        kl_http_server_free(&server);
        sg_api_free(s_api_ctx);
        return 1;
    }
    printf("Solve pool: queue depth %zu, timeout %.1fs, %d workers\n",
           s_config.server.work_queue_depth,
           s_config.server.work_queue_timeout,
           s_config.num_workers);

    AppCtx app;
    memset(&app, 0, sizeof(app));
    app.async.server    = &server;
    app.async.pool      = pool;
    app.async.cors      = &s_cors;
    app.async.timeout_s = s_config.server.work_queue_timeout;

    /* Routes. */
    kl_http_server_route(&server, "GET", "/api/v1/health",  handle_health,  NULL, NULL);
    kl_http_server_route(&server, "GET", "/api/v1/version", handle_version, NULL, NULL);
    kl_http_server_route(&server, "GET", "/api/v1/stats",   handle_stats,   NULL, NULL);
    kl_http_server_route(&server, "*",   "/api/v1/solve",   handle_solve,   &app,
                         solve_body_reader);
    kl_http_server_route(&server, "GET", "/metrics",        handle_metrics, NULL, NULL);

    /*
     * Middleware runs in registration order, before routing. mw_not_found must
     * come last: it short-circuits anything the route table would not match.
     */
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_rate_limit, NULL);
    kl_http_server_use(&server, "*", "/*", mw_not_found, &app);

    /*
     * sh_log reads every field value with va_arg(..., const char *), so the
     * value must be a string -- passing the int port here dereferenced it as
     * a pointer and crashed on startup.
     */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", s_config.server.port);
    SH_LOG_INFO("Server starting", "port", port_str);

    printf("\nSurge VRP Solver Server\n");
    printf("Listening on http://0.0.0.0:%d\n\n", s_config.server.port);
    printf("Endpoints:\n");
    printf("  GET  /api/v1/health    - Health check\n");
    printf("  GET  /api/v1/version   - Version info\n");
    printf("  POST /api/v1/solve     - Solve VRP/PDPTW\n");
    printf("  GET  /api/v1/stats     - Statistics\n");
    printf("  GET  /metrics          - Prometheus metrics\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop: blocks until SIGINT/SIGTERM. */
    kl_http_server_run(&server);

    printf("\nShutting down...\n");

    /* Pool first: drains in-flight solves, fires cancel_fn for queued ones. */
    kl_thread_pool_free(pool);

    /* Print rate limiter stats */
    if (s_rate_limiter) {
        ShRateLimitStats rl_stats;
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
        printf("Rate limiter: %lu allowed, %lu denied\n",
               (unsigned long)rl_stats.requests_allowed,
               (unsigned long)rl_stats.requests_denied);
    }

    /* Cleanup */
    kl_http_server_free(&server);
    sh_ratelimit_free(s_rate_limiter);
    sg_api_free(s_api_ctx);

    SH_LOG_INFO("Server shutdown complete");
    sh_metrics_shutdown();
    sh_log_shutdown();

    return 0;
}
