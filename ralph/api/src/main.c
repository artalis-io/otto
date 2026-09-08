/*
 * Ralph LP/MIP Solver - HTTP Server
 *
 * A thin HTTP wrapper around the transport-agnostic Ralph API, served by
 * Keel (MIT). Designed for WASM demos and lightweight deployments.
 *
 * The cheap GET endpoints (health, formats) run inline on the event loop.
 * /api/v1/solve is CPU-bound (simplex + branch & bound), so it runs on a Keel
 * thread pool with the connection suspended via KlAsyncOp: the event loop
 * keeps serving while a solve is in flight, and the pool's bounded queue gives
 * backpressure -- 503 when full, 504 on deadline. Every endpoint except the
 * instant monitoring GETs is rate limited per IP.
 *
 * ralph_api_handle() owns all routing and status decisions -- including 404
 * for unknown paths and the solve response body.
 *
 * Port: 8084 (default)
 * Endpoints: /api/v1/health, /api/v1/formats, /api/v1/solve
 */

#include <keel/keel.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ralph_api.h"
#include "shared.h"          /* sh_ratelimit, sh_cors, sh_args */
#include "sh_httpserver.h"   /* sh_http_reply_*, sh_http_check_rate_limit */
#include "sh_httpasync.h"    /* sh_http_async_dispatch, ShHttpAsync */

/* Server configuration (from sh_args) */
static ShServerConfig s_config;

/* CORS configuration */
static ShCorsConfig s_cors;

/* Rate limiter instance */
static ShRateLimiter *s_rate_limiter = NULL;

/* API context. Read-only after init; ralph_api_handle() is stateless, so it is
   safe to call from the solve pool's worker threads. */
static RalphAPIContext *s_ctx = NULL;

/*
 * Solve pool. KlThreadPool exposes no statistics of its own; the dispatch
 * protocol still requires a stats slot, so keep one even though no endpoint
 * publishes it here.
 */
static KlThreadPool *s_pool = NULL;
static ShHttpAsyncStats s_qstats;

typedef struct {
    ShHttpAsync async;   /* server, pool, cors, timeout, stats */
} AppCtx;

/*
 * Request body cap. Ralph tops out at 100 vars/constraints, so payloads are
 * small, but MPS is verbose; Keel's own default is 1 MB.
 */
#define RALPH_MAX_BODY_SIZE (4u * 1024u * 1024u)

/* ============================================================================
 * Request marshalling
 * ============================================================================ */

/* Copy a Keel string slice into a NUL-terminated buffer. */
static void slice_to_buf(const char *src, size_t src_len, char *buf,
                         size_t buf_size) {
    size_t len = src_len;
    if (!src) { buf[0] = '\0'; return; }
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, src, len);
    buf[len] = '\0';
}

/*
 * Run ralph_api_handle() inline and write its response with CORS. Used for the
 * instant GET endpoints and the 404/405 fallback -- only /solve, which is
 * CPU-bound, needs the pool. Keeps ralph_api_handle() the single source of
 * truth for status and body.
 */
static void dispatch_inline(KlHttpRequest *req, KlHttpResponse *res) {
    char method[16], path[256], query[1024];
    slice_to_buf(req->method, req->method_len, method, sizeof(method));
    slice_to_buf(req->path, req->path_len, path, sizeof(path));
    slice_to_buf(req->query, req->query_len, query, sizeof(query));

    /* Body is (pointer, length): Keel's buffer reader does not NUL-terminate. */
    const char *body = NULL;
    size_t body_len = 0;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (br && br->len > 0) { body = br->data; body_len = br->len; }

    ShApiRequest api_req = {
        .method = method,
        .path = path,
        .query = query[0] ? query : NULL,
        .body = body,
        .body_len = body_len
    };

    ShApiResponse resp;
    if (ralph_api_handle(s_ctx, &api_req, &resp) != 0) {
        sh_http_reply_error(res, 500, &s_cors, NULL, "Internal server error");
        return;
    }

    /* Binary-safe (ptr, len): ShApiResponse.body carries its own length and is
       not guaranteed NUL-terminated. */
    sh_http_reply_body(res, resp.status_code,
                       resp.content_type ? resp.content_type : "application/json",
                       &s_cors, NULL, (const char *)resp.body, resp.body_len);
    sh_api_response_free(&resp);
}

