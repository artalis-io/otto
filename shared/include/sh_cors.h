/*
 * sh_cors.h - CORS Header Utilities
 *
 * Server-side utilities for generating CORS (Cross-Origin Resource Sharing)
 * headers. Use with Keel or other HTTP servers.
 *
 * Usage:
 *   ShCorsConfig cors;
 *   sh_cors_init(&cors);
 *   sh_cors_add_origin(&cors, "https://app.example.com");
 *
 *   // For regular requests
 *   if (sh_cors_is_allowed(&cors, origin_header)) {
 *       char headers[512];
 *       sh_cors_headers(&cors, origin_header, headers, sizeof(headers));
 *       // Add headers to response
 *   }
 *
 *   // For preflight (OPTIONS) requests
 *   char headers[512];
 *   sh_cors_preflight_headers(&cors, origin_header, headers, sizeof(headers));
 *   // Return 204 No Content with headers
 */

#ifndef SH_CORS_H
#define SH_CORS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

#define SH_CORS_MAX_ORIGINS 16
#define SH_CORS_ORIGIN_SIZE 256

/*
 * CORS configuration.
 */
typedef struct ShCorsConfig {
    char allowed_origins[SH_CORS_MAX_ORIGINS][SH_CORS_ORIGIN_SIZE];
    int origin_count;           /* 0 = allow all (*) */
    char allowed_methods[128];  /* Default: "GET, POST, OPTIONS" */
    char allowed_headers[256];  /* Default: "Content-Type, Authorization" */
    int allow_credentials;      /* 1 = include credentials header */
    int max_age_seconds;        /* Preflight cache time (default: 86400) */
} ShCorsConfig;

/*
 * Default configuration: allow all origins, standard methods/headers
 */
#define SH_CORS_DEFAULT_CONFIG { \
    .origin_count = 0, \
    .allowed_methods = "GET, POST, OPTIONS", \
    .allowed_headers = "Content-Type, Authorization", \
    .allow_credentials = 0, \
    .max_age_seconds = 86400 \
}

/* ============================================================================
 * CORS API
 * ============================================================================ */

/*
 * Initialize CORS config with defaults.
 * Allows all origins (*) by default.
 *
 * @param config Config to initialize
 */
void sh_cors_init(ShCorsConfig *config);

/*
 * Add an allowed origin.
 * If no origins are added, all origins are allowed (*).
 * Once at least one origin is added, only those origins are allowed.
 *
 * @param config Config to modify
 * @param origin Origin to allow (e.g., "https://example.com")
 * @return 1 on success, 0 if full or invalid
 */
int sh_cors_add_origin(ShCorsConfig *config, const char *origin);

/*
 * Set allowed methods.
 *
 * @param config Config to modify
 * @param methods Comma-separated methods (e.g., "GET, POST, PUT, DELETE")
 */
void sh_cors_set_methods(ShCorsConfig *config, const char *methods);

/*
 * Set allowed headers.
 *
 * @param config Config to modify
 * @param headers Comma-separated headers (e.g., "Content-Type, X-Custom-Header")
 */
void sh_cors_set_headers(ShCorsConfig *config, const char *headers);

/*
 * Check if an origin is allowed.
 *
 * @param config CORS config
 * @param origin Origin header value from request
 * @return 1 if allowed, 0 if not
 */
int sh_cors_is_allowed(const ShCorsConfig *config, const char *origin);

/*
 * Generate CORS headers for a regular response.
 *
 * Generates:
 *   Access-Control-Allow-Origin: <origin or *>
 *   Access-Control-Allow-Credentials: true (if enabled)
 *
 * @param config CORS config
 * @param origin Origin header from request
 * @param buf    Output buffer
 * @param size   Buffer size
 * @return Number of bytes written (excluding null), or -1 on error
 */
int sh_cors_headers(const ShCorsConfig *config, const char *origin, char *buf, size_t size);

/*
 * Generate CORS headers for a preflight (OPTIONS) response.
 *
 * Generates:
 *   Access-Control-Allow-Origin: <origin or *>
 *   Access-Control-Allow-Methods: <methods>
 *   Access-Control-Allow-Headers: <headers>
 *   Access-Control-Max-Age: <seconds>
 *   Access-Control-Allow-Credentials: true (if enabled)
 *
 * @param config CORS config
 * @param origin Origin header from request
 * @param buf    Output buffer
 * @param size   Buffer size
 * @return Number of bytes written (excluding null), or -1 on error
 */
int sh_cors_preflight_headers(const ShCorsConfig *config, const char *origin, char *buf, size_t size);

/*
 * Parse comma-separated origins and add them.
 * Useful for parsing from environment variable.
 *
 * @param config  Config to modify
 * @param origins Comma-separated origins (e.g., "https://a.com,https://b.com")
 * @return Number of origins added
 */
int sh_cors_parse_origins(ShCorsConfig *config, const char *origins);

#ifdef __cplusplus
}
#endif

#endif /* SH_CORS_H */
