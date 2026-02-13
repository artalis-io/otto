/*
 * nx_merge.c - Continuation Row Merging
 *
 * Merges continuation rows (key columns empty) into their parent data rows.
 * Runs between Stage A (extraction) and Stage B (transform).
 */

#include "nx_merge.h"
#include "sh_json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#define MAX_MERGE_COLS     128
#define MAX_MERGE_CELL_LEN 1024
#define MAX_KEY_COLUMNS    16

/* ============================================================================
 * Internal Types
 * ============================================================================ */

typedef struct {
    int  key_columns[MAX_KEY_COLUMNS];
    int  key_count;
    char separator[64];
    char strip_pattern[256];
} NxMergeConfig;

/* ============================================================================
 * Config Parsing
 * ============================================================================ */

/* Parse "row_merge" from schema. Returns false if not present. */
static bool parse_merge_config(ShJsonValue *schema, NxMergeConfig *cfg)
{
    ShJsonValue *rm = sh_json_get(schema, "row_merge");
    if (!rm) return false;

    /* key_columns (required) */
    ShJsonValue *keys = sh_json_get(rm, "key_columns");
    if (!keys) return false;

    cfg->key_count = 0;
    size_t nkeys = sh_json_array_len(keys);
    for (size_t i = 0; i < nkeys && cfg->key_count < MAX_KEY_COLUMNS; i++) {
        int col = (int)sh_json_as_int(sh_json_array_get(keys, i), -1);
        if (col >= 0) {
            cfg->key_columns[cfg->key_count++] = col;
        }
    }
    if (cfg->key_count == 0) return false;

    /* separator (optional, default " ") */
    const char *sep = sh_json_as_string(sh_json_get(rm, "separator"), " ");
    snprintf(cfg->separator, sizeof(cfg->separator), "%s", sep);

    /* strip_pattern (optional) */
    const char *strip = sh_json_as_string(sh_json_get(rm, "strip_pattern"), "");
    snprintf(cfg->strip_pattern, sizeof(cfg->strip_pattern), "%s", strip);

    return true;
}

/* ============================================================================
 * Row Classification
 * ============================================================================ */

/* Check if row should be stripped (page header repeat) */
static bool should_strip(const ShJsonValue *cells, size_t ncells,
                         const char *pattern)
{
    if (pattern[0] == '\0') return false;
    size_t plen = strlen(pattern);

    for (size_t c = 0; c < ncells; c++) {
        const char *val = sh_json_as_string(sh_json_array_get(cells, c), "");
        size_t vlen = strlen(val);
        if (vlen < plen) continue;
        /* Substring search */
        for (size_t i = 0; i <= vlen - plen; i++) {
            if (memcmp(val + i, pattern, plen) == 0) return true;
        }
    }
    return false;
}

/* Check if row is a continuation (all key columns empty) */
static bool is_continuation(const ShJsonValue *cells, size_t ncells,
                            const NxMergeConfig *cfg)
{
    for (int k = 0; k < cfg->key_count; k++) {
        int col = cfg->key_columns[k];
        if (col >= (int)ncells) continue;
        const char *val = sh_json_as_string(sh_json_array_get(cells, col), "");
        if (val[0] != '\0') return false;
    }
    return true;
}

/* ============================================================================
 * Cell Buffer
 * ============================================================================ */

typedef struct {
    char   cells[MAX_MERGE_COLS][MAX_MERGE_CELL_LEN];
    int    ncols;
    int    row_num;      /* Original row number of the parent */
    bool   active;       /* Has an active parent row */
} ParentRow;

static void parent_init(ParentRow *p)
{
    memset(p, 0, sizeof(*p));
}

static void parent_load(ParentRow *p, const ShJsonValue *cells, size_t ncells,
                        int row_num)
{
    p->ncols = (int)ncells;
    if (p->ncols > MAX_MERGE_COLS) p->ncols = MAX_MERGE_COLS;
    for (int c = 0; c < p->ncols; c++) {
        const char *val = sh_json_as_string(sh_json_array_get(cells, c), "");
        snprintf(p->cells[c], MAX_MERGE_CELL_LEN, "%s", val);
    }
    p->row_num = row_num;
    p->active = true;
}