/* ============================================================================
 * Handlers
 * ============================================================================ */

/* Health and formats: instant, run inline on the event loop. */
static void handle_get(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    dispatch_inline(req, res);
}

/*
 * Solve: CPU-bound, so run on the pool with the connection suspended.
 * sh_http_async_dispatch() copies the body, runs ralph_api_handle() on a
 * worker, and answers 503 when the queue is full or 504 on the deadline.
 */
static void handle_solve(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    AppCtx *app = (AppCtx *)ud;

    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    ShApiRequest api_req;
    memset(&api_req, 0, sizeof(api_req));
    api_req.method   = "POST";
    api_req.path     = "/api/v1/solve";
    api_req.body     = (br && br->len > 0) ? br->data : NULL;
    api_req.body_len = (br && br->len > 0) ? br->len : 0;

    sh_http_async_dispatch(&app->async, req, res, ralph_api_handle, s_ctx, &api_req);
}

/* ============================================================================
 * Middleware
 * ============================================================================ */

/* CORS preflight, before rate limiting. */
static int mw_preflight(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    sh_http_reply_preflight(res, &s_cors, NULL);
    return 1;  /* short-circuit */
}

/*
 * Rate limit everything except the instant monitoring GETs -- a health probe
 * must not consume the request budget. Middleware patterns have no alternation,
 * so the exemptions are checked here.
 */
static int mw_rate_limit(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;

    static const char *exempt[] = { "/api/v1/health", "/api/v1/formats" };
    for (size_t i = 0; i < sizeof(exempt) / sizeof(exempt[0]); i++) {
        size_t len = strlen(exempt[i]);
        if (req->path_len == len && memcmp(req->path, exempt[i], len) == 0)
            return 0;
    }

    if (!sh_http_check_rate_limit(req, res, s_rate_limiter, &s_cors, NULL))
        return 1;  /* 429 already written */
    return 0;
}

/*
 * Anything the route table would not match.
 *
 * Keel route patterns have no wildcard -- '*' is only special in middleware
 * patterns -- so a catch-all route is not expressible. Forwarding to
 * ralph_api_handle() keeps its own {"error":...} 404 body and the CORS headers
 * rather than Keel's built-in text/plain 404.
 */
static int mw_fallback(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    KlHttpServer *server = (KlHttpServer *)ud;
    KlHttpRoute *matched = NULL;
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS];
    int num_params = 0;

    int rc = kl_http_router_match(&server->router, req->method, req->method_len,
                                  req->path, req->path_len,
                                  &matched, params, &num_params);
    if (rc == 200) return 0;  /* a route will handle this */

    dispatch_inline(req, res);
    return 1;  /* short-circuit */
}

/* Body reader factory: kl_http_body_reader_buffer() reads max_size from the
   route's user_data, so wrap it to keep the cap explicit. */
static KlHttpBodyReader *solve_body_reader(KlAllocator *alloc,
                                           const KlHttpRequest *req,
                                           void *user_data) {
    (void)user_data;
    return kl_http_body_reader_buffer(alloc, req,
                                      (void *)(size_t)RALPH_MAX_BODY_SIZE);
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Ralph LP/MIP Solver - HTTP Server\n\n");
    sh_args_usage(prog,
        "\nEndpoints:\n"
        "  GET  /api/v1/health   Health check\n"
        "  GET  /api/v1/formats  Supported formats\n"
        "  POST /api/v1/solve    Solve LP/MIP problem\n");
}

