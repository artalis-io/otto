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
 *   other requests while a solve is in flight. (The previous mongoose server
 *   blocked the event loop in sh_completion_wait(), which serialized every
 *   request behind the running solve.)
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
#include "sh_keelserver.h"
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
    KlHttpServer *server;
    KlThreadPool *pool;
} AppCtx;

/* ============================================================================
 * Solve Work Item
 *
 * OWNERSHIP / LIFETIME
 *
 *   work_fn     runs on a pool worker thread and touches only `body` and the
 *               response_* fields.
 *   done_fn     runs on the event loop thread after work_fn returns.
 *   on_cancel   runs on the event loop thread if the connection dies while
 *               the op is suspended.
 *   on_deadline runs on the event loop thread when the solve budget expires.
 *
 * The context is freed in exactly one place: done_fn (the item ran) or
 * cancel_fn (the item was dropped during pool shutdown before starting).
 * on_cancel and on_deadline never free, because work_fn may still be running
 * on a worker thread; they only set `detached`. `detached` is a plain int:
 * every reader and writer of it runs on the event loop thread.
 * ============================================================================ */

typedef struct {
    KlAsyncOp op;
    AppCtx *app;

    /* Request body (copied out of the buffer reader before suspending) */
    char *body;
    size_t body_len;

    /* Response (set by worker) */
    char *response_json;
    size_t response_len;
    int status_code;
    char error_msg[128];

    /* 1 once the connection is gone or the deadline already replied. */
    int detached;

    ShMetricsTimer solve_timer;
} SolveCtx;

static void solve_ctx_free(SolveCtx *ctx) {
    if (!ctx) return;
    free(ctx->response_json);
    free(ctx->body);
    free(ctx);
}

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
 * Solve Worker
 * ============================================================================ */

/*
 * Process a solve request. Runs on a pool worker thread.
 */
static void process_solve(SolveCtx *ctx) {
    int status_code = 500;
    size_t out_len = 0;

    char *result = sg_api_solve(ctx->body, ctx->body_len, &status_code, &out_len);

    if (result) {
        ctx->response_json = result;
        ctx->response_len = out_len;
        ctx->status_code = status_code;
    } else {
        ctx->status_code = 500;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Solver internal error");
    }
}

/* Worker thread: run the solve. Touches only this context. */
static void solve_work_fn(void *user_data) {
    process_solve((SolveCtx *)user_data);
}

/* Event loop thread: write the response and resume the connection. */
static void solve_done_fn(void *user_data) {
    SolveCtx *ctx = (SolveCtx *)user_data;

    sh_metrics_timer_observe(ctx->solve_timer, "surge_solve_duration_ms",
                             "endpoint:solve", NULL);

    /* Connection already gone, or the deadline already sent a 504. */
    if (ctx->detached) {
        solve_ctx_free(ctx);
        return;
    }

    KlHttpResponse *res = kl_http_conn_response(ctx->op.conn);

    if (ctx->status_code >= 200 && ctx->status_code < 300) {
        if (ctx->response_json && ctx->response_len > 0) {
            sh_kl_reply_json(res, ctx->status_code, &s_cors, NULL,
                             ctx->response_json);
        } else {
            sh_kl_reply_json(res, 200, &s_cors, NULL, "{}");
        }
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:200", "endpoint:solve", NULL);
    } else {
        if (ctx->response_json && ctx->response_len > 0) {
            sh_kl_reply_json(res, ctx->status_code, &s_cors, NULL,
                             ctx->response_json);
        } else {
            sh_kl_reply_error(res, ctx->status_code, &s_cors, NULL,
                              ctx->error_msg[0] ? ctx->error_msg
                                                : "Internal error");
        }
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:500", "endpoint:solve", NULL);
    }

    kl_async_complete(ctx->app->server, &ctx->op);
    solve_ctx_free(ctx);
}

/*
 * Pool shutdown dropped the item before it started. No worker will ever touch
 * this context, so it is safe to free here.
 */
static void solve_cancel_fn(void *user_data) {
    solve_ctx_free((SolveCtx *)user_data);
}

/* Connection died while suspended. The worker may still be running. */
static void solve_on_cancel(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    SolveCtx *ctx = (SolveCtx *)((char *)op - offsetof(SolveCtx, op));
    ctx->detached = 1;
}

static void solve_on_resume(KlAsyncOp *op, void *user_data) {
    (void)op; (void)user_data;
}

/*
 * Solve budget expired. Reply 504 and resume the connection now; the worker
 * keeps running and done_fn will free the context without touching it.
 */
static void solve_on_deadline(KlAsyncOp *op, void *user_data) {
    (void)user_data;
    SolveCtx *ctx = (SolveCtx *)((char *)op - offsetof(SolveCtx, op));

    if (ctx->detached) return;
    ctx->detached = 1;

    sh_kl_reply_error(kl_http_conn_response(op->conn), 504, &s_cors, NULL,
                      "Solve timeout");
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:504", "endpoint:solve", NULL);
    SH_LOG_WARN("Solve deadline exceeded", "status", "504");

    kl_async_complete(ctx->app->server, op);
}

/* ============================================================================
 * Middleware
 * ============================================================================ */

