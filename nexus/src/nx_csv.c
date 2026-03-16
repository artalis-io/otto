/*
 * nx_csv.c - CSV Parser (Stage 2)
 *
 * Converts CSV data to nx_raw JSON format using sh_csv.
 * Same output format as nx_xlsx and nx_pdf.
 */

#include "nx_csv.h"
#include "sh_csv.h"
#include "sh_json.h"
#include "sh_hash_sha256.h"
#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal: Collect rows from CSV
 * ============================================================================ */

#define INITIAL_ROW_CAP  256
#define INITIAL_COL_CAP  64

typedef struct {
    char **cells;       /* Array of cell strings */
    int    col_count;   /* Number of cells in this row */
} CsvRow;

typedef struct {
    /* Header row */
    char **headers;
    int    header_count;

    /* Data rows */
    CsvRow *rows;
    int     row_count;
    int     row_cap;

    int     max_cols;   /* Widest row seen (including header) */
} CsvTable;

static int ensure_row_cap(CsvTable *t, SHArena *arena)
{
    if (t->row_count >= t->row_cap) {
        int new_cap = t->row_cap * 2;
        CsvRow *new_rows = (CsvRow *)sh_arena_alloc(arena,
            (size_t)new_cap * sizeof(CsvRow));
        if (!new_rows) return -1;
        if (t->rows && t->row_count > 0) {
            memcpy(new_rows, t->rows, (size_t)t->row_count * sizeof(CsvRow));
        }
        t->rows = new_rows;
        t->row_cap = new_cap;
    }
    return 0;
}

static char *arena_strdup(SHArena *arena, const char *data, size_t len)
{
    char *s = (char *)sh_arena_alloc(arena, len + 1);
    if (!s) return NULL;
    memcpy(s, data, len);
    s[len] = '\0';
    return s;
}