static void parent_merge(ParentRow *p, const ShJsonValue *cells, size_t ncells,
                         const char *separator)
{
    if (!p->active) return;

    int merge_cols = (int)ncells;
    if (merge_cols > p->ncols) merge_cols = p->ncols;
    if (merge_cols > MAX_MERGE_COLS) merge_cols = MAX_MERGE_COLS;

    for (int c = 0; c < merge_cols; c++) {
        const char *val = sh_json_as_string(sh_json_array_get(cells, c), "");
        if (val[0] == '\0') continue; /* Empty cell: don't append */

        size_t plen = strlen(p->cells[c]);
        if (plen == 0) {
            /* Parent cell was empty, just copy */
            snprintf(p->cells[c], MAX_MERGE_CELL_LEN, "%s", val);
        } else {
            /* Append with separator */
            size_t remain = MAX_MERGE_CELL_LEN - plen - 1;
            size_t seplen = strlen(separator);
            if (remain > seplen) {
                memcpy(p->cells[c] + plen, separator, seplen);
                snprintf(p->cells[c] + plen + seplen,
                         MAX_MERGE_CELL_LEN - plen - seplen, "%s", val);
            }
        }
    }
}

/* ============================================================================
 * JSON Output
 * ============================================================================ */

static void flush_parent(ShJsonWriter *w, const ParentRow *p, int *out_row,
                         bool *first_row)
{
    if (!p->active) return;

    if (!*first_row) {
        /* Comma handled by json writer */
    }
    *first_row = false;

    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "row", ++(*out_row));
    sh_json_write_key(w, "cells");
    sh_json_write_array_start(w);
    for (int c = 0; c < p->ncols; c++) {
        sh_json_write_string(w, p->cells[c]);
    }
    sh_json_write_array_end(w);
    sh_json_write_object_end(w);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char *nx_merge_status_str(NxMergeStatus status)
{
    switch (status) {
        case NX_MERGE_OK:           return "OK";
        case NX_MERGE_ERR_NULL:     return "NULL input";
        case NX_MERGE_ERR_JSON:     return "Invalid JSON";
        case NX_MERGE_ERR_SCHEMA:   return "Invalid schema";
        case NX_MERGE_ERR_NO_TABLE: return "No tables in raw JSON";
        case NX_MERGE_ERR_ARENA:    return "Arena allocation failure";
        default:                    return "Unknown error";
    }
}

