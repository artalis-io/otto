/*
 * nx_merge.h - Continuation Row Merging
 *
 * PDF table extraction often splits long cell text across multiple rows.
 * This module merges continuation rows (where key columns are empty) back
 * into their parent data rows before the transform stage.
 *
 * Pipeline position: Stage A (extraction) → **merge** → Stage B (transform)
 *
 * Schema config:
 *   "row_merge": {
 *     "key_columns": [0, 1, 2],    // Columns that identify a data row
 *     "separator": " ",             // Inserted between parent + continuation text
 *     "strip_pattern": "GLS Csomag" // Rows matching this are removed entirely
 *   }
 *
 * If "row_merge" is absent from the schema, merge is a no-op.
 */

#ifndef NX_MERGE_H
#define NX_MERGE_H

#include "sh_arena.h"
#include "nx_issue.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NX_MERGE_OK = 0,
    NX_MERGE_ERR_NULL,
    NX_MERGE_ERR_JSON,
    NX_MERGE_ERR_SCHEMA,
    NX_MERGE_ERR_NO_TABLE,
    NX_MERGE_ERR_ARENA
} NxMergeStatus;

/*
 * Merge continuation rows in raw JSON based on schema config.
 *
 * Parses the schema for a "row_merge" config block. If absent,
 * sets *out_json = NULL (no-op signal; caller should use original raw JSON).
 *
 * @param raw_json     Raw JSON from Stage A (nx_raw format)
 * @param raw_len      Length of raw JSON
 * @param schema_json  Transform schema JSON (checked for "row_merge" key)
 * @param schema_len   Length of schema JSON
 * @param arena        Arena for intermediate JSON parsing
 * @param issues       Issue list for structured reporting (NULL to skip)
 * @param out_json     Output: heap-allocated merged JSON (caller must free),
 *                     or NULL if no row_merge config found (no-op)
 * @param out_len      Output: length of merged JSON
 * @return NX_MERGE_OK on success (including no-op case)
 */
NxMergeStatus nx_merge_rows(const char *raw_json, size_t raw_len,
                            const char *schema_json, size_t schema_len,
                            SHArena *arena, NxIssueList *issues,
                            char **out_json, size_t *out_len);

const char *nx_merge_status_str(NxMergeStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_MERGE_H */
