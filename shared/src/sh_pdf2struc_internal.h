/*
 * sh_pdf2struc_internal.h - Internal structures shared between sh_pdf2struc.c
 *                           and sh_pdf2struc_text.c
 */

#ifndef SH_PDF2STRUC_INTERNAL_H
#define SH_PDF2STRUC_INTERNAL_H

#include "sh_pdf2struc.h"
#include "sh_arena.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Limits
 * ============================================================================ */

#define PDF_MAX_ERROR_LEN      256
#define PDF_MAX_OBJECTS        100000
#define PDF_MAX_PAGES          10000
#define PDF_MAX_FONTS          256
#define PDF_MAX_GSTATE_DEPTH   64
#define PDF_MAX_OPERAND_STACK  256
#define PDF_MAX_TOUNICODE      65536
#define PDF_MAX_WIDTHS         65536
#define PDF_MAX_CONTENT_OPS    500000
#define PDF_MAX_RUNS           200000
#define PDF_MAX_DECOMPRESS     (64 * 1024 * 1024)  /* 64 MB */
#define PDF_MAX_OBJSTM_OBJS   500
#define PDF_MAX_XREF_SIZE     100000
#define PDF_ENCODING_ENTRIES  256   /* Single-byte encoding = 256 char codes */

/* ============================================================================
 * PDF Scanner (shared tokenizer state)
 * ============================================================================ */

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
} PdfScanner;

/* ============================================================================
 * PDF Object Model
 * ============================================================================ */

typedef enum {
    PDF_OBJ_NULL = 0,
    PDF_OBJ_BOOL,
    PDF_OBJ_INT,
    PDF_OBJ_REAL,
    PDF_OBJ_NAME,
    PDF_OBJ_STRING,
    PDF_OBJ_HEXSTRING,
    PDF_OBJ_ARRAY,
    PDF_OBJ_DICT,
    PDF_OBJ_STREAM,
    PDF_OBJ_REF
} PdfObjType;

typedef struct PdfObj PdfObj;

typedef struct {
    PdfObj **items;
    int      count;
    int      capacity;
} PdfArray;

typedef struct {
    const char **keys;   /* Name strings (without /) */
    PdfObj     **vals;
    int          count;
    int          capacity;
} PdfDict;

struct PdfObj {
    PdfObjType type;
    union {
        int         bool_val;
        int64_t     int_val;
        double      real_val;
        const char *name_val;    /* Without leading / */
        struct {
            const uint8_t *data;
            size_t         len;
        } string_val;
        PdfArray    array_val;
        PdfDict     dict_val;
        struct {
            PdfDict     dict;
            size_t      offset;    /* Byte offset to stream data in PDF */
            size_t      length;    /* /Length value (compressed size) */
        } stream_val;
        struct {
            int obj_num;
            int gen_num;
        } ref_val;
    };
};

/* ============================================================================
 * Xref Table
 * ============================================================================ */

typedef struct {
    size_t offset;     /* Byte offset in file */
    int    gen;        /* Generation number */
    int    in_use;     /* 1 = in use, 0 = free */
    int    compressed; /* 1 = in ObjStm */
    int    stm_obj;    /* Object stream number (if compressed) */
    int    stm_idx;    /* Index within object stream (if compressed) */
} PdfXrefEntry;

/* ============================================================================
 * Font Structures
 * ============================================================================ */

typedef enum {
    PDF_FONT_UNKNOWN = 0,
    PDF_FONT_TYPE1,       /* WinAnsi / Standard encoding */
    PDF_FONT_TRUETYPE,    /* WinAnsi */
    PDF_FONT_TYPE0,       /* CID composite (Identity-H) */
    PDF_FONT_TYPE3
} PdfFontType;

typedef struct {
    uint32_t glyph_id;
    uint32_t codepoint;   /* Primary Unicode codepoint (for single-char) */
    char     text[16];    /* UTF-8 string (for multi-codepoint sequences) */
} PdfToUnicodeEntry;

typedef struct {
    uint32_t cid;
    double   width;       /* In 1/1000 of text space */
} PdfCidWidth;

typedef struct {
    const char    *name;          /* Font resource name (e.g., "F1") */
    PdfFontType    type;
    double         ascent;        /* Fraction of em (e.g., 0.8) */
    double         descent;       /* Fraction of em (e.g., -0.2) */

    /* WinAnsi / simple font */
    int            first_char;
    int            last_char;
    double        *widths;        /* widths[charcode - first_char], 1/1000 units */
    int            widths_count;
    const uint16_t *encoding;     /* Custom encoding table (NULL = WinAnsi) */
    uint16_t       encoding_buf[PDF_ENCODING_ENTRIES]; /* Storage for custom encoding */

    /* CID / composite font */
    PdfCidWidth   *cid_widths;
    int            cid_width_count;
    double         default_width; /* /DW, default 1000 */

    /* ToUnicode CMap */
    PdfToUnicodeEntry *tounicode;
    int                tounicode_count;
    int                has_tounicode;
} PdfFont;

