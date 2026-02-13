/*
 * nx_pdf.c - PDF Table Reconstructor (Stage A)
 *
 * Reconstructs tables from positioned text runs by clustering on
 * y-coordinates (rows) and detecting column boundaries via x-gaps.
 *
 * Algorithm:
 * 1. Parse text-run JSON from pdf-to-text-json.py
 * 2. Sort text runs by y-coordinate (top to bottom)
 * 3. Cluster into rows: runs within row_tolerance of same y
 * 4. Within each row, sort by x-coordinate (left to right)
 * 5. Detect column boundaries from consistent x-gaps
 * 6. Build cell grid and emit raw rows JSON
 */

#include "nx_pdf.h"
#include "sh_json.h"
#include "sh_hash_sha256.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

#define MAX_TEXT_RUNS  16384
#define MAX_ROWS       4096
#define MAX_COLS        256
#define MAX_TEXT_LEN    1024

typedef struct {
    double x, y, w, h;
    const char *text;
    size_t text_len;
    int page;
} TextRun;

typedef struct {
    TextRun *runs;
    int count;
    double y_mid;     /* Average y of all runs in this row */
} ClusterRow;

/* ============================================================================
 * Sorting
 * ============================================================================ */

static int cmp_by_y(const void *a, const void *b)
{
    const TextRun *ra = (const TextRun *)a;
    const TextRun *rb = (const TextRun *)b;
    if (ra->y < rb->y) return -1;
    if (ra->y > rb->y) return 1;
    if (ra->x < rb->x) return -1;
    if (ra->x > rb->x) return 1;
    return 0;
}

static int cmp_by_x(const void *a, const void *b)
{
    const TextRun *ra = (const TextRun *)a;
    const TextRun *rb = (const TextRun *)b;
    if (ra->x < rb->x) return -1;
    if (ra->x > rb->x) return 1;
    return 0;
}

/* ============================================================================
 * Text Run Parsing
 * ============================================================================ */

static int parse_text_runs(const char *json, size_t json_len,
                           SHArena *arena, TextRun *runs, int max_runs)
{
    ShJsonValue *root = NULL;
    if (sh_json_parse(json, json_len, arena, &root) != SH_JSON_OK)
        return -1;

    ShJsonValue *pages = sh_json_get(root, "pages");
    if (!pages) return -1;

    int count = 0;
    size_t npages = sh_json_array_len(pages);

    for (size_t p = 0; p < npages && count < max_runs; p++) {
        ShJsonValue *page = sh_json_array_get(pages, p);
        int page_num = sh_json_as_int(sh_json_get(page, "page"), (int)p + 1);

        ShJsonValue *texts = sh_json_get(page, "texts");
        if (!texts) continue;

        size_t ntexts = sh_json_array_len(texts);
        for (size_t t = 0; t < ntexts && count < max_runs; t++) {
            ShJsonValue *txt = sh_json_array_get(texts, t);

            const char *text = sh_json_as_string(sh_json_get(txt, "text"), "");
            if (!text[0]) continue; /* Skip empty text */

            runs[count].text = text;
            runs[count].text_len = strlen(text);
            runs[count].x = sh_json_as_double(sh_json_get(txt, "x"), 0);
            runs[count].y = sh_json_as_double(sh_json_get(txt, "y"), 0);
            runs[count].w = sh_json_as_double(sh_json_get(txt, "w"), 0);
            runs[count].h = sh_json_as_double(sh_json_get(txt, "h"), 0);
            runs[count].page = page_num;
            count++;
        }
    }

    return count;
}

/* ============================================================================
 * Row Clustering
 * ============================================================================ */

