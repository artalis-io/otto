/*
 * sh_httpserver.h - Keel-backed HTTP helpers for OTTO API servers
 *
 * CORS, JSON/error replies, health, metrics, rate limiting and trace
 * propagation for Keel-backed API servers.
 *
 * This file is NOT part of libshared.a -- it needs Keel headers, so API
 * servers compile it directly:
 *
 *   $(CC) $(CFLAGS) -I../shared/include -I../vendor/keel/include \
 *         ../shared/src/sh_httpserver.c
 *
 * BODY LIFETIME
 *   kl_http_response_json() and kl_http_response_error() *borrow* their body
 *   (see keel/src/protocols/http/http_response.c). Every sh_http_* reply below
 *   copies instead, via kl_http_response_body_copy(), so callers are free to
 *   free() or stack-scope the JSON they pass in.
 */

#ifndef SH_HTTPSERVER_H
#define SH_HTTPSERVER_H

#include <stddef.h>

#include <keel/http_request.h>
#include <keel/http_response.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ShCorsConfig;
struct ShRateLimiter;

/* ============================================================================
 * Request Helpers
 * ============================================================================ */

/*
 * Extract the Origin header as a NUL-terminated string.
 *
 * Returns a pointer to a thread-local buffer valid until the next call from
 * the same thread, or NULL when the header is absent/empty. Mirrors the
 * lifetime contract of sh_mg_trace_header_getter().
 */
const char *sh_http_origin(const KlHttpRequest *req);

/*
 * Header getter for sh_trace_from_headers(). Pass the KlHttpRequest* as ctx:
 *
 *   sh_trace_from_headers(sh_http_trace_header_getter, req);
 *
 * Returns a thread-local buffer, valid until the next call from this thread.
 */
const char *sh_http_trace_header_getter(const char *name, void *ctx);

/* ============================================================================
 * Response Helpers
 * ============================================================================ */

/*
 * Append the configured CORS headers to res. Falls back to
 * "Access-Control-Allow-Origin: *" when cors is NULL, matching sh_mg_*.
 */
void sh_http_apply_cors(KlHttpResponse *res, const struct ShCorsConfig *cors,
                      const char *origin);

/*
 * Reply with an arbitrary content type and a binary-safe body.
 *
 * The general form: Carta returns PNG and MVT, so a JSON-only helper cannot
 * carry every response. The body is copied; caller keeps ownership.
 */
void sh_http_reply_body(KlHttpResponse *res, int status,
                      const char *content_type,
                      const struct ShCorsConfig *cors, const char *origin,
                      const char *body, size_t body_len);

/* Reply with application/json. The body is copied; caller keeps ownership. */
void sh_http_reply_json(KlHttpResponse *res, int status,
                      const struct ShCorsConfig *cors, const char *origin,
                      const char *json);

/* Reply with {"error": "<message>"} as application/json. */
void sh_http_reply_error(KlHttpResponse *res, int status,
                       const struct ShCorsConfig *cors, const char *origin,
                       const char *message);

/* Reply 204 to a CORS preflight (OPTIONS) request. */
void sh_http_reply_preflight(KlHttpResponse *res, const struct ShCorsConfig *cors,
                           const char *origin);

/* Standard {"status":"healthy", "service":..., "version":...} payload. */
void sh_http_handle_health(KlHttpResponse *res, const struct ShCorsConfig *cors,
                         const char *origin, const char *service,
                         const char *version);

/* Prometheus text exposition of the sh_metrics registry. */
void sh_http_handle_metrics(KlHttpResponse *res);

/* ============================================================================
 * Rate Limiting
 * ============================================================================ */

/*
 * Token-bucket check keyed on the request's peer address.
 *
 * Returns 1 when the request may proceed. Returns 0 when it was rate limited,
 * in which case a 429 response has already been written to res and the caller
 * must return without further writes. A NULL limiter always allows.
 */
int sh_http_check_rate_limit(const KlHttpRequest *req, KlHttpResponse *res,
                           struct ShRateLimiter *limiter,
                           const struct ShCorsConfig *cors, const char *origin);

#ifdef __cplusplus
}
#endif

#endif /* SH_HTTPSERVER_H */
