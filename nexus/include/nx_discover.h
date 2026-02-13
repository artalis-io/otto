/*
 * nx_discover.h - Auto Schema Discovery
 *
 * Profiles raw JSON (Stage A output) and generates a draft transform schema
 * without requiring LLMs. Uses heuristics:
 *
 *   - Type detection: double/int/string by parsing success rate
 *   - Lat/lon detection: range + header name matching
 *   - Required: non-empty in all rows
 *   - Transforms: trim (whitespace), replace (tilde prefix)
 *   - Geo bounds: computed from detected lat/lon columns
 *   - Unique: columns where all values are distinct
 *   - Row ID: first unique string column
 */

#ifndef NX_DISCOVER_H
#define NX_DISCOVER_H

#include "sh_arena.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NX_DISCOVER_OK = 0,
    NX_DISCOVER_ERR_NULL,
    NX_DISCOVER_ERR_JSON,       /* Invalid raw JSON */
    NX_DISCOVER_ERR_NO_TABLE,   /* No tables in raw JSON */
    NX_DISCOVER_ERR_NO_ROWS,    /* Table has no rows */
    NX_DISCOVER_ERR_ARENA
} NxDiscoverStatus;

/*
 * Discover a transform schema from raw JSON (Stage A output).
 *
 * Profiles all columns in tables[0] and generates a draft nx_schema v1
 * with inferred types, transforms, validation rules, and row ID.
 *
 * @param raw_json   Raw JSON from Stage A (nx_raw format)
 * @param raw_len    Length of raw JSON
 * @param arena      Arena for intermediate allocations
 * @param out_json   Output: heap-allocated schema JSON (caller must free)
 * @param out_len    Output: length of schema JSON
 * @return NX_DISCOVER_OK on success
 */
NxDiscoverStatus nx_discover_schema(const char *raw_json, size_t raw_len,
                                     SHArena *arena,
                                     char **out_json, size_t *out_len);

const char *nx_discover_status_str(NxDiscoverStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_DISCOVER_H */