static int collect_csv_rows(const char *data, size_t len,
                            const ShCsvOpts *csv_opts,
                            const NxCsvLimits *limits,
                            SHArena *arena, NxIssueList *issues,
                            CsvTable *table)
{
    ShCsvReader r;
    ShCsvOpts opts;

    if (csv_opts) {
        opts = *csv_opts;
    } else {
        sh_csv_opts_default(&opts);
        opts.delimiter = 0;      /* auto-detect */
        opts.has_header = 1;
        opts.trim_fields = 1;
        opts.skip_empty_rows = 1;
    }

    ShCsvStatus cs = sh_csv_init(&r, data, len, &opts, arena);
    if (cs != SH_CSV_OK) return -1;

    /* Initialize table */
    table->headers = NULL;
    table->header_count = 0;
    table->rows = (CsvRow *)sh_arena_alloc(arena,
        (size_t)INITIAL_ROW_CAP * sizeof(CsvRow));
    if (!table->rows) return -1;
    table->row_count = 0;
    table->row_cap = INITIAL_ROW_CAP;
    table->max_cols = 0;

    /* Collect fields for current row */
    char **cur_fields = (char **)sh_arena_alloc(arena,
        (size_t)INITIAL_COL_CAP * sizeof(char *));
    if (!cur_fields) return -1;
    int cur_col_count = 0;
    int cur_col_cap = INITIAL_COL_CAP;
    int is_header = opts.has_header ? 1 : 0;

    ShCsvToken tok;
    ShCsvTokenType type;
    while ((type = sh_csv_next(&r, &tok)) != SH_CSV_TOKEN_EOF) {
        if (type == SH_CSV_TOKEN_FIELD) {
            /* Grow field array if needed */
            if (cur_col_count >= cur_col_cap) {
                int new_cap = cur_col_cap * 2;
                char **new_fields = (char **)sh_arena_alloc(arena,
                    (size_t)new_cap * sizeof(char *));
                if (!new_fields) {
                    if (issues)
                        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                                      -1, "", "arena_exhausted",
                                      "Arena exhausted growing field buffer at row %d",
                                      table->row_count);
                    goto done_csv;
                }
                memcpy(new_fields, cur_fields,
                       (size_t)cur_col_count * sizeof(char *));
                cur_fields = new_fields;
                cur_col_cap = new_cap;
            }

            char *val = arena_strdup(arena, tok.data, tok.len);
            if (!val) {
                if (issues)
                    nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                                  -1, "", "arena_exhausted",
                                  "Arena exhausted storing field at row %d",
                                  table->row_count);
                goto done_csv;
            }
            cur_fields[cur_col_count++] = val;

        } else if (type == SH_CSV_TOKEN_ROW_END) {
            if (cur_col_count == 0) continue; /* skip empty rows */

            if (cur_col_count > table->max_cols)
                table->max_cols = cur_col_count;

            if (is_header) {
                /* Store as header row */
                table->headers = (char **)sh_arena_alloc(arena,
                    (size_t)cur_col_count * sizeof(char *));
                if (!table->headers) {
                    if (issues)
                        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                                      -1, "", "arena_exhausted",
                                      "Arena exhausted storing header row");
                    goto done_csv;
                }
                memcpy(table->headers, cur_fields,
                       (size_t)cur_col_count * sizeof(char *));
                table->header_count = cur_col_count;
                is_header = 0;
            } else {
                /* Check row limit */
                if (limits && limits->max_rows > 0 &&
                    table->row_count >= limits->max_rows)
                    break;

                /* Store as data row */
                if (ensure_row_cap(table, arena) < 0) {
                    if (issues)
                        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                                      -1, "", "arena_exhausted",
                                      "Arena exhausted after %d rows",
                                      table->row_count);
                    goto done_csv;
                }
                CsvRow *row = &table->rows[table->row_count];

                /* Apply column limit */
                int ncols = cur_col_count;
                if (limits && limits->max_cols > 0 && ncols > limits->max_cols)
                    ncols = limits->max_cols;

                row->cells = (char **)sh_arena_alloc(arena,
                    (size_t)ncols * sizeof(char *));
                if (!row->cells) {
                    if (issues)
                        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                                      -1, "", "arena_exhausted",
                                      "Arena exhausted allocating cells at row %d",
                                      table->row_count);
                    goto done_csv;
                }
                memcpy(row->cells, cur_fields,
                       (size_t)ncols * sizeof(char *));
                row->col_count = ncols;
                table->row_count++;
            }

            /* Reset current row */
            cur_col_count = 0;
        }
    }

    /* Handle last row if no trailing newline */
    if (cur_col_count > 0) {
        if (cur_col_count > table->max_cols)
            table->max_cols = cur_col_count;

        if (is_header) {
            table->headers = (char **)sh_arena_alloc(arena,
                (size_t)cur_col_count * sizeof(char *));
            if (!table->headers) goto done_csv;
            memcpy(table->headers, cur_fields,
                   (size_t)cur_col_count * sizeof(char *));
            table->header_count = cur_col_count;
        } else {
            if (!(limits && limits->max_rows > 0 &&
                  table->row_count >= limits->max_rows)) {
                if (ensure_row_cap(table, arena) < 0) goto done_csv;
                CsvRow *row = &table->rows[table->row_count];
                int ncols = cur_col_count;
                if (limits && limits->max_cols > 0 && ncols > limits->max_cols)
                    ncols = limits->max_cols;
                row->cells = (char **)sh_arena_alloc(arena,
                    (size_t)ncols * sizeof(char *));
                if (!row->cells) goto done_csv;
                memcpy(row->cells, cur_fields,
                       (size_t)ncols * sizeof(char *));
                row->col_count = ncols;
                table->row_count++;
            }
        }
    }

done_csv:
    /* Apply column limit to max_cols */
    if (limits && limits->max_cols > 0 && table->max_cols > limits->max_cols)
        table->max_cols = limits->max_cols;

    return 0;
}

/* ============================================================================
 * JSON Output
 * ============================================================================ */

