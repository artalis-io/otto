/*
 * nx_xform.h - Schema-Driven Transform Engine (Stage B)
 *
 * Takes raw rows JSON (from Stage A) and a transform schema, produces
 * canonical output JSON with typed fields, validation, and audit trail.
 *
 * Schema features:
 * - Column mapping by source index
 * - Type coercion: string, int, double, bool
 * - Transforms: trim, lowercase, uppercase
 * - Validation: required, min/max for numerics
 * - Derived fields: constant values added to every record
 * - Row ID generation with slugification
 */

#ifndef NX_XFORM_H
#define NX_XFORM_H

#include "sh_arena.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    NX_XFORM_OK = 0,
    NX_XFORM_ERR_NULL,         /* NULL input */
    NX_XFORM_ERR_SCHEMA,       /* Invalid schema JSON */
    NX_XFORM_ERR_RAW,          /* Invalid raw JSON input */
    NX_XFORM_ERR_NO_TABLE,     /* Selected table not found */
    NX_XFORM_ERR_ARENA         /* Arena allocation failure */
} NxXformStatus;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Apply transform schema to raw rows JSON, producing canonical output.
 *
 * @param raw_json    Raw rows JSON from Stage A (nx_raw format)
 * @param raw_len     Length of raw JSON
 * @param schema_json Transform schema JSON (nx_schema format)
 * @param schema_len  Length of schema JSON
 * @param arena       Arena for intermediate allocations
 * @param out_json    Output: heap-allocated canonical JSON (caller must free)
 * @param out_len     Output: length of canonical JSON
 * @return NX_XFORM_OK on success
 */
NxXformStatus nx_xform_apply(const char *raw_json, size_t raw_len,
                             const char *schema_json, size_t schema_len,
                             SHArena *arena, char **out_json, size_t *out_len);

/*
 * Get human-readable error message.
 */
const char *nx_xform_status_str(NxXformStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_XFORM_H */
