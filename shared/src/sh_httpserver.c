/*
 * sh_httpserver.c - Keel-backed HTTP helpers for OTTO API servers
 *
 * See sh_httpserver.h for the contract.
 */

#include "sh_httpserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <keel/http_server.h>
#include <keel/sockaddr.h>

#include "sh_cors.h"
#include "sh_json.h"
#include "sh_metrics.h"
#include "sh_ratelimit.h"

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/*
 * sh_cors_* emits a raw "Name: value\r\n" block, but Keel takes headers as
 * name/value pairs. Split the block and forward each line. Malformed lines
 * (no colon) are skipped rather than passed through, so a bad CORS config can
 * never inject a partial header.
 */
static void sh_http_append_header_block(KlHttpResponse *res, const char *block)
{
    const char *p = block;

    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        const char *colon = memchr(p, ':', line_len);
        if (colon) {
            char name[128];
            char value[512];

            size_t name_len = (size_t)(colon - p);

            /* Skip the colon and any leading spaces in the value. */
            const char *vstart = colon + 1;
            while (vstart < p + line_len && *vstart == ' ') vstart++;
            size_t value_len = (size_t)((p + line_len) - vstart);

            if (name_len > 0 && name_len < sizeof(name) &&
                value_len < sizeof(value)) {
                memcpy(name, p, name_len);
                name[name_len] = '\0';
                memcpy(value, vstart, value_len);
                value[value_len] = '\0';
                kl_http_response_header(res, name, value);
            }
        }

        if (!eol) break;
        p = eol + 2;
    }
}

/* Copy a header value into the caller's buffer as a NUL-terminated string. */
static const char *sh_http_header_dup(const KlHttpRequest *req, const char *name,
                                    char *buf, size_t buflen)
{
    if (!req || !name) return NULL;

    size_t name_len = strlen(name);

    for (int i = 0; i < req->num_headers; i++) {
        if (req->headers[i].name_len == name_len &&
            kl_ascii_strncasecmp(req->headers[i].name, name, name_len) == 0) {
            size_t len = req->headers[i].value_len;
            if (len == 0) return NULL;
            if (len > buflen - 1) len = buflen - 1;
            memcpy(buf, req->headers[i].value, len);
            buf[len] = '\0';
            return buf;
        }
    }
    return NULL;
}

/* ============================================================================
 * Request Helpers
 * ============================================================================ */

const char *sh_http_origin(const KlHttpRequest *req)
{
    static __thread char origin_buf[256];
    return sh_http_header_dup(req, "Origin", origin_buf, sizeof(origin_buf));
}

const char *sh_http_trace_header_getter(const char *name, void *ctx)
{
    static __thread char hdr_buf[128];
    return sh_http_header_dup((const KlHttpRequest *)ctx, name,
                            hdr_buf, sizeof(hdr_buf));
}

/* ============================================================================
 * Response Helpers
 * ============================================================================ */

void sh_http_apply_cors(KlHttpResponse *res, const struct ShCorsConfig *cors,
                      const char *origin)
{
    if (!res) return;

    char cors_hdrs[512] = "";
    if (cors) {
        sh_cors_headers(cors, origin, cors_hdrs, sizeof(cors_hdrs));
    } else {
        snprintf(cors_hdrs, sizeof(cors_hdrs),
                 "Access-Control-Allow-Origin: *\r\n");
    }
    sh_http_append_header_block(res, cors_hdrs);
}

void sh_http_reply_body(KlHttpResponse *res, int status,
                      const char *content_type,
                      const struct ShCorsConfig *cors, const char *origin,
                      const char *body, size_t body_len)
{
    if (!res) return;

    kl_http_response_status(res, status);
    kl_http_response_header(res, "Content-Type",
                            content_type ? content_type : "application/json");
    sh_http_apply_cors(res, cors, origin);

    /* Copy: Keel's response_json/_error borrow, and callers free their
     * buffers. Same reason every other sh_http_reply_* copies. */
    if (body && body_len > 0) {
        kl_http_response_body_copy(res, body, body_len);
    }
}