static int cluster_rows(TextRun *runs, int count, double tolerance,
                        SHArena *arena, ClusterRow *rows, int max_rows)
{
    if (count == 0) return 0;

    /* Sort by y */
    qsort(runs, (size_t)count, sizeof(TextRun), cmp_by_y);

    int nrows = 0;
    int i = 0;

    while (i < count && nrows < max_rows) {
        /* Start a new row cluster */
        ClusterRow *cr = &rows[nrows];
        int start = i;
        double y_sum = runs[i].y;
        i++;

        /* Absorb runs with similar y */
        while (i < count) {
            double dy = runs[i].y - runs[i - 1].y;
            if (dy < 0) dy = -dy;
            if (dy > tolerance) break;
            y_sum += runs[i].y;
            i++;
        }

        int run_count = i - start;
        cr->runs = (TextRun *)sh_arena_alloc(arena,
            (size_t)run_count * sizeof(TextRun));
        if (!cr->runs) return -1;
        memcpy(cr->runs, runs + start, (size_t)run_count * sizeof(TextRun));
        cr->count = run_count;
        cr->y_mid = y_sum / run_count;

        /* Sort this row's runs by x */
        qsort(cr->runs, (size_t)cr->count, sizeof(TextRun), cmp_by_x);

        nrows++;
    }

    return nrows;
}

/* ============================================================================
 * Column Detection
 * ============================================================================ */

/* Detect column boundaries by finding consistent x-positions across rows */
static int detect_columns(ClusterRow *rows, int nrows, double col_gap_min,
                          double *col_starts, int max_cols)
{
    if (nrows == 0) return 0;

    /* Collect all x-positions */
    double all_x[MAX_TEXT_RUNS];
    int nx = 0;

    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < rows[r].count && nx < MAX_TEXT_RUNS; c++) {
            all_x[nx++] = rows[r].runs[c].x;
        }
    }

    if (nx == 0) return 0;

    /* Sort x-positions */
    for (int i = 1; i < nx; i++) {
        double key = all_x[i];
        int j = i - 1;
        while (j >= 0 && all_x[j] > key) {
            all_x[j + 1] = all_x[j];
            j--;
        }
        all_x[j + 1] = key;
    }

    /* Cluster x-positions into columns */
    int ncols = 0;
    col_starts[ncols++] = all_x[0];

    for (int i = 1; i < nx && ncols < max_cols; i++) {
        if (all_x[i] - all_x[i - 1] > col_gap_min) {
            col_starts[ncols++] = all_x[i];
        }
    }

    return ncols;
}

/* Find which column a text run belongs to */
static int find_column(double x, const double *col_starts, int ncols)
{
    /* Find the column whose start is closest to (but not after) x */
    int best = 0;
    for (int c = 1; c < ncols; c++) {
        if (col_starts[c] <= x + 1.0) /* small tolerance */
            best = c;
        else
            break;
    }
    return best;
}

/* ============================================================================
 * JSON Output
 * ============================================================================ */