NxMergeStatus nx_merge_rows(const char *raw_json, size_t raw_len,
                            const char *schema_json, size_t schema_len,
                            SHArena *arena,
                            char **out_json, size_t *out_len)
{
    if (!raw_json || !schema_json || !out_json || !out_len)
        return NX_MERGE_ERR_NULL;

    *out_json = NULL;
    *out_len = 0;

    /* Parse schema to check for row_merge config */
    ShJsonValue *schema_root = NULL;
    ShJsonStatus sst = sh_json_parse(schema_json, schema_len, arena, &schema_root);
    if (sst != SH_JSON_OK || !schema_root)
        return NX_MERGE_ERR_SCHEMA;

    NxMergeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (!parse_merge_config(schema_root, &cfg)) {
        /* No row_merge config — signal no-op */
        return NX_MERGE_OK;
    }

    /* Parse raw JSON */
    ShJsonValue *raw_root = NULL;
    ShJsonStatus rst = sh_json_parse(raw_json, raw_len, arena, &raw_root);
    if (rst != SH_JSON_OK || !raw_root)
        return NX_MERGE_ERR_JSON;

    /* Get tables[0] */
    ShJsonValue *tables = sh_json_get(raw_root, "tables");
    if (!tables || sh_json_array_len(tables) == 0)
        return NX_MERGE_ERR_NO_TABLE;

    ShJsonValue *table = sh_json_array_get(tables, 0);
    if (!table) return NX_MERGE_ERR_NO_TABLE;

    ShJsonValue *rows_arr = sh_json_get(table, "rows");
    if (!rows_arr) return NX_MERGE_ERR_NO_TABLE;

    size_t nrows = sh_json_array_len(rows_arr);

    /* Preserve metadata from the original raw JSON */
    ShJsonValue *source = sh_json_get(raw_root, "source");
    ShJsonValue *headers_arr = sh_json_get(table, "headers");
    ShJsonValue *warnings = sh_json_get(raw_root, "warnings");
    const char *table_name = sh_json_as_string(sh_json_get(table, "name"), "Page1");
    int table_index = (int)sh_json_as_int(sh_json_get(table, "index"), 0);
    int header_row = (int)sh_json_as_int(sh_json_get(table, "header_row"), 0);
    int col_count = (int)sh_json_as_int(sh_json_get(table, "col_count"), 0);

    /* Build merged output JSON */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "nx_raw", 1);

    /* source */
    if (source) {
        sh_json_write_key(&w, "source");
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "filename",
            sh_json_as_string(sh_json_get(source, "filename"), ""));
        sh_json_write_kv_string(&w, "sha256",
            sh_json_as_string(sh_json_get(source, "sha256"), ""));
        sh_json_write_kv_string(&w, "format",
            sh_json_as_string(sh_json_get(source, "format"), ""));
        sh_json_write_object_end(&w);
    }

    /* tables array */
    sh_json_write_key(&w, "tables");
    sh_json_write_array_start(&w);
    sh_json_write_object_start(&w);

    sh_json_write_kv_string(&w, "name", table_name);
    sh_json_write_kv_int(&w, "index", table_index);

    /* headers */
    if (headers_arr) {
        sh_json_write_key(&w, "headers");
        sh_json_write_array_start(&w);
        for (size_t i = 0; i < sh_json_array_len(headers_arr); i++) {
            sh_json_write_string(&w,
                sh_json_as_string(sh_json_array_get(headers_arr, i), ""));
        }
        sh_json_write_array_end(&w);
    }

    sh_json_write_kv_int(&w, "header_row", header_row);

    /* Merged rows */
    sh_json_write_key(&w, "rows");
    sh_json_write_array_start(&w);

    ParentRow parent;
    parent_init(&parent);
    int out_row = 0;
    bool first_row = true;

    for (size_t r = 0; r < nrows; r++) {
        ShJsonValue *row = sh_json_array_get(rows_arr, r);
        ShJsonValue *cells = row ? sh_json_get(row, "cells") : NULL;
        if (!cells) continue;

        size_t ncells = sh_json_array_len(cells);

        /* Strip matching rows */
        if (should_strip(cells, ncells, cfg.strip_pattern))
            continue;

        if (is_continuation(cells, ncells, &cfg)) {
            /* Merge into parent (if exists) */
            if (parent.active) {
                parent_merge(&parent, cells, ncells, cfg.separator);
            }
            /* If no parent yet, discard (continuation before first data row) */
        } else {
            /* Flush previous parent */
            flush_parent(&w, &parent, &out_row, &first_row);
            /* Start new parent */
            int row_num = (int)sh_json_as_int(sh_json_get(row, "row"), 0);
            parent_load(&parent, cells, ncells, row_num);
        }
    }

    /* Flush last parent */
    flush_parent(&w, &parent, &out_row, &first_row);

    sh_json_write_array_end(&w);  /* rows */

    sh_json_write_kv_int(&w, "row_count", out_row);
    sh_json_write_kv_int(&w, "col_count", col_count);

    sh_json_write_object_end(&w);  /* table */
    sh_json_write_array_end(&w);   /* tables */

    /* warnings */
    sh_json_write_key(&w, "warnings");
    if (warnings) {
        sh_json_write_array_start(&w);
        for (size_t i = 0; i < sh_json_array_len(warnings); i++) {
            sh_json_write_string(&w,
                sh_json_as_string(sh_json_array_get(warnings, i), ""));
        }
        sh_json_write_array_end(&w);
    } else {
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
    }

    sh_json_write_object_end(&w);  /* root */

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_MERGE_ERR_ARENA;
    }

    *out_json = sh_json_buf_take(&jb);
    *out_len = strlen(*out_json);

    return NX_MERGE_OK;
}
