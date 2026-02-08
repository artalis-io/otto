/*
 * sh_query.c - Transport-agnostic query string parsing
 */

#include "sh_query.h"
#include <stdlib.h>
#include <string.h>

int sh_query_get_int(const char *query, const char *key, int default_val) {
    if (!query || !key) return default_val;

    size_t key_len = strlen(key);
    const char *p = query;

    while (*p) {
        /* Match key */
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return atoi(p + key_len + 1);
        }
        /* Skip to next parameter */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return default_val;
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
            return atof(p + key_len + 1);
        }
        /* Skip to next parameter */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return default_val;
}
