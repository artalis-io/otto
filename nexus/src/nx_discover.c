/*
 * nx_discover.c - Auto Schema Discovery
 *
 * Profiles raw JSON columns and emits a draft transform schema.
 * Single-pass profiling, O(rows * cols) with hash-based uniqueness check.
 */

#include "nx_discover.h"
#include "sh_json.h"
#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>

#define MAX_COLS         128
#define MAX_HEADER_LEN   256
#define UNIQUENESS_LIMIT 10000  /* Stop checking uniqueness after N rows */
#define GEO_BOUNDS_PAD   0.05  /* 5% padding on discovered bounds */

/* ============================================================================
 * Column Profile
 * ============================================================================ */

typedef struct {
    char header[MAX_HEADER_LEN];
    int  total;            /* Total rows seen */
    int  non_empty;        /* Non-empty cells */
    int  parse_double;     /* Cells that parse as double */
    int  parse_int;        /* Cells that parse as int */
    int  has_tilde;        /* Cells with ~ prefix */
    int  has_whitespace;   /* Cells with leading/trailing whitespace */
    double min_val;        /* Min numeric value */
    double max_val;        /* Max numeric value */
    bool   unique;         /* All values distinct so far */
    /* Simple hash set for uniqueness: open addressing, FNV-1a */
    uint32_t *hash_set;
    size_t    hash_cap;
    size_t    hash_count;
} ColProfile;

/* ============================================================================
 * Heuristic Helpers
 * ============================================================================ */

static uint32_t fnv1a(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* Insert into hash set, return false if duplicate */
static bool hash_set_insert(ColProfile *p, const char *val, size_t len)
{
    if (!p->hash_set || !p->unique) return true;
    if (p->hash_count >= p->hash_cap / 2) {
        /* Load factor > 50%, stop tracking */
        p->unique = false;
        return true;
    }
    uint32_t h = fnv1a(val, len);
    size_t mask = p->hash_cap - 1;
    size_t idx = h & mask;
    for (size_t i = 0; i < p->hash_cap; i++) {
        size_t pos = (idx + i) & mask;
        if (p->hash_set[pos] == 0) {
            p->hash_set[pos] = h | 1; /* Ensure non-zero */
            p->hash_count++;
            return true;
        }
        if (p->hash_set[pos] == (h | 1)) {
            /* Possible duplicate (hash collision possible, but good enough) */
            p->unique = false;
            return false;
        }
    }
    p->unique = false;
    return false;
}

/* Try to parse a cell value as a number, handling tilde prefix */
static bool try_parse_double(const char *s, size_t len, double *out)
{
    if (len == 0) return false;

    const char *p = s;
    size_t plen = len;

    /* Skip tilde prefix */
    if (*p == '~') { p++; plen--; }
    if (plen == 0) return false;

    /* Skip whitespace */
    while (plen > 0 && (*p == ' ' || *p == '\t')) { p++; plen--; }
    if (plen == 0) return false;

    char buf[64];
    if (plen >= sizeof(buf)) return false;
    memcpy(buf, p, plen);
    buf[plen] = '\0';

    /* Handle comma as decimal separator (European) */
    for (size_t i = 0; i < plen; i++) {
        if (buf[i] == ',') buf[i] = '.';
    }

    char *end = NULL;
    double val = strtod(buf, &end);
    if (end == buf || *end != '\0') return false;
    if (!isfinite(val)) return false;

    *out = val;
    return true;
}

static bool try_parse_int(const char *s, size_t len)
{
    if (len == 0) return false;
    const char *p = s;
    if (*p == '-' || *p == '+') { p++; len--; }
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
    }
    return true;
}

static bool has_leading_trailing_space(const char *s, size_t len)
{
    if (len == 0) return false;
    return s[0] == ' ' || s[0] == '\t' || s[len - 1] == ' ' || s[len - 1] == '\t';
}

