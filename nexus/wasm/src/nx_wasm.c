/*
 * nx_wasm.c - Nexus WASM API
 *
 * Exposes the three-stage document ingestion pipeline to JavaScript:
 *   Stage A: Extract (XLSX/PDF/CSV -> raw JSON rows)
 *   Stage B: Transform (schema-driven -> canonical JSON)
 *   Stage X: Validate (geo_bounds, format, unique, outlier)
 *
 * For PDF files, the WASM wrapper performs sh_pdf2struc extraction
 * in-process (no filesystem), then passes text-run JSON to nx_ingest.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "nx_ingest.h"
#include "nx_merge.h"
#include "nx_xform.h"
#include "nx_validate.h"
#include "nx_discover.h"
#include "sh_arena.h"
#include "sh_pdf2struc.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

#define NX_WASM_ARENA_SIZE (32 * 1024 * 1024)  /* 32 MB */
#define MAX_PDF_TEXT_SIZE   (16 * 1024 * 1024)  /* 16 MB max text-run JSON */

/* ============================================================================
 * Global Response Buffers (heap-allocated, freed on next call)
 * ============================================================================ */

static char  *g_extract_buf   = NULL;  static size_t g_extract_len   = 0;
static char  *g_transform_buf = NULL;  static size_t g_transform_len = 0;
static char  *g_validate_buf  = NULL;  static size_t g_validate_len  = 0;
static char  *g_discover_buf  = NULL;  static size_t g_discover_len  = 0;

/* ============================================================================
 * PDF Text Extraction (copied from nx_pipeline.c, adapted for memory input)
 * ============================================================================ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    cur_page;
    int    text_count;
    int    error;
} PdfJsonCollector;

static void pdf_json_append(PdfJsonCollector *c, const char *s, size_t slen)
{
    if (c->error) return;
    while (c->len + slen + 1 > c->cap) {
        size_t newcap = c->cap * 2;
        if (newcap < c->cap || newcap > MAX_PDF_TEXT_SIZE) { c->error = 1; return; }
        char *nb = (char *)realloc(c->buf, newcap);
        if (!nb) { c->error = 1; return; }
        c->buf = nb;
        c->cap = newcap;
    }
    memcpy(c->buf + c->len, s, slen);
    c->len += slen;
}

static void pdf_json_appendf(PdfJsonCollector *c, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) pdf_json_append(c, tmp, (size_t)n);
}

static void pdf_json_append_escaped(PdfJsonCollector *c, const char *s)
{
    pdf_json_append(c, "\"", 1);
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  pdf_json_append(c, "\\\"", 2); break;
            case '\\': pdf_json_append(c, "\\\\", 2); break;
            case '\b': pdf_json_append(c, "\\b", 2); break;
            case '\f': pdf_json_append(c, "\\f", 2); break;
            case '\n': pdf_json_append(c, "\\n", 2); break;
            case '\r': pdf_json_append(c, "\\r", 2); break;
            case '\t': pdf_json_append(c, "\\t", 2); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    pdf_json_append(c, esc, 6);
                } else {
                    pdf_json_append(c, p, 1);
                }
                break;
        }
    }
    pdf_json_append(c, "\"", 1);
}

static void pdf_close_page(PdfJsonCollector *c)
{
    if (c->cur_page >= 0) {
        pdf_json_append(c, "]}", 2);
    }
}

static void pdf_block_cb(void *user, const ShPdf2strucBlock *block)
{
    PdfJsonCollector *c = (PdfJsonCollector *)user;
    if (c->error) return;

    if (block->page_index != c->cur_page) {
        pdf_close_page(c);
        if (c->cur_page >= 0) pdf_json_append(c, ",", 1);
        pdf_json_appendf(c, "{\"page\":%d,\"width\":%.1f,\"height\":%.1f,\"texts\":[",
                         block->page_index + 1, block->page_width, block->page_height);
        c->cur_page = block->page_index;
        c->text_count = 0;
    }

    if (c->text_count > 0) pdf_json_append(c, ",", 1);
    pdf_json_append(c, "{\"text\":", 8);
    pdf_json_append_escaped(c, block->text);
    pdf_json_appendf(c, ",\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}",
                     block->x, block->y, block->w, block->h);
    c->text_count++;
}

/*
 * Extract text runs from PDF bytes in memory.
 * Returns heap-allocated JSON string, caller must free.
 */