void sh_http_reply_json(KlHttpResponse *res, int status,
                      const struct ShCorsConfig *cors, const char *origin,
                      const char *json)
{
    if (!res) return;

    if (!json) json = "{}";

    kl_http_response_status(res, status);
    kl_http_response_header(res, "Content-Type", "application/json");
    sh_http_apply_cors(res, cors, origin);

    /* Copy: kl_http_response_json() borrows, and callers free their buffers. */
    kl_http_response_body_copy(res, json, strlen(json));
}

void sh_http_reply_error(KlHttpResponse *res, int status,
                       const struct ShCorsConfig *cors, const char *origin,
                       const char *message)
{
    if (!res) return;

    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "error");
    sh_json_write_string(&jw, message ? message : "Unknown error");
    sh_json_write_object_end(&jw);

    char *json = sh_json_buf_take(&jb);
    if (!json || jw.error) {
        sh_json_buf_free(&jb);
        sh_http_reply_json(res, status, cors, origin,
                         "{\"error\":\"JSON write error\"}");
        free(json);
        return;
    }

    sh_http_reply_json(res, status, cors, origin, json);
    free(json);
}

void sh_http_reply_preflight(KlHttpResponse *res, const struct ShCorsConfig *cors,
                           const char *origin)
{
    if (!res) return;

    char cors_hdrs[512] = "";
    if (cors) {
        sh_cors_preflight_headers(cors, origin, cors_hdrs, sizeof(cors_hdrs));
    } else {
        snprintf(cors_hdrs, sizeof(cors_hdrs),
                 "Access-Control-Allow-Origin: *\r\n");
    }

    kl_http_response_status(res, 204);
    sh_http_append_header_block(res, cors_hdrs);
    kl_http_response_body_borrow(res, "", 0);
}

void sh_http_handle_health(KlHttpResponse *res, const struct ShCorsConfig *cors,
                         const char *origin, const char *service,
                         const char *version)
{
    if (!res) return;

    char body[512];
    snprintf(body, sizeof(body),
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"%s\",\n"
        "  \"version\": \"%s\"\n"
        "}\n",
        service ? service : "unknown",
        version ? version : "0.0.0");

    sh_http_reply_json(res, 200, cors, origin, body);
}

void sh_http_handle_metrics(KlHttpResponse *res)
{
    if (!res) return;

    char *prom = sh_metrics_prometheus_output();
    if (!prom) {
        static const char msg[] = "Failed to generate metrics\n";
        kl_http_response_status(res, 500);
        kl_http_response_header(res, "Content-Type", "text/plain");
        kl_http_response_body_copy(res, msg, sizeof(msg) - 1);
        return;
    }

    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", "text/plain; version=0.0.4");
    kl_http_response_header(res, "Access-Control-Allow-Origin", "*");
    kl_http_response_body_copy(res, prom, strlen(prom));
    free(prom);
}

/* ============================================================================
 * Rate Limiting
 * ============================================================================ */

int sh_http_check_rate_limit(const KlHttpRequest *req, KlHttpResponse *res,
                           struct ShRateLimiter *limiter,
                           const struct ShCorsConfig *cors, const char *origin)
{
    if (!req || !res || !limiter) return 1;  /* No limiter = allow */

    const KlSockAddr *peer = kl_http_request_peer_sockaddr(req);
    if (!peer) return 1;  /* Unknown peer = allow */

    ShRateLimitAddr client_addr;
    if (peer->family == KL_AF_INET6) {
        uint64_t hi, lo;
        memcpy(&hi, &peer->u.ip[0], sizeof(hi));
        memcpy(&lo, &peer->u.ip[8], sizeof(lo));
        sh_ratelimit_addr_ipv6(&client_addr, hi, lo);
    } else if (peer->family == KL_AF_INET) {
        uint32_t ip4;
        memcpy(&ip4, &peer->u.ip[0], sizeof(ip4));
        sh_ratelimit_addr_ipv4(&client_addr, ip4);
    } else {
        return 1;  /* AF_UNIX / unspec: not rate limited by address */
    }

    if (sh_ratelimit_check(limiter, &client_addr)) return 1;

    static const char msg[] = "Rate limit exceeded\n";
    kl_http_response_status(res, 429);
    kl_http_response_header(res, "Content-Type", "text/plain");
    kl_http_response_header(res, "Retry-After", "1");
    sh_http_apply_cors(res, cors, origin);
    kl_http_response_body_copy(res, msg, sizeof(msg) - 1);
    return 0;
}
