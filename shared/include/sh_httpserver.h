/*
 * sh_httpserver.h - Multi-threaded HTTP Server Abstraction
 *
 * Wraps Mongoose with production-ready defaults:
 * - Multiple event loops (one per CPU core, uses SO_REUSEPORT)
 * - Socket write timeout (prevents slow client DoS)
 * - Integration with sh_workqueue for compute offload
 * - Integration with sh_ratelimit for per-IP throttling
 * - CORS header handling
 *
 * Usage:
 *   ShHttpServerConfig cfg = {
 *       .port = 8081,
 *       .num_threads = 0,  // Auto-detect CPU count
 *       .socket_timeout_ms = 5000,
 *   };
 *   ShHttpServer *srv = sh_httpserver_create(&cfg);
 *
 *   sh_httpserver_route(srv, "/api/tiles/[*]", handle_tiles);
 *   sh_httpserver_route(srv, "/api/health", handle_health);
 *
 *   sh_httpserver_run(srv);  // Blocking until sh_httpserver_stop()
 *   sh_httpserver_free(srv);
 *
 * Handler example:
 *   void handle_tiles(ShHttpRequest *req, ShHttpResponse *res) {
 *       // Parse request
 *       const char *path = sh_http_req_path(req);
 *
 *       // Generate response
 *       uint8_t *data = render_tile(...);
 *       sh_http_res_status(res, 200);
 *       sh_http_res_header(res, "Content-Type", "image/png");
 *       sh_http_res_body(res, data, len);
 *   }
 */

#ifndef SH_HTTPSERVER_H
#define SH_HTTPSERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct ShHttpServer ShHttpServer;
typedef struct ShHttpRequest ShHttpRequest;
typedef struct ShHttpResponse ShHttpResponse;
struct ShRateLimiter;  /* From sh_ratelimit.h */
struct ShWorkQueue;    /* From sh_workqueue.h */
struct mg_connection;  /* From mongoose.h */

/* ============================================================================
 * Configuration
 * ============================================================================ */

/*
 * Server configuration.
 */
typedef struct {
    /* Network */
    int port;                  /* Listen port (default: 8080) */
    const char *host;          /* Bind address (default: "0.0.0.0") */

    /* Threading */
    int num_threads;           /* HTTP worker threads (0 = auto-detect CPU) */

    /* Timeouts */
    int socket_timeout_ms;     /* Socket write timeout in ms (default: 5000) */
                               /* Protects against slow client DoS */

    /* Optional integrations (can be NULL) */
    struct ShRateLimiter *rate_limiter;  /* Per-IP rate limiting */
    struct ShWorkQueue *work_queue;      /* Compute offload queue */

    /* Static files */
    const char *static_dir;    /* Directory for static files (NULL = disabled) */

    /* User context passed to all handlers */
    void *user_ctx;
} ShHttpServerConfig;

/*
 * Initialize config with defaults.
 */
void sh_httpserver_config_init(ShHttpServerConfig *cfg);

/* ============================================================================
 * Server Lifecycle
 * ============================================================================ */

/*
 * Create HTTP server.
 *
 * @param cfg Configuration (copied, can be stack-allocated)
 * @return Server instance, or NULL on error
 */
ShHttpServer *sh_httpserver_create(const ShHttpServerConfig *cfg);

/*
 * Request handler callback.
 *
 * @param req  Request object (read-only)
 * @param res  Response builder (write to this)
 * @param ctx  User context from config
 */
typedef void (*ShHttpHandler)(ShHttpRequest *req, ShHttpResponse *res, void *ctx);

/*
 * Register a route handler.
 * Patterns support wildcards: "/api/[*]", "/tiles/{z}/{x}/{y}.png"
 *
 * @param srv     Server instance
 * @param pattern URL pattern to match
 * @param handler Handler function
 */
void sh_httpserver_route(ShHttpServer *srv, const char *pattern, ShHttpHandler handler);

/*
 * Start the server (blocking).
 * Returns when sh_httpserver_stop() is called or on error.
 *
 * @param srv Server instance
 * @return 0 on clean shutdown, -1 on error
 */