static void write_pdf_raw_json(ShJsonWriter *w, const char *filename,
                               const char *sha256_hex,
                               ClusterRow *rows, int nrows,
                               const double *col_starts, int ncols)
{
    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "nx_raw", 1);

    /* source */
    sh_json_write_key(w, "source");
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "filename", filename ? filename : "");
    sh_json_write_kv_string(w, "sha256", sha256_hex);
    sh_json_write_kv_string(w, "format", "pdf");
    sh_json_write_object_end(w);

    /* tables - single table from PDF */
    sh_json_write_key(w, "tables");
    sh_json_write_array_start(w);
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "name", "Page1");
    sh_json_write_kv_int(w, "index", 0);

    /* Headers from first row */
    sh_json_write_key(w, "headers");
    sh_json_write_array_start(w);
    if (nrows > 0) {
        for (int c = 0; c < ncols; c++) {
            /* Find text in first row for this column */
            const char *hdr = "";
            for (int t = 0; t < rows[0].count; t++) {
                if (find_column(rows[0].runs[t].x, col_starts, ncols) == c) {
                    hdr = rows[0].runs[t].text;
                    break;
                }
            }
            sh_json_write_string(w, hdr);
        }
    }
    sh_json_write_array_end(w);
    sh_json_write_kv_int(w, "header_row", 0);

    /* Data rows (skip header) */
    sh_json_write_key(w, "rows");
    sh_json_write_array_start(w);
    int data_rows = 0;
    for (int r = 1; r < nrows; r++) {
        sh_json_write_object_start(w);
        sh_json_write_kv_int(w, "row", r);
        sh_json_write_key(w, "cells");
        sh_json_write_array_start(w);

        for (int c = 0; c < ncols; c++) {
            /* Concatenate all text runs in this cell */
            char cell_buf[MAX_TEXT_LEN] = "";
            int cell_len = 0;

            for (int t = 0; t < rows[r].count; t++) {
                if (find_column(rows[r].runs[t].x, col_starts, ncols) == c) {
                    if (cell_len > 0 && cell_len < (int)sizeof(cell_buf) - 2) {
                        cell_buf[cell_len++] = ' ';
                    }
                    int avail = (int)sizeof(cell_buf) - cell_len - 1;
                    int copy = (int)rows[r].runs[t].text_len;
                    if (copy > avail) copy = avail;
                    memcpy(cell_buf + cell_len, rows[r].runs[t].text, (size_t)copy);
                    cell_len += copy;
                }
            }
            cell_buf[cell_len] = '\0';
            sh_json_write_string(w, cell_buf);
        }

        sh_json_write_array_end(w);
        sh_json_write_object_end(w);
        data_rows++;
    }
    sh_json_write_array_end(w);

    sh_json_write_kv_int(w, "row_count", data_rows);
    sh_json_write_kv_int(w, "col_count", ncols);
    sh_json_write_object_end(w);
    sh_json_write_array_end(w);

    /* warnings */
    sh_json_write_key(w, "warnings");
    sh_json_write_array_start(w);
    sh_json_write_array_end(w);

    sh_json_write_object_end(w);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char *nx_pdf_status_str(NxPdfStatus status)
{
    switch (status) {
        case NX_PDF_OK:        return "OK";
        case NX_PDF_ERR_NULL:  return "NULL input";
        case NX_PDF_ERR_JSON:  return "Invalid JSON input";
        case NX_PDF_ERR_NO_TEXT: return "No text runs found";
        case NX_PDF_ERR_ARENA: return "Arena allocation failure";
        default:               return "Unknown error";
    }
}

NxPdfStatus nx_pdf_extract_tables(const char *json_data, size_t json_len,
                                  const NxPdfOptions *opts, const char *filename,
                                  SHArena *arena, char **out_json, size_t *out_len)
{
    NxPdfOptions default_opts = NX_PDF_DEFAULT_OPTIONS;

    if (!json_data || !out_json || !out_len) return NX_PDF_ERR_NULL;
    if (!arena) return NX_PDF_ERR_ARENA;
    if (!opts) opts = &default_opts;

    *out_json = NULL;
    *out_len = 0;

    /* Compute SHA-256 of input */
    char sha256_hex[65];
    sh_sha256_hex(json_data, json_len, sha256_hex);

    /* Parse text runs */
    TextRun *runs = (TextRun *)sh_arena_alloc(arena,
        MAX_TEXT_RUNS * sizeof(TextRun));
    if (!runs) return NX_PDF_ERR_ARENA;

    int nruns = parse_text_runs(json_data, json_len, arena, runs, MAX_TEXT_RUNS);
    if (nruns < 0) return NX_PDF_ERR_JSON;
    if (nruns == 0) return NX_PDF_ERR_NO_TEXT;

    /* Cluster into rows */
    ClusterRow *cluster_rows_arr = (ClusterRow *)sh_arena_alloc(arena,
        MAX_ROWS * sizeof(ClusterRow));
    if (!cluster_rows_arr) return NX_PDF_ERR_ARENA;

    int nrows = cluster_rows(runs, nruns, opts->row_tolerance,
                             arena, cluster_rows_arr, MAX_ROWS);
    if (nrows < 0) return NX_PDF_ERR_ARENA;
    if (nrows == 0) return NX_PDF_ERR_NO_TEXT;

    /* Detect columns */
    double col_starts[MAX_COLS];
    int ncols = detect_columns(cluster_rows_arr, nrows, opts->col_gap_min,
                               col_starts, MAX_COLS);
    if (ncols == 0) return NX_PDF_ERR_NO_TEXT;

    /* Generate JSON */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    write_pdf_raw_json(&w, filename, sha256_hex,
                       cluster_rows_arr, nrows, col_starts, ncols);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_PDF_ERR_ARENA;
    }

    *out_json = sh_json_buf_take(&jb);
    if (*out_json)
        *out_len = strlen(*out_json);

    return NX_PDF_OK;
}