static void write_csv_raw_json(ShJsonWriter *w, const char *filename,
                                const char *sha256_hex,
                                const CsvTable *table)
{
    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "nx_raw", 1);

    /* source */
    sh_json_write_key(w, "source");
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "filename", filename ? filename : "");
    sh_json_write_kv_string(w, "sha256", sha256_hex);
    sh_json_write_kv_string(w, "format", "csv");
    sh_json_write_object_end(w);

    /* tables (single table for CSV) */
    sh_json_write_key(w, "tables");
    sh_json_write_array_start(w);

    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "name", "Sheet1");
    sh_json_write_kv_int(w, "index", 0);

    /* headers */
    sh_json_write_key(w, "headers");
    sh_json_write_array_start(w);
    int max_cols = table->max_cols;
    for (int j = 0; j < max_cols; j++) {
        const char *val = (table->headers && j < table->header_count)
            ? table->headers[j] : "";
        sh_json_write_string(w, val);
    }
    sh_json_write_array_end(w);

    sh_json_write_kv_int(w, "header_row", 0);

    /* rows */
    sh_json_write_key(w, "rows");
    sh_json_write_array_start(w);
    for (int i = 0; i < table->row_count; i++) {
        const CsvRow *row = &table->rows[i];

        sh_json_write_object_start(w);
        sh_json_write_kv_int(w, "row", i + 1); /* 1-based (header is row 0) */
        sh_json_write_key(w, "cells");
        sh_json_write_array_start(w);
        for (int j = 0; j < max_cols; j++) {
            const char *val = (j < row->col_count && row->cells[j])
                ? row->cells[j] : "";
            sh_json_write_string(w, val);
        }
        sh_json_write_array_end(w);
        sh_json_write_object_end(w);
    }
    sh_json_write_array_end(w);

    sh_json_write_kv_int(w, "row_count", table->row_count);
    sh_json_write_kv_int(w, "col_count", max_cols);
    sh_json_write_object_end(w);

    sh_json_write_array_end(w); /* end tables */

    /* warnings */
    sh_json_write_key(w, "warnings");
    sh_json_write_array_start(w);
    sh_json_write_array_end(w);

    sh_json_write_object_end(w);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char *nx_csv_status_str(NxCsvStatus status)
{
    switch (status) {
        case NX_CSV_OK:         return "OK";
        case NX_CSV_ERR_NULL:   return "NULL input";
        case NX_CSV_ERR_PARSE:  return "CSV parse error";
        case NX_CSV_ERR_NO_DATA: return "No data rows found";
        case NX_CSV_ERR_ARENA:  return "Arena allocation failure";
        case NX_CSV_ERR_EMPTY:  return "No data rows found (empty document)";
        default:                return "Unknown error";
    }
}

NxCsvStatus nx_csv_parse(const char *data, size_t len,
                          const ShCsvOpts *csv_opts,
                          const NxCsvLimits *limits,
                          const char *filename,
                          SHArena *arena, NxIssueList *issues,
                          char **out_json, size_t *out_len)
{
    if (!data || !out_json || !out_len) return NX_CSV_ERR_NULL;
    if (!arena) return NX_CSV_ERR_ARENA;

    *out_json = NULL;
    *out_len = 0;

    /* Compute SHA-256 */
    char sha256_hex[65];
    sh_sha256_hex(data, len, sha256_hex);

    /* Parse CSV into table structure */
    CsvTable table = {0};
    if (collect_csv_rows(data, len, csv_opts, limits, arena, issues, &table) < 0) {
        return NX_CSV_ERR_PARSE;
    }

    if (table.row_count == 0 && table.header_count == 0) {
        return NX_CSV_ERR_NO_DATA;
    }

    /* Generate JSON */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    write_csv_raw_json(&w, filename, sha256_hex, &table);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_CSV_ERR_ARENA;
    }

    *out_json = sh_json_buf_take(&jb);
    if (*out_json)
        *out_len = strlen(*out_json);

    return NX_CSV_OK;
}