int sh_httpserver_run(ShHttpServer *srv);

/*
 * Stop the server.
 * Can be called from signal handler or another thread.
 *
 * @param srv Server instance
 */
void sh_httpserver_stop(ShHttpServer *srv);

/*
 * Free server resources.
 * Call sh_httpserver_stop() first if server is running.
 *
 * @param srv Server instance (may be NULL)
 */
void sh_httpserver_free(ShHttpServer *srv);

/* ============================================================================
 * Request API
 * ============================================================================ */

/*
 * Get request method.
 * @return "GET", "POST", etc.
 */
const char *sh_http_req_method(const ShHttpRequest *req);

/*
 * Get request path (without query string).
 * @return Path string (e.g., "/api/tiles/14/8934/5678.png")
 */
const char *sh_http_req_path(const ShHttpRequest *req);

/*
 * Get query string (without leading '?').
 * @return Query string or empty string if none
 */
const char *sh_http_req_query(const ShHttpRequest *req);

/*
 * Get a query parameter value.
 *
 * @param req   Request
 * @param name  Parameter name
 * @param buf   Buffer to store value
 * @param len   Buffer size
 * @return Length written, or -1 if not found
 */
int sh_http_req_query_param(const ShHttpRequest *req, const char *name,
                            char *buf, size_t len);

/*
 * Get request header value.
 *
 * @param req  Request
 * @param name Header name (case-insensitive)
 * @return Header value or NULL if not present
 */
const char *sh_http_req_header(const ShHttpRequest *req, const char *name);

/*
 * Get request body.
 *
 * @param req  Request
 * @param len  Output: body length
 * @return Body data (not null-terminated)
 */
const void *sh_http_req_body(const ShHttpRequest *req, size_t *len);

/*
 * Get client IP address as string.
 *
 * @param req Request
 * @param buf Buffer for IP string
 * @param len Buffer size (at least 46 for IPv6)
 * @return buf on success, NULL on error
 */
char *sh_http_req_client_ip(const ShHttpRequest *req, char *buf, size_t len);

/*
 * Get underlying Mongoose connection.
 * Use for advanced operations (streaming, websockets, etc.)
 *
 * @param req Request
 * @return Mongoose connection
 */
struct mg_connection *sh_http_req_connection(const ShHttpRequest *req);

/* ============================================================================
 * Response API
 * ============================================================================ */

/*
 * Set response status code.
 * @param res    Response
 * @param status HTTP status code (200, 404, etc.)
 */
void sh_http_res_status(ShHttpResponse *res, int status);

/*
 * Add response header.
 * @param res   Response
 * @param name  Header name
 * @param value Header value
 */
void sh_http_res_header(ShHttpResponse *res, const char *name, const char *value);

/*
 * Add standard CORS headers.
 * Uses the configured CORS policy or defaults to allow all origins.
 *
 * @param res Response
 */
void sh_http_res_cors(ShHttpResponse *res);

/*
 * Set response body (copies data).
 *
 * @param res  Response
 * @param data Body data
 * @param len  Data length
 */
void sh_http_res_body(ShHttpResponse *res, const void *data, size_t len);

/*
 * Set response body from null-terminated string.
 *
 * @param res Response
 * @param str Body string
 */
void sh_http_res_body_str(ShHttpResponse *res, const char *str);

/*
 * Send response with JSON body.
 * Automatically sets Content-Type: application/json
 *
 * @param res  Response
 * @param json JSON string
 */
void sh_http_res_json(ShHttpResponse *res, const char *json);

/*
 * Send error response.
 *
 * @param res     Response
 * @param status  HTTP status code
 * @param message Error message
 */
void sh_http_res_error(ShHttpResponse *res, int status, const char *message);

/* ============================================================================
 * Chunked Transfer Encoding
 * ============================================================================ */

/*
 * Begin chunked response.
 * Use for streaming large responses without full buffering.
 *
 * @param res          Response
 * @param status       HTTP status code
 * @param content_type Content-Type header value
 */
void sh_http_res_chunked_begin(ShHttpResponse *res, int status,
                                const char *content_type);

