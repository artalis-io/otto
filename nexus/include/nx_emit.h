/*
 * nx_emit.h - Nexus Output Emitters
 *
 * Converts canonical JSON (Stage X output) to downstream formats:
 *   - GeoJSON FeatureCollection (RFC 7946) for Carta visualization
 *   - CSV (RFC 4180) for operations export
 *
 * Thin wrapper: parses canonical JSON, calls shared sh_geojson/sh_csv encoders.
 *
 * Dependencies: sh_json.h, sh_geojson.h, sh_csv.h, sh_arena.h
 */

#ifndef NX_EMIT_H
#define NX_EMIT_H

#include "nx_issue.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SHArena SHArena;

typedef enum {
    NX_EMIT_OK = 0,
    NX_EMIT_ERR_NULL,
    NX_EMIT_ERR_JSON,
    NX_EMIT_ERR_NO_RECORDS,
    NX_EMIT_ERR_NO_LATLON,
    NX_EMIT_ERR_ALLOC
} NxEmitStatus;

typedef struct {
    const char *lat_field;   /* default: "lat" */
    const char *lon_field;   /* default: "lon" */
    const char *id_field;    /* default: "id" */
    int precision;           /* default: 6 */
} NxEmitGeoJsonOpts;

#define NX_EMIT_GEOJSON_DEFAULTS { "lat", "lon", "id", 6 }

typedef struct {
    char delimiter;          /* default: ',' */
} NxEmitCsvOpts;

#define NX_EMIT_CSV_DEFAULTS { ',' }

/*
 * Emit canonical JSON as GeoJSON FeatureCollection.
 *
 * Iterates records[], extracts lat/lon for Point geometry, puts remaining
 * fields in properties. Records missing lat/lon are skipped.
 *
 * @param canonical_json  Canonical JSON from Stage X
 * @param canon_len       Length of canonical JSON
 * @param opts            Options (NULL for defaults)
 * @param arena           Arena for JSON parsing
 * @param issues          Issue list for structured reporting (NULL to skip)
 * @param out_json        Output: heap-allocated GeoJSON string (caller frees)
 * @param out_len         Output: length of GeoJSON string
 * @return NX_EMIT_OK on success
 */
NxEmitStatus nx_emit_geojson(const char *canonical_json, size_t canon_len,
                              const NxEmitGeoJsonOpts *opts, SHArena *arena,
                              NxIssueList *issues,
                              char **out_json, size_t *out_len);

/*
 * Emit canonical JSON as CSV.
 *
 * Writes header row from field names of first record, then data rows.
 *
 * @param canonical_json  Canonical JSON from Stage X
 * @param canon_len       Length of canonical JSON
 * @param opts            Options (NULL for defaults)
 * @param arena           Arena for JSON parsing
 * @param issues          Issue list for structured reporting (NULL to skip)
 * @param out_csv         Output: heap-allocated CSV string (caller frees)
 * @param out_len         Output: length of CSV string
 * @return NX_EMIT_OK on success
 */
NxEmitStatus nx_emit_csv(const char *canonical_json, size_t canon_len,
                          const NxEmitCsvOpts *opts, SHArena *arena,
                          NxIssueList *issues,
                          char **out_csv, size_t *out_len);

/*
 * Get human-readable string for emit status.
 */
const char *nx_emit_status_str(NxEmitStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_EMIT_H */