static char *extract_pdf_mem(const uint8_t *data, size_t len, size_t *out_len)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    if (!ctx) return NULL;

    ShPdf2strucOpts opts;
    sh_pdf2struc_opts_default(&opts);
    opts.emit_mode = SH_PDF2STRUC_EMIT_BLOCKS;
    opts.origin_top_left = 1;

    PdfJsonCollector collector = {0};
    collector.cap = 256 * 1024;
    collector.cur_page = -1;
    collector.buf = (char *)malloc(collector.cap);
    if (!collector.buf) {
        sh_pdf2struc_destroy(ctx);
        return NULL;
    }

    pdf_json_append(&collector, "{\"pages\":[", 10);

    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(
        ctx, data, len, &opts, pdf_block_cb, &collector);

    sh_pdf2struc_destroy(ctx);

    if (st != SH_PDF2STRUC_OK || collector.error) {
        free(collector.buf);
        return NULL;
    }

    pdf_close_page(&collector);
    pdf_json_append(&collector, "]}", 2);
    collector.buf[collector.len] = '\0';

    *out_len = collector.len;
    return collector.buf;
}

/* ============================================================================
 * WASM Exports
 * ============================================================================ */

WASM_EXPORT
int nx_wasm_version(void)
{
    return 1;
}

/*
 * Stage A: Extract raw rows from document bytes.
 *
 * format: 0 = XLSX, 1 = PDF_JSON (text-run JSON), 2 = CSV, 3 = PDF bytes
 * Returns 0 on success, -1 on error.
 */
WASM_EXPORT
int nx_wasm_extract(const uint8_t *data, int len, int format)
{
    free(g_extract_buf);
    g_extract_buf = NULL;
    g_extract_len = 0;

    if (!data || len <= 0) return -1;

    /* PDF bytes: extract text-run JSON first, then pass to nx_ingest as PDF_JSON */
    if (format == 3) {
        size_t pdf_json_len = 0;
        char *pdf_json = extract_pdf_mem(data, (size_t)len, &pdf_json_len);
        if (!pdf_json) return -1;

        /* Now run Stage A with the text-run JSON */
        char *raw = NULL, *canon = NULL;
        size_t raw_len = 0, canon_len = 0;
        NxIngestStatus st = nx_ingest(
            pdf_json, pdf_json_len,
            NX_FORMAT_PDF_JSON, "upload.pdf",
            NULL, 0,           /* No schema for Stage A only */
            &raw, &raw_len,
            &canon, &canon_len);

        free(pdf_json);
        free(canon);

        if (st != NX_INGEST_OK || !raw) return -1;
        g_extract_buf = raw;
        g_extract_len = raw_len;
        return 0;
    }

    /* Map WASM format to NxIngestFormat */
    NxIngestFormat nx_fmt;
    const char *filename;
    switch (format) {
        case 0: nx_fmt = NX_FORMAT_XLSX;     filename = "upload.xlsx"; break;
        case 1: nx_fmt = NX_FORMAT_PDF_JSON; filename = "upload.pdf";  break;
        case 2: nx_fmt = NX_FORMAT_CSV;      filename = "upload.csv";  break;
        default: return -1;
    }

    char *raw = NULL, *canon = NULL;
    size_t raw_len = 0, canon_len = 0;
    NxIngestStatus st = nx_ingest(
        data, (size_t)len,
        nx_fmt, filename,
        NULL, 0,
        &raw, &raw_len,
        &canon, &canon_len);

    free(canon);
    if (st != NX_INGEST_OK || !raw) return -1;
    g_extract_buf = raw;
    g_extract_len = raw_len;
    return 0;
}

WASM_EXPORT
const char *nx_wasm_extract_result(void)
{
    return g_extract_buf ? g_extract_buf : "";
}

WASM_EXPORT
int nx_wasm_extract_result_len(void)
{
    return (int)g_extract_len;
}

