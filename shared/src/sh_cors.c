/*
 * sh_cors.c - CORS Header Utilities Implementation
 */

#include "../include/sh_cors.h"
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Safe string copy with null termination.
 */
static void safe_strcpy(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dest_size) {
        len = dest_size - 1;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

/*
 * Skip whitespace.
 */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void sh_cors_init(ShCorsConfig *config)
{
    if (!config) return;

    memset(config, 0, sizeof(*config));
    config->origin_count = 0;  /* Allow all by default */
    safe_strcpy(config->allowed_methods, sizeof(config->allowed_methods), "GET, POST, OPTIONS");
    safe_strcpy(config->allowed_headers, sizeof(config->allowed_headers), "Content-Type, Authorization");
    config->allow_credentials = 0;
    config->max_age_seconds = 86400;
}

int sh_cors_add_origin(ShCorsConfig *config, const char *origin)
{
    if (!config || !origin) return 0;
    if (config->origin_count >= SH_CORS_MAX_ORIGINS) return 0;

    /* Skip empty origins */
    if (origin[0] == '\0') return 0;

    safe_strcpy(config->allowed_origins[config->origin_count],
                SH_CORS_ORIGIN_SIZE, origin);
    config->origin_count++;
    return 1;
}

void sh_cors_set_methods(ShCorsConfig *config, const char *methods)
{
    if (!config) return;
    if (methods) {
        safe_strcpy(config->allowed_methods, sizeof(config->allowed_methods), methods);
    } else {
        config->allowed_methods[0] = '\0';
    }
}

void sh_cors_set_headers(ShCorsConfig *config, const char *headers)
{
    if (!config) return;
    if (headers) {
        safe_strcpy(config->allowed_headers, sizeof(config->allowed_headers), headers);
    } else {
        config->allowed_headers[0] = '\0';
    }
}

int sh_cors_is_allowed(const ShCorsConfig *config, const char *origin)
{
    if (!config) return 0;
    if (!origin || origin[0] == '\0') return 0;

    /* If no origins configured, allow all */
    if (config->origin_count == 0) {
        return 1;
    }

    /* Check against whitelist */
    for (int i = 0; i < config->origin_count; i++) {
        if (strcmp(config->allowed_origins[i], origin) == 0) {
            return 1;
        }
    }

    return 0;
}

int sh_cors_headers(const ShCorsConfig *config, const char *origin, char *buf, size_t size)
{
    if (!config || !buf || size == 0) return -1;

    buf[0] = '\0';
    int written = 0;

    /* Determine what origin to return */
    const char *allow_origin = NULL;
    if (config->origin_count == 0) {
        allow_origin = "*";
    } else if (sh_cors_is_allowed(config, origin)) {
        allow_origin = origin;
    } else {
        /* Origin not allowed - return empty headers */
        return 0;
    }

    /* Access-Control-Allow-Origin */
    int n = snprintf(buf + written, size - (size_t)written,
                     "Access-Control-Allow-Origin: %s\r\n", allow_origin);
    if (n < 0 || (size_t)n >= size - (size_t)written) return -1;
    written += n;

    /* Access-Control-Allow-Credentials (only for non-wildcard origins) */
    if (config->allow_credentials && config->origin_count > 0) {
        n = snprintf(buf + written, size - (size_t)written,
                     "Access-Control-Allow-Credentials: true\r\n");
        if (n < 0 || (size_t)n >= size - (size_t)written) return -1;
        written += n;
    }

    return written;
}

int sh_cors_preflight_headers(const ShCorsConfig *config, const char *origin, char *buf, size_t size)
{
    if (!config || !buf || size == 0) return -1;

    buf[0] = '\0';
    int written = 0;

    /* Determine what origin to return */
    const char *allow_origin = NULL;
    if (config->origin_count == 0) {
        allow_origin = "*";
    } else if (sh_cors_is_allowed(config, origin)) {
        allow_origin = origin;
    } else {
        /* Origin not allowed */
        return 0;
    }

    /* Access-Control-Allow-Origin */
    int n = snprintf(buf + written, size - (size_t)written,
                     "Access-Control-Allow-Origin: %s\r\n", allow_origin);
    if (n < 0 || (size_t)n >= size - (size_t)written) return -1;
    written += n;

    /* Access-Control-Allow-Methods */
    if (config->allowed_methods[0]) {
        n = snprintf(buf + written, size - (size_t)written,
                     "Access-Control-Allow-Methods: %s\r\n", config->allowed_methods);
        if (n < 0 || (size_t)n >= size - (size_t)written) return -1;
        written += n;
    }

    /* Access-Control-Allow-Headers */
    if (config->allowed_headers[0]) {
        n = snprintf(buf + written, size - (size_t)written,
                     "Access-Control-Allow-Headers: %s\r\n", config->allowed_headers);
        if (n < 0 || (size_t)n >= size - (size_t)written) return -1;
        written += n;
    }

    /* Access-Control-Max-Age */
    if (config->max_age_seconds > 0) {
        n = snprintf(buf + written, size - (size_t)written,
                     "Access-Control-Max-Age: %d\r\n", config->max_age_seconds);
        if (n < 0 || (size_t)n >= size - (size_t)written) return -1;
        written += n;
    }

    /* Access-Control-Allow-Credentials (only for non-wildcard origins) */
    if (config->allow_credentials && config->origin_count > 0) {
        n = snprintf(buf + written, size - (size_t)written,
                     "Access-Control-Allow-Credentials: true\r\n");
        if (n < 0 || (size_t)n >= size - (size_t)written) return -1;
        written += n;
    }

    return written;
}

int sh_cors_parse_origins(ShCorsConfig *config, const char *origins)
{
    if (!config || !origins) return 0;

    int added = 0;
    const char *p = origins;

    while (*p) {
        /* Skip leading whitespace */
        p = skip_ws(p);
        if (!*p) break;

        /* Find end of this origin (comma or end of string) */
        const char *start = p;
        while (*p && *p != ',') p++;

        /* Trim trailing whitespace */
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

        /* Add if non-empty */
        if (end > start) {
            /* Temporarily null-terminate */
            size_t len = (size_t)(end - start);
            char origin_buf[SH_CORS_ORIGIN_SIZE];
            if (len >= SH_CORS_ORIGIN_SIZE) {
                len = SH_CORS_ORIGIN_SIZE - 1;
            }
            memcpy(origin_buf, start, len);
            origin_buf[len] = '\0';

            if (sh_cors_add_origin(config, origin_buf)) {
                added++;
            }
        }

        /* Skip comma */
        if (*p == ',') p++;
    }

    return added;
}
