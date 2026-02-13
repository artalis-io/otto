/*
 * sh_pdf2struc.h - Pure C PDF Text Extraction Library
 *
 * Extracts positioned text blocks from PDFs, producing {text, x, y, w, h}
 * output compatible with pdfplumber's extract_words() format.
 *
 * Target PDFs: Excel/Word-generated landscape A4 table documents.
 * Supports classic xref, xref streams, object streams, FlateDecode,
 * WinAnsi + CID/Identity-H fonts with ToUnicode CMaps.
 *
 * Dependencies: sh_arena.h, sh_inflate.h, <stdio.h>, <math.h>
 */

#ifndef SH_PDF2STRUC_H
#define SH_PDF2STRUC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct ShPdf2strucCtx ShPdf2strucCtx;

typedef struct {
    int    page_index;   /* 0-based */
    double x, y, w, h;  /* PDF points */
    const char *text;    /* UTF-8, null-terminated, valid until ctx destroyed */
} ShPdf2strucBlock;

typedef enum {
    SH_PDF2STRUC_OK = 0,
    SH_PDF2STRUC_ERR_INVALID_PDF,
    SH_PDF2STRUC_ERR_UNSUPPORTED,   /* encrypted, unknown filter, etc. */
    SH_PDF2STRUC_ERR_IO,
    SH_PDF2STRUC_ERR_OOM
} ShPdf2strucStatus;

typedef enum {
    SH_PDF2STRUC_EMIT_RUNS   = 0,   /* One block per Tj/TJ string */
    SH_PDF2STRUC_EMIT_BLOCKS = 1    /* Merge runs into word-level blocks */
} ShPdf2strucEmitMode;

typedef struct {
    ShPdf2strucEmitMode emit_mode;
    int    origin_top_left;   /* 1 = convert y to top-left (default) */
    int    approx_widths;     /* 1 = estimate w when font widths missing */
    double merge_y_epsilon;   /* Block grouping y tolerance (default: 1.0) */
    double merge_x_gap;       /* Block grouping x gap max (default: 3.0) */
} ShPdf2strucOpts;

typedef void (*ShPdf2strucCallback)(void *user, const ShPdf2strucBlock *block);

/* ============================================================================
 * API
 * ============================================================================ */

ShPdf2strucCtx    *sh_pdf2struc_create(void);
void               sh_pdf2struc_destroy(ShPdf2strucCtx *ctx);
void               sh_pdf2struc_opts_default(ShPdf2strucOpts *opt);

ShPdf2strucStatus  sh_pdf2struc_extract_file(
    ShPdf2strucCtx *ctx, const char *path,
    const ShPdf2strucOpts *opt, ShPdf2strucCallback cb, void *user);

ShPdf2strucStatus  sh_pdf2struc_extract_mem(
    ShPdf2strucCtx *ctx, const uint8_t *data, size_t size,
    const ShPdf2strucOpts *opt, ShPdf2strucCallback cb, void *user);

const char *sh_pdf2struc_last_error(const ShPdf2strucCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SH_PDF2STRUC_H */