/* Rate limit every request before routing. */
static int mw_rate_limit(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;

    /* Extract or generate trace ID for this request. */
    sh_trace_from_headers(sh_kl_trace_header_getter, req);

    if (!sh_kl_check_rate_limit(req, res, s_rate_limiter, &s_cors,
                                sh_kl_origin(req))) {
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
    sh_kl_reply_preflight(res, &s_cors, sh_kl_origin(req));
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
 * JSON+CORS shape the mongoose server used.
 */
static int mw_not_found(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    AppCtx *app = (AppCtx *)ud;
    KlHttpRoute *matched = NULL;
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS];
    int num_params = 0;

    int rc = kl_http_router_match(&app->server->router,
                                  req->method, req->method_len,
                                  req->path, req->path_len,
                                  &matched, params, &num_params);
    if (rc == 200) return 0;  /* a route will handle this */

    if (rc == 405) {
        sh_kl_reply_error(res, 405, &s_cors, sh_kl_origin(req),
                          "Method not allowed");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:405", "endpoint:unknown", NULL);
    } else {
        sh_kl_reply_error(res, 404, &s_cors, sh_kl_origin(req), "Not found");
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
    sh_kl_handle_health(res, &s_cors, sh_kl_origin(req), "surge", sg_version());
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", "endpoint:health", NULL);
    sh_trace_clear();
}

static void handle_version(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    size_t out_len;
    char *json = sg_api_version(&out_len);
    if (json) {
        sh_kl_reply_json(res, 200, &s_cors, sh_kl_origin(req), json);
        free(json);
    } else {
        sh_kl_reply_error(res, 500, &s_cors, sh_kl_origin(req),
                          "Failed to generate version response");
    }
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", "endpoint:version", NULL);
    sh_trace_clear();
}

static void handle_stats(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    SGAPIRequest api_req = { .path = "/api/v1/stats" };
    SGAPIResponse resp = {0};

    if (sg_api_handle(s_api_ctx, &api_req, &resp) == 0 && resp.body) {
        sh_kl_reply_json(res, resp.status_code, &s_cors, sh_kl_origin(req),
                         resp.body);
    } else {
        sh_kl_reply_error(res, 500, &s_cors, sh_kl_origin(req),
                          "Failed to generate stats response");
    }
    sg_api_response_free(&resp);

    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", "endpoint:stats", NULL);
    sh_trace_clear();
}

static void handle_metrics(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    sh_kl_handle_metrics(res);
    sh_trace_clear();
}

static void handle_solve(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    AppCtx *app = (AppCtx *)ud;
    const char *origin = sh_kl_origin(req);

    /* Registered for "*" so a non-POST gets 405 rather than the catch-all 404. */
    if (req->method_len != 4 || memcmp(req->method, "POST", 4) != 0) {
        sh_kl_reply_error(res, 405, &s_cors, origin,
                          "Method not allowed. Use POST.");
        sh_trace_clear();
        return;
    }

    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (!br || br->len == 0) {
        sh_kl_reply_error(res, 400, &s_cors, origin, "Empty request body");
        sh_trace_clear();
        return;
    }

    SolveCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        sh_kl_reply_error(res, 500, &s_cors, origin, "Out of memory");
        sh_trace_clear();
        return;
    }

    /*
     * Copy the body: br->data belongs to the connection and is recycled once
     * the response is sent, but the worker reads it afterwards.
     */
    ctx->body = malloc(br->len + 1);
    if (!ctx->body) {
        free(ctx);
        sh_kl_reply_error(res, 500, &s_cors, origin, "Out of memory");
        sh_trace_clear();
        return;
    }
    memcpy(ctx->body, br->data, br->len);
    ctx->body[br->len] = '\0';
    ctx->body_len = br->len;

    ctx->app = app;
    ctx->status_code = 500;
    ctx->solve_timer = sh_metrics_timer_start();

    ctx->op.on_resume = solve_on_resume;
    ctx->op.on_cancel = solve_on_cancel;
    ctx->op.on_deadline = solve_on_deadline;
    if (s_config.server.work_queue_timeout > 0.0) {
        ctx->op.deadline_ms = kl_monotonic_ms() +
            (uint64_t)(s_config.server.work_queue_timeout * 1000.0);
    }

    if (kl_async_suspend(app->server, kl_http_request_conn(req), &ctx->op) < 0) {
        solve_ctx_free(ctx);
        sh_kl_reply_error(res, 500, &s_cors, origin, "Failed to suspend request");
        sh_trace_clear();
        return;
    }

    KlWorkItem item = {
        .work_fn   = solve_work_fn,
        .done_fn   = solve_done_fn,
        .cancel_fn = solve_cancel_fn,
        .user_data = ctx,
    };

    if (kl_thread_pool_submit(app->pool, &item) < 0) {
        /* Queue full: backpressure, same 503 the old work queue returned. */
        ctx->detached = 1;
        sh_kl_reply_error(res, 503, &s_cors, origin,
                          "Server busy, try again later");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:503", "endpoint:solve", NULL);
        kl_async_complete(app->server, &ctx->op);
        solve_ctx_free(ctx);
    }

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

    AppCtx app = { .server = &server, .pool = pool };

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
    kl_http_server_use(&server, "*", "/*", mw_rate_limit, NULL);
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_not_found, &app);

    SH_LOG_INFO("Server starting", "port", s_config.server.port);
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
