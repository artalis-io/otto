/*
 * nx_emit.c - Nexus Output Emitters
 *
 * Thin wrapper: parses canonical JSON records[], calls shared sh_geojson
 * and sh_csv encoders for downstream output.
 */

#include "nx_emit.h"
#include "sh_json.h"
#include "sh_geojson.h"
#include "sh_csv.h"
#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Get string representation of a JSON value (number, bool, string, null) */
static const char *json_value_as_str(const ShJsonValue *v, char *buf, size_t bufsz)
{
    if (!v || sh_json_is_null(v)) return "";
    switch (sh_json_type(v)) {
        case SH_JSON_STRING:
            return sh_json_as_string(v, "");
        case SH_JSON_NUMBER: {
            double d = sh_json_as_double(v, 0.0);
            /* Check if integer */
            if (d == (double)(long long)d && d >= -1e15 && d <= 1e15) {
                snprintf(buf, bufsz, "%lld", (long long)d);
            } else {
                snprintf(buf, bufsz, "%g", d);
            }
            return buf;
        }
        case SH_JSON_BOOL:
            return sh_json_as_bool(v, 0) ? "true" : "false";
        default:
            return "";
    }
}

/* ============================================================================
 * GeoJSON Emitter
 * ============================================================================ */

NxEmitStatus nx_emit_geojson(const char *canonical_json, size_t canon_len,
                              const NxEmitGeoJsonOpts *opts, SHArena *arena,
                              char **out_json, size_t *out_len)
{
    if (!canonical_json || !out_json || !out_len) return NX_EMIT_ERR_NULL;
    if (!arena) return NX_EMIT_ERR_NULL;

    *out_json = NULL;
    *out_len = 0;

    NxEmitGeoJsonOpts defaults = NX_EMIT_GEOJSON_DEFAULTS;
    if (!opts) opts = &defaults;

    /* Parse canonical JSON */
    ShJsonValue *root = NULL;
    if (sh_json_parse(canonical_json, canon_len, arena, &root) != SH_JSON_OK) {
        return NX_EMIT_ERR_JSON;
    }

    ShJsonValue *records = sh_json_get(root, "records");
    if (!records || sh_json_type(records) != SH_JSON_ARRAY) {
        return NX_EMIT_ERR_NO_RECORDS;
    }

    size_t nrec = sh_json_array_len(records);

    /* Set up GeoJSON writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);

    for (size_t i = 0; i < nrec; i++) {
        ShJsonValue *rec = sh_json_array_get(records, i);
        if (!rec || sh_json_type(rec) != SH_JSON_OBJECT) continue;

        /* Extract lat/lon */
        ShJsonValue *lat_v = sh_json_get(rec, opts->lat_field);
        ShJsonValue *lon_v = sh_json_get(rec, opts->lon_field);
        if (!lat_v || !lon_v) continue;
        if (sh_json_type(lat_v) != SH_JSON_NUMBER ||
            sh_json_type(lon_v) != SH_JSON_NUMBER) continue;

        double lat = sh_json_as_double(lat_v, 0.0);
        double lon = sh_json_as_double(lon_v, 0.0);

        /* Extract ID */
        const char *id = NULL;
        if (opts->id_field) {
            ShJsonValue *id_v = sh_json_get(rec, opts->id_field);
            if (id_v && sh_json_type(id_v) == SH_JSON_STRING) {
                id = sh_json_as_string(id_v, NULL);
            }
        }

        /* Collect non-geo properties */
        size_t nmembers = sh_json_object_len(rec);
        ShGeoJsonProp *props = NULL;
        size_t prop_count = 0;

        if (nmembers > 0) {
            /* Allocate on arena for temp usage */
            props = (ShGeoJsonProp *)sh_arena_alloc(arena,
                        nmembers * sizeof(ShGeoJsonProp));
            if (props) {
                /* Iterate object members, skip lat/lon/id */
                for (size_t m = 0; m < nmembers; m++) {
                    const ShJsonMember *mem = &rec->u.object_val.members[m];
                    if (strcmp(mem->key, opts->lat_field) == 0) continue;
                    if (strcmp(mem->key, opts->lon_field) == 0) continue;
                    if (opts->id_field && strcmp(mem->key, opts->id_field) == 0) continue;

                    /* Convert value to string */
                    char *vbuf = (char *)sh_arena_alloc(arena, 64);
                    props[prop_count].key = mem->key;
                    props[prop_count].value = json_value_as_str(mem->value, vbuf, 64);
                    prop_count++;
                }
            }
        }

        sh_geojson_point_feature(&w, id, lon, lat, opts->precision,
                                  props, prop_count);
    }

    sh_geojson_end(&w);

    if (sh_json_writer_error(&w)) {
        sh_json_buf_free(&jb);
        return NX_EMIT_ERR_ALLOC;
    }

    *out_json = sh_json_buf_take(&jb);
    *out_len = jb.len;
    if (*out_json) {
        *out_len = strlen(*out_json);
    }
    sh_json_buf_free(&jb);

    return NX_EMIT_OK;
}