/* ============================================================================
 * Text State & Graphics State
 * ============================================================================ */

typedef struct {
    double a, b, c, d, e, f;  /* 3x3 matrix: [a b 0; c d 0; e f 1] */
} PdfMatrix;

typedef struct {
    PdfMatrix ctm;
    /* Text state */
    int    font_idx;     /* Index into ctx->fonts[] */
    double font_size;
    double char_space;   /* Tc */
    double word_space;   /* Tw */
    double h_scaling;    /* Tz (percentage, 100 = normal) */
    double leading;      /* TL */
    double rise;         /* Ts */
} PdfGState;

/* ============================================================================
 * Raw Text Run (before grouping)
 * ============================================================================ */

typedef struct {
    int    page_index;
    double x, y, w, h;
    const char *text;    /* Arena-allocated UTF-8 */
} PdfTextRun;

/* ============================================================================
 * Page Info
 * ============================================================================ */

typedef struct {
    int     obj_num;
    double  width;
    double  height;
    int     rotate;      /* /Rotate value (0, 90, 180, 270) */
    PdfObj *resources;   /* /Resources dict (may be inherited) */
    PdfObj *contents;    /* /Contents ref or array */
    PdfObj *mediabox;    /* /MediaBox array */
} PdfPageInfo;

/* ============================================================================
 * Main Context
 * ============================================================================ */

struct ShPdf2strucCtx {
    SHArena *arena;

    /* Error */
    char error[PDF_MAX_ERROR_LEN];

    /* Input data */
    const uint8_t *data;
    size_t         data_size;

    /* Xref table */
    PdfXrefEntry  *xref;
    int            xref_size;

    /* Parsed objects cache: obj_num -> PdfObj* */
    PdfObj       **obj_cache;
    int            obj_cache_size;

    /* Page tree */
    PdfPageInfo   *pages;
    int            page_count;

    /* Fonts */
    PdfFont       *fonts;
    int            font_count;

    /* Text runs (raw output before grouping) */
    PdfTextRun    *runs;
    int            run_count;
    int            run_capacity;
};

/* ============================================================================
 * Inline Helpers (shared between .c files)
 * ============================================================================ */

static inline int pdf_is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\0';
}

/* ============================================================================
 * Internal Functions (sh_pdf2struc.c -> used by sh_pdf2struc_text.c)
 * ============================================================================ */

/* Set error message */
void pdf_set_error(ShPdf2strucCtx *ctx, const char *fmt, ...);

/* Resolve an indirect reference to its target object */
PdfObj *pdf_resolve(ShPdf2strucCtx *ctx, PdfObj *obj);

/* Dict helpers */
PdfObj     *pdf_dict_get(PdfDict *d, const char *key);
int64_t     pdf_dict_get_int(ShPdf2strucCtx *ctx, PdfDict *d, const char *key, int64_t def);
double      pdf_dict_get_real(ShPdf2strucCtx *ctx, PdfDict *d, const char *key, double def);
const char *pdf_dict_get_name(ShPdf2strucCtx *ctx, PdfDict *d, const char *key);

/* Array helpers */
int     pdf_array_len(PdfObj *arr);
PdfObj *pdf_array_get(PdfObj *arr, int idx);

/* Decompress a stream object, returns arena-allocated buffer */
uint8_t *pdf_decompress_stream(ShPdf2strucCtx *ctx, PdfObj *stream_obj,
                                size_t *out_len);

/* Arena string duplication */
char *pdf_arena_strndup(ShPdf2strucCtx *ctx, const char *s, size_t len);

/* Parse a PDF object from a scanner (needed by content stream parser) */
PdfObj *pdf_parse_obj(ShPdf2strucCtx *ctx, PdfScanner *s);

/* ============================================================================
 * Internal Functions (sh_pdf2struc_text.c)
 * ============================================================================ */

/* Extract text from all pages (called from sh_pdf2struc.c) */
ShPdf2strucStatus pdf_extract_text(ShPdf2strucCtx *ctx, const ShPdf2strucOpts *opt);

/* Group raw runs into word-level blocks */
ShPdf2strucStatus pdf_group_runs(ShPdf2strucCtx *ctx, const ShPdf2strucOpts *opt,
                                  ShPdf2strucCallback cb, void *user);

#endif /* SH_PDF2STRUC_INTERNAL_H */