/*
 * Stage B: Transform raw JSON to canonical using schema.
 * Returns 0 on success, -1 on error.
 */
WASM_EXPORT
int nx_wasm_transform(const char *raw_json, int raw_len,
                      const char *schema_json, int schema_len)
{
    free(g_transform_buf);
    g_transform_buf = NULL;
    g_transform_len = 0;

    if (!raw_json || raw_len <= 0 || !schema_json || schema_len <= 0) return -1;

    SHArena *arena = sh_arena_create(NX_WASM_ARENA_SIZE);
    if (!arena) return -1;

    /* Merge continuation rows if schema has row_merge config */
    const char *xform_input = raw_json;
    size_t xform_input_len = (size_t)raw_len;
    char *merged = NULL;
    size_t merged_len = 0;
    NxMergeStatus ms = nx_merge_rows(raw_json, (size_t)raw_len,
                                      schema_json, (size_t)schema_len,
                                      arena, &merged, &merged_len);
    if (ms == NX_MERGE_OK && merged) {
        xform_input = merged;
        xform_input_len = merged_len;
    }

    /* Reset arena for transform (merge output is heap-allocated) */
    sh_arena_reset(arena);

    char *out = NULL;
    size_t out_len = 0;
    NxXformStatus st = nx_xform_apply(
        xform_input, xform_input_len,
        schema_json, (size_t)schema_len,
        arena, &out, &out_len);

    sh_arena_free(arena);
    free(merged);

    if (st != NX_XFORM_OK || !out) return -1;
    g_transform_buf = out;
    g_transform_len = out_len;
    return 0;
}

WASM_EXPORT
const char *nx_wasm_transform_result(void)
{
    return g_transform_buf ? g_transform_buf : "";
}

WASM_EXPORT
int nx_wasm_transform_result_len(void)
{
    return (int)g_transform_len;
}

/*
 * Stage X: Validate canonical JSON against schema rules.
 * Returns 0 on success, -1 on error.
 */
WASM_EXPORT
int nx_wasm_validate(const char *canonical_json, int canon_len,
                     const char *schema_json, int schema_len)
{
    free(g_validate_buf);
    g_validate_buf = NULL;
    g_validate_len = 0;

    if (!canonical_json || canon_len <= 0 || !schema_json || schema_len <= 0) return -1;

    SHArena *arena = sh_arena_create(NX_WASM_ARENA_SIZE);
    if (!arena) return -1;

    char *out = NULL;
    size_t out_len = 0;
    NxValidateStatus st = nx_validate(
        canonical_json, (size_t)canon_len,
        schema_json, (size_t)schema_len,
        arena, &out, &out_len);

    sh_arena_free(arena);

    if (st != NX_VALIDATE_OK || !out) return -1;
    g_validate_buf = out;
    g_validate_len = out_len;
    return 0;
}

WASM_EXPORT
const char *nx_wasm_validate_result(void)
{
    return g_validate_buf ? g_validate_buf : "";
}

WASM_EXPORT
int nx_wasm_validate_result_len(void)
{
    return (int)g_validate_len;
}

/*
 * Auto-discover schema from raw JSON (Stage A output).
 * Returns 0 on success, -1 on error.
 */
WASM_EXPORT
int nx_wasm_discover(const char *raw_json, int raw_len)
{
    free(g_discover_buf);
    g_discover_buf = NULL;
    g_discover_len = 0;

    if (!raw_json || raw_len <= 0) return -1;

    SHArena *arena = sh_arena_create(NX_WASM_ARENA_SIZE);
    if (!arena) return -1;

    char *out = NULL;
    size_t out_len = 0;
    NxDiscoverStatus st = nx_discover_schema(
        raw_json, (size_t)raw_len,
        arena, &out, &out_len);

    sh_arena_free(arena);

    if (st != NX_DISCOVER_OK || !out) return -1;
    g_discover_buf = out;
    g_discover_len = out_len;
    return 0;
}

WASM_EXPORT
const char *nx_wasm_discover_result(void)
{
    return g_discover_buf ? g_discover_buf : "";
}

WASM_EXPORT
int nx_wasm_discover_result_len(void)
{
    return (int)g_discover_len;
}
