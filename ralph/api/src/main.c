/*
 * Ralph LP/MIP Solver - HTTP Server
 *
 * A simple HTTP wrapper around the transport-agnostic Ralph API.
 * Designed for WASM demos and lightweight deployments.
 *
 * Port: 8084 (default)
 * Endpoints: /api/v1/health, /api/v1/formats, /api/v1/solve
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "mongoose.h"
#include "ralph_api.h"
#include "sh_args.h"

/* Configuration (uses shared library) */
static ShServerConfig s_config;
static volatile sig_atomic_t s_running = 1;

/* Global API context */
static RalphAPIContext *s_ctx = NULL;

/* Signal handler for graceful shutdown */
static void signal_handler(int sig) {
    (void)sig;
    s_running = 0;
}

/* Add CORS headers */
static void add_cors_headers(struct mg_connection *c) {
    mg_printf(c,
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Access-Control-Max-Age: 86400\r\n");
}

/* Extract path from URI (without query string) */
static void extract_path(struct mg_str uri, char *buf, size_t buf_size) {
    size_t len = uri.len;
    const char *q = memchr(uri.buf, '?', uri.len);
    if (q) {
        len = (size_t)(q - uri.buf);
    }
    if (len >= buf_size) {
        len = buf_size - 1;
    }
    memcpy(buf, uri.buf, len);
    buf[len] = '\0';
}

/* Extract query string from URI */
static void extract_query(struct mg_str uri, char *buf, size_t buf_size) {
    const char *q = memchr(uri.buf, '?', uri.len);
    if (q) {
        size_t len = uri.len - (size_t)(q - uri.buf) - 1;
        if (len >= buf_size) {
            len = buf_size - 1;
        }
        memcpy(buf, q + 1, len);
        buf[len] = '\0';
    } else {
        buf[0] = '\0';
    }
}

/* Extract method string */
static const char *method_str(struct mg_str method) {
    if (mg_strcmp(method, mg_str("GET")) == 0) return "GET";
    if (mg_strcmp(method, mg_str("POST")) == 0) return "POST";
    if (mg_strcmp(method, mg_str("OPTIONS")) == 0) return "OPTIONS";
    return "UNKNOWN";
}

/* HTTP event handler */
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;

        /* Handle CORS preflight */
        if (mg_strcmp(hm->method, mg_str("OPTIONS")) == 0) {
            mg_printf(c, "HTTP/1.1 204 No Content\r\n");
            add_cors_headers(c);
            mg_printf(c, "\r\n");
            return;
        }

        /* Extract request info */
        char path[256];
        char query[1024];
        extract_path(hm->uri, path, sizeof(path));
        extract_query(hm->uri, query, sizeof(query));

        /* Build API request */
        RalphAPIRequest req = {
            .method = method_str(hm->method),
            .path = path,
            .query = query[0] ? query : NULL,
            .body = hm->body.len > 0 ? hm->body.buf : NULL,
            .body_len = hm->body.len
        };

        /* Call transport-agnostic handler */
        RalphAPIResponse resp;
        int result = ralph_api_handle(s_ctx, &req, &resp);

        if (result != 0) {
            /* Handler error - shouldn't happen */
            mg_printf(c, "HTTP/1.1 500 Internal Server Error\r\n");
            add_cors_headers(c);
            mg_printf(c, "Content-Type: application/json\r\n\r\n");
            mg_printf(c, "{\"error\":\"Internal server error\"}");
            return;
        }

        /* Send HTTP response */
        mg_printf(c, "HTTP/1.1 %d %s\r\n", resp.status_code,
            resp.status_code == 200 ? "OK" :
            resp.status_code == 400 ? "Bad Request" :
            resp.status_code == 404 ? "Not Found" :
            resp.status_code == 408 ? "Request Timeout" :
            resp.status_code == 413 ? "Payload Too Large" :
            "Error");
        add_cors_headers(c);
        mg_printf(c, "Content-Type: %s\r\n", resp.content_type);
        mg_printf(c, "Content-Length: %lu\r\n\r\n", (unsigned long)resp.body_len);
        mg_send(c, resp.body, resp.body_len);

        /* Clean up */
        ralph_api_response_free(&resp);
    }
}

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

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

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
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    char url[320];  /* http:// + 253 char DNS + :port */
    snprintf(url, sizeof(url), "http://%s:%d", s_config.host, s_config.port);

    struct mg_connection *c = mg_http_listen(&mgr, url, ev_handler, NULL);
    if (!c) {
        fprintf(stderr, "Failed to bind to %s\n", url);
        ralph_api_free(s_ctx);
        return 1;
    }

    printf("Listening on %s\n", url);
    printf("Press Ctrl+C to stop\n\n");

    /* Event loop */
    while (s_running) {
        mg_mgr_poll(&mgr, 100);
    }

    printf("\nShutting down...\n");

    /* Cleanup */
    mg_mgr_free(&mgr);
    ralph_api_free(s_ctx);

    return 0;
}