/*
 * Send a chunk of data.
 * Can be called multiple times after sh_http_res_chunked_begin().
 *
 * @param res  Response
 * @param data Chunk data
 * @param len  Chunk length
 */
void sh_http_res_chunked_send(ShHttpResponse *res, const void *data, size_t len);

/*
 * End chunked response.
 * Must be called after all chunks are sent.
 *
 * @param res Response
 */
void sh_http_res_chunked_end(ShHttpResponse *res);

/* ============================================================================
 * Mongoose Connection Helpers
 * ============================================================================ */

/*
 * Set socket write timeout on a Mongoose connection.
 * Call this after accepting a connection to protect against slow clients.
 *
 * @param c          Mongoose connection
 * @param timeout_ms Write timeout in milliseconds
 */
void sh_mg_set_write_timeout(struct mg_connection *c, int timeout_ms);

/* ============================================================================
 * Mongoose Direct Response Helpers
 *
 * These functions work directly with mg_connection for API servers
 * that don't use the full ShHttpServer abstraction.
 * ============================================================================ */

struct ShCorsConfig;  /* From sh_cors.h */

/*
 * Send JSON response with CORS headers.
 *
 * @param c       Mongoose connection
 * @param status  HTTP status code
 * @param cors    CORS config (may be NULL for default CORS)
 * @param origin  Origin header from request (may be NULL)
 * @param json    JSON string to send
 */
void sh_mg_reply_json(struct mg_connection *c, int status,
                      const struct ShCorsConfig *cors, const char *origin,
                      const char *json);

/*
 * Send error response with CORS headers.
 * Response format: {"error": "message"}
 *
 * @param c       Mongoose connection
 * @param status  HTTP status code
 * @param cors    CORS config (may be NULL for default CORS)
 * @param origin  Origin header from request (may be NULL)
 * @param message Error message
 */
void sh_mg_reply_error(struct mg_connection *c, int status,
                       const struct ShCorsConfig *cors, const char *origin,
                       const char *message);

/*
 * Handle /metrics endpoint (Prometheus format).
 * Uses sh_metrics_prometheus_output() internally.
 *
 * @param c Mongoose connection
 */
void sh_mg_handle_metrics(struct mg_connection *c);

/*
 * Handle /health endpoint with standard JSON format.
 * Response: {"status": "healthy", "service": "...", "version": "..."}
 *
 * @param c       Mongoose connection
 * @param cors    CORS config (may be NULL for default CORS)
 * @param origin  Origin header from request (may be NULL)
 * @param service Service name (e.g., "carta", "velo")
 * @param version Version string
 */
void sh_mg_handle_health(struct mg_connection *c,
                         const struct ShCorsConfig *cors, const char *origin,
                         const char *service, const char *version);

/*
 * Check rate limit and send 429 if exceeded.
 * Handles both IPv4 and IPv6 addresses.
 *
 * @param c         Mongoose connection
 * @param limiter   Rate limiter instance
 * @param cors      CORS config (may be NULL)
 * @param origin    Origin header from request (may be NULL)
 * @return 1 if request allowed, 0 if rate limited (429 already sent)
 */
int sh_mg_check_rate_limit(struct mg_connection *c,
                           struct ShRateLimiter *limiter,
                           const struct ShCorsConfig *cors, const char *origin);

/*
 * Send chunked response start.
 * Low-level helper for direct Mongoose usage.
 *
 * @param c            Mongoose connection
 * @param status       HTTP status code
 * @param content_type Content-Type header
 * @param extra_hdrs   Additional headers (may be NULL)
 */
void sh_mg_chunked_begin(struct mg_connection *c, int status,
                         const char *content_type, const char *extra_hdrs);

/*
 * Send a chunk.
 *
 * @param c    Mongoose connection
 * @param data Chunk data
 * @param len  Chunk length
 */
void sh_mg_chunked_send(struct mg_connection *c, const void *data, size_t len);

/*
 * End chunked response.
 *
 * @param c Mongoose connection
 */
void sh_mg_chunked_end(struct mg_connection *c);

#ifdef __cplusplus
}
#endif

#endif /* SH_HTTPSERVER_H */
