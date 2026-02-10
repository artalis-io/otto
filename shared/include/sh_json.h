/*
 * sh_json.h - Arena-Allocated JSON Parser for OTTO Platform
 *
 * Minimal, zero-dependency JSON parser with arena allocation.
 * All parsed values are allocated from a single arena and freed together.
 *
 * Features:
 * - Recursive descent parser
 * - Full JSON escape sequence handling (\n, \uXXXX, etc.)
 * - NULL-safe value accessors with defaults
 * - Path-based access (e.g., "user.name", "items[0].id")
 * - WASM compatible (no external dependencies)
 *
 * Usage:
 *   SHArena *arena = sh_arena_create(4096);
 *   ShJsonValue *root;
 *   if (sh_json_parse(json_str, strlen(json_str), arena, &root) == SH_JSON_OK) {
 *       double price = sh_json_as_double(sh_json_get(root, "price"), 0.0);
 *       const char *name = sh_json_as_string(sh_json_get(root, "name"), "");
 *       // ...
 *   }
 *   sh_arena_free(arena);  // Frees all parsed values
 */

#ifndef SH_JSON_H
#define SH_JSON_H

#include "sh_arena.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#ifndef SH_JSON_MAX_DEPTH
#define SH_JSON_MAX_DEPTH 64
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    SH_JSON_OK = 0,
    SH_JSON_ERR_NULL,              /* NULL input pointer */
    SH_JSON_ERR_EMPTY,             /* Empty or whitespace-only input */
    SH_JSON_ERR_SYNTAX,            /* Syntax error */
    SH_JSON_ERR_UNTERMINATED_STRING,
    SH_JSON_ERR_INVALID_ESCAPE,
    SH_JSON_ERR_INVALID_NUMBER,
    SH_JSON_ERR_DEPTH_EXCEEDED,
    SH_JSON_ERR_ARENA_FULL
} ShJsonStatus;

/* ============================================================================
 * Value Types
 * ============================================================================ */

typedef enum {
    SH_JSON_NULL,
    SH_JSON_BOOL,
    SH_JSON_NUMBER,
    SH_JSON_STRING,
    SH_JSON_ARRAY,
    SH_JSON_OBJECT
} ShJsonType;

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/* Forward declaration */
typedef struct ShJsonValue ShJsonValue;

/* Object member (key-value pair) */
typedef struct {
    const char *key;      /* Key string (arena-allocated, null-terminated) */
    size_t key_len;       /* Key length (excludes null terminator) */
    ShJsonValue *value;   /* Value (arena-allocated) */
} ShJsonMember;

/* JSON value (arena-allocated) */
struct ShJsonValue {
    ShJsonType type;
    union {
        bool bool_val;
        double num_val;
        struct {
            const char *str;  /* Null-terminated string */
            size_t len;       /* Length (excludes null terminator) */
        } string_val;
        struct {
            ShJsonValue **items;
            size_t count;
        } array_val;
        struct {
            ShJsonMember *members;
            size_t count;
        } object_val;
    } u;
};

/* ============================================================================
 * Parsing
 * ============================================================================ */

/*
 * Parse JSON string into arena-allocated DOM.
 *
 * @param json  JSON string (not modified)
 * @param len   Length of JSON string
 * @param arena Arena for allocations (caller manages lifecycle)
 * @param out   Output value pointer (set on success)
 * @return SH_JSON_OK on success, error code otherwise
 */
ShJsonStatus sh_json_parse(const char *json, size_t len,
                           SHArena *arena, ShJsonValue **out);

/*
 * Get human-readable error message for status code.
 */
const char *sh_json_status_str(ShJsonStatus status);

/* ============================================================================
 * Type Checking
 * ============================================================================ */

/*
 * Get type of value (SH_JSON_NULL if v is NULL).
 */
ShJsonType sh_json_type(const ShJsonValue *v);

/*
 * Check if value is null (or pointer is NULL).
 */
bool sh_json_is_null(const ShJsonValue *v);

/* ============================================================================
 * Value Access (NULL-safe, returns default on type mismatch)
 * ============================================================================ */

/*
 * Get boolean value.
 * Returns def if v is NULL or not a boolean.
 */
bool sh_json_as_bool(const ShJsonValue *v, bool def);

/*
 * Get numeric value.
 * Returns def if v is NULL or not a number.
 */
double sh_json_as_double(const ShJsonValue *v, double def);

/*
 * Get integer value (truncates toward zero).
 * Returns def if v is NULL or not a number.
 */
int sh_json_as_int(const ShJsonValue *v, int def);

/*
 * Get string value.
 * Returns def if v is NULL or not a string.
 */
const char *sh_json_as_string(const ShJsonValue *v, const char *def);

/*
 * Get string length.
 * Returns 0 if v is NULL or not a string.
 */
size_t sh_json_string_len(const ShJsonValue *v);

/* ============================================================================
 * Array Access
 * ============================================================================ */

/*
 * Get array length.
 * Returns 0 if v is NULL or not an array.
 */
size_t sh_json_array_len(const ShJsonValue *v);

/*
 * Get array element by index.
 * Returns NULL if v is NULL, not an array, or index out of bounds.
 */
ShJsonValue *sh_json_array_get(const ShJsonValue *v, size_t index);

/* ============================================================================
 * Object Access
 * ============================================================================ */

/*
 * Get object member count.
 * Returns 0 if v is NULL or not an object.
 */
size_t sh_json_object_len(const ShJsonValue *v);

/*
 * Get object member by key.
 * Returns NULL if v is NULL, not an object, or key not found.
 * Uses linear search (O(n) for n members).
 */
ShJsonValue *sh_json_get(const ShJsonValue *v, const char *key);

/*
 * Get object member by key with length.
 * Useful when key is not null-terminated.
 */
ShJsonValue *sh_json_get_n(const ShJsonValue *v, const char *key, size_t key_len);

/* ============================================================================
 * Path Access
 * ============================================================================ */

/*
 * Access nested value by path.
 *
 * Path syntax:
 *   - "key" - object key
 *   - "key.subkey" - nested object keys
 *   - "array[0]" - array index
 *   - "obj.arr[2].name" - combined access
 *
 * Returns NULL if path is invalid, any segment not found, or type mismatch.
 *
 * Examples:
 *   sh_json_get_path(root, "name")           // root.name
 *   sh_json_get_path(root, "user.email")     // root.user.email
 *   sh_json_get_path(root, "items[0]")       // root.items[0]
 *   sh_json_get_path(root, "data.items[2].id") // root.data.items[2].id
 */
ShJsonValue *sh_json_get_path(const ShJsonValue *v, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SH_JSON_H */
