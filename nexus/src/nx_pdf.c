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

#define MAX_TEXT_RUNS  65536
#define MAX_ROWS       8192
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
    /* Sort by page first, then y, then x */
    if (ra->page < rb->page) return -1;
    if (ra->page > rb->page) return 1;
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
 * Auto-Detection of Clustering Parameters
 * ============================================================================ */

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/*
 * Compute median text height from parsed text runs.
 * Returns 0 if no valid heights found.
 */
static double compute_median_height(TextRun *runs, int count, SHArena *arena)
{
    if (count == 0) return 0;

    double *heights = (double *)sh_arena_alloc(arena,
        (size_t)count * sizeof(double));
    if (!heights) return 0;

    int nh = 0;
    for (int i = 0; i < count; i++) {
        if (runs[i].h > 0.1) /* Skip degenerate heights */
            heights[nh++] = runs[i].h;
    }

    if (nh == 0) return 0;

    qsort(heights, (size_t)nh, sizeof(double), cmp_double);
    return heights[nh / 2]; /* Median */
}

/*
 * Auto-detect row_tolerance from text run statistics.
 *
 * Row tolerance uses the median text height heuristic.
 * Column gap detection uses the percentile-gap algorithm in detect_columns()
 * when col_gap_min remains negative (auto mode).
 */