/* Case-insensitive substring check */
static bool str_contains_ci(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* ============================================================================
 * Lat/Lon Detection
 * ============================================================================ */

typedef enum { COL_STRING, COL_INT, COL_DOUBLE } ColType;

static ColType infer_type(const ColProfile *p)
{
    if (p->non_empty == 0) return COL_STRING;
    double ratio_double = (double)p->parse_double / p->non_empty;
    double ratio_int = (double)p->parse_int / p->non_empty;

    if (ratio_double >= 0.9) {
        /* If all doubles are also ints, prefer int */
        if (ratio_int >= 0.9 && p->min_val >= -2147483648.0 && p->max_val <= 2147483647.0)
            return COL_INT;
        return COL_DOUBLE;
    }
    return COL_STRING;
}

static bool header_looks_like_lat(const char *h)
{
    return str_contains_ci(h, "lat") || str_contains_ci(h, "szelesseg") ||
           str_contains_ci(h, "szélesség");
}

static bool header_looks_like_lon(const char *h)
{
    return str_contains_ci(h, "lon") || str_contains_ci(h, "lng") ||
           str_contains_ci(h, "hossz") || str_contains_ci(h, "hosszúság");
}

/* Detect lat/lon columns: returns indices, -1 if not found */
static void detect_lat_lon(const ColProfile *cols, int ncols,
                           int *lat_idx, int *lon_idx)
{
    *lat_idx = -1;
    *lon_idx = -1;

    /* First pass: header name matching + range check */
    for (int i = 0; i < ncols; i++) {
        if (infer_type(&cols[i]) != COL_DOUBLE) continue;

        if (header_looks_like_lat(cols[i].header) &&
            cols[i].min_val >= -90.0 && cols[i].max_val <= 90.0) {
            *lat_idx = i;
        }
        if (header_looks_like_lon(cols[i].header) &&
            cols[i].min_val >= -180.0 && cols[i].max_val <= 180.0) {
            *lon_idx = i;
        }
    }

    /* If we found one but not the other, try range-only detection */
    if (*lat_idx >= 0 && *lon_idx < 0) {
        for (int i = 0; i < ncols; i++) {
            if (i == *lat_idx) continue;
            if (infer_type(&cols[i]) != COL_DOUBLE) continue;
            if (cols[i].min_val >= -180.0 && cols[i].max_val <= 180.0) {
                *lon_idx = i;
                break;
            }
        }
    }
    if (*lon_idx >= 0 && *lat_idx < 0) {
        for (int i = 0; i < ncols; i++) {
            if (i == *lon_idx) continue;
            if (infer_type(&cols[i]) != COL_DOUBLE) continue;
            if (cols[i].min_val >= -90.0 && cols[i].max_val <= 90.0) {
                *lat_idx = i;
                break;
            }
        }
    }

    /* Fallback: if no header match, look for adjacent double columns in range */
    if (*lat_idx < 0 && *lon_idx < 0) {
        for (int i = 0; i < ncols - 1; i++) {
            if (infer_type(&cols[i]) != COL_DOUBLE) continue;
            if (infer_type(&cols[i + 1]) != COL_DOUBLE) continue;
            if (cols[i].min_val >= -90.0 && cols[i].max_val <= 90.0 &&
                cols[i + 1].min_val >= -180.0 && cols[i + 1].max_val <= 180.0) {
                *lat_idx = i;
                *lon_idx = i + 1;
                break;
            }
        }
    }
}

/* ============================================================================
 * Schema Generation
 * ============================================================================ */

static void write_column(ShJsonWriter *w, const ColProfile *p, int source_idx,
                         const char *target, ColType type, bool required,
                         bool is_lat, bool is_lon)
{
    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "source", source_idx);
    sh_json_write_kv_string(w, "target", target);

    switch (type) {
        case COL_DOUBLE:
            sh_json_write_kv_string(w, "type", "double");
            sh_json_write_kv_int(w, "precision", 6);
            break;
        case COL_INT:
            sh_json_write_kv_string(w, "type", "int");
            break;
        default:
            sh_json_write_kv_string(w, "type", "string");
            break;
    }

    if (required)
        sh_json_write_kv_bool(w, "required", true);

    /* Transforms */
    bool need_trim = p->has_whitespace > 0;
    bool need_tilde = p->has_tilde > 0 && type == COL_DOUBLE;

    if (need_trim || need_tilde) {
        sh_json_write_key(w, "transforms");
        sh_json_write_array_start(w);
        if (need_trim) sh_json_write_string(w, "trim");
        if (need_tilde) {
            sh_json_write_object_start(w);
            sh_json_write_key(w, "replace");
            sh_json_write_array_start(w);
            sh_json_write_string(w, "~");
            sh_json_write_string(w, "");
            sh_json_write_array_end(w);
            sh_json_write_object_end(w);
        }
        sh_json_write_array_end(w);
    }

    /* Inline validation for lat/lon */
    if (is_lat) {
        sh_json_write_key(w, "validate");
        sh_json_write_object_start(w);
        sh_json_write_kv_double(w, "min", -90.0);
        sh_json_write_kv_double(w, "max", 90.0);
        sh_json_write_object_end(w);
    } else if (is_lon) {
        sh_json_write_key(w, "validate");
        sh_json_write_object_start(w);
        sh_json_write_kv_double(w, "min", -180.0);
        sh_json_write_kv_double(w, "max", 180.0);
        sh_json_write_object_end(w);
    }

    sh_json_write_object_end(w);
}

