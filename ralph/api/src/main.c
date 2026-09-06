/*
 * Ralph LP/MIP Solver - HTTP Server
 *
 * A simple HTTP wrapper around the transport-agnostic Ralph API.
 * Designed for WASM demos and lightweight deployments.
 *
 * Served by Keel (MIT). The transport layer stays thin: every request is
 * marshalled into a RalphAPIRequest and handed to ralph_api_handle(), which
 * owns all routing and status decisions -- including 404 for unknown paths.
 *
 * Port: 8084 (default)
 * Endpoints: /api/v1/health, /api/v1/formats, /api/v1/solve
 */

#include <keel/keel.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ralph_api.h"
#include "sh_args.h"

/* Configuration (uses shared library) */
static ShServerConfig s_config;

/* Global API context */
static RalphAPIContext *s_ctx = NULL;

/*
 * Request body cap. Ralph tops out at 100 vars/constraints, so payloads are
 * small, but MPS is verbose; Keel's own default is 1 MB.
 */
#define RALPH_MAX_BODY_SIZE (4u * 1024u * 1024u)

/* ============================================================================
 * Response Helpers
 * ============================================================================ */

/* Same four headers the mongoose server emitted on every response. */
static void add_cors_headers(KlHttpResponse *res) {
    kl_http_response_header(res, "Access-Control-Allow-Origin", "*");
    kl_http_response_header(res, "Access-Control-Allow-Methods",
                            "GET, POST, OPTIONS");
    kl_http_response_header(res, "Access-Control-Allow-Headers", "Content-Type");
    kl_http_response_header(res, "Access-Control-Max-Age", "86400");
}

/* Copy a Keel string slice into a NUL-terminated buffer. */
static void slice_to_buf(const char *src, size_t src_len, char *buf,
                         size_t buf_size) {
    size_t len = src_len;
    if (!src) { buf[0] = '\0'; return; }
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, src, len);
    buf[len] = '\0';
}

/* ============================================================================
 * Dispatch
 * ============================================================================ */

/*
 * Marshal a Keel request into a RalphAPIRequest and write back whatever
 * ralph_api_handle() decides. Used by every route and by the catch-all
 * middleware, so unknown paths and wrong methods get Ralph's own 404 body
 * rather than a transport-invented one.
 */
static void dispatch(KlHttpRequest *req, KlHttpResponse *res) {
    char method[16];
    char path[256];
    char query[1024];

    slice_to_buf(req->method, req->method_len, method, sizeof(method));
    slice_to_buf(req->path, req->path_len, path, sizeof(path));
    slice_to_buf(req->query, req->query_len, query, sizeof(query));

    /*
     * Body is passed as (pointer, length) exactly as the mongoose server did:
     * neither mongoose's slice nor Keel's buffer reader NUL-terminates it.
     */
    const char *body = NULL;
    size_t body_len = 0;
    KlHttpBufReader *br = (KlHttpBufReader *)req->body_reader;
    if (br && br->len > 0) {
        body = br->data;
        body_len = br->len;
    }

    RalphAPIRequest api_req = {
        .method = method,
        .path = path,
        .query = query[0] ? query : NULL,
        .body = body,
        .body_len = body_len
    };

    RalphAPIResponse resp;
    if (ralph_api_handle(s_ctx, &api_req, &resp) != 0) {
        /* Handler error - shouldn't happen */
        static const char err[] = "{\"error\":\"Internal server error\"}";
        kl_http_response_status(res, 500);
        add_cors_headers(res);
        kl_http_response_header(res, "Content-Type", "application/json");
        kl_http_response_body_copy(res, err, sizeof(err) - 1);
        return;
    }

    kl_http_response_status(res, resp.status_code);
    add_cors_headers(res);
    kl_http_response_header(res, "Content-Type",
                            resp.content_type ? resp.content_type
                                              : "application/json");
    /* Copy: resp.body is freed below, and Keel's body setters borrow. */
    kl_http_response_body_copy(res, (const char *)resp.body, resp.body_len);

    ralph_api_response_free(&resp);
}

/* ============================================================================
 * Handlers and Middleware
 * ============================================================================ */

static void handle_api(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    dispatch(req, res);
}

/* CORS preflight: 204 + headers, same as before. */
static int mw_preflight(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    kl_http_response_status(res, 204);
    add_cors_headers(res);
    kl_http_response_body_borrow(res, "", 0);
    return 1;  /* short-circuit */
}

/*
 * Anything the route table would not match.
 *
 * Keel route patterns have no wildcard -- '*' is only special in middleware
 * patterns -- so a catch-all route is not expressible. Without this, unmatched
 * paths would hit Keel's built-in text/plain 404, losing both the CORS headers
 * and Ralph's own {"error":"Endpoint not found"} body. Forwarding to dispatch()
 * keeps Ralph the single source of truth for status and body, exactly as when
 * mongoose handed it every request.
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

    dispatch(req, res);
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
    s_config.port = 8084;  /* Ralph default port */

    /* Load from environment variables */
    sh_args_load_env(&s_config, SH_API_RALPH);

    /* Parse command line arguments */
    int arg_index = sh_args_parse(&s_config, argc, argv);
    if (arg_index == -2) {
        /* --help was requested */
        print_usage(argv[0]);
        return 0;
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
        ralph_api_free(s_ctx);
        return 1;
    }

    /* Routes. All three forward to Ralph's own dispatcher. */
    kl_http_server_route(&server, "GET",  "/api/v1/health",  handle_api, NULL, NULL);
    kl_http_server_route(&server, "GET",  "/api/v1/formats", handle_api, NULL, NULL);
    kl_http_server_route(&server, "POST", "/api/v1/solve",   handle_api, NULL,
                         solve_body_reader);

    /*
     * Middleware runs in registration order, before routing. mw_fallback must
     * come last: it short-circuits anything the route table would not match.
     */
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_fallback, &server);

    printf("Listening on http://%s:%d\n", s_config.host, s_config.port);
    printf("Press Ctrl+C to stop\n\n");

    /* Event loop: blocks until SIGINT/SIGTERM. */
    kl_http_server_run(&server);

    printf("\nShutting down...\n");

    /* Cleanup */
    kl_http_server_free(&server);
    ralph_api_free(s_ctx);

    return 0;
}
