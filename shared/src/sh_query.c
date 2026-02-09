/*
 * sh_query.c - Transport-agnostic query string parsing
 */

#include "sh_query.h"
#include "sh_args.h"  /* For sh_parse_int */
#include <stdlib.h>
#include <string.h>

/* Forward declaration - extract string first, then parse */
size_t sh_query_get_str(const char *query, const char *key,
                        char *buf, size_t buf_size);

int sh_query_get_int(const char *query, const char *key, int default_val) {
    char buf[32];
    if (sh_query_get_str(query, key, buf, sizeof(buf)) == 0) {
        return default_val;
    }
    /* Use INT_MIN/INT_MAX as bounds for unbounded parsing */
    return sh_parse_int(buf, default_val, -2147483648, 2147483647);
}

int sh_query_get_int_bounded(const char *query, const char *key, int default_val,
                             int min_val, int max_val) {
    char buf[32];
    if (sh_query_get_str(query, key, buf, sizeof(buf)) == 0) {
        return default_val;
    }
    return sh_parse_int(buf, default_val, min_val, max_val);
}

size_t sh_query_get_str(const char *query, const char *key,
                        char *buf, size_t buf_size) {
    if (!query || !key || !buf || buf_size == 0) return 0;

    buf[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = query;

    while (*p) {
        /* Match key */
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            size_t i = 0;
            while (val[i] && val[i] != '&' && i < buf_size - 1) {
                buf[i] = val[i];
                i++;
            }
            buf[i] = '\0';
            return i;
        }
        /* Skip to next parameter */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return 0;
}

int sh_query_has(const char *query, const char *key) {
    if (!query || !key) return 0;

    size_t key_len = strlen(key);
    const char *p = query;

    while (*p) {
        /* Match key (with = or at end of string/parameter) */
        if (strncmp(p, key, key_len) == 0 &&
            (p[key_len] == '=' || p[key_len] == '&' || p[key_len] == '\0')) {
            return 1;
        }
        /* Skip to next parameter */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return 0;
}

double sh_query_get_double(const char *query, const char *key, double default_val) {
    if (!query || !key) return default_val;

    size_t key_len = strlen(key);
    const char *p = query;

    while (*p) {
        /* Match key */
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            char *end;
            double result = strtod(val, &end);
            /* Return default if no valid conversion occurred */
            if (end == val) return default_val;
            return result;
        }
        /* Skip to next parameter */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return default_val;
}