/* ============================================================================
 * CSV Emitter
 * ============================================================================ */

NxEmitStatus nx_emit_csv(const char *canonical_json, size_t canon_len,
                          const NxEmitCsvOpts *opts, SHArena *arena,
                          char **out_csv, size_t *out_len)
{
    if (!canonical_json || !out_csv || !out_len) return NX_EMIT_ERR_NULL;
    if (!arena) return NX_EMIT_ERR_NULL;

    *out_csv = NULL;
    *out_len = 0;

    NxEmitCsvOpts defaults = NX_EMIT_CSV_DEFAULTS;
    if (!opts) opts = &defaults;

    /* Parse canonical JSON */
    ShJsonValue *root = NULL;
    if (sh_json_parse(canonical_json, canon_len, arena, &root) != SH_JSON_OK) {
        return NX_EMIT_ERR_JSON;
    }

    ShJsonValue *records = sh_json_get(root, "records");
    if (!records || sh_json_type(records) != SH_JSON_ARRAY) {
        return NX_EMIT_ERR_NO_RECORDS;
    }

    size_t nrec = sh_json_array_len(records);

    /* Collect field names from first record */
    const char **field_names = NULL;
    size_t nfields = 0;

    if (nrec > 0) {
        ShJsonValue *first = sh_json_array_get(records, 0);
        if (first && sh_json_type(first) == SH_JSON_OBJECT) {
            nfields = sh_json_object_len(first);
            field_names = (const char **)sh_arena_alloc(arena,
                              nfields * sizeof(const char *));
            if (field_names) {
                for (size_t m = 0; m < nfields; m++) {
                    field_names[m] = first->u.object_val.members[m].key;
                }
            }
        }
    }

    /* Set up CSV writer */
    ShCsvBuf cb;
    sh_csv_buf_init(&cb);
    ShCsvWriter w;
    sh_csv_writer_init(&w, sh_csv_buf_write, &cb, opts->delimiter);

    /* Write header row */
    if (field_names) {
        for (size_t f = 0; f < nfields; f++) {
            sh_csv_write_field_str(&w, field_names[f]);
        }
        sh_csv_write_row_end(&w);
    }

    /* Write data rows */
    for (size_t i = 0; i < nrec; i++) {
        ShJsonValue *rec = sh_json_array_get(records, i);
        if (!rec || sh_json_type(rec) != SH_JSON_OBJECT) continue;

        for (size_t f = 0; f < nfields; f++) {
            ShJsonValue *val = sh_json_get(rec, field_names[f]);
            char numbuf[64];
            const char *str = json_value_as_str(val, numbuf, sizeof(numbuf));
            sh_csv_write_field_str(&w, str);
        }
        sh_csv_write_row_end(&w);
    }

    if (sh_csv_writer_error(&w)) {
        sh_csv_buf_free(&cb);
        return NX_EMIT_ERR_ALLOC;
    }

    *out_csv = sh_csv_buf_take(&cb, out_len);
    sh_csv_buf_free(&cb);

    return NX_EMIT_OK;
}

/* ============================================================================
 * Status Strings
 * ============================================================================ */

const char *nx_emit_status_str(NxEmitStatus status)
{
    switch (status) {
        case NX_EMIT_OK:          return "ok";
        case NX_EMIT_ERR_NULL:    return "null input";
        case NX_EMIT_ERR_JSON:    return "invalid JSON";
        case NX_EMIT_ERR_NO_RECORDS: return "no records array";
        case NX_EMIT_ERR_NO_LATLON:  return "missing lat/lon fields";
        case NX_EMIT_ERR_ALLOC:   return "allocation failed";
        default:                  return "unknown error";
    }
}