int main(int argc, char *argv[]) {
    /* Initialize config with defaults */
    sh_args_init(&s_config);
    sh_cors_init(&s_cors);

    /* Ralph-specific defaults. Solves are small and bounded (30s max), so a
       modest queue and a deadline just above that ceiling suffice. */
    s_config.port = 8084;
    s_config.rate_limit_rps = 20.0;
    s_config.rate_limit_burst = 40.0;
    s_config.work_queue_depth = 64;
    s_config.work_queue_timeout = 35.0;
    s_config.worker_threads = 0;   /* auto-detect */

    /* Load from environment variables */
    sh_args_load_env(&s_config, SH_API_RALPH);

    /* Parse command line arguments */
    int arg_index = sh_args_parse(&s_config, argc, argv);
    if (arg_index == -2) {   /* --help */
        print_usage(argv[0]);
        return 0;
    }
    if (arg_index < 0) {     /* parse error */
        print_usage(argv[0]);
        return 1;
    }

    /* Create API context */
    s_ctx = ralph_api_create();
    if (!s_ctx) {
        fprintf(stderr, "Failed to create Ralph API context\n");
        return 1;
    }

    printf("Ralph LP/MIP Solver API v%s\n", ralph_api_version());
    printf("Limits: %d vars/constraints (LP), %d (MIP)\n", 100, 50);
    printf("Timeout: 5s default, 30s max\n\n");

    /* Rate limiter */
    if (s_config.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(
            s_config.rate_limit_rps, s_config.rate_limit_burst,
            s_config.rate_limit_buckets > 0 ? s_config.rate_limit_buckets : 4096);
        if (!s_rate_limiter) {
            fprintf(stderr, "Failed to create rate limiter\n");
            ralph_api_free(s_ctx);
            return 1;
        }
    }

    /* Create HTTP server */
    KlHttpServer server;
    KlHttpServerConfig http_cfg = {
        .port = s_config.port,
        .bind_addr = s_config.host,
        .install_signal_handlers = 1,
        .max_body_size = RALPH_MAX_BODY_SIZE,
        .drain_timeout_ms = 5000,
    };

    if (kl_http_server_init(&server, &http_cfg) < 0) {
        fprintf(stderr, "Failed to bind to %s:%d\n", s_config.host,
                s_config.port);
        sh_ratelimit_free(s_rate_limiter);
        ralph_api_free(s_ctx);
        return 1;
    }

    /* Solve pool. queue_capacity gives the backpressure (503 when full). */
    if (s_config.work_queue_enabled) {
        KlThreadPoolConfig pool_cfg = {
            .num_workers = s_config.worker_threads > 0 ? s_config.worker_threads : 0,
            .queue_capacity = (int)s_config.work_queue_depth,
        };
        s_pool = kl_thread_pool_create(kl_http_server_event_ctx(&server), &pool_cfg);
        if (!s_pool) {
            fprintf(stderr, "Failed to create solve thread pool\n");
            kl_http_server_free(&server);
            sh_ratelimit_free(s_rate_limiter);
            ralph_api_free(s_ctx);
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
     * Routes. Health/formats are instant GETs; solve is CPU-bound and carries
     * the app context so it can reach the pool.
     */
    kl_http_server_route(&server, "GET",  "/api/v1/health",  handle_get,   NULL, NULL);
    kl_http_server_route(&server, "GET",  "/api/v1/formats", handle_get,   NULL, NULL);
    kl_http_server_route(&server, "POST", "/api/v1/solve",   handle_solve, &app,
                         solve_body_reader);

    /*
     * Middleware runs in registration order, before routing. Preflight first
     * (it must not be rate limited), then the limiter, then the fallback, which
     * must be last because it short-circuits unmatched requests.
     */
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_rate_limit, NULL);
    kl_http_server_use(&server, "*", "/*", mw_fallback, &server);

    printf("Listening on http://%s:%d\n", s_config.host, s_config.port);
    printf("Rate limiting: %s", s_config.rate_limit_enabled ? "enabled" : "disabled");
    if (s_config.rate_limit_enabled) {
        printf(" (%.1f RPS, burst %.0f)", s_config.rate_limit_rps,
               s_config.rate_limit_burst);
    }
    printf("\n");
    printf("Solve queue: %s", s_pool ? "enabled" : "disabled");
    if (s_pool) {
        printf(" (depth %zu, timeout %.1fs)", s_config.work_queue_depth,
               s_config.work_queue_timeout);
    }
    printf("\nPress Ctrl+C to stop\n\n");
    fflush(stdout);

    /* Event loop: blocks until SIGINT/SIGTERM. */
    kl_http_server_run(&server);

    printf("\nShutting down...\n");

    /* Pool first: drains in-flight solves, fires cancel_fn for queued items. */
    if (s_pool) kl_thread_pool_free(s_pool);
    kl_http_server_free(&server);
    sh_ratelimit_free(s_rate_limiter);
    ralph_api_free(s_ctx);

    return 0;
}