static void auto_detect_options(TextRun *runs, int count, SHArena *arena,
                                NxPdfOptions *opts)
{
    double median_h = compute_median_height(runs, count, arena);

    if (median_h < 0.5) {
        /* Fallback to defaults if heights are too small/missing */
        if (opts->row_tolerance < 0) opts->row_tolerance = 3.0;
        /* col_gap_min stays negative → detect_columns() will use adaptive P75 */
        return;
    }

    if (opts->row_tolerance < 0)
        opts->row_tolerance = 0.7 * median_h;
    /* col_gap_min stays negative → detect_columns() will use adaptive P75 */
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

        /* Absorb runs with similar y on the same page */
        while (i < count) {
            if (runs[i].page != runs[i - 1].page) break; /* Page boundary */
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
 * Run Splitting — Break wide text runs at multi-space gaps
 * ============================================================================ */

/*
 * Some PDFs emit a single text run for what should be multiple columns,
 * using internal whitespace (3+ consecutive spaces) to separate values.
 * This pass splits such runs into separate TextRuns with estimated
 * x-positions so that column detection can properly separate them.
 *
 * Example: "68 7 308               21 6 35247.2901607" at x=152.5, w=23.1
 * → split into "68 7 308", "21 6 352", "47.2901607" as separate runs.
 */
static int split_wide_runs(ClusterRow *rows, int nrows, SHArena *arena,
                           int *total_runs)
{
    int added = 0;

    for (int r = 0; r < nrows; r++) {
        ClusterRow *cr = &rows[r];
        int orig_count = cr->count;
        /* Max fragments: each run could split into many pieces */
        int max_new = orig_count * 8;
        TextRun *new_runs = (TextRun *)sh_arena_alloc(arena,
            (size_t)max_new * sizeof(TextRun));
        if (!new_runs) continue;
        int nout = 0;

        for (int i = 0; i < orig_count && nout < max_new; i++) {
            TextRun *run = &cr->runs[i];
            const char *text = run->text;
            int len = (int)run->text_len;

            /* Scan for runs of 3+ spaces */
            int frag_start = 0;
            int in_space = 0;
            int space_start = 0;
            int nsplits = 0;

            for (int j = 0; j <= len && nout < max_new; j++) {
                if (j < len && text[j] == ' ') {
                    if (!in_space) {
                        space_start = j;
                        in_space = 1;
                    }
                } else {
                    if (in_space && (j - space_start) >= 3) {
                        /* Split here: emit fragment before the space run */
                        int frag_len = space_start - frag_start;
                        if (frag_len > 0) {
                            /* Estimate x offset proportionally */
                            double frac_start = (double)frag_start / len;
                            double frac_end = (double)space_start / len;
                            TextRun *nr = &new_runs[nout++];
                            *nr = *run; /* Copy base properties */
                            nr->x = run->x + frac_start * run->w;
                            nr->w = (frac_end - frac_start) * run->w;
                            nr->text = text + frag_start;
                            nr->text_len = (size_t)frag_len;
                            nsplits++;
                        }
                        frag_start = j; /* Start next fragment after the space run */
                    }
                    in_space = 0;
                }
            }

            /* Emit final fragment */
            if (nsplits > 0) {
                int frag_len = len - frag_start;
                if (frag_len > 0 && nout < max_new) {
                    double frac_start = (double)frag_start / len;
                    TextRun *nr = &new_runs[nout++];
                    *nr = *run;
                    nr->x = run->x + frac_start * run->w;
                    nr->w = run->w * (1.0 - frac_start);
                    nr->text = text + frag_start;
                    nr->text_len = (size_t)frag_len;
                }
                added += nsplits; /* We replaced 1 run with nsplits+1 */
            } else {
                /* No splits needed, keep original */
                new_runs[nout++] = *run;
            }
        }

        if (nout != orig_count) {
            cr->runs = new_runs;
            cr->count = nout;
            /* Re-sort by x after splitting */
            qsort(cr->runs, (size_t)cr->count, sizeof(TextRun), cmp_by_x);
        }
    }

    *total_runs += added;
    return added;
}

/*
 * Split data runs that span multiple column boundaries.
 * Uses detected columns (from header) to split proportionally.
 * This handles the case where the PDF encodes separate column values
 * as a single text run with fine kerning (no whitespace separator).
 */
static void split_cross_column_runs(ClusterRow *rows, int nrows,
                                     const double *col_starts, int ncols,
                                     SHArena *arena, int *total_runs)
{
    int added = 0;

    /* Process all rows — split any that span multiple column boundaries */
    for (int r = 0; r < nrows; r++) {
        ClusterRow *cr = &rows[r];
        int orig_count = cr->count;
        int max_new = orig_count * 4; /* Each run could split into several */
        TextRun *new_runs = (TextRun *)sh_arena_alloc(arena,
            (size_t)max_new * sizeof(TextRun));
        if (!new_runs) continue;
        int nout = 0;

        for (int i = 0; i < orig_count && nout < max_new; i++) {
            TextRun *run = &cr->runs[i];
            double run_left = run->x;
            double run_right = run->x + run->w;
            int len = (int)run->text_len;

            if (run->w <= 0 || len <= 1) {
                new_runs[nout++] = *run;
                continue;
            }

            /* Find all column boundaries that fall within this run */
            int splits[32];
            int nsplits = 0;
            for (int c = 0; c < ncols && nsplits < 30; c++) {
                double cs = col_starts[c];
                /* Column boundary falls inside this run (with some margin) */
                if (cs > run_left + 1.0 && cs < run_right - 1.0) {
                    splits[nsplits++] = c;
                }
            }

            if (nsplits == 0) {
                new_runs[nout++] = *run;
                continue;
            }

            /* Split the text at column boundaries, proportionally */
            double chars_per_unit = (double)len / run->w;
            int prev_char = 0;
            double prev_x = run_left;

            for (int s = 0; s < nsplits && nout < max_new; s++) {
                double split_x = col_starts[splits[s]];
                int char_pos = (int)((split_x - run_left) * chars_per_unit + 0.5);
                if (char_pos <= prev_char) char_pos = prev_char + 1;
                if (char_pos >= len) char_pos = len - 1;

                /* Try to split at a space boundary if one is nearby */
                int best_pos = char_pos;
                for (int d = -3; d <= 3; d++) {
                    int p = char_pos + d;
                    if (p > prev_char && p < len && run->text[p] == ' ') {
                        best_pos = p;
                        break;
                    }
                }
                char_pos = best_pos;

                /* Ensure we don't split in the middle of a UTF-8 sequence */
                while (char_pos > prev_char && char_pos < len &&
                       ((unsigned char)run->text[char_pos] & 0xC0) == 0x80)
                    char_pos++; /* Advance past continuation bytes */

                /* Emit fragment before split point */
                int frag_len = char_pos - prev_char;
                if (frag_len > 0) {
                    /* Ensure fragment doesn't end with an incomplete UTF-8 sequence.
                     * Walk back from the end to find the start of the last multi-byte
                     * char, then check if the full sequence fits within frag_len. */
                    {
                        int pos = prev_char + frag_len - 1;
                        /* Back up past continuation bytes */
                        while (pos > prev_char &&
                               ((unsigned char)run->text[pos] & 0xC0) == 0x80)
                            pos--;
                        unsigned char lead = (unsigned char)run->text[pos];
                        if (lead >= 0xC0) {
                            /* Determine expected sequence length */
                            int seq_len = (lead < 0xE0) ? 2 :
                                          (lead < 0xF0) ? 3 : 4;
                            /* Only strip if the sequence is incomplete */
                            if (pos + seq_len > prev_char + frag_len)
                                frag_len = pos - prev_char;
                        }
                    }
                    /* Trim trailing spaces */
                    while (frag_len > 0 && run->text[prev_char + frag_len - 1] == ' ')
                        frag_len--;
                    if (frag_len > 0) {
                        TextRun *nr = &new_runs[nout++];
                        *nr = *run;
                        nr->x = prev_x;
                        nr->w = split_x - prev_x;
                        nr->text = run->text + prev_char;
                        nr->text_len = (size_t)frag_len;
                    }
                }

                /* Skip leading spaces for next fragment */
                prev_char = char_pos;
                while (prev_char < len && run->text[prev_char] == ' ')
                    prev_char++;
                prev_x = split_x;
            }

            /* Emit final fragment */
            int frag_len = len - prev_char;
            if (frag_len > 0 && nout < max_new) {
                TextRun *nr = &new_runs[nout++];
                *nr = *run;
                nr->x = prev_x;
                nr->w = run_right - prev_x;
                nr->text = run->text + prev_char;
                nr->text_len = (size_t)frag_len;
            }

            added += (nsplits > 0 ? nsplits : 0);
        }

        if (nout != orig_count) {
            cr->runs = new_runs;
            cr->count = nout;
            qsort(cr->runs, (size_t)cr->count, sizeof(TextRun), cmp_by_x);
        }
    }

    *total_runs += added;
}

/* ============================================================================
 * Column Detection
 * ============================================================================ */

/*
 * Detect column boundaries from the header row.
 *
 * Strategy: The header row (row 0) defines the intended column structure.
 * Each text run in the header starts a column. Text runs with x-positions
 * within merge_tol of each other are treated as the same column (handles
 * multi-line headers where rows stack above each other).
 *
 * If col_gap_min is explicitly set (>= 0), falls back to all-rows gap-based
 * detection for backward compatibility.
 */
static int detect_columns(ClusterRow *rows, int nrows, double *col_gap_min,
                          double *col_starts, int max_cols,
                          SHArena *arena, int total_runs)
{
    if (nrows == 0) return 0;

    /* If caller explicitly set col_gap_min, use all-rows gap-based detection */
    if (*col_gap_min >= 0) {
        double *all_x = (double *)sh_arena_alloc(arena,
            (size_t)total_runs * sizeof(double));
        if (!all_x) return 0;
        int nx = 0;

        for (int r = 0; r < nrows; r++) {
            ClusterRow *cr = &rows[r];
            for (int c = 0; c < cr->count && nx < total_runs; c++)
                all_x[nx++] = cr->runs[c].x;
        }
        if (nx == 0) return 0;

        qsort(all_x, (size_t)nx, sizeof(double), cmp_double);

        int ncols = 0;
        col_starts[ncols++] = all_x[0];
        for (int i = 1; i < nx && ncols < max_cols; i++) {
            if (all_x[i] - all_x[i - 1] > *col_gap_min)
                col_starts[ncols++] = all_x[i];
        }
        return ncols;
    }

    /*
     * Adaptive: Find the best reference row to define columns.
     * Instead of assuming row 0 is the header, find the row with the most
     * text runs — this handles both cases:
     *   - PDF01: header IS row 0 (20 runs, more than any data row)
     *   - PDF02: header is at the bottom of the page, but data rows
     *            have 7-8 runs which is still the most populated row type
     * We pick the row with the most runs as the column template.
     */
    int best_row = 0;
    int best_count = rows[0].count;
    for (int r = 1; r < nrows; r++) {
        if (rows[r].count > best_count) {
            best_count = rows[r].count;
            best_row = r;
        }
    }

    ClusterRow *hdr = &rows[best_row];
    if (hdr->count == 0) return 0;

    /* Collect all header x-positions, filtering out corrupted runs */
    double *hdr_x = (double *)sh_arena_alloc(arena,
        (size_t)(hdr->count + 1) * sizeof(double));
    if (!hdr_x) return 0;

    int nhdr = 0;
    for (int i = 0; i < hdr->count; i++) {
        /* Skip runs with negative x positions (off-page) */
        if (hdr->runs[i].x < 0) continue;
        hdr_x[nhdr++] = hdr->runs[i].x;
    }
    if (nhdr == 0) return 0;

    qsort(hdr_x, (size_t)nhdr, sizeof(double), cmp_double);

    /* Merge x-positions that are very close (same column, multi-line header) */
    double merge_tol = 2.0; /* 2pt tolerance for same-column merging */
    int ncols = 0;
    col_starts[ncols++] = hdr_x[0];

    for (int i = 1; i < nhdr && ncols < max_cols; i++) {
        if (hdr_x[i] - hdr_x[i - 1] > merge_tol) {
            col_starts[ncols++] = hdr_x[i];
        }
    }

    /* Write back estimated gap for diagnostics */
    if (ncols >= 2) {
        /* Compute minimum gap between detected columns */
        double min_gap = col_starts[1] - col_starts[0];
        for (int i = 2; i < ncols; i++) {
            double g = col_starts[i] - col_starts[i - 1];
            if (g < min_gap) min_gap = g;
        }
        *col_gap_min = min_gap;
    }

    return ncols;
}

/* Find which column a text run belongs to.
 * Uses midpoint boundaries: the boundary between columns c and c+1 is the
 * midpoint of their start positions. This handles data runs that start
 * slightly before their column header (common in PDFs with centered headers).
 */
static int find_column(double x, const double *col_starts, int ncols)
{
    if (ncols <= 1) return 0;
    for (int c = ncols - 1; c >= 1; c--) {
        double mid = (col_starts[c - 1] + col_starts[c]) / 2.0;
        if (x >= mid)
            return c;
    }
    return 0;
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
                    /* Don't truncate mid-UTF-8 sequence */
                    if (copy < (int)rows[r].runs[t].text_len) {
                        while (copy > 0 && (rows[r].runs[t].text[copy - 1] & 0xC0) == 0x80)
                            copy--; /* Back up past continuation bytes */
                        if (copy > 0 && (unsigned char)rows[r].runs[t].text[copy - 1] >= 0xC0)
                            copy--; /* Lead byte at end of truncated copy is always incomplete */
                    }
                    if (copy > 0) {
                        memcpy(cell_buf + cell_len, rows[r].runs[t].text, (size_t)copy);
                        cell_len += copy;
                    }
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
                                  NxPdfOptions *opts, const char *filename,
                                  SHArena *arena, NxIssueList *issues,
                                  char **out_json, size_t *out_len)
{
    NxPdfOptions auto_opts = NX_PDF_AUTO_OPTIONS;

    if (!json_data || !out_json || !out_len) return NX_PDF_ERR_NULL;
    if (!arena) return NX_PDF_ERR_ARENA;

    /* Use caller's options, or auto-detect if NULL */
    if (!opts)
        opts = &auto_opts;

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
    if (nruns == MAX_TEXT_RUNS && issues)
        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                      -1, "", "text_runs_limit",
                      "Hit MAX_TEXT_RUNS=%d cap", MAX_TEXT_RUNS);

    /* Auto-detect clustering parameters if requested (writes back to opts) */
    if (opts->row_tolerance < 0 || opts->col_gap_min < 0)
        auto_detect_options(runs, nruns, arena, opts);

    /* Cluster into rows */
    ClusterRow *cluster_rows_arr = (ClusterRow *)sh_arena_alloc(arena,
        MAX_ROWS * sizeof(ClusterRow));
    if (!cluster_rows_arr) return NX_PDF_ERR_ARENA;

    int nrows = cluster_rows(runs, nruns, opts->row_tolerance,
                             arena, cluster_rows_arr, MAX_ROWS);
    if (nrows < 0) return NX_PDF_ERR_ARENA;
    if (nrows == 0) return NX_PDF_ERR_NO_TEXT;
    if (nrows == MAX_ROWS && issues)
        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                      -1, "", "rows_limit",
                      "Hit MAX_ROWS=%d cap", MAX_ROWS);

    /* Split wide text runs that contain multi-space gaps (column separators
     * encoded as whitespace within a single TJ/Tj text run) */
    split_wide_runs(cluster_rows_arr, nrows, arena, &nruns);

    /* Detect columns */
    double col_starts[MAX_COLS];
    int ncols = detect_columns(cluster_rows_arr, nrows, &opts->col_gap_min,
                               col_starts, MAX_COLS, arena, nruns);
    if (ncols == 0) return NX_PDF_ERR_NO_TEXT;

    /* Split data runs that span multiple column boundaries.
     * This handles PDFs where column values are encoded in a single text run
     * with fine kerning adjustments instead of separate text operations. */
    split_cross_column_runs(cluster_rows_arr, nrows, col_starts, ncols,
                            arena, &nruns);

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