/* Slugify header for use as field target name */
static void slugify_header(const char *header, char *out, size_t outlen)
{
    size_t j = 0;
    for (size_t i = 0; header[i] && j < outlen - 1; i++) {
        unsigned char c = (unsigned char)header[i];
        if (c >= 'A' && c <= 'Z') {
            out[j++] = (char)(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = (char)c;
        } else if (c == ' ' || c == '_' || c == '-' || c == '\t') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        }
        /* Skip other chars (accented, special) */
    }
    /* Trim trailing underscore */
    while (j > 0 && out[j - 1] == '_') j--;
    if (j == 0) {
        /* Fallback */
        snprintf(out, outlen, "col");
    }
    out[j] = '\0';
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char *nx_discover_status_str(NxDiscoverStatus status)
{
    switch (status) {
        case NX_DISCOVER_OK:        return "OK";
        case NX_DISCOVER_ERR_NULL:  return "NULL input";
        case NX_DISCOVER_ERR_JSON:  return "Invalid raw JSON";
        case NX_DISCOVER_ERR_NO_TABLE: return "No tables in raw JSON";
        case NX_DISCOVER_ERR_NO_ROWS:  return "No rows in table";
        case NX_DISCOVER_ERR_ARENA: return "Arena allocation failure";
        default:                    return "Unknown error";
    }
}

NxDiscoverStatus nx_discover_schema(const char *raw_json, size_t raw_len,
                                     SHArena *arena,
                                     char **out_json, size_t *out_len)
{
    if (!raw_json || !out_json || !out_len) return NX_DISCOVER_ERR_NULL;
    *out_json = NULL;
    *out_len = 0;

    /* Parse raw JSON */
    ShJsonValue *root = NULL;
    ShJsonStatus jst = sh_json_parse(raw_json, raw_len, arena, &root);
    if (jst != SH_JSON_OK || !root) return NX_DISCOVER_ERR_JSON;

    /* Get tables[0] */
    ShJsonValue *tables = sh_json_get(root, "tables");
    if (!tables || sh_json_array_len(tables) == 0) return NX_DISCOVER_ERR_NO_TABLE;

    ShJsonValue *table = sh_json_array_get(tables, 0);
    if (!table) return NX_DISCOVER_ERR_NO_TABLE;

    ShJsonValue *headers_arr = sh_json_get(table, "headers");
    ShJsonValue *rows_arr = sh_json_get(table, "rows");
    if (!rows_arr) return NX_DISCOVER_ERR_NO_ROWS;

    size_t nrows = sh_json_array_len(rows_arr);
    if (nrows == 0) return NX_DISCOVER_ERR_NO_ROWS;

    size_t ncols = headers_arr ? sh_json_array_len(headers_arr) : 0;
    if (ncols == 0) {
        /* Infer from first row */
        ShJsonValue *row0 = sh_json_array_get(rows_arr, 0);
        ShJsonValue *cells0 = row0 ? sh_json_get(row0, "cells") : NULL;
        if (cells0) ncols = sh_json_array_len(cells0);
    }
    if (ncols == 0 || ncols > MAX_COLS) return NX_DISCOVER_ERR_NO_TABLE;

    /* Initialize column profiles */
    ColProfile *cols = (ColProfile *)sh_arena_calloc(arena, ncols, sizeof(ColProfile));
    if (!cols) return NX_DISCOVER_ERR_ARENA;

    for (size_t c = 0; c < ncols; c++) {
        cols[c].min_val = 1e300;
        cols[c].max_val = -1e300;
        cols[c].unique = true;

        /* Extract header */
        if (headers_arr && c < sh_json_array_len(headers_arr)) {
            const char *h = sh_json_as_string(sh_json_array_get(headers_arr, c), "");
            snprintf(cols[c].header, MAX_HEADER_LEN, "%s", h);
        } else {
            snprintf(cols[c].header, MAX_HEADER_LEN, "col_%zu", c);
        }

        /* Allocate hash set for uniqueness (power of 2, 4x rows) */
        size_t cap = 64;
        while (cap < nrows * 4 && cap < UNIQUENESS_LIMIT * 4) cap *= 2;
        cols[c].hash_cap = cap;
        cols[c].hash_set = (uint32_t *)sh_arena_calloc(arena, cap, sizeof(uint32_t));
        if (!cols[c].hash_set) cols[c].unique = false;
    }

    /* Profile each row */
    for (size_t r = 0; r < nrows; r++) {
        ShJsonValue *row = sh_json_array_get(rows_arr, r);
        ShJsonValue *cells = row ? sh_json_get(row, "cells") : NULL;
        if (!cells) continue;

        size_t row_ncols = sh_json_array_len(cells);

        for (size_t c = 0; c < ncols; c++) {
            ColProfile *p = &cols[c];
            p->total++;

            if (c >= row_ncols) continue;

            const char *val = sh_json_as_string(sh_json_array_get(cells, c), "");
            size_t vlen = strlen(val);

            if (vlen == 0) continue;
            p->non_empty++;

            /* Whitespace check */
            if (has_leading_trailing_space(val, vlen))
                p->has_whitespace++;

            /* Tilde prefix */
            if (val[0] == '~')
                p->has_tilde++;

            /* Numeric parsing */
            double dval;
            if (try_parse_double(val, vlen, &dval)) {
                p->parse_double++;
                if (dval < p->min_val) p->min_val = dval;
                if (dval > p->max_val) p->max_val = dval;
            }
            if (try_parse_int(val, vlen))
                p->parse_int++;

            /* Uniqueness */
            if (p->unique && r < UNIQUENESS_LIMIT)
                hash_set_insert(p, val, vlen);
        }
    }

    /* Detect continuation rows (PDF line-wrap pattern).
     *
     * Strategy: find rows that are "partial" (some cells empty, some not).
     * Then find columns that are consistently empty across these partial rows.
     * Those columns are likely key columns for merge detection.
     */
    int cont_key_cols[MAX_COLS];
    int cont_key_count = 0;
    bool emit_row_merge = false;

    /* Pass 1: identify potential continuation rows (some cells empty, some not) */
    int *partial_empty = (int *)sh_arena_calloc(arena, ncols, sizeof(int));
    int partial_count = 0;

    if (partial_empty) {
        for (size_t r = 0; r < nrows; r++) {
            ShJsonValue *row = sh_json_array_get(rows_arr, r);
            ShJsonValue *cells = row ? sh_json_get(row, "cells") : NULL;
            if (!cells) continue;

            size_t row_ncols = sh_json_array_len(cells);
            int empty_count = 0;
            int nonempty_count = 0;
            for (size_t c = 0; c < ncols && c < row_ncols; c++) {
                const char *val = sh_json_as_string(sh_json_array_get(cells, c), "");
                if (val[0] == '\0') empty_count++;
                else nonempty_count++;
            }

            /* Partial row: has both empty and non-empty cells */
            if (empty_count > 0 && nonempty_count > 0 && empty_count >= nonempty_count) {
                partial_count++;
                /* Track which columns are empty in partial rows */
                for (size_t c = 0; c < ncols && c < row_ncols; c++) {
                    const char *val = sh_json_as_string(sh_json_array_get(cells, c), "");
                    if (val[0] == '\0') partial_empty[c]++;
                }
            }
        }

        /* Pass 2: key columns are those empty in >90% of partial rows */
        if (partial_count > 0 && (double)partial_count / (double)nrows >= 0.05) {
            for (size_t c = 0; c < ncols; c++) {
                double empty_rate = (double)partial_empty[c] / (double)partial_count;
                if (empty_rate >= 0.90 && cont_key_count < MAX_COLS) {
                    cont_key_cols[cont_key_count++] = (int)c;
                }
            }

            /* Verify: count rows where ALL key cols are empty AND some other has data */
            if (cont_key_count > 0) {
                int verified_count = 0;
                for (size_t r = 0; r < nrows; r++) {
                    ShJsonValue *row = sh_json_array_get(rows_arr, r);
                    ShJsonValue *cells = row ? sh_json_get(row, "cells") : NULL;
                    if (!cells) continue;

                    size_t row_ncols = sh_json_array_len(cells);
                    bool all_keys_empty = true;
                    for (int k = 0; k < cont_key_count; k++) {
                        int ci = cont_key_cols[k];
                        if (ci >= (int)row_ncols) continue;
                        const char *val = sh_json_as_string(
                            sh_json_array_get(cells, ci), "");
                        if (val[0] != '\0') { all_keys_empty = false; break; }
                    }
                    if (!all_keys_empty) continue;

                    bool has_other = false;
                    for (size_t c = 0; c < row_ncols; c++) {
                        const char *val = sh_json_as_string(
                            sh_json_array_get(cells, c), "");
                        if (val[0] != '\0') { has_other = true; break; }
                    }
                    if (has_other) verified_count++;
                }

                if ((double)verified_count / (double)nrows >= 0.05) {
                    emit_row_merge = true;
                } else {
                    cont_key_count = 0;
                }
            }
        }
    }

    /* Detect lat/lon */
    int lat_idx = -1, lon_idx = -1;
    detect_lat_lon(cols, (int)ncols, &lat_idx, &lon_idx);

    /* Generate target names */
    char targets[MAX_COLS][64];
    for (size_t c = 0; c < ncols; c++) {
        if ((int)c == lat_idx) {
            strcpy(targets[c], "lat");
        } else if ((int)c == lon_idx) {
            strcpy(targets[c], "lon");
        } else {
            slugify_header(cols[c].header, targets[c], sizeof(targets[c]));
        }
    }

    /* Deduplicate target names by appending _N */
    for (size_t c = 0; c < ncols; c++) {
        for (size_t j = 0; j < c; j++) {
            if (strcmp(targets[c], targets[j]) == 0) {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%s_%zu", targets[c], c);
                strcpy(targets[c], tmp);
                break;
            }
        }
    }

    /* Find row ID candidate: first unique string column */
    int row_id_col = -1;
    for (size_t c = 0; c < ncols; c++) {
        if (infer_type(&cols[c]) == COL_STRING && cols[c].unique && cols[c].non_empty > 0) {
            row_id_col = (int)c;
            break;
        }
    }

    /* Find unique columns for validation */
    /* Find columns that should be required: non-empty in all rows */

    /* ── Build schema JSON ─────────────────────────────── */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "nx_schema", 1);
    sh_json_write_kv_string(&w, "version", "auto-discovered");
    sh_json_write_kv_string(&w, "output_type", "generic");

    /* table_selector */
    sh_json_write_key(&w, "table_selector");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "index", 0);
    sh_json_write_object_end(&w);

    sh_json_write_kv_int(&w, "skip_rows", 0);

    /* row_merge (if continuation pattern detected) */
    if (emit_row_merge) {
        sh_json_write_key(&w, "row_merge");
        sh_json_write_object_start(&w);
        sh_json_write_key(&w, "key_columns");
        sh_json_write_array_start(&w);
        for (int k = 0; k < cont_key_count; k++)
            sh_json_write_int(&w, cont_key_cols[k]);
        sh_json_write_array_end(&w);
        sh_json_write_kv_string(&w, "separator", " ");
        sh_json_write_object_end(&w);
    }

    /* columns */
    sh_json_write_key(&w, "columns");
    sh_json_write_array_start(&w);

    for (size_t c = 0; c < ncols; c++) {
        ColProfile *p = &cols[c];
        ColType type = infer_type(p);
        bool required = p->non_empty == p->total && p->total > 0;
        bool is_lat = ((int)c == lat_idx);
        bool is_lon = ((int)c == lon_idx);

        write_column(&w, p, (int)c, targets[c], type, required, is_lat, is_lon);
    }

    sh_json_write_array_end(&w);

    /* derived (empty) */
    sh_json_write_key(&w, "derived");
    sh_json_write_array_start(&w);
    sh_json_write_array_end(&w);

    /* row_id */
    if (row_id_col >= 0) {
        sh_json_write_key(&w, "row_id");
        sh_json_write_object_start(&w);
        char tmpl[128];
        snprintf(tmpl, sizeof(tmpl), "{%s}", targets[row_id_col]);
        sh_json_write_kv_string(&w, "template", tmpl);
        sh_json_write_kv_bool(&w, "slugify", true);
        sh_json_write_object_end(&w);
    }

    /* validate (geo_bounds + unique) */
    bool has_geo = (lat_idx >= 0 && lon_idx >= 0);
    bool has_unique = false;
    for (size_t c = 0; c < ncols; c++) {
        if (cols[c].unique && cols[c].non_empty > 0 && infer_type(&cols[c]) == COL_STRING) {
            has_unique = true;
            break;
        }
    }

    if (has_geo || has_unique) {
        sh_json_write_key(&w, "validate");
        sh_json_write_array_start(&w);

        if (has_geo) {
            double lat_range = cols[lat_idx].max_val - cols[lat_idx].min_val;
            double lon_range = cols[lon_idx].max_val - cols[lon_idx].min_val;
            double lat_pad = lat_range * GEO_BOUNDS_PAD;
            double lon_pad = lon_range * GEO_BOUNDS_PAD;
            if (lat_pad < 0.1) lat_pad = 0.1;
            if (lon_pad < 0.1) lon_pad = 0.1;

            sh_json_write_object_start(&w);
            sh_json_write_kv_string(&w, "type", "geo_bounds");
            sh_json_write_kv_string(&w, "lat_field", "lat");
            sh_json_write_kv_string(&w, "lon_field", "lon");
            sh_json_write_key(&w, "bounds");
            sh_json_write_object_start(&w);
            sh_json_write_kv_double_fmt(&w, "min_lat", cols[lat_idx].min_val - lat_pad, 4);
            sh_json_write_kv_double_fmt(&w, "max_lat", cols[lat_idx].max_val + lat_pad, 4);
            sh_json_write_kv_double_fmt(&w, "min_lon", cols[lon_idx].min_val - lon_pad, 4);
            sh_json_write_kv_double_fmt(&w, "max_lon", cols[lon_idx].max_val + lon_pad, 4);
            sh_json_write_object_end(&w);
            sh_json_write_kv_string(&w, "severity", "error");
            sh_json_write_object_end(&w);
        }

        if (has_unique) {
            for (size_t c = 0; c < ncols; c++) {
                if (!cols[c].unique || cols[c].non_empty == 0) continue;
                if (infer_type(&cols[c]) != COL_STRING) continue;

                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "type", "unique");
                sh_json_write_key(&w, "fields");
                sh_json_write_array_start(&w);
                sh_json_write_string(&w, targets[c]);
                sh_json_write_array_end(&w);
                sh_json_write_kv_string(&w, "severity", "error");
                sh_json_write_object_end(&w);
                break; /* Only first unique column */
            }
        }

        sh_json_write_array_end(&w);
    }

    sh_json_write_object_end(&w);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_DISCOVER_ERR_ARENA;
    }

    *out_json = sh_json_buf_take(&jb);
    *out_len = strlen(*out_json);

    return NX_DISCOVER_OK;
}
