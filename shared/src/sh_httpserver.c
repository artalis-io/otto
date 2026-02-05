/*
 * sh_httpserver.c - Multi-threaded HTTP Server Implementation
 *
 * This provides Mongoose helper functions and the multi-threaded server
 * abstraction for OTTO API servers.
 */

/* On macOS, enable Darwin extensions for signal.h compatibility with mongoose */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "sh_httpserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/tcp.h>
#endif

/* Include mongoose - assumes it's available via vendor path */
#include "mongoose.h"

/* ============================================================================
 * Configuration Defaults
 * ============================================================================ */

void sh_httpserver_config_init(ShHttpServerConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 8080;
    cfg->host = "0.0.0.0";
    cfg->num_threads = 0;  /* Auto-detect */
    cfg->socket_timeout_ms = 5000;
}

/* ============================================================================
 * Socket Write Timeout
 * ============================================================================ */

void sh_mg_set_write_timeout(struct mg_connection *c, int timeout_ms)
{
    if (!c) return;

#ifdef _WIN32
    /* Windows uses DWORD milliseconds */
    DWORD tv = (DWORD)timeout_ms;
    setsockopt((SOCKET)(size_t)c->fd, SOL_SOCKET, SO_SNDTIMEO,
               (const char *)&tv, sizeof(tv));
#else
    /* POSIX uses struct timeval */
    /* Note: c->fd is void* in mongoose 7.x but holds the socket fd */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt((int)(intptr_t)c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

/* ============================================================================
 * Chunked Transfer Encoding (Low-level Mongoose helpers)
 * ============================================================================ */

void sh_mg_chunked_begin(struct mg_connection *c, int status,
                         const char *content_type, const char *extra_hdrs)
{
    if (!c) return;

    mg_printf(c,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "%s"
        "\r\n",
        status, status == 200 ? "OK" : "Error",
        content_type ? content_type : "application/octet-stream",
        extra_hdrs ? extra_hdrs : "");
}

void sh_mg_chunked_send(struct mg_connection *c, const void *data, size_t len)
{
    if (!c || !data || len == 0) return;

    /* Send chunk size in hex, then data, then CRLF */
    mg_printf(c, "%lx\r\n", (unsigned long)len);
    mg_send(c, data, len);
    mg_send(c, "\r\n", 2);
}

void sh_mg_chunked_end(struct mg_connection *c)
{
    if (!c) return;

    /* Final chunk: "0\r\n\r\n" */
    mg_send(c, "0\r\n\r\n", 5);
}

/* ============================================================================
 * Request/Response Structures (for full server implementation)
 * ============================================================================ */

struct ShHttpRequest {
    struct mg_connection *conn;
    struct mg_http_message *hm;
    char path_buf[512];
    char query_buf[512];
    char method_buf[16];
};

struct ShHttpResponse {
    struct mg_connection *conn;
    int status;
    char headers[2048];
    size_t headers_len;
    uint8_t *body;
    size_t body_len;
    int sent;           /* 1 if response already sent */
    int chunked;        /* 1 if using chunked encoding */
};

/* ============================================================================
 * Request API Implementation
 * ============================================================================ */

const char *sh_http_req_method(const ShHttpRequest *req)
{
    return req ? req->method_buf : "";
}

const char *sh_http_req_path(const ShHttpRequest *req)
{
    return req ? req->path_buf : "";
}

const char *sh_http_req_query(const ShHttpRequest *req)
{
    return req ? req->query_buf : "";
}

int sh_http_req_query_param(const ShHttpRequest *req, const char *name,
                            char *buf, size_t len)
{
    if (!req || !name || !buf || len == 0) return -1;

    struct mg_str val = mg_http_var(req->hm->query, mg_str(name));
    if (val.buf == NULL) return -1;

    size_t copy_len = val.len < len - 1 ? val.len : len - 1;
    memcpy(buf, val.buf, copy_len);
    buf[copy_len] = '\0';
    return (int)copy_len;
}

const char *sh_http_req_header(const ShHttpRequest *req, const char *name)
{
    if (!req || !name || !req->hm) return NULL;

    struct mg_str *hdr = mg_http_get_header(req->hm, name);
    if (!hdr || !hdr->buf) return NULL;

    /* Return pointer into message buffer (valid until request completes) */
    return hdr->buf;
}

const void *sh_http_req_body(const ShHttpRequest *req, size_t *len)
{
    if (!req || !req->hm) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = req->hm->body.len;
    return req->hm->body.buf;
}

char *sh_http_req_client_ip(const ShHttpRequest *req, char *buf, size_t len)
{
    if (!req || !buf || len < 16) return NULL;

    /* Format IP address from mg_connection (mongoose 7.x uses addr union) */
    if (req->conn->rem.is_ip6) {
        snprintf(buf, len, "[IPv6]");  /* TODO: Format IPv6 properly */
    } else {
        uint8_t *ip = (uint8_t *)&req->conn->rem.addr.ip4;
        snprintf(buf, len, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    }
    return buf;
}

struct mg_connection *sh_http_req_connection(const ShHttpRequest *req)
{
    return req ? req->conn : NULL;
}

/* ============================================================================
 * Response API Implementation
 * ============================================================================ */

void sh_http_res_status(ShHttpResponse *res, int status)
{
    if (res) res->status = status;
}

void sh_http_res_header(ShHttpResponse *res, const char *name, const char *value)
{
    if (!res || !name || !value) return;
    if (res->sent) return;  /* Too late */

    size_t avail = sizeof(res->headers) - res->headers_len;
    int written = snprintf(res->headers + res->headers_len, avail,
                           "%s: %s\r\n", name, value);
    if (written > 0 && (size_t)written < avail) {
        res->headers_len += written;
    }
}

void sh_http_res_cors(ShHttpResponse *res)
{
    sh_http_res_header(res, "Access-Control-Allow-Origin", "*");
    sh_http_res_header(res, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    sh_http_res_header(res, "Access-Control-Allow-Headers", "Content-Type");
}

void sh_http_res_body(ShHttpResponse *res, const void *data, size_t len)
{
    if (!res || res->sent) return;

    free(res->body);
    res->body = NULL;
    res->body_len = 0;

    if (data && len > 0) {
        res->body = malloc(len);
        if (res->body) {
            memcpy(res->body, data, len);
            res->body_len = len;
        }
    }
}

void sh_http_res_body_str(ShHttpResponse *res, const char *str)
{
    if (str) {
        sh_http_res_body(res, str, strlen(str));
    }
}

void sh_http_res_json(ShHttpResponse *res, const char *json)
{
    sh_http_res_header(res, "Content-Type", "application/json");
    sh_http_res_body_str(res, json);
}

void sh_http_res_error(ShHttpResponse *res, int status, const char *message)
{
    sh_http_res_status(res, status);
    sh_http_res_header(res, "Content-Type", "text/plain");
    sh_http_res_body_str(res, message ? message : "Error");
}

/* ============================================================================
 * Chunked Response API Implementation
 * ============================================================================ */

void sh_http_res_chunked_begin(ShHttpResponse *res, int status,
                                const char *content_type)
{
    if (!res || res->sent) return;

    res->chunked = 1;
    res->sent = 1;
    sh_mg_chunked_begin(res->conn, status, content_type, res->headers);
}

void sh_http_res_chunked_send(ShHttpResponse *res, const void *data, size_t len)
{
    if (!res || !res->chunked) return;
    sh_mg_chunked_send(res->conn, data, len);
}

void sh_http_res_chunked_end(ShHttpResponse *res)
{
    if (!res || !res->chunked) return;
    sh_mg_chunked_end(res->conn);
}

/* ============================================================================
 * Server Implementation (Stub - full implementation for later)
 *
 * The full multi-threaded server implementation will be added when migrating
 * Velo/FuelWise to use this abstraction. For now, the low-level helpers
 * (sh_mg_set_write_timeout, sh_mg_chunked_*) can be used directly with
 * existing Mongoose setups.
 * ============================================================================ */

/*
 * Internal server structure (placeholder for full implementation)
 */
struct ShHttpServer {
    ShHttpServerConfig cfg;
    volatile int running;
    /* TODO: Thread pool, route table, etc. */
};

ShHttpServer *sh_httpserver_create(const ShHttpServerConfig *cfg)
{
    if (!cfg) return NULL;

    ShHttpServer *srv = calloc(1, sizeof(ShHttpServer));
    if (!srv) return NULL;

    srv->cfg = *cfg;

    /* Auto-detect thread count */
    if (srv->cfg.num_threads <= 0) {
#ifdef _SC_NPROCESSORS_ONLN
        srv->cfg.num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
#else
        srv->cfg.num_threads = 4;  /* Fallback */
#endif
    }

    return srv;
}

void sh_httpserver_route(ShHttpServer *srv, const char *pattern,
                         ShHttpHandler handler)
{
    /* TODO: Store routes in a table */
    (void)srv;
    (void)pattern;
    (void)handler;
}

int sh_httpserver_run(ShHttpServer *srv)
{
    if (!srv) return -1;

    /* TODO: Full multi-threaded implementation
     *
     * 1. Create num_threads worker threads
     * 2. Each thread:
     *    - mg_mgr_init()
     *    - mg_http_listen() with SO_REUSEPORT
     *    - Event loop with mg_mgr_poll()
     * 3. On connection: sh_mg_set_write_timeout()
     * 4. Route requests to handlers
     *
     * For now, this is a stub. Use the individual components directly:
     * - sh_mg_set_write_timeout() for socket timeouts
     * - sh_mg_chunked_*() for streaming
     * - sh_workqueue for compute offload
     */

    fprintf(stderr, "sh_httpserver_run: Full implementation pending.\n");
    fprintf(stderr, "Use sh_mg_* helpers directly with existing Mongoose setup.\n");
    return -1;
}

void sh_httpserver_stop(ShHttpServer *srv)
{
    if (srv) srv->running = 0;
}

void sh_httpserver_free(ShHttpServer *srv)
{
    if (!srv) return;
    free(srv);
}
