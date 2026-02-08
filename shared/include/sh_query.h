/*
 * sh_query.h - Transport-agnostic query string parsing
 *
 * Parses URL query strings in format: "key1=val1&key2=val2"
 * Used by API handlers that receive raw query strings (HTTP, WASM, etc.)
 */

#ifndef SH_QUERY_H
#define SH_QUERY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Get integer parameter from query string.
 *
 * @param query       Query string (e.g., "width=80&height=40")
 * @param key         Parameter name to find
 * @param default_val Value to return if not found
 * @return Parameter value or default_val
 */
int sh_query_get_int(const char *query, const char *key, int default_val);

/*
 * Get string parameter from query string.
 *
 * @param query    Query string
 * @param key      Parameter name to find
 * @param buf      Buffer to store value
 * @param buf_size Size of buffer
 * @return Number of bytes written (excluding null), or 0 if not found
 */
size_t sh_query_get_str(const char *query, const char *key,
                        char *buf, size_t buf_size);

/*
 * Check if parameter exists in query string.
 *
 * @param query Query string
 * @param key   Parameter name to find
 * @return 1 if found, 0 if not
 */
int sh_query_has(const char *query, const char *key);

/*
 * Get double parameter from query string.
 *
 * @param query       Query string
 * @param key         Parameter name to find
 * @param default_val Value to return if not found
 * @return Parameter value or default_val
 */
double sh_query_get_double(const char *query, const char *key, double default_val);

#ifdef __cplusplus
}
#endif

#endif /* SH_QUERY_H */
