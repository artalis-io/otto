/*
 * nx_validate.h - Semantic Validation Engine (Stage X)
 *
 * Post-transform validation engine that runs on canonical JSON output.
 * Applies rules defined in schema's "validate" section and produces a
 * validation audit trail.
 *
 * Validation modifies the canonical JSON by:
 * - Adding a "validation" section inside "audit"
 * - Removing records that fail "error" severity rules
 * - Updating rows_accepted/rows_rejected counts
 *
 * Validation rules:
 * - geo_bounds: Check lat/lon are within bounding box
 * - format: Regex match on field (POSIX extended regex)
 * - unique: No duplicate values for given fields (first occurrence kept)
 * - outlier: IQR-based outlier detection on numeric field
 */

#ifndef NX_VALIDATE_H
#define NX_VALIDATE_H

#include "sh_arena.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    NX_VALIDATE_OK = 0,
    NX_VALIDATE_ERR_NULL,      /* NULL input */
    NX_VALIDATE_ERR_JSON,      /* Invalid JSON */
    NX_VALIDATE_ERR_ARENA      /* Arena allocation failure */
} NxValidateStatus;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Apply validation rules to canonical JSON.
 *
 * Rules are in the schema's "validate" array. If the schema has no "validate"
 * section, the canonical JSON is passed through unchanged.
 *
 * Validation modifies the canonical JSON to add a "validation" section
 * inside "audit", and removes records that fail "error" severity rules.
 *
 * @param canonical_json Canonical JSON from Stage B (nx_canonical format)
 * @param canon_len      Length of canonical JSON
 * @param schema_json    Transform schema JSON (nx_schema format)
 * @param schema_len     Length of schema JSON
 * @param arena          Arena for intermediate allocations
 * @param out_json       Output: heap-allocated validated JSON (caller must free)
 * @param out_len        Output: length of validated JSON
 * @return NX_VALIDATE_OK on success
 *
 * Example schema "validate" section:
 * {
 *   "validate": [
 *     {"type": "geo_bounds", "lat_field": "lat", "lon_field": "lon",
 *      "bounds": {"min_lat": 45.7, "max_lat": 48.6, "min_lon": 16.1, "max_lon": 22.9},
 *      "severity": "error"},
 *     {"type": "format", "field": "zip", "pattern": "^\\d{4}$",
 *      "message": "Must be 4 digits", "severity": "error"},
 *     {"type": "unique", "fields": ["id"], "severity": "error"},
 *     {"type": "outlier", "field": "lat", "method": "iqr", "factor": 1.5, "severity": "warning"}
 *   ]
 * }
 *
 * Output "validation" section in audit:
 * {
 *   "audit": {
 *     "rows_processed": 7, "rows_accepted": 6, "rows_rejected": 1,
 *     "rejections": [...],
 *     "validation": {
 *       "rules_applied": 4,
 *       "errors": 1,
 *       "warnings": 2,
 *       "details": [
 *         {"row": 3, "rule": "geo_bounds", "field": "lat", "severity": "error",
 *          "message": "lat 44.5 outside bounds [45.7, 48.6]"}
 *       ]
 *     }
 *   }
 * }
 */
NxValidateStatus nx_validate(const char *canonical_json, size_t canon_len,
                             const char *schema_json, size_t schema_len,
                             SHArena *arena,
                             char **out_json, size_t *out_len);

/*
 * Get human-readable error message.
 */
const char *nx_validate_status_str(NxValidateStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_VALIDATE_H */
